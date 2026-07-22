"""Shared configuration helpers."""

from __future__ import annotations

import logging
import os
from pathlib import Path

logger = logging.getLogger("xteink_agenda.config")


class ConfigError(ValueError):
    """Raised for invalid configuration — fails fast at boot with a clear message."""


def parse_views() -> list[tuple[str, list[str]]]:
    """Parse VIEW_0, VIEW_1, ... env vars into (name, [cal_id, ...]) tuples.

    Format: VIEW_0=Name:cal_id1,cal_id2
    Raises ConfigError with an example if format is wrong — caller should log and exit.
    """
    views = []
    i = 0
    while True:
        raw = os.getenv(f"VIEW_{i}")
        if raw is None:
            break
        if ":" not in raw:
            raise ConfigError(
                f"VIEW_{i} must be 'Name:cal_id1,cal_id2' "
                f"(example: VIEW_0=Work:me@co.com,team@co.com). Got: {raw!r}"
            )
        name, cal_ids_str = raw.split(":", 1)
        cal_ids = [c.strip() for c in cal_ids_str.split(",") if c.strip()]
        if not name.strip():
            raise ConfigError(f"VIEW_{i} name is empty. Example: VIEW_0=Work:cal_id")
        if not cal_ids:
            raise ConfigError(f"VIEW_{i} has no calendar IDs. Example: VIEW_0=Work:cal_id")
        views.append((name, cal_ids))
        i += 1
    return views


def all_calendar_ids() -> list[str]:
    """Extract unique calendar IDs from all configured views."""
    ids = set()
    for _, cal_ids in parse_views():
        ids.update(cal_ids)
    return list(ids)


def _clamped_int(env_key: str, default: int, lo: int, hi: int) -> int:
    """Read an int env var, clamp to [lo, hi], and WARN if clamping changed the value."""
    raw = os.getenv(env_key, str(default)) or str(default)
    try:
        value = int(raw)
    except ValueError:
        logger.warning("%s=%r is not an integer — using default %d", env_key, raw, default)
        return default
    clamped = max(lo, min(hi, value))
    if clamped != value:
        logger.warning(
            "%s=%d is out of range [%d, %d] — using %d instead",
            env_key, value, lo, hi, clamped,
        )
    return clamped


def load_config() -> dict:
    """Read all config from environment."""
    return {
        "sa_path": Path(os.getenv("GCAL_SERVICE_ACCOUNT", "service-account.json")),
        "calendar_ids": all_calendar_ids(),
        # Waking-day window for the on-device day-progress bar.
        "day_start": _clamped_int("DAY_START", 7, 0, 22),
        "day_end": _clamped_int("DAY_END", 23, 1, 23),
    }


def get_env_path() -> Path:
    """Get the .env file path."""
    return Path(__file__).parent.parent.parent / ".env"


def read_env_raw() -> dict[str, str]:
    """Read .env file as raw key=value pairs."""
    env_path = get_env_path()
    result = {}
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                key, val = line.split("=", 1)
                result[key.strip()] = val.strip()
    return result


def write_env(values: dict[str, str]):
    """Write values to .env file, preserving comments."""
    env_path = get_env_path()
    lines = []
    existing_keys = set()

    # Preserve existing comments and update values
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                lines.append(line)
                continue
            if "=" in stripped:
                key = stripped.split("=", 1)[0].strip()
                existing_keys.add(key)
                if key in values:
                    lines.append(f"{key}={values[key]}")
                else:
                    lines.append(line)
            else:
                lines.append(line)

    # Add new keys not in the file
    for key, val in values.items():
        if key not in existing_keys:
            lines.append(f"{key}={val}")

    env_path.write_text("\n".join(lines) + "\n")
