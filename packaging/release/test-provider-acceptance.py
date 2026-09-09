#!/usr/bin/env python3
"""Expanded exact-binary provider acceptance using disposable loopback accounts.

Uses a real Radicale server, HTTPS transport with an isolated CA namespace, and
synthetic credential storage. It cannot qualify a real desktop keyring or hosted
Google/Nextcloud/Fastmail account. Evidence and synthetic data are retained.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sys
import time
import traceback
import uuid

SPEC = importlib.util.spec_from_file_location("provider_smoke", Path(__file__).with_name("live-provider-smoke.py"))
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)
require = smoke.require
wait_for = smoke.wait_for


class Suite:
    def __init__(self, harness, server, start, root):
        self.h = harness
        self.server = server
        self.start = start
        self.root = root
        self.results = []
        self.account_id = ""
        self.calendar_id = ""

    def case(self, name, test):
        began = time.monotonic()
        try:
            details = test()
            result = {"name": name, "status": "PASS", "details": details}
        except Exception as error:
            result = {"name": name, "status": "FAIL", "error": str(error), "traceback": traceback.format_exc()}
            diagnostics = {}
            for method, params in (("sync.status", {}), ("operations.list", {}), ("conflicts.list", {"unresolvedOnly": False})):
                try:
                    diagnostics[method] = self.h.call(method, params)
                except Exception as diagnostic_error:
                    diagnostics[method] = str(diagnostic_error)
            (self.root / ("failure-%02d.json" % (len(self.results) + 1))).write_text(json.dumps(diagnostics, indent=2) + "\n")
        result["seconds"] = round(time.monotonic() - began, 3)
        self.results.append(result)
        (self.root / "results.json").write_text(json.dumps(self.results, indent=2) + "\n")
        print(result["status"] + ": " + name, flush=True)
        if result["status"] == "FAIL":
            print(result["error"], flush=True)

    def sync(self):
        self.h.call("sync.account", {"accountId": self.account_id})

    def rows(self, calendar=None):
        return self.h.call("events.list", {"calendarIds": [calendar or self.calendar_id],
            "start": (self.start - timedelta(days=2)).isoformat(),
            "end": (self.start + timedelta(days=15)).isoformat(), "limit": 500})["events"]

    def find(self, summary, calendar=None):
        return next((e for e in self.rows(calendar) if e.get("summary") == summary), None)

    def get(self, event):
        return self.h.call("events.get", {"eventId": event["id"]})

    def update(self, event, patch, scope="series", rid=None):
        current = self.get(event)
        ref = {"eventId": event["id"]}
        if rid:
            ref["recurrenceId"] = rid
        return self.h.call("events.update", {"eventRef": ref,
            "expectedLocalRevision": current["localRevision"], "clientMutationId": str(uuid.uuid4()),
            "recurrenceScope": scope, "guestNotificationPolicy": "none", "patch": patch})

    def create(self, summary, **fields):
        event = {"calendarId": self.calendar_id, "summary": summary,
            "startUtc": self.start.isoformat(), "endUtc": (self.start + timedelta(hours=1)).isoformat(),
            "startTimeZone": "UTC", "endTimeZone": "UTC", **fields}
        if event.get("allDay"):
            for key in ("startUtc", "endUtc", "startTimeZone", "endTimeZone"):
                event.pop(key, None)
            event["timeKind"] = "all_day"
        created = self.h.call("events.create", {"event": event, "clientMutationId": str(uuid.uuid4()),
            "recurrenceScope": "series", "guestNotificationPolicy": "none"})
        wait_for(lambda: self.server.contains(summary), "remote create " + summary)
        wait_for(lambda: self.get(created).get("syncState") == "clean", "acknowledged create " + summary,
            timeout=65 if event.get("allDay") else 20)
        return self.get(created)

    def seed(self, name, summary, extra=(), all_day=False):
        start = self.start.strftime("%Y%m%dT%H%M%SZ")
        lines = ["BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//OmaCalendar//Disposable acceptance//EN",
            "BEGIN:VEVENT", "UID:acceptance-" + name, "DTSTAMP:" + start, "SUMMARY:" + summary]
        if all_day:
            lines += ["DTSTART;VALUE=DATE:" + self.start.strftime("%Y%m%d"),
                "DTEND;VALUE=DATE:" + (self.start + timedelta(days=2)).strftime("%Y%m%d")]
        else:
            lines += ["DTSTART:" + start, "DTEND:" + (self.start + timedelta(hours=1)).strftime("%Y%m%dT%H%M%SZ")]
        lines += list(extra) + ["END:VEVENT", "END:VCALENDAR", ""]
        path = self.server.collection + name + ".ics"
        status, _ = self.server.request("PUT", path, "\r\n".join(lines).encode())
        require(status in (201, 204), "remote seed rejected")
        self.sync()
        event = wait_for(lambda: self.find(summary), "remote seed sync " + summary)
        return event, path

    def remote(self, path):
        status, body = self.server.request("GET", path)
        require(status == 200, "remote resource readback failed")
        return body.decode()

    def baseline(self):
        smoke.run_radicale(self.h, self.server, self.start)
        self.account_id = next(a["id"] for a in self.h.call("accounts.list")["accounts"] if a["provider"] == "caldav")
        calendar = smoke.calendars(self.h, self.account_id)[0]
        self.calendar_id = calendar["id"]
        (self.root / "radicale-capabilities.json").write_text(json.dumps(calendar.get("capabilities"), indent=2))
        return "Real remote CRUD, offline durable queue, cache, daemon/server restart and remote deletion"

    def fidelity(self):
        event = self.create("Acceptance zoned alarms", startTimeZone="America/New_York", endTimeZone="America/New_York",
            timeKind="zoned", description="Synthetic notes", location="Test room", reminders=[{"method": "popup", "minutes": 15}, {"method": "popup", "minutes": 60}])
        require(event.get("startTimeZone") == "America/New_York", "zone identity not retained")
        require(len(event.get("reminders", [])) == 2, "alarm count not retained")
        require(event.get("description") == "Synthetic notes" and event.get("location") == "Test room", "event text not retained")
        self.create("Acceptance multi-day", allDay=True, timeKind="date",
            startDate=self.start.date().isoformat(), endDate=(self.start + timedelta(days=3)).date().isoformat())
        self.h.stop(); self.h.start(); self.sync()
        multi = wait_for(lambda: self.find("Acceptance multi-day"), "all-day restart")
        require(multi.get("allDay") is True and multi.get("endDate") == (self.start + timedelta(days=3)).date().isoformat(), "exclusive all-day dates changed")
        status, body = self.server.request("GET", self.server.collection)
        require(status == 200 and body.count(b"BEGIN:VALARM") >= 2, "remote alarms missing")
        require(b"TZID=America/New_York" in body and b"VALUE=DATE" in body, "remote date or zone identity missing")
        return "Timed timezone, text, multiple alarms and exclusive multi-day dates verified remotely and after restart"

    def recurrence(self):
        event, path = self.seed("occurrence", "Acceptance daily series", ["RRULE:FREQ=DAILY;COUNT=5"])
        occurrences = [e for e in self.rows() if e["id"] == event["id"]]
        require(len(occurrences) == 5, "recurring series did not expand to five occurrences")
        occurrence = occurrences[1]
        rid = occurrence.get("recurrenceId")
        require(rid, "daemon did not expose occurrence identity")
        self.update(event, {"summary": "Acceptance detached edit", "startUtc": (self.start + timedelta(days=1, hours=2)).isoformat(),
            "endUtc": (self.start + timedelta(days=1, hours=3)).isoformat()}, "occurrence", rid)
        wait_for(lambda: "SUMMARY:Acceptance detached edit" in self.remote(path), "detached readback")
        text = self.remote(path)
        require("RECURRENCE-ID" in text and "RRULE:FREQ=DAILY;COUNT=5" in text, "detached mutation lost master or recurrence identity")
        self.h.stop(); self.h.start(); self.sync()
        wait_for(lambda: self.find("Acceptance detached edit"), "detached restart")
        remaining = [e for e in self.rows() if e["id"] == event["id"] and e.get("summary") == "Acceptance daily series"]
        require(len(remaining) == 4, "occurrence edit changed other occurrences")
        target = remaining[-1]
        current = self.get(event)
        removed = self.h.call("events.remove", {"eventRef": {"eventId": event["id"], "recurrenceId": target["recurrenceId"]},
            "expectedLocalRevision": current["localRevision"], "clientMutationId": str(uuid.uuid4()),
            "recurrenceScope": "occurrence", "guestNotificationPolicy": "none"})
        wait_for(lambda: "STATUS:CANCELLED" in self.remote(path) or "EXDATE" in self.remote(path), "occurrence cancel remote", timeout=30)
        self.sync()
        require(len([e for e in self.rows() if e.get("summary") in ("Acceptance daily series", "Acceptance detached edit") and e.get("status") != "cancelled"]) == 4, "occurrence cancellation affected incorrect count")
        return "Five-instance master, one shifted detached edit, untouched siblings, restart and one occurrence cancellation"

    def automatic_conflict(self, remote_first=False):
        suffix = "local-is-newer" if remote_first else "remote-is-newer"
        original = "Acceptance automatic " + suffix
        event, path = self.seed("automatic-" + suffix, original)
        original_body = self.remote(path)
        remote_summary, local_summary = "Automatic remote " + suffix, "Automatic local " + suffix
        def changed_body():
            timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            body = original_body.replace("SUMMARY:" + original, "SUMMARY:" + remote_summary)
            body = re.sub(r"DTSTAMP:[^\r\n]+", "DTSTAMP:" + timestamp, body)
            return body.replace("END:VEVENT", "LAST-MODIFIED:" + timestamp + "\r\nEND:VEVENT").encode()
        timeline = {}
        try:
            if remote_first:
                self.h.stop()
                status, _ = self.server.request("PUT", path, changed_body())
                require(status in (201, 204), "competing remote edit failed")
                timeline["remoteWritten"] = datetime.now(timezone.utc).isoformat()
                self.server.stop(); self.h.start()
                time.sleep(1.15)
                self.update(event, {"summary": local_summary})
                timeline["localWritten"] = datetime.now(timezone.utc).isoformat()
                self.h.stop(); self.server.start(); self.h.start()
            else:
                self.server.stop()
                self.update(event, {"summary": local_summary})
                timeline["localWritten"] = datetime.now(timezone.utc).isoformat()
                self.h.stop(); self.server.start()
                time.sleep(1.15)
                status, _ = self.server.request("PUT", path, changed_body())
                require(status in (201, 204), "competing remote edit failed")
                timeline["remoteWritten"] = datetime.now(timezone.utc).isoformat()
                self.h.start()
            self.sync()
            conflict = wait_for(lambda: next((c for c in self.h.call("conflicts.list", {"unresolvedOnly": False})["conflicts"]
                if c.get("eventId") == event["id"] and c.get("state") != "unresolved"), None), "automatic conflict resolution", timeout=30)
            (self.root / ("conflict-" + suffix + ".json")).write_text(json.dumps({"timeline": timeline, "conflict": conflict}, indent=2))
            expected = local_summary if remote_first else remote_summary
            actual = self.get(event).get("summary")
            require(actual == expected, "newest-update resolution selected %r instead of %r; actual chronological timestamps and conflict saved" % (actual, expected))
            wait_for(lambda: self.server.contains(expected), "automatic winner remote readback")
            return {"timeline": timeline, "resolution": conflict["state"], "winner": expected}
        finally:
            if self.server.process is None:
                self.server.start()
            if self.h.process is None:
                self.h.start()

    def ics_semantics(self, server):
        original_payload = smoke.payload
        original_handler = server.server.RequestHandlerClass
        state = {"mode": "valid"}
        body = original_payload("Acceptance feed recurrence", "feed-recurrence", self.start).decode()
        body = body.replace("END:VEVENT", "RRULE:FREQ=DAILY;COUNT=4\r\nEXDATE:" + (self.start + timedelta(days=1)).strftime("%Y%m%dT%H%M%SZ") +
            "\r\nRDATE:" + (self.start + timedelta(days=7)).strftime("%Y%m%dT%H%M%SZ") + "\r\nEND:VEVENT")
        def dynamic_payload(*_args):
            if state["mode"] == "empty":
                return b"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//OmaCalendar//Empty fixture//EN\r\nEND:VCALENDAR\r\n"
            if state["mode"] == "malformed":
                return b"this is not an iCalendar feed"
            return body.encode()
        class Handler(original_handler):
            def do_GET(handler):
                if state["mode"] == "server_error":
                    handler.send_error(503)
                elif state["mode"] == "missing":
                    handler.send_error(404)
                else:
                    super().do_GET()
        smoke.payload = dynamic_payload
        server.server.RequestHandlerClass = Handler
        server.generation += 1
        try:
            added = self.h.call("accounts.addIcs", {"url": server.url, "displayName": "Acceptance feed semantics", "refreshSeconds": 90,
                "username": smoke.FIXTURE_USER, "password": server.password})
            account_id = added["account"]["id"]
            calendar = wait_for(lambda: next(iter(smoke.calendars(self.h, account_id)), None), "ICS semantics calendar")
            calendar_id = calendar["id"]
            wait_for(lambda: len(self.rows(calendar_id)) == 4, "ICS recurrence and inclusion/exclusion expansion")
            error = self.h.call_error("events.create", {"clientMutationId": str(uuid.uuid4()), "guestNotificationPolicy": "none",
                "event": {"calendarId": calendar_id, "summary": "Must reject feed write", "startUtc": self.start.isoformat(),
                    "endUtc": (self.start + timedelta(hours=1)).isoformat()}})
            require(error.get("code") == "calendar_read_only", "ICS write not rejected as read-only")
            statuses = {}
            for mode in ("server_error", "missing", "malformed"):
                wait_for(lambda: self.h.call("sync.status", {"accountId": account_id}).get("state") != "refreshing", "ICS settled before " + mode)
                state["mode"] = mode; server.generation += 1
                self.h.call("ics.refresh", {"accountId": account_id})
                status = wait_for(lambda: (s if (s := self.h.call("sync.status", {"accountId": account_id})).get("state") == "error" else None), "ICS " + mode + " visible error")
                require(len(self.rows(calendar_id)) == 4, "ICS " + mode + " discarded valid cached events")
                statuses[mode] = {"state": status.get("state"), "errorCode": status.get("errorCode")}
            state["mode"] = "empty"; server.generation += 1
            self.h.call("ics.refresh", {"accountId": account_id})
            wait_for(lambda: len(self.rows(calendar_id)) == 0, "successful empty ICS remote deletion")
            self.h.call("accounts.remove", {"accountId": account_id})
            return {"errors": statuses, "coverage": "RRULE/RDATE/EXDATE, read-only writes, 503/404/malformed cache retention, authoritative empty feed deletes cached events"}
        finally:
            smoke.payload = original_payload
            server.server.RequestHandlerClass = original_handler

    def future(self):
        event, path = self.seed("future", "Acceptance future series", ["RRULE:FREQ=DAILY;COUNT=5"])
        occurrences = [e for e in self.rows() if e["id"] == event["id"]]
        self.update(event, {"summary": "Acceptance future changed"}, "future", occurrences[2]["recurrenceId"])
        wait_for(lambda: "RANGE=THISANDFUTURE" in self.remote(path), "qualified future remote mutation", timeout=45)
        calendar = next(c for c in smoke.calendars(self.h, self.account_id) if c["id"] == self.calendar_id)
        require(calendar.get("capabilities", {}).get("thisAndFuture") is True, "successful probe did not persist capability")
        self.h.stop(); self.h.start(); self.sync()
        rows = [e for e in self.rows() if e.get("summary") in ("Acceptance future changed", "Acceptance future series")]
        require(sum(e.get("summary") == "Acceptance future changed" for e in rows) == 3, "future edit did not affect last three instances")
        require(sum(e.get("summary") == "Acceptance future series" for e in rows) == 2, "future edit altered earlier instances")
        return "RANGE=THISANDFUTURE write/readback, persisted capability and two-before/three-after presentation"

    def future_after_external_proof(self):
        event, path = self.seed("future-proof", "External proof master", ["RRULE:FREQ=DAILY;COUNT=5"])
        body = self.remote(path)
        boundary = self.start + timedelta(days=2)
        exception = "\r\n".join(["BEGIN:VEVENT", "UID:acceptance-future-proof", "DTSTAMP:" + self.start.strftime("%Y%m%dT%H%M%SZ"),
            "RECURRENCE-ID;RANGE=THISANDFUTURE:" + boundary.strftime("%Y%m%dT%H%M%SZ"),
            "DTSTART:" + boundary.strftime("%Y%m%dT%H%M%SZ"), "DTEND:" + (boundary + timedelta(hours=1)).strftime("%Y%m%dT%H%M%SZ"),
            "SUMMARY:External proof future", "END:VEVENT", ""])
        status, _ = self.server.request("PUT", path, body.replace("END:VCALENDAR", exception + "END:VCALENDAR").encode())
        require(status in (201, 204), "Radicale rejected externally seeded RANGE")
        require("RANGE=THISANDFUTURE" in self.remote(path), "Radicale stripped externally seeded RANGE")
        self.sync()
        wait_for(lambda: next(c for c in smoke.calendars(self.h, self.account_id) if c["id"] == self.calendar_id).get("capabilities", {}).get("thisAndFuture") is True,
            "capability from observed external RANGE")
        result = self.future()
        return {"scope": "Server support established by real external PUT/GET plus daemon pull; does not exercise first-mutation probe", "subsequentMutation": result}

    def conflict(self, strategy, deleted=False):
        suffix = strategy + ("-deleted" if deleted else "")
        original = "Acceptance conflict " + suffix
        event, path = self.seed("conflict-" + suffix, original)
        original_body = self.remote(path)
        try:
            self.server.stop()
            self.update(event, {"summary": "Local " + suffix})
            local_modified = datetime.fromisoformat(self.get(event)["updatedAt"].replace("Z", "+00:00"))
            self.h.stop()
            self.server.start()
            if deleted:
                status, _ = self.server.request("DELETE", path)
            else:
                remote_body = original_body.replace("SUMMARY:" + original, "SUMMARY:Remote " + suffix)
                remote_body = remote_body.replace("END:VEVENT", "LAST-MODIFIED:" + local_modified.strftime("%Y%m%dT%H%M%SZ") + "\r\nEND:VEVENT")
                status, _ = self.server.request("PUT", path, remote_body.encode())
            require(status in (200, 201, 204), "competing remote change failed")
            self.h.start(); self.sync()
            conflict = wait_for(lambda: next((c for c in self.h.call("conflicts.list")["conflicts"] if c.get("eventId") == event["id"]), None), "visible conflict " + suffix, timeout=45)
            params = {"id": conflict["id"], "strategy": strategy}
            expected = "Remote " + suffix if strategy == "keep_remote" else "Local " + suffix
            if strategy == "merge":
                expected = "Merged " + suffix
                params["mergedEvent"] = {**self.get(event), "summary": expected, "description": "Merged synthetic notes"}
            self.h.call("conflicts.resolve", params)
            wait_for(lambda: not any(c.get("id") == conflict["id"] for c in self.h.call("conflicts.list")["conflicts"]), "conflict resolution")
            wait_for(lambda: self.server.contains(expected), "resolved remote content " + suffix, timeout=45)
            require(self.find(expected) is not None, "resolved local presentation is incorrect")
            if deleted:
                require(self.server.request("GET", path)[0] == 404, "keep-local reused remotely deleted resource identity")
            self.h.stop(); self.h.start()
            require(self.find(expected) is not None, "resolved conflict lost after restart")
            return "Real competing remote " + ("deletion" if deleted else "ETag change") + "; " + strategy + " preserves chosen content remotely and across restart"
        finally:
            if self.server.process is None:
                self.server.start()
            if self.h.process is None:
                self.h.start()

    def disconnect(self):
        before = len(self.rows())
        self.h.call("accounts.disconnect", {"accountId": self.account_id})
        wait_for(lambda: smoke.account(self.h, self.account_id).get("authStatus") == "disconnected", "CalDAV disconnect")
        require(len(self.rows()) == before, "disconnect discarded cached events")
        self.h.stop(); self.h.start()
        require(smoke.account(self.h, self.account_id).get("authStatus") == "disconnected", "restart reconnected disconnected account")
        self.h.call("accounts.update", {"accountId": self.account_id, "username": smoke.FIXTURE_USER, "password": smoke.FIXTURE_PASSWORD})
        wait_for(lambda: smoke.account(self.h, self.account_id).get("authStatus") == "connected", "CalDAV reauthorization")
        self.sync()
        wait_for(lambda: len(self.rows()) == before, "reconnected cache")
        self.h.call("accounts.remove", {"accountId": self.account_id})
        wait_for(lambda: not smoke.account(self.h, self.account_id), "CalDAV remove")
        require(not smoke.calendars(self.h, self.account_id), "removed account retained calendars")
        require(self.server.contains("Acceptance zoned alarms"), "local account removal deleted remote events")
        return "Disconnect retains cache across restart, reconnect restores access, removal clears local account only"

    def import_export(self):
        body = smoke.payload("Acceptance ICS semantics", "acceptance-import", self.start).decode()
        body = body.replace("END:VEVENT", "RRULE:FREQ=DAILY;COUNT=4\r\nEXDATE:" + (self.start + timedelta(days=1)).strftime("%Y%m%dT%H%M%SZ") +
            "\r\nRDATE:" + (self.start + timedelta(days=7)).strftime("%Y%m%dT%H%M%SZ") +
            "\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT900S\r\nDESCRIPTION:Synthetic alarm\r\nEND:VALARM\r\nEND:VEVENT")
        params = {"content": body, "destinationCalendarId": "local-default"}
        preview = self.h.call("import.preview", params)
        first = self.h.call("import.commit", params)
        rows = [e for e in self.rows("local-default") if e.get("summary") == "Acceptance ICS semantics"]
        require(len(rows) == 4, "import lost recurrence inclusions or exclusions")
        event = rows[0]
        require(len(event.get("reminders", [])) == 1, "import lost alarm")
        second = self.h.call("import.commit", params)
        require(len([e for e in self.rows("local-default") if e.get("summary") == "Acceptance ICS semantics"]) == 4, "skip duplicate policy created duplicates")
        exported = self.h.call("export.create", {"eventId": event["id"]})["content"]
        require("RDATE" in exported and "EXDATE" in exported and "BEGIN:VALARM" in exported, "export lost recurrence or alarm")
        self.h.call("import.commit", {**params, "content": exported, "duplicatePolicy": "replace"})
        require(len([e for e in self.rows("local-default") if e.get("summary") == "Acceptance ICS semantics"]) == 4, "replace import changed expanded count")
        self.h.call("import.commit", {**params, "content": exported, "duplicatePolicy": "copy"})
        require(len([e for e in self.rows("local-default") if e.get("summary") == "Acceptance ICS semantics"]) == 8, "copy import did not create independent complete recurrence")
        self.h.stop(); self.h.start()
        require(len([e for e in self.rows("local-default") if e.get("summary") == "Acceptance ICS semantics"]) == 8, "ICS import not persisted")
        return {"preview": preview, "first": first, "duplicate_skip": second, "coverage": "RRULE/RDATE/EXDATE and alarm import/export, skip/replace/copy and restart"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--radicale", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manual-conflicts", action="store_true", help="Also reproduce the manual-conflict acceptance mismatch")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    # Only this short symlink is in /tmp; all data/logs stay on the workspace disk.
    link = Path("/tmp") / ("ocpa-" + uuid.uuid4().hex[:10])
    link.symlink_to(output, target_is_directory=True)
    start = (datetime.now(timezone.utc) + timedelta(days=1)).replace(hour=13, minute=0, second=0, microsecond=0)
    h = None
    with ExitStack() as cleanup:
        cleanup.callback(link.unlink)
        daemon_root = link / "daemon"
        daemon_root.mkdir(mode=0o700)
        h = smoke.DaemonHarness(args.daemon.resolve(), args.cli.resolve(), daemon_root)
        smoke.install_secret_fixture(h)
        ics = smoke.AuthenticatedIcsServer(link / "ics", start)
        cleanup.callback(ics.stop)
        launcher = daemon_root / "launch-with-fixture-trust"
        launcher.write_text("#!/bin/sh\nexec " + shlex.join([shutil.which("bwrap"), "--die-with-parent", "--unshare-pid", "--ro-bind", "/", "/",
            "--bind", str(output / "daemon"), str(output / "daemon"), "--ro-bind", str(ics.trust_directory), "/etc/ssl/certs", "--proc", "/proc", "--", str(args.daemon.resolve())]) + "\n")
        launcher.chmod(0o700)
        h.daemon = launcher
        server = smoke.RadicaleServer(args.radicale.resolve(), link / "radicale")
        config = server.root / "config"
        config.write_text(config.read_text().replace("level = warning", "level = info"))
        cleanup.callback(server.stop)
        server.start(); server.seed(start)
        def stop_and_save():
            h.stop()
            (output / "daemon.log").write_text(h.log_text() + "\n")
        cleanup.callback(stop_and_save)
        h.start()
        identity = {"system": h.call("system.info"), "daemonSha256": hashlib.sha256(args.daemon.read_bytes()).hexdigest(),
            "cliSha256": hashlib.sha256(args.cli.read_bytes()).hexdigest(), "command": shlex.join(sys.argv),
            "limits": "Synthetic Secret Service fixture; process-owned loopback providers only; no real user state"}
        (output / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
        suite = Suite(h, server, start, output)
        suite.case("Radicale baseline exact-binary transport", suite.baseline)
        if suite.calendar_id:
            suite.case("Radicale recurrence exception and cancellation", suite.recurrence)
            suite.case("Radicale this-and-future qualification", suite.future)
            suite.case("Radicale this-and-future after external server proof", suite.future_after_external_proof)
            suite.case("Radicale automatic resolution with newer remote edit", suite.automatic_conflict)
            suite.case("Radicale automatic resolution with newer local edit", lambda: suite.automatic_conflict(True))
            if args.manual_conflicts:
                for strategy in ("keep_remote", "keep_local", "merge"):
                    suite.case("Radicale manual conflict " + strategy, lambda s=strategy: suite.conflict(s))
                suite.case("Radicale remote deletion keep_local recreation", lambda: suite.conflict("keep_local", True))
            suite.case("Radicale timezone, multi-day and alarm fidelity", suite.fidelity)
            suite.case("Radicale disconnect/reconnect/remove", suite.disconnect)
        suite.case("Authenticated HTTPS ICS transport/lifecycle", lambda: smoke.run_ics(h, ics, start))
        suite.case("HTTPS ICS recurrence, read-only, errors and remote deletion", lambda: suite.ics_semantics(ics))
        suite.case("ICS recurrence/alarm import export and duplicate policies", suite.import_export)
        h.stop()
        (output / "daemon.log").write_text(h.log_text() + "\n")
        print("Evidence:", output, flush=True)
        return 1 if any(r["status"] == "FAIL" for r in suite.results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
