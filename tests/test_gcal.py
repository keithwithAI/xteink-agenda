"""Tests for Google Calendar agenda fetching."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from pathlib import Path

from xteink_agenda import gcal

TZ = timezone(timedelta(hours=1))


def _event_resource(
    *,
    event_id: str,
    ical_uid: str,
    summary: str = "Sleep",
    start: str = "2026-05-02T22:00:00+01:00",
    end: str = "2026-05-03T07:00:00+01:00",
) -> dict:
    return {
        "id": event_id,
        "iCalUID": ical_uid,
        "summary": summary,
        "start": {"dateTime": start},
        "end": {"dateTime": end},
    }


class _FakeRequest:
    def __init__(self, items: list[dict]):
        self._items = items

    def execute(self) -> dict:
        return {"items": self._items}


class _FakeEventsResource:
    def __init__(self, items_by_calendar: dict[str, list[dict]], fetched: list[str] | None = None):
        self._items_by_calendar = items_by_calendar
        self._fetched = fetched

    def list(self, *, calendarId: str, **_kwargs) -> _FakeRequest:
        if self._fetched is not None:
            self._fetched.append(calendarId)
        return _FakeRequest(self._items_by_calendar.get(calendarId, []))


class _FakeService:
    def __init__(self, items_by_calendar: dict[str, list[dict]], fetched: list[str] | None = None):
        self._items_by_calendar = items_by_calendar
        self._fetched = fetched

    def events(self) -> _FakeEventsResource:
        return _FakeEventsResource(self._items_by_calendar, self._fetched)


def test_get_agenda_deduplicates_shared_event_copies_by_ical_uid(monkeypatch):
    items_by_calendar = {
        "personal": [_event_resource(event_id="personal-copy", ical_uid="sleep-uid@example.com")],
        "work": [_event_resource(event_id="work-copy", ical_uid="sleep-uid@example.com")],
    }
    monkeypatch.setattr(gcal, "_build_service", lambda _path, readonly=True: _FakeService(items_by_calendar))

    agenda = gcal.get_agenda(
        service_account_path=Path("unused.json"),
        calendar_ids=["personal", "work"],
        target_start=datetime(2026, 5, 2, tzinfo=TZ),
        target_end=datetime(2026, 5, 3, tzinfo=TZ),
    )

    assert len(agenda.next) == 1
    assert agenda.next[0].title == "Sleep"
    assert agenda.next[0].gcal_event_id == "personal-copy"


def test_get_agenda_fetches_duplicate_calendar_id_once(monkeypatch):
    fetched = []
    items_by_calendar = {
        "personal": [_event_resource(event_id="sleep-event", ical_uid="sleep-uid@example.com")],
    }
    monkeypatch.setattr(
        gcal,
        "_build_service",
        lambda _path, readonly=True: _FakeService(items_by_calendar, fetched),
    )

    agenda = gcal.get_agenda(
        service_account_path=Path("unused.json"),
        calendar_ids=["personal", "personal"],
        target_start=datetime(2026, 5, 2, tzinfo=TZ),
        target_end=datetime(2026, 5, 3, tzinfo=TZ),
    )

    assert fetched == ["personal"]
    assert len(agenda.next) == 1


def test_get_agenda_skips_all_day_events(monkeypatch):
    items_by_calendar = {
        "personal": [
            {
                "id": "allday",
                "iCalUID": "allday@example.com",
                "summary": "Holiday",
                "start": {"date": "2026-05-02"},
                "end": {"date": "2026-05-03"},
            },
            _event_resource(event_id="timed", ical_uid="timed@example.com", summary="Standup"),
        ],
    }
    monkeypatch.setattr(gcal, "_build_service", lambda _path, readonly=True: _FakeService(items_by_calendar))

    agenda = gcal.get_agenda(
        service_account_path=Path("unused.json"),
        calendar_ids=["personal"],
        target_start=datetime(2026, 5, 2, tzinfo=TZ),
        target_end=datetime(2026, 5, 3, tzinfo=TZ),
    )

    assert [e.title for e in agenda.next] == ["Standup"]
