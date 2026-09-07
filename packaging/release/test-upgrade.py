#!/usr/bin/env python3
"""Exercise an immutable prior artifact -> candidate -> backup restore in isolation."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from test_daemon_contract import DaemonHarness, require


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--previous-bin", type=Path, required=True)
    parser.add_argument("--candidate-bin", type=Path, required=True)
    args = parser.parse_args()
    previous = args.previous_bin.resolve()
    candidate = args.candidate_bin.resolve()
    with tempfile.TemporaryDirectory(prefix="omacalendar-upgrade-") as tmp:
        root = Path(tmp)
        harness = DaemonHarness(previous / "omacalendard", previous / "omacalendarctl", root)
        try:
            harness.start()
            previous_version = harness.call("system.info")["version"]
            require(previous_version == "1.0.0-alpha", "wrong prior artifact")
            calendars = harness.call("calendars.list")["calendars"]
            calendar_id = next(item["id"] for item in calendars if not item["readOnly"])
            events = []
            for label, dates in (
                ("Timed", {"startUtc": "2026-09-08T13:00:00Z", "endUtc": "2026-09-08T14:00:00Z"}),
                ("Multi-day", {"allDay": True, "startDate": "2026-09-08", "endDate": "2026-09-11"}),
                ("Recurring", {"startUtc": "2026-09-09T13:00:00Z", "endUtc": "2026-09-09T14:00:00Z", "recurrenceRule": "FREQ=DAILY;COUNT=3"}),
            ):
                event = harness.call("events.create", {
                    "clientMutationId": "upgrade-" + label,
                    "event": {"calendarId": calendar_id, "summary": "Synthetic upgrade " + label,
                              "startTimeZone": "UTC", "endTimeZone": "UTC", **dates},
                })
                events.append(event)
            harness.call("settings.set", {"key": "notificationPrivacy", "value": "title_only"})
            before_settings = harness.call("settings.get", {"key": "notificationPrivacy"})
            harness.stop()
            backup = root / "backup"
            backup.mkdir(mode=0o700)
            shutil.copytree(root / "data", backup / "data")
            shutil.copytree(root / "config", backup / "config")
            backup_hash = hashlib.sha256((backup / "data/omacalendar/calendar.sqlite3").read_bytes()).hexdigest()
            harness.daemon = candidate / "omacalendard"
            harness.cli = candidate / "omacalendarctl"
            for _ in range(2):
                harness.start()
                require(harness.call("system.info")["version"] == "1.0.0-beta.1", "wrong candidate")
                require(harness.call("system.health")["ok"], "candidate unhealthy")
                for old in events:
                    current = harness.call("events.get", {"eventId": old["id"]})
                    require(current["summary"] == old["summary"], "event changed during upgrade")
                require(harness.call("settings.get", {"key": "notificationPrivacy"}) == before_settings, "preferences changed during upgrade")
                harness.stop()
            # Restore only the older backup before restarting the older binary.
            (root / "data").rename(root / "candidate-data")
            (root / "config").rename(root / "candidate-config")
            shutil.copytree(backup / "data", root / "data")
            shutil.copytree(backup / "config", root / "config")
            harness.daemon = previous / "omacalendard"
            harness.cli = previous / "omacalendarctl"
            harness.start()
            require(harness.call("system.health")["ok"], "restored alpha unhealthy")
            for old in events:
                require(harness.call("events.get", {"eventId": old["id"]})["summary"] == old["summary"], "restore lost event")
            harness.stop()
            require(hashlib.sha256((backup / "data/omacalendar/calendar.sqlite3").read_bytes()).hexdigest() == backup_hash,
                    "backup was modified")
            print("PASS: published alpha -> beta, timed/all-day/recurring data and settings, beta restart, older-backup restore")
        finally:
            harness.stop()


if __name__ == "__main__":
    main()
