#include "captive_portal.h"

#include <HTTPClient.h>
#include <WiFi.h>

namespace {

// Apple/Google use these tiny endpoints exactly so devices can detect captive
// portals. /generate_204 returns 204 No Content on real internet; behind a
// portal it gets rewritten to a 200 HTML page or 302 redirect.
constexpr const char* kProbeUrl = "http://www.gstatic.com/generate_204";
constexpr int kProbeTimeoutMs = 5000;
constexpr int kFormTimeoutMs = 8000;
constexpr int kMaxAcceptAttempts = 2;
constexpr const char* kFakeUserAgent =
    "Mozilla/5.0 (Linux; XteinkAgenda) AppleWebKit/537.36";

String g_lastPortalUrl;

String urlEncode(const String& s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else if (c == ' ') {
            out += '+';
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            out += buf;
        }
    }
    return out;
}

// Read attribute value from an HTML tag string. Handles single quotes,
// double quotes, and unquoted values. Case-insensitive on the attribute name.
String attrValue(const String& tag, const char* attrName) {
    String haystack = tag;
    haystack.toLowerCase();
    String needle = String(attrName) + "=";
    int pos = haystack.indexOf(needle);
    if (pos < 0) return "";
    pos += needle.length();
    if (pos >= (int)tag.length()) return "";
    char q = tag[pos];
    int valStart, valEnd;
    if (q == '"' || q == '\'') {
        valStart = pos + 1;
        valEnd = tag.indexOf(q, valStart);
    } else {
        valStart = pos;
        valEnd = valStart;
        while (valEnd < (int)tag.length()) {
            char c = tag[valEnd];
            if (c == ' ' || c == '\t' || c == '\n' || c == '>' || c == '/') break;
            valEnd++;
        }
    }
    if (valEnd < 0 || valEnd <= valStart) return "";
    return tag.substring(valStart, valEnd);
}

// Resolve `target` (possibly relative) against `base` (absolute).
String resolveUrl(const String& base, const String& target) {
    if (target.length() == 0) return base;
    if (target.startsWith("http://") || target.startsWith("https://")) return target;

    int schemeEnd = base.indexOf("://");
    if (schemeEnd < 0) return target;
    int hostStart = schemeEnd + 3;

    if (target.startsWith("/")) {
        int pathStart = base.indexOf('/', hostStart);
        return (pathStart < 0 ? base : base.substring(0, pathStart)) + target;
    }
    // Relative: strip filename from base path, then append target.
    int slash = base.lastIndexOf('/');
    if (slash <= hostStart) return base + "/" + target;
    return base.substring(0, slash + 1) + target;
}

// Probe gstatic /generate_204 once. Tri-state classification:
//   Clear         — 204 (real internet)
//   PortalLikely  — 200 OR 3xx with Location header (something rewrote the response)
//   Ambiguous     — timeout / DNS / connection error / 4xx / 5xx (no portal evidence)
//
// We treat 200 as portal-likely because real /generate_204 NEVER returns 200 —
// only a portal would rewrite the response. 4xx/5xx/timeouts get Ambiguous so
// a slow router doesn't get misdiagnosed as a portal.
CaptivePortal::ProbeResult probeOnce() {
    using CaptivePortal::ProbeStatus;
    CaptivePortal::ProbeResult result{ProbeStatus::Ambiguous, String(), 0};

    HTTPClient http;
    http.setConnectTimeout(kProbeTimeoutMs);
    http.setTimeout(kProbeTimeoutMs);
    const char* trackHeaders[] = {"Location"};
    http.begin(kProbeUrl);
    http.collectHeaders(trackHeaders, 1);
    int code = http.GET();
    result.code = code;
    String loc = (code > 0) ? http.header("Location") : String();
    int bodyLen = (code == 200) ? (int)http.getSize() : 0;
    http.end();

    if (code == 204) {
        result.status = ProbeStatus::Clear;
    } else if (code >= 300 && code < 400 && loc.length() > 0) {
        result.status = ProbeStatus::PortalLikely;
        result.portalUrl = loc;
    } else if (code == 200) {
        result.status = ProbeStatus::PortalLikely;
        result.portalUrl = String(kProbeUrl);  // portal intercepts the same URL
    } else {
        result.status = ProbeStatus::Ambiguous;
    }

    const char* label = (result.status == ProbeStatus::Clear) ? "Clear"
                      : (result.status == ProbeStatus::PortalLikely) ? "PortalLikely"
                      : "Ambiguous";
    Serial.printf("Probe: code=%d body_len=%d loc=%s classified=%s\n",
                  code, bodyLen, loc.c_str(), label);
    return result;
}

// Fetch portal HTML, find first <form>, build a submit body, send it.
// Returns true if a submission was made (which doesn't mean it worked — caller
// re-probes). Returns false if the portal needs human input we can't fake.
bool tryAcceptForm(String& portalUrl) {
    Serial.printf("Captive: GET %s\n", portalUrl.c_str());
    HTTPClient http;
    http.setUserAgent(kFakeUserAgent);
    http.setConnectTimeout(kFormTimeoutMs);
    http.setTimeout(kFormTimeoutMs);
    const char* trackHeaders[] = {"Location"};
    http.begin(portalUrl);
    http.collectHeaders(trackHeaders, 1);
    int code = http.GET();
    String html = http.getString();
    String redirect = http.header("Location");
    http.end();

    String currentUrl = portalUrl;

    // Some portals redirect through 1-2 hops before serving the form.
    for (int i = 0; i < 2 && (code == 301 || code == 302) && redirect.length() > 0; i++) {
        currentUrl = resolveUrl(currentUrl, redirect);
        Serial.printf("Captive: -> %s\n", currentUrl.c_str());
        http.begin(currentUrl);
        http.collectHeaders(trackHeaders, 1);
        code = http.GET();
        html = http.getString();
        redirect = http.header("Location");
        http.end();
    }

    if (code != 200 || html.length() == 0) {
        Serial.printf("Captive: portal fetch failed (code=%d, len=%u)\n",
                      code, (unsigned)html.length());
        return false;
    }
    portalUrl = currentUrl;

    int formStart = html.indexOf("<form");
    if (formStart < 0) {
        // Pure-JS portal — we can't do anything.
        Serial.println("Captive: no <form> in HTML, JS-only portal?");
        return false;
    }
    int formTagEnd = html.indexOf('>', formStart);
    int formEnd = html.indexOf("</form>", formStart);
    if (formTagEnd < 0 || formEnd < 0) return false;

    String formTag = html.substring(formStart, formTagEnd + 1);
    String action = attrValue(formTag, "action");
    String method = attrValue(formTag, "method");
    method.toLowerCase();
    if (method.length() == 0) method = "get";

    String submitUrl = action.length() > 0 ? resolveUrl(currentUrl, action) : currentUrl;
    String formInner = html.substring(formTagEnd + 1, formEnd);

    // Walk all <input> tags, building a urlencoded body. Bail if we hit a
    // password/email/tel/number field — those need real human input.
    String body;
    int searchPos = 0;
    while (true) {
        int inputStart = formInner.indexOf("<input", searchPos);
        if (inputStart < 0) break;
        int inputEnd = formInner.indexOf('>', inputStart);
        if (inputEnd < 0) break;
        String inputTag = formInner.substring(inputStart, inputEnd + 1);
        searchPos = inputEnd + 1;

        String name = attrValue(inputTag, "name");
        if (name.length() == 0) continue;

        String type = attrValue(inputTag, "type");
        type.toLowerCase();
        String value = attrValue(inputTag, "value");

        String submitVal;
        if (type == "" || type == "hidden" || type == "submit" || type == "text" ||
            type == "button" || type == "image") {
            submitVal = value;  // pass through whatever the server set
        } else if (type == "checkbox" || type == "radio") {
            submitVal = value.length() > 0 ? value : "on";  // assume "I accept" should be on
        } else if (type == "password" || type == "email" || type == "tel" ||
                   type == "number" || type == "search") {
            Serial.printf("Captive: form needs real input (%s) — bailing\n", type.c_str());
            return false;
        } else {
            submitVal = value;
        }

        if (body.length() > 0) body += "&";
        body += urlEncode(name) + "=" + urlEncode(submitVal);
    }

    Serial.printf("Captive: submit %s %s\n", method.c_str(), submitUrl.c_str());
    HTTPClient submit;
    submit.setUserAgent(kFakeUserAgent);
    submit.setConnectTimeout(kFormTimeoutMs);
    submit.setTimeout(kFormTimeoutMs);
    int submitCode;
    if (method == "post") {
        submit.begin(submitUrl);
        submit.addHeader("Content-Type", "application/x-www-form-urlencoded");
        submitCode = submit.POST(body);
    } else {
        String url = submitUrl;
        url += (url.indexOf('?') >= 0 ? "&" : "?");
        url += body;
        submit.begin(url);
        submitCode = submit.GET();
    }
    Serial.printf("Captive: submit code=%d\n", submitCode);
    submit.end();

    delay(1500);  // give the gateway time to whitelist us
    return true;
}

}  // namespace

namespace CaptivePortal {

ProbeResult probe() {
    return probeOnce();
}

Result tryAutoAccept() {
    g_lastPortalUrl = "";
    ProbeResult r = probeOnce();
    if (r.status == ProbeStatus::Clear) return NotPresent;

    // Inconclusive first probe — could be a slow DHCP/DNS settle, not a portal.
    // Retry once before deciding either way.
    if (r.status == ProbeStatus::Ambiguous) {
        delay(1500);
        ProbeResult r2 = probeOnce();
        if (r2.status == ProbeStatus::Clear)     return NotPresent;
        if (r2.status == ProbeStatus::Ambiguous) {
            // Two ambiguous probes in a row — don't claim a portal. Let the
            // user proceed; downstream agenda fetch will surface a real error
            // if the network truly can't reach the server.
            Serial.println("Captive: probe inconclusive after retry, proceeding as connected");
            return NotPresent;
        }
        r = r2;  // promoted to PortalLikely
    }

    // PortalLikely: try the form-submit dance.
    g_lastPortalUrl = r.portalUrl;
    String portalUrl = r.portalUrl;
    for (int attempt = 0; attempt < kMaxAcceptAttempts; attempt++) {
        if (!tryAcceptForm(portalUrl)) break;
        ProbeResult after = probeOnce();
        if (after.status == ProbeStatus::Clear) {
            g_lastPortalUrl = "";
            return Resolved;
        }
        // Some portals return a second form (the actual landing page) after the
        // first POST. Try once more before giving up.
    }
    return Unresolvable;
}

const char* lastPortalUrl() {
    return g_lastPortalUrl.c_str();
}

}  // namespace CaptivePortal
