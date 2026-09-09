#!/usr/bin/env python3
"""Exercise exact prior/candidate artifacts in disposable profiles, never user data."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import stat
import sys
import tempfile
import time
from typing import Any, Iterator

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from test_daemon_contract import ContractError, DaemonHarness, LoopbackProviderFixture, require

_spec = importlib.util.spec_from_file_location(
    "upgrade_provider_fixture", Path(__file__).with_name("live-provider-smoke.py"))
assert _spec is not None and _spec.loader is not None
_provider_fixture = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_provider_fixture)


@contextmanager
def profile(work: Path, label: str) -> Iterator[Path]:
    """Keep datasets on the selected disk and Unix socket paths short."""
    with tempfile.TemporaryDirectory(prefix=label + "-", dir=work) as directory:
        with tempfile.TemporaryDirectory(prefix="omau-") as aliases:
            alias = Path(aliases) / "p"
            alias.symlink_to(directory, target_is_directory=True)
            yield alias


def tree_digest(root: Path) -> dict[str, str]:
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file()}


def equal(actual: Any, expected: Any, context: str) -> None:
    if actual == expected:
        return
    if isinstance(actual, dict) and isinstance(expected, dict):
        changed = [key for key in actual.keys() | expected.keys() if actual.get(key) != expected.get(key)]
        details = {key: {"expected": expected.get(key), "actual": actual.get(key)} for key in sorted(changed)}
    else:
        details = {"expected": expected, "actual": actual}
    raise ContractError(f"{context}\n{json.dumps(details, sort_keys=True)}")


def utc_text(value: datetime) -> str:
    return value.isoformat(timespec="seconds").replace("+00:00", "Z")


def payloads(calendar_id: str) -> list[dict[str, Any]]:
    start = datetime.now(timezone.utc).replace(microsecond=0) + timedelta(days=2)
    common = {
        "calendarId": calendar_id, "description": "Synthetic notes: café\nSecond line",
        "location": "Synthetic room 7", "url": "https://example.test/event",
        "conferenceUrl": "https://example.test/meeting",
        "startTimeZone": "America/New_York", "endTimeZone": "America/New_York",
        "status": "confirmed", "transparency": "transparent", "visibility": "private",
        "reminders": [{"method": "popup", "minutes": 15}, {"method": "popup", "minutes": 60}],
    }
    timed = {"startUtc": utc_text(start), "endUtc": utc_text(start + timedelta(hours=2))}
    return [
        {**common, **timed, "summary": "Synthetic timed", "timeKind": "zoned",
         "organizer": {"email": "organizer@example.test", "displayName": "Synthetic organizer"},
         "attendees": [{"email": "guest@example.test", "displayName": "Synthetic guest",
                        "responseStatus": "accepted"}]},
        {**common, "summary": "Synthetic multi-day", "allDay": True,
         "timeKind": "all_day", "startDate": start.date().isoformat(),
         "endDate": (start.date() + timedelta(days=3)).isoformat()},
        {**common, **timed, "summary": "Synthetic recurring", "recurrenceRule": "FREQ=DAILY;COUNT=5"},
        {**common, **timed, "summary": "Synthetic floating", "timeKind": "floating",
         "startTimeZone": "", "endTimeZone": ""},
    ]


def add_provider(harness: DaemonHarness, fixture: LoopbackProviderFixture) -> str:
    account = harness.call("accounts.addCalDav", {
        "endpoint": fixture.base_url + "/dav/", "displayName": "Synthetic upgrade CalDAV",
        "username": "fixture-user", "password": "synthetic-upgrade-password",
    })
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        result = harness.call("events.search", {"query": "Cached CalDAV contract event", "limit": 20})
        if result.get("events"):
            return account["id"]
        time.sleep(0.05)
    raise ContractError("synthetic CalDAV reconnect never completed provider sync")


def snapshot(h: DaemonHarness, ids: list[str], settings: dict[str, Any], interval: dict[str, Any]) -> dict[str, Any]:
    fields = ("id", "provider", "displayName", "principal", "authStatus", "enabled")
    sets = {k: v for k, v in h.call("calendarSets.list").items() if k != "revision"}
    # Sync timestamps can advance after restart; account identity/status cannot.
    accounts = [{k: a.get(k) for k in fields} for a in h.call("accounts.list")["accounts"]]
    calendars = [{k: v for k, v in c.items() if k != "lastSyncAt"}
                 for c in h.call("calendars.list")["calendars"]]
    # RC4 explicitly advertises the previously absent/unsupported future RSVP
    # capability. Normalize only that absent false default, retaining all
    # existing capabilities and detecting any true/false behavior change.
    for calendar in calendars:
        capabilities = calendar.get("capabilities", {})
        if capabilities.get("provider") == "caldav":
            capabilities.setdefault("rsvpThisAndFuture", False)
    return {
        "events": [h.call("events.get", {"eventId": item}) for item in ids],
        "occurrences": h.call("events.list", interval)["events"],
        "settings": {key: h.call("settings.get", {"key": key}) for key in settings},
        "calendars": calendars, "sets": sets,
        "accounts": sorted(accounts, key=lambda item: item["id"]),
        "reminders": sorted(h.call("reminders.list", {"limit": 500})["reminders"], key=lambda item: item["id"]),
    }


def upgrade(previous: Path, candidate: Path, old_version: str, new_version: str,
            work: Path, logs: Path | None) -> None:
    with profile(work, "upgrade") as root, LoopbackProviderFixture() as fixture:
        h = DaemonHarness(previous / "omacalendard", previous / "omacalendarctl", root)
        _provider_fixture.install_secret_fixture(h)
        try:
            h.start()
            require(h.call("system.info")["version"] == old_version, "wrong prior artifact")
            local = next(a for a in h.call("accounts.list")["accounts"] if a["provider"] == "local")
            h.call("accounts.update", {"accountId": local["id"], "displayName": "Synthetic upgraded local account"})
            calendar_id = "synthetic-upgrade-calendar"
            for identifier, enabled, color, position, ignore in (
                (calendar_id, True, "#abcdef", 7, False),
                ("synthetic-hidden-calendar", False, "#fedcba", 9, True),
            ):
                h.call("calendars.upsert", {"calendar": {
                    "id": identifier, "accountId": local["id"], "name": identifier,
                    "color": "#112233", "timeZone": "UTC",
                }})
                h.call("calendars.updatePreferences", {"calendarId": identifier,
                    "colorOverride": color, "position": position, "enabled": enabled, "ignoreAlerts": ignore})
            h.call("calendarSets.upsert", {"calendarSet": {
                "id": "synthetic-upgrade-set", "name": "Synthetic ordered set",
                "calendarIds": [calendar_id, "local-default"], "defaultCalendarId": calendar_id,
            }})
            h.call("calendarSets.activate", {"calendarSetId": "synthetic-upgrade-set"})
            settings = {
                "notificationPrivacy": "title_only", "firstDayOfWeek": 1, "workDayStart": 7,
                "workDayEnd": 16, "timeFormat": "24h", "displayTimeZone": "Europe/London",
                "defaultDuration": 45, "defaultCalendarId": calendar_id, "currentView": "week",
            }
            for key, value in settings.items():
                h.call("settings.set", {"key": key, "value": value})
            events = []
            for index, payload in enumerate(payloads(calendar_id)):
                event = h.call("events.create", {"clientMutationId": f"upgrade-{index}",
                    "guestNotificationPolicy": "none", "recurrenceScope": "series", "event": payload})
                for key in ("summary", "description", "location", "url", "conferenceUrl", "startTimeZone",
                            "endTimeZone", "transparency", "visibility", "reminders"):
                    equal(event[key], payload[key], f"seed discarded {key}")
                for key in ("organizer", "attendees"):
                    equal(event[key], payload.get(key, {} if key == "organizer" else []), f"seed discarded {key}")
                events.append(event)
            start = datetime.fromisoformat(events[0]["startUtc"].replace("Z", "+00:00"))
            interval = {"start": utc_text(start - timedelta(days=1)), "end": utc_text(start + timedelta(days=8)),
                        "calendarIds": [calendar_id], "limit": 100}
            series = events[2]
            instances = [e for e in h.call("events.list", interval)["events"] if e["summary"] == "Synthetic recurring"]
            require(len(instances) == 5, "seed recurrence did not produce five instances")
            edited = h.call("events.update", {
                "eventRef": {"eventId": series["id"], "recurrenceId": instances[1]["recurrenceId"]},
                "expectedLocalRevision": series["localRevision"], "clientMutationId": "upgrade-exception",
                "recurrenceScope": "occurrence", "guestNotificationPolicy": "none",
                "patch": {"summary": "Synthetic exception", "startUtc": instances[1]["startUtc"],
                          "endUtc": instances[1]["endUtc"]},
            })
            master = h.call("events.get", {"eventId": series["id"]})
            h.call("events.remove", {
                "eventRef": {"eventId": series["id"], "recurrenceId": instances[3]["recurrenceId"]},
                "expectedLocalRevision": master["localRevision"], "clientMutationId": "upgrade-cancel-occurrence",
                "recurrenceScope": "occurrence", "guestNotificationPolicy": "none",
            })
            add_provider(h, fixture)
            ids = [e["id"] for e in events] + [edited["id"]]
            before = snapshot(h, ids, settings, interval)
            require(len(before["reminders"]) >= 2, "seed did not create persisted alarm jobs")
            require(sum(e["summary"] == "Synthetic recurring" for e in before["occurrences"]) == 3,
                    "seed cancellation did not remove only one occurrence")
            require(any(e["summary"] == "Synthetic exception" for e in before["occurrences"]),
                    "seed detached exception not visible")
            h.stop()
            backup = root / "backup"
            backup.mkdir(mode=0o700)
            for name in ("data", "config"):
                shutil.copytree(root / name, backup / name)
            original_backup = tree_digest(backup)
            original_secret_fixture = tree_digest(root / "synthetic-secrets")
            require(bool(original_backup), "backup is empty")
            h.daemon, h.cli = candidate / "omacalendard", candidate / "omacalendarctl"
            for number in range(2):
                h.start()
                require(h.call("system.info")["version"] == new_version, "wrong candidate")
                require(h.call("system.health")["ok"], "candidate unhealthy")
                equal(snapshot(h, ids, settings, interval), before, f"candidate restart {number + 1} changed profile")
                h.stop()
            for name in ("data", "config"):
                (root / name).rename(root / ("candidate-" + name))
                shutil.copytree(backup / name, root / name)
            h.daemon, h.cli = previous / "omacalendard", previous / "omacalendarctl"
            h.start()
            require(h.call("system.info")["version"] == old_version, "wrong restored artifact")
            require(h.call("system.health")["ok"], "restored prior version unhealthy")
            equal(snapshot(h, ids, settings, interval), before, "older-backup restore changed profile")
            h.stop()
            equal(tree_digest(backup), original_backup, "original backup changed")
            equal(tree_digest(root / "synthetic-secrets"), original_secret_fixture,
                  "synthetic credential storage changed during upgrade/restore")
            print(f"PASS: {old_version} -> {new_version}; four event shapes, full event DTOs, detached/cancelled "
                  "recurrence, alarm jobs, nine settings, calendar colors/visibility/order/ignored-alerts, "
                  "active ordered set/default, local/CalDAV account continuity, two candidate restarts, "
                  "complete older-backup restore and immutable backup hashes")
        finally:
            h.stop()
            if logs:
                (logs / "upgrade-daemon.log").write_text(h.log_text() + "\n")


def legacy_database(path: Path, sidecars: bool) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    connection = sqlite3.connect(path)
    try:
        if sidecars:
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute("PRAGMA wal_autocheckpoint=0")
        connection.execute("CREATE TABLE legacy_events(id TEXT PRIMARY KEY, title TEXT)")
        connection.execute("INSERT INTO legacy_events VALUES('synthetic', 'Preserve this synthetic legacy event')")
        connection.execute("PRAGMA user_version=1")
        connection.commit()
        if sidecars:
            connection.execute("PRAGMA wal_checkpoint(FULL)").fetchall()
            saved = {s: Path(str(path) + s).read_bytes() for s in ("-wal", "-shm")}
    finally:
        connection.close()
    if sidecars:
        for suffix, content in saved.items():
            Path(str(path) + suffix).write_bytes(content)


def assert_legacy(path: Path) -> None:
    with sqlite3.connect(f"file:{path}?mode=ro&immutable=1", uri=True) as connection:
        require(connection.execute("PRAGMA user_version").fetchone()[0] == 1, "legacy schema changed")
        equal(connection.execute("SELECT title FROM legacy_events WHERE id='synthetic'").fetchone()[0],
              "Preserve this synthetic legacy event", "legacy data lost")


def schema_one(candidate: Path, version: str, work: Path, logs: Path | None) -> None:
    for blocked in (False, True):
        label = "schema1-blocked" if blocked else "schema1-wal"
        with profile(work, label) as root, LoopbackProviderFixture() as fixture:
            h = DaemonHarness(candidate / "omacalendard", candidate / "omacalendarctl", root)
            _provider_fixture.install_secret_fixture(h)
            try:
                legacy_database(h.database_path, sidecars=not blocked)
                staging = Path(str(h.database_path) + ".schema2-transition")
                if blocked:
                    staging.mkdir(mode=0o700)
                    try:
                        h.start()
                    except ContractError:
                        require("Unable to remove stale schema transition file" in h.log_text(),
                                "startup failed for an unexpected reason")
                    else:
                        raise ContractError("obstructed schema transition unexpectedly succeeded")
                    assert_legacy(h.database_path)
                    archives = list(h.database_path.parent.glob("*.pre-v2-*.backup"))
                    require(len(archives) == 1, "failed transition did not retain one archive")
                    assert_legacy(archives[0])
                    staging.rmdir()
                h.start()
                info = h.call("system.info")
                require(info["version"] == version and info["schemaVersion"] == 2, "wrong migrated runtime")
                accounts = h.call("accounts.list")["accounts"]
                require(len(accounts) == 1 and accounts[0]["provider"] == "local", "transition did not reset to local-only state")
                archives = list(h.database_path.parent.glob("*.pre-v2-*.backup"))
                require(len(archives) == (2 if blocked else 1), "wrong archive count")
                for archive in archives:
                    assert_legacy(archive)
                    for item in (archive, Path(str(archive) + "-wal"), Path(str(archive) + "-shm")):
                        if not blocked:
                            require(item.is_file(), "schema-1 WAL/SHM archive missing")
                        if item.exists():
                            require(stat.S_IMODE(item.stat().st_mode) == 0o600, "archive not private")
                require(stat.S_IMODE(h.database_path.stat().st_mode) == 0o600, "active database not private")
                add_provider(h, fixture)
                h.stop()
                h.start()
                require(len(list(h.database_path.parent.glob("*.pre-v2-*.backup"))) == len(archives), "schema-2 restart repeated archival")
                require(not staging.exists(), "schema staging path remained")
                h.stop()
                print(f"PASS: {label}; private recoverable archive, schema-2 local reset, explicit synthetic "
                      "CalDAV reconnect/sync, idempotent restart" + (", failed initialization preserves original and retries successfully" if blocked else ""))
            finally:
                h.stop()
                if logs:
                    (logs / (label + "-daemon.log")).write_text(h.log_text() + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--previous-bin", type=Path, required=True)
    parser.add_argument("--candidate-bin", type=Path, required=True)
    parser.add_argument("--previous-version", default="1.0.0-alpha")
    parser.add_argument("--candidate-version", default="1.0.0-beta.1")
    parser.add_argument("--work-dir", type=Path, help="Disk for profiles; only short symlink aliases use /tmp")
    parser.add_argument("--log-dir", type=Path, help="Retain synthetic daemon logs")
    parser.add_argument("--skip-schema-one", action="store_true")
    args = parser.parse_args()
    previous, candidate = args.previous_bin.resolve(), args.candidate_bin.resolve()
    work = args.work_dir.resolve() if args.work_dir else Path(tempfile.gettempdir())
    work.mkdir(mode=0o700, parents=True, exist_ok=True)
    logs = args.log_dir.resolve() if args.log_dir else None
    if logs:
        logs.mkdir(mode=0o700, parents=True, exist_ok=True)
    upgrade(previous, candidate, args.previous_version, args.candidate_version, work, logs)
    if not args.skip_schema_one:
        schema_one(candidate, args.candidate_version, work, logs)
    print("LIMITS: synthetic daemon/CLI profiles; no real keyring, OAuth, provider account, installed desktop, or owner signoff")


if __name__ == "__main__":
    main()
