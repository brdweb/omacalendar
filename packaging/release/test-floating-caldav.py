#!/usr/bin/env python3
"""Exercise floating recurrence writes against disposable real Radicale."""
from __future__ import annotations

import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import sqlite3
from pathlib import Path
import tempfile

spec = importlib.util.spec_from_file_location("provider_smoke", Path(__file__).with_name("live-provider-smoke.py"))
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)
require = smoke.require
wait_for = smoke.wait_for


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("daemon", "cli", "radicale", "work", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--previous-daemon", type=Path)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    report = {"daemonSha256": hashlib.sha256(args.daemon.read_bytes()).hexdigest(),
              "cliSha256": hashlib.sha256(args.cli.read_bytes()).hexdigest(), "checks": []}
    harness = None
    try:
        with tempfile.TemporaryDirectory(prefix="floating-", dir=args.work.resolve()) as disk, \
             tempfile.TemporaryDirectory(prefix="ocfloat-") as short, ExitStack() as cleanup:
            root = Path(short) / "work"
            root.symlink_to(disk, target_is_directory=True)
            (root / "daemon").mkdir(mode=0o700)
            harness = smoke.DaemonHarness(args.daemon.resolve(), args.cli.resolve(), root / "daemon")
            if args.previous_daemon:
                harness.daemon = args.previous_daemon.resolve()
                report["previousDaemonSha256"] = hashlib.sha256(args.previous_daemon.read_bytes()).hexdigest()
            smoke.install_secret_fixture(harness)
            harness.env["TZ"] = "America/New_York"
            server = smoke.RadicaleServer(args.radicale.resolve(), root / "radicale")
            cleanup.callback(server.stop)
            cleanup.callback(harness.stop)
            server.start()
            server.seed(datetime(2030, 3, 9, 14, tzinfo=timezone.utc))
            body = ("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\n"
                "UID:floating-provider-acceptance@example.test\r\nDTSTAMP:20300301T120000Z\r\n"
                "DTSTART:20300309T090000\r\nDTEND:20300309T100000\r\n"
                "RRULE:FREQ=DAILY;COUNT=3\r\nSUMMARY:Floating acceptance seed\r\n"
                "END:VEVENT\r\nEND:VCALENDAR\r\n").encode()
            status, _ = server.request("PUT", server.collection + "floating.ics", body)
            require(status == 201, "floating resource seed rejected")
            if args.previous_daemon:
                detached = body.replace(b"floating-provider-acceptance@example.test", b"floating-detached-upgrade@example.test").replace(
                    b"Floating acceptance seed", b"Floating legacy detached")
                detached = detached.replace(b"END:VCALENDAR\r\n", (
                    "BEGIN:VEVENT\r\nUID:floating-detached-upgrade@example.test\r\nDTSTAMP:20300301T120000Z\r\n"
                    "RECURRENCE-ID:20300311T090000\r\nDTSTART:20300311T090000\r\nDTEND:20300311T100000\r\n"
                    "SUMMARY:Floating legacy detached exception\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n").encode())
                status, _ = server.request("PUT", server.collection + "floating-detached.ics", detached)
                require(status == 201, "legacy detached resource seed rejected")
            harness.start()
            report["version"] = harness.call("system.info")["version"]
            aid = harness.call("accounts.addCalDav", {
                "endpoint": server.url + "/smoke/", "username": smoke.FIXTURE_USER,
                "password": smoke.FIXTURE_PASSWORD, "displayName": "Floating acceptance",
            })["id"]
            cid = wait_for(lambda: next(iter(smoke.calendars(harness, aid)), None), "calendar discovery")["id"]

            def rows(prefix):
                return [e for e in harness.call("events.list", {
                    "start": "2030-03-08T00:00:00Z", "end": "2030-03-13T00:00:00Z", "calendarIds": [cid],
                })["events"] if e.get("summary", "").startswith(prefix)]

            def get(event):
                return harness.call("events.get", {"eventId": event["id"]})

            def resource_event(summary):
                status, data = server.request("GET", server.collection)
                require(status == 200, "provider readback failed")
                return next((chunk.split("END:VEVENT", 1)[0] for chunk in data.decode().split("BEGIN:VEVENT")
                             if "SUMMARY:" + summary + "\r\n" in chunk), "")

            wait_for(lambda: len(rows("Floating acceptance seed")) == 3, "floating seed pull")
            if args.previous_daemon:
                report["previousVersion"] = report["version"]
                report["cachedKindsBeforeUpgrade"] = sorted({e["timeKind"] for e in rows("Floating acceptance seed")})
                require(report["cachedKindsBeforeUpgrade"] == ["zoned"], "upgrade fixture was not misclassified by RC3")
                server.stop()
                master = rows("Floating acceptance seed")[0]
                harness.call("events.update", {
                    "eventId": master["id"], "expectedLocalRevision": master["localRevision"],
                    "clientMutationId": "legacy-offline-pending", "recurrenceScope": "series",
                    "patch": {"description": "Unsent RC3 notes survive metadata refresh"}})
                harness.stop()

                def snapshot():
                    with sqlite3.connect(harness.database_path) as db:
                        db.row_factory = sqlite3.Row
                        return {
                            "events": [dict(row) for row in db.execute(
                                "SELECT e.* FROM events e JOIN calendars c ON c.id=e.calendar_id WHERE c.account_id=? ORDER BY e.id", (aid,))],
                            "outbox": [dict(row) for row in db.execute(
                                "SELECT * FROM outbox WHERE account_id=? ORDER BY id", (aid,))],
                            "version": db.execute("SELECT value_json FROM provider_state WHERE account_id=? AND key='caldav_time_kind_version'", (aid,)).fetchone(),
                        }

                baseline = snapshot()
                require(len(baseline["outbox"]) == 1, "expected one durable offline mutation")
                with sqlite3.connect(harness.database_path) as db:
                    db.execute("CREATE TRIGGER reject_refresh_checkpoint BEFORE INSERT ON provider_state "
                               "WHEN new.key='caldav_time_kind_version' BEGIN SELECT RAISE(ABORT,'acceptance interruption'); END")
                harness.daemon = args.daemon.resolve()
                harness.start()
                report["version"] = harness.call("system.info")["version"]
                harness.stop()
                require(snapshot() == baseline, "interrupted refresh changed rows/queue/checkpoint")
                report["checks"].append("Interrupted/failed upgrade leaves cached rows and pending mutation intact; checkpoint remains absent")
                with sqlite3.connect(harness.database_path) as db:
                    db.execute("DROP TRIGGER reject_refresh_checkpoint")
                harness.start()
                report["cachedKindsAfterUpgradeSync"] = sorted({e["timeKind"] for e in rows("Floating acceptance seed")})
                require(report["cachedKindsAfterUpgradeSync"] == ["floating"],
                        "offline upgrade failed to refresh unchanged cached floating metadata")
                harness.stop()
                normalized = snapshot()
                require(normalized["version"] is not None, "completed refresh did not checkpoint")
                require(len(baseline["events"]) == len(normalized["events"]) and
                        len(baseline["outbox"]) == len(normalized["outbox"]), "refresh added or removed durable rows")
                for original, after in zip(baseline["events"], normalized["events"]):
                    expected = dict(original)
                    if original["uid"] in ("floating-provider-acceptance@example.test", "floating-detached-upgrade@example.test"):
                        expected["time_kind"] = "floating"
                    require(after == expected, "metadata refresh changed canonical fields, local edit, identifiers or timestamps")
                for original, after in zip(baseline["outbox"], normalized["outbox"]):
                    expected = dict(original)
                    old_payload = json.loads(expected.pop("payload_json"))
                    new_payload = json.loads(after["payload_json"])
                    old_payload["timeKind"] = "floating"
                    require(old_payload == new_payload, "metadata refresh changed queued operation content")
                    require(expected == {key: value for key, value in after.items() if key != "payload_json"},
                            "metadata refresh changed durable mutation identity, state or timestamps")
                report["checks"].append("Offline RC3 master/detached metadata refresh preserves event IDs, UTC references, local notes, revisions and queue state")
                harness.start()
                require(all(e["timeKind"] == "floating" for e in rows("Floating acceptance seed")), "offline normalized cache did not survive restart")
                require(len(rows("Floating legacy detached")) == 3, "legacy detached refresh duplicated or lost an occurrence")
                server.start()
                harness.call("sync.account", {"accountId": aid})
                wait_for(lambda: "Unsent RC3 notes survive metadata refresh" in resource_event("Floating acceptance seed"), "legacy queued provider write")
                wait_for(lambda: get(master).get("syncState") == "clean", "legacy queued mutation acknowledgment")
                require("DTSTART:20300309T090000\r\n" in resource_event("Floating acceptance seed"),
                        "legacy queued mutation changed floating time on the wire")
                report["checks"].append("Previously queued RC3 edit replays to real Radicale as floating after offline upgrade and clean acknowledgment")
            require(all(e["timeKind"] == "floating" for e in rows("Floating acceptance seed")),
                    "provider floating times were not classified as floating")
            report["checks"].append("Provider-seeded floating recurrence parsed as Floating")

            created = harness.call("events.create", {"clientMutationId": "floating-native-create",
                "event": {"calendarId": cid, "summary": "Floating acceptance created",
                    "startUtc": "2030-03-09T14:00:00Z", "endUtc": "2030-03-09T15:00:00Z",
                    "startTimeZone": "", "endTimeZone": "", "timeKind": "floating",
                    "recurrenceRule": "FREQ=DAILY;COUNT=3"}})
            wait_for(lambda: get(created).get("syncState") == "clean", "floating create acknowledgment")
            require("DTSTART:20300309T090000\r\n" in resource_event("Floating acceptance created"),
                    "app-created floating DTSTART changed its local wall time")
            report["checks"].append("App-created floating recurring master written/read back/acknowledged")

            for origin in ("seed", "created"):
                prefix = "Floating acceptance " + origin
                before = rows(prefix)
                require(len(before) == 3, "expected three original floating occurrences")
                selected = before[1]
                require(selected["startUtc"] == "2030-03-10T13:00:00.000Z", "DST fixture has incorrect selected instant")
                changed = harness.call("events.update", {
                    "eventRef": {"eventId": selected["id"], "recurrenceId": selected["recurrenceId"]},
                    "expectedLocalRevision": selected["localRevision"], "clientMutationId": "floating-change-" + origin,
                    "recurrenceScope": "occurrence", "patch": {"summary": prefix + " edited"}})
                wait_for(lambda: bool(resource_event(prefix + " edited")), "floating occurrence provider write")
                wait_for(lambda: get(changed).get("syncState") == "clean", "floating detached acknowledgment")
                wire = resource_event(prefix + " edited")
                require("RECURRENCE-ID:20300310T090000\r\n" in wire,
                        "floating recurrence ID was not converted to the original local wall time")
                require("DTSTART:20300310T090000\r\n" in wire, "floating selected DTSTART moved")
                require(len(rows(prefix)) == 3, "floating write duplicated or lost an occurrence")
                current = get(changed)
                require(current["startUtc"] == selected["startUtc"] and current["endUtc"] == selected["endUtc"],
                        "floating acknowledgment changed dates")
                report["checks"].append(origin + ": correct wall recurrence ID, clean acknowledgment, three instances")
                original_reference = harness.call("events.get", {
                    "eventId": selected["id"], "recurrenceId": selected["recurrenceId"]})
                require(original_reference["id"] == current["id"],
                        "the original presentation reference no longer resolves after acknowledgment")

                updated = harness.call("events.update", {
                    "eventRef": {"eventId": current["id"], "recurrenceId": selected["recurrenceId"]},
                    "expectedLocalRevision": current["localRevision"], "clientMutationId": "floating-second-" + origin,
                    "recurrenceScope": "occurrence", "patch": {"description": "Retained floating follow-up"}})
                wait_for(lambda: "DESCRIPTION:Retained floating follow-up\r\n" in resource_event(prefix + " edited"),
                         "floating follow-up provider write")
                wait_for(lambda: get(updated).get("syncState") == "clean", "floating follow-up acknowledgment")
                harness.stop()
                harness.start()
                harness.call("sync.account", {"accountId": aid})
                wait_for(lambda: harness.call("sync.status", {"accountId": aid}).get("state") == "idle", "restart sync")
                current = get(updated)
                require(current["description"] == "Retained floating follow-up" and current["timeKind"] == "floating",
                        "floating restart lost notes or time kind")
                require(len(rows(prefix)) == 3 and current["startUtc"] == selected["startUtc"],
                        "floating restart duplicated or moved selected instance")
                report["checks"].append(origin + ": second patch and restart preserve dates, notes and three instances")
            report["status"] = "PASS"
    except Exception as error:
        report["status"] = "FAIL"
        report["error"] = str(error)
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        if harness is not None:
            args.report.with_suffix(".daemon.log").write_text(harness.log_text())
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
