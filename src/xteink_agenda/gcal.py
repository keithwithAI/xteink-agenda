"""Google Calendar integration — fetch the day's agenda."""

from __future__ import annotations

import logging
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

from google.oauth2.service_account import Credentials
from googleapiclient.discovery import build
from googleapiclient.errors import HttpError

logger = logging.getLogger("xteink_agenda.gcal")

# Per-calendar fetches are I/O-bound — run in parallel to halve agenda latency
# when multiple calendars are configured. Cap workers so we don't thrash GCal.
_GCAL_FETCH_WORKERS = 4


@dataclass
class Event:
    title: str
    start: datetime
    end: datetime
    calendar_id: str
    duration_min: int
    description: str = ""
    gcal_event_id: str = ""
    ical_uid: str = ""

    @property
    def is_active(self) -> bool:
        now = datetime.now(self.start.tzinfo)
        return self.start <= now < self.end


@dataclass
class AgendaState:
    now: Event | None
    next: list[Event]
    remaining_today: int
    day_progress: float  # 0.0–1.0
    generated_at: datetime
    view_name: str = ""


def _build_service(service_account_path: Path, readonly: bool = True):
    scope = "https://www.googleapis.com/auth/calendar.readonly" if readonly else \
        "https://www.googleapis.com/auth/calendar"
    creds = Credentials.from_service_account_file(
        str(service_account_path),
        scopes=[scope],
    )
    import httplib2
    from google_auth_httplib2 import AuthorizedHttp
    http = AuthorizedHttp(creds, http=httplib2.Http(timeout=30))
    return build("calendar", "v3", http=http)


def _parse_event_time(raw: dict, tz: ZoneInfo) -> datetime:
    if "dateTime" in raw:
        return datetime.fromisoformat(raw["dateTime"])
    # All-day event — return start of day
    return datetime.strptime(raw["date"], "%Y-%m-%d").replace(tzinfo=tz)


def _is_all_day(event_resource: dict) -> bool:
    return "date" in event_resource.get("start", {})


def _unique_preserving_order(values: list[str]) -> list[str]:
    seen = set()
    result = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return result


def _event_dedupe_key(event: Event) -> tuple[str, str, str, str]:
    start = event.start.isoformat()
    end = event.end.isoformat()
    if event.ical_uid:
        return ("ical", event.ical_uid, start, end)
    if event.gcal_event_id:
        return ("gcal", f"{event.calendar_id}:{event.gcal_event_id}", start, end)
    return ("fallback", f"{event.calendar_id}:{event.title}", start, end)


def _dedupe_events(events: list[Event]) -> list[Event]:
    deduped = []
    seen = set()
    for event in events:
        key = _event_dedupe_key(event)
        if key in seen:
            logger.info(
                "Dropping duplicate calendar event %r from %s (%s-%s)",
                event.title,
                event.calendar_id,
                event.start.isoformat(),
                event.end.isoformat(),
            )
            continue
        seen.add(key)
        deduped.append(event)
    return deduped


def get_agenda(
    service_account_path: Path,
    calendar_ids: list[str],
    view_name: str = "",
    lookahead_hours: int = 24,
    tz: ZoneInfo | None = None,
    target_start: datetime | None = None,
    target_end: datetime | None = None,
    day_progress_start: int = 7,
    day_progress_end: int = 23,
) -> AgendaState:
    """Fetch events from the given calendars and build an AgendaState.

    If target_start/target_end are provided, query that range.
    Otherwise, query from now through lookahead_hours.
    """
    if tz is None:
        tz = datetime.now().astimezone().tzinfo

    now = datetime.now(tz)
    if target_start and target_end:
        time_min = target_start.isoformat()
        time_max = target_end.isoformat()
    else:
        time_min = now.isoformat()
        time_max = (now + timedelta(hours=lookahead_hours)).isoformat()

    calendar_ids = _unique_preserving_order(calendar_ids)

    def _fetch_one(cal_id: str) -> list[Event]:
        """Fetch events for a single calendar. Returns [] on error (logged + skipped).

        Builds a fresh service per worker because httplib2/AuthorizedHttp aren't
        thread-safe — sharing a client across threads causes sporadic corruption.
        """
        events: list[Event] = []
        try:
            service = _build_service(service_account_path)
            page_token = None
            while True:
                result = (
                    service.events()
                    .list(
                        calendarId=cal_id,
                        timeMin=time_min,
                        timeMax=time_max,
                        singleEvents=True,
                        orderBy="startTime",
                        maxResults=250,
                        pageToken=page_token,
                    )
                    .execute()
                )
                for item in result.get("items", []):
                    if _is_all_day(item):
                        continue
                    start = _parse_event_time(item["start"], tz)
                    end = _parse_event_time(item["end"], tz)
                    duration = max(1, int((end - start).total_seconds() / 60))
                    events.append(
                        Event(
                            title=item.get("summary", "(No title)"),
                            start=start,
                            end=end,
                            calendar_id=cal_id,
                            duration_min=duration,
                            description=item.get("description", ""),
                            gcal_event_id=item.get("id", ""),
                            ical_uid=item.get("iCalUID", ""),
                        )
                    )
                page_token = result.get("nextPageToken")
                if not page_token:
                    break
        except HttpError as e:
            # Skip this calendar (404 deleted, 403 revoked, etc.) — don't fail entire agenda.
            status = getattr(e.resp, "status", "?")
            logger.warning("Skipping calendar %r (HTTP %s): %s", cal_id, status, e)
        except Exception as e:
            logger.warning("Skipping calendar %r (unexpected error): %s", cal_id, e)
        return events

    all_events: list[Event] = []
    if len(calendar_ids) == 1:
        # Single calendar — skip thread pool overhead
        all_events.extend(_fetch_one(calendar_ids[0]))
    elif calendar_ids:
        workers = min(len(calendar_ids), _GCAL_FETCH_WORKERS)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            for cal_events in pool.map(_fetch_one, calendar_ids):
                all_events.extend(cal_events)

    # Sort by start time, then collapse duplicate event copies returned from
    # multiple calendars. Shared/invited events can appear once per calendar but
    # keep the same iCalUID, which should render as one agenda item on the X4.
    all_events.sort(key=lambda e: e.start)
    all_events = _dedupe_events(all_events)

    # Find the currently active event (only when viewing today)
    current: Event | None = None
    upcoming: list[Event] = []

    for event in all_events:
        if not target_start and event.is_active and current is None:
            current = event
        else:
            upcoming.append(event)

    # Day progress: fraction of waking hours elapsed
    day_start = now.replace(hour=day_progress_start, minute=0, second=0, microsecond=0)
    day_end = now.replace(hour=max(day_progress_start + 1, day_progress_end), minute=0, second=0, microsecond=0)
    if now < day_start:
        progress = 0.0
    elif now > day_end:
        progress = 1.0
    else:
        span = (day_end - day_start).total_seconds()
        progress = (now - day_start).total_seconds() / span if span > 0 else 0.0

    # Return all events for the day — firmware scrolls and caps at MAX_ITEMS
    display_events = upcoming
    remaining = 0

    return AgendaState(
        now=current,
        next=display_events,
        remaining_today=remaining,
        day_progress=progress,
        generated_at=now,
        view_name=view_name,
    )


def delete_event(service_account_path: Path, calendar_id: str, event_id: str) -> None:
    """Delete an event, falling back to declining it when we're only an attendee.

    For attendee-only events (the service account's user isn't the organizer),
    delete() returns 403. Patch our attendee response to "declined" instead —
    that removes the event from the user's calendar view.
    """
    service = _build_service(service_account_path, readonly=False)
    try:
        service.events().delete(calendarId=calendar_id, eventId=event_id).execute()
    except HttpError as delete_err:
        if getattr(delete_err.resp, "status", None) != 403:
            raise
        ev = service.events().get(calendarId=calendar_id, eventId=event_id).execute()
        attendees = ev.get("attendees", [])
        patched = False
        for a in attendees:
            if a.get("email") == calendar_id or a.get("self"):
                a["responseStatus"] = "declined"
                patched = True
        if not patched:
            raise
        service.events().patch(
            calendarId=calendar_id,
            eventId=event_id,
            sendUpdates="none",
            body={"attendees": attendees},
        ).execute()
        logger.info("delete_event: declined attendee event %s", event_id)
