"""Tests for the X4 device API endpoints."""

from __future__ import annotations

from datetime import datetime, timedelta
from unittest.mock import patch

import pytest
from fastapi.testclient import TestClient

from xteink_agenda import server


@pytest.fixture
def client():
    return TestClient(server.app)


@pytest.fixture(autouse=True)
def fresh_cache():
    server._invalidate_agenda_cache()
    yield
    server._invalidate_agenda_cache()


def _items_for_today():
    now = datetime.now().astimezone()
    start = now - timedelta(minutes=10)
    end = now + timedelta(minutes=50)
    later = now + timedelta(hours=2)
    return [
        {
            "id": "gcal-cal-1",
            "title": "Standup",
            "start": start.strftime("%H:%M"),
            "end": end.strftime("%H:%M"),
            "start_dt": start.isoformat(),
            "end_dt": end.isoformat(),
            "duration_min": 60,
            "type": "event",
            "completed": False,
            "calendar_id": "me@example.com",
            "gcal_event_id": "ev123",
        },
        {
            "id": "gcal-cal-2",
            "title": "Review",
            "start": later.strftime("%H:%M"),
            "end": (later + timedelta(minutes=30)).strftime("%H:%M"),
            "start_dt": later.isoformat(),
            "end_dt": (later + timedelta(minutes=30)).isoformat(),
            "duration_min": 30,
            "type": "event",
            "completed": False,
            "calendar_id": "me@example.com",
            "gcal_event_id": "ev456",
        },
    ]


class TestAgenda:
    def test_agenda_marks_active_event_current(self, client):
        with patch.object(server, "_get_agenda_items_fresh", lambda cal_ids, day_offset=0: _items_for_today()):
            resp = client.get("/api/x4/agenda")
        assert resp.status_code == 200
        body = resp.json()
        assert body["current_index"] == 0
        assert body["items"][0]["type"] == "event"
        assert all(item["type"] == "event" for item in body["items"])
        assert 0 < body["wake_in_sec"] <= 3600

    def test_agenda_clamps_day_offset(self, client):
        with patch.object(server, "_get_agenda_items_fresh", lambda cal_ids, day_offset=0: []):
            resp = client.get("/api/x4/agenda", params={"day_offset": 9999})
        assert resp.json()["day_offset"] == server.DAY_OFFSET_MAX

    def test_agenda_caches_items(self, client):
        calls = []

        def fake_fresh(cal_ids, day_offset=0):
            calls.append(day_offset)
            return []

        with patch.object(server, "_get_agenda_items_fresh", fake_fresh):
            client.get("/api/x4/agenda")
            client.get("/api/x4/agenda")
        assert len(calls) == 1


class TestCompleteSelected:
    def test_complete_is_a_no_op_400(self, client):
        resp = client.post("/api/x4/complete-selected", params={"cursor": 0})
        assert resp.status_code == 400
        assert resp.json()["error"] == "cannot complete calendar events"


class TestDeleteEvent:
    def test_delete_out_of_range_cursor(self, client):
        with patch.object(server, "_get_agenda_items_fresh", lambda cal_ids, day_offset=0: []):
            resp = client.post("/api/x4/delete-event", params={"cursor": 5})
        assert resp.status_code == 400

    def test_delete_calls_gcal_and_invalidates_cache(self, client):
        deleted = []

        def fake_delete(sa_path, calendar_id, event_id):
            deleted.append((calendar_id, event_id))

        with patch.object(server, "_get_agenda_items_fresh", lambda cal_ids, day_offset=0: _items_for_today()), \
             patch.object(server, "delete_event", fake_delete):
            resp = client.post("/api/x4/delete-event", params={"cursor": 0})
        assert resp.status_code == 200
        assert resp.json()["deleted"] == "ev123"
        assert deleted == [("me@example.com", "ev123")]
        assert len(server._agenda_cache) == 0

    def test_delete_failure_returns_502(self, client):
        def boom(sa_path, calendar_id, event_id):
            raise RuntimeError("api down")

        with patch.object(server, "_get_agenda_items_fresh", lambda cal_ids, day_offset=0: _items_for_today()), \
             patch.object(server, "delete_event", boom):
            resp = client.post("/api/x4/delete-event", params={"cursor": 0})
        assert resp.status_code == 502


class TestMisc:
    def test_health(self, client):
        assert client.get("/health").json() == {"status": "ok"}

    def test_device_config_defaults_empty(self, client, monkeypatch):
        for key in ("DEVICE_WIFI_SSID", "DEVICE_WIFI_PASS", "DEVICE_SERVER_HOST", "DEVICE_SERVER_PORT"):
            monkeypatch.delenv(key, raising=False)
        body = client.get("/api/x4/config").json()
        assert body == {"wifi_ssid": "", "wifi_pass": "", "server_host": "", "server_port": 0}

    def test_firmware_version_without_manifest(self, client):
        resp = client.get("/api/x4/firmware/version")
        assert resp.status_code == 200
        body = resp.json()
        assert "version" in body

    def test_settings_page_renders(self, client):
        resp = client.get("/settings")
        assert resp.status_code == 200
        assert "Xteink Agenda" in resp.text
