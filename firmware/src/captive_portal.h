#pragma once

#include <Arduino.h>

// Best-effort captive-portal auto-accepter. Works only on the simplest portals
// (single HTML form with an "I accept" checkbox or just hidden state). Bails
// on anything that requires real input (email, password, SMS, captcha) or
// runs JavaScript to compute tokens.
//
// Detection is tri-state: gstatic /generate_204 should return exactly 204 on
// real internet; any other code (200, 3xx, 4xx, timeout) is either a portal
// rewriting the response (200/3xx) or a generic network failure (timeout/4xx/
// 5xx). We classify these separately so a slow-but-portal-free network never
// gets stuck on the "couldn't auto-accept" screen.
namespace CaptivePortal {
    enum Result {
        NotPresent,    // /generate_204 returned 204 — real internet, do nothing
        Resolved,      // Portal detected and our auto-submit cleared it
        Unresolvable,  // Portal detected but couldn't auto-accept (real human needed)
    };

    enum class ProbeStatus {
        Clear,          // 204 No Content — definitely real internet
        PortalLikely,   // 200 OR 3xx with Location — something rewrote our request
        Ambiguous,      // timeout / DNS / connection error / 4xx / 5xx — inconclusive
    };

    struct ProbeResult {
        ProbeStatus status;
        String portalUrl;  // captured Location on 3xx, or probe URL on 200; empty otherwise
        int code;          // HTTP code or negative HTTPClient error
    };

    // Probe gstatic once. Single round-trip. Caller can branch on status.
    ProbeResult probe();

    // Detect and try to auto-accept. Caller should be already associated to an AP
    // (DHCP lease in hand). Blocks for up to ~25 s. Treats Ambiguous probe
    // results as "no portal" after one retry — better to let the user proceed
    // and surface a downstream network error than show a false-positive stuck
    // screen for a normal home WiFi.
    Result tryAutoAccept();

    // The portal URL we last saw (for showing the user where to sign in manually).
    // Valid only after tryAutoAccept() returns Unresolvable.
    const char* lastPortalUrl();
}
