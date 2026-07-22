"""Tests for VIEW_* parsing and config clamping."""

from __future__ import annotations

import pytest

from xteink_agenda import config


def test_parse_views_reads_sequential_env_vars(monkeypatch):
    monkeypatch.setenv("VIEW_0", "All:a@x.com,b@x.com")
    monkeypatch.setenv("VIEW_1", "Work:b@x.com")
    monkeypatch.delenv("VIEW_2", raising=False)

    assert config.parse_views() == [
        ("All", ["a@x.com", "b@x.com"]),
        ("Work", ["b@x.com"]),
    ]


def test_parse_views_rejects_missing_colon(monkeypatch):
    monkeypatch.setenv("VIEW_0", "no-colon-here")
    with pytest.raises(config.ConfigError):
        config.parse_views()


def test_parse_views_rejects_empty_name(monkeypatch):
    monkeypatch.setenv("VIEW_0", ":a@x.com")
    with pytest.raises(config.ConfigError):
        config.parse_views()


def test_all_calendar_ids_unions_views(monkeypatch):
    monkeypatch.setenv("VIEW_0", "All:a@x.com,b@x.com")
    monkeypatch.setenv("VIEW_1", "Work:b@x.com")
    monkeypatch.delenv("VIEW_2", raising=False)

    assert sorted(config.all_calendar_ids()) == ["a@x.com", "b@x.com"]


def test_load_config_clamps_day_window(monkeypatch):
    monkeypatch.setenv("DAY_START", "99")
    monkeypatch.setenv("DAY_END", "-3")
    cfg = config.load_config()
    assert cfg["day_start"] == 22
    assert cfg["day_end"] == 1
