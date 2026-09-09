#!/usr/bin/env python3
"""Real Nextcloud CalDAV acceptance, using one disposable bounded local container.

Only the created container, private network and volume are mutated/removed.
Synthetic credentials are kept out of output; no user profile is accessed.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import traceback
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, ProxyHandler, build_opener
import uuid
import xml.etree.ElementTree as ET

SPEC = importlib.util.spec_from_file_location("nextcloud_upgrade_helpers", Path(__file__).with_name("test-upgrade.py"))
helpers = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(helpers)
smoke = helpers._provider_fixture
require = smoke.require


def wait(check, label, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.25)
    raise AssertionError("Timed out: " + label)


class Nextcloud:
    def __init__(self, image, output):
        self.image, self.output = image, output
        self.name = "omacalendar-acceptance-nextcloud-" + secrets.token_hex(5)
        self.volume, self.network = self.name + "-data", self.name + "-net"
        self.user, self.password = "acceptance", "NcTest_" + secrets.token_urlsafe(32)
        self.created = []
        self.opener = build_opener(ProxyHandler({}))
        self.base = ""
        self.collection = "/remote.php/dav/calendars/acceptance/omacalendar/"
        self.observed = {}

    def command(self, *args, env=None, timeout=60):
        result = subprocess.run(["docker", *args], env=env, text=True, capture_output=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError("Docker " + args[0] + " failed: " + self.redact(result.stderr))
        return result.stdout.strip()

    def redact(self, value):
        return value.replace(self.password, "<synthetic-password-redacted>")

    def provision(self):
        self.command("network", "create", self.network)
        self.created.append("network")
        self.command("volume", "create", self.volume)
        self.created.append("volume")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        env = dict(os.environ, NEXTCLOUD_ADMIN_USER=self.user, NEXTCLOUD_ADMIN_PASSWORD=self.password)
        self.command("run", "-d", "--name", self.name, "--network", self.network,
                     "--memory", "768m", "--memory-swap", "768m", "--cpus", "1", "--pids-limit", "128",
                     "--publish", f"127.0.0.1:{port}:80", "--mount", f"type=volume,source={self.volume},target=/var/www/html",
                     "--env", "NEXTCLOUD_ADMIN_USER", "--env", "NEXTCLOUD_ADMIN_PASSWORD",
                     "--env", "SQLITE_DATABASE=acceptance", "--env", "NEXTCLOUD_TRUSTED_DOMAINS=127.0.0.1 localhost",
                     "--env", "PHP_MEMORY_LIMIT=256M", "--env", "PHP_OPCACHE_MEMORY_CONSUMPTION=64",
                     self.image, env=env)
        self.created.append("container")
        mapping = self.command("port", self.name, "80/tcp")
        require(mapping.startswith("127.0.0.1:"), "Nextcloud port is not loopback-only")
        self.base = "http://" + mapping
        self.wait_ready()
        self.command("exec", "--user", "www-data", self.name, "php", "occ", "config:system:set", "appstoreenabled", "--value=false", "--type=boolean")
        status, _ = self.request("MKCALENDAR", self.collection,
            b'<?xml version="1.0"?><c:mkcalendar xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:set><d:prop><d:displayname>Synthetic acceptance</d:displayname><c:supported-calendar-component-set><c:comp name="VEVENT"/></c:supported-calendar-component-set></d:prop></d:set></c:mkcalendar>')
        require(status == 201, "Nextcloud synthetic calendar creation failed")
        config = json.loads(self.command("inspect", self.name, "--format", '{{json .HostConfig}}'))
        self.observed.update(image=self.image, memoryBytes=config["Memory"], memorySwapBytes=config["MemorySwap"],
                             nanoCpus=config["NanoCpus"], pidsLimit=config["PidsLimit"], loopbackOnly=True,
                             dedicatedNetwork=True, database="SQLite", container=self.name)
        (self.output / "server.json").write_text(json.dumps(self.observed, indent=2) + "\n")

    def wait_ready(self):
        def ready():
            try:
                with self.opener.open(self.base + "/status.php", timeout=3) as response:
                    status = json.load(response)
                if status.get("installed") and not status.get("maintenance"):
                    require(status.get("versionstring") == "34.0.3", "unexpected Nextcloud version")
                    self.observed["status"] = status
                    return True
            except (URLError, TimeoutError, ValueError, ConnectionError):
                pass
            return False
        wait(ready, "Nextcloud installation/status", timeout=240)

    def request(self, method, path, body=None, headers=None):
        require(path.startswith(self.collection) or path == "/remote.php/dav/", "unscoped Nextcloud request")
        auth = base64.b64encode((self.user + ":" + self.password).encode()).decode()
        request = Request(self.base + path, data=body, method=method,
                          headers={"Authorization": "Basic " + auth, "Content-Type": "text/calendar; charset=utf-8", **(headers or {})})
        try:
            with self.opener.open(request, timeout=15) as response:
                return response.status, response.read()
        except HTTPError as error:
            return error.code, error.read()

    def resources(self):
        query = b'<c:calendar-query xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:prop><d:getetag/><c:calendar-data/></d:prop><c:filter><c:comp-filter name="VCALENDAR"><c:comp-filter name="VEVENT"/></c:comp-filter></c:filter></c:calendar-query>'
        status, body = self.request("REPORT", self.collection, query, {"Depth": "1", "Content-Type": "application/xml"})
        require(status == 207, "Nextcloud calendar REPORT failed with " + str(status))
        root = ET.fromstring(body)
        return [{"path": r.findtext("{DAV:}href"), "body": r.findtext(".//{urn:ietf:params:xml:ns:caldav}calendar-data") or ""}
                for r in root.findall("{DAV:}response")]

    def matching(self, summary):
        return [r for r in self.resources() if "SUMMARY:" + summary in r["body"]]

    def stop(self):
        self.command("stop", "--time", "10", self.name)

    def start(self):
        self.command("start", self.name)
        self.wait_ready()

    def cleanup(self):
        if "container" in self.created:
            result = subprocess.run(["docker", "logs", self.name], capture_output=True, text=True, timeout=15)
            (self.output / "server.log").write_text(self.redact(result.stdout + result.stderr))
            state = self.command("inspect", self.name, "--format", '{{json .State}}')
            (self.output / "final-container-state.json").write_text(state + "\n")
            self.command("rm", "-f", self.name)
        if "volume" in self.created:
            self.command("volume", "rm", self.volume)
        if "network" in self.created:
            self.command("network", "rm", self.network)
        self.observed["cleanedUp"] = True
        (self.output / "server.json").write_text(json.dumps(self.observed, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--image", required=True, help="Digest-pinned official Nextcloud 34.0.3 apache image")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ack-timeout", type=float, default=65,
                        help="Seconds to await clean acknowledgement; retain failures and continue")
    args = parser.parse_args()
    require(args.image.startswith("nextcloud@sha256:"), "digest-pinned Nextcloud image required")
    args.bin, args.work, args.output = args.bin.resolve(), args.work.resolve(), args.output.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    server, results, h = Nextcloud(args.image, args.output), [], None
    started = time.monotonic()
    def passed(label, detail):
        results.append({"check": label, "status": "PASS", "detail": detail})
        print("PASS: " + label, flush=True)
    try:
        server.provision()
        passed("Nextcloud 34.0.3 provisioning", "Official digest pin, 768 MiB, one CPU, private network, loopback-only HTTP, disposable SQLite")
        with helpers.profile(args.work, "nextcloud-profile") as root:
            h = helpers.DaemonHarness(args.bin / "omacalendard", args.bin / "omacalendarctl", root)
            smoke.install_secret_fixture(h)
            h.start()
            require(h.call("system.info")["version"] == "1.0.0-rc.3", "wrong candidate version")
            account = h.call("accounts.addCalDav", {"endpoint": server.base + "/remote.php/dav/", "username": server.user,
                "password": server.password, "displayName": "Synthetic Nextcloud"})
            account_id = account["id"]
            calendar = wait(lambda: next((c for c in smoke.calendars(h, account_id) if c["name"] == "Synthetic acceptance"), None), "Nextcloud discovery")
            calendar_id = calendar["id"]
            (args.output / "capabilities.json").write_text(json.dumps(calendar["capabilities"], indent=2) + "\n")
            passed("Discovery", "Real DAV current-user-principal/calendar-home-set and writable calendar discovery")
            start = (datetime.now(timezone.utc) + timedelta(days=2)).replace(hour=13, minute=0, second=0, microsecond=0)
            def rows():
                return smoke.events(h, calendar_id, start)
            def find(summary):
                return next((e for e in rows() if e["summary"] == summary), None)
            def sync():
                h.call("sync.account", {"accountId": account_id})
            def create(summary, **extra):
                payload = {"calendarId": calendar_id, "summary": summary, "startUtc": start.isoformat(),
                    "endUtc": (start + timedelta(hours=1)).isoformat(), "startTimeZone": "America/New_York",
                    "endTimeZone": "America/New_York", **extra}
                if payload.get("allDay"):
                    for key in ("startUtc", "endUtc", "startTimeZone", "endTimeZone"):
                        payload.pop(key, None)
                event = h.call("events.create", {"event": payload, "clientMutationId": str(uuid.uuid4()), "guestNotificationPolicy": "none", "recurrenceScope": "series"})
                wait(lambda: len(server.matching(summary)) == 1, "Nextcloud remote create " + summary, timeout=65)
                try:
                    wait(lambda: h.call("events.get", {"eventId": event["id"]})["syncState"] == "clean",
                         "Nextcloud create acknowledgement", timeout=args.ack_timeout)
                except AssertionError:
                    details = {"event": h.call("events.get", {"eventId": event["id"]}),
                               "operations": h.call("operations.list", {"limit": 100}),
                               "sync": h.call("sync.status", {"accountId": account_id}),
                               "remote": server.matching(summary)}
                    (args.output / "create-acknowledgement-failure.json").write_text(server.redact(json.dumps(details, indent=2)) + "\n")
                    results.append({"check": "Create acknowledgement: " + summary, "status": "FAIL",
                                    "detail": "Remote object exists but local syncState did not become clean"})
                    print("FAIL: Create acknowledgement: " + summary, flush=True)
                return h.call("events.get", {"eventId": event["id"]})
            timed = create("Nextcloud timed", description="Synthetic notes", location="Synthetic room", reminders=[{"method": "popup", "minutes": 15}, {"method": "popup", "minutes": 60}])
            resource = server.matching("Nextcloud timed")[0]
            require(resource["body"].count("BEGIN:VALARM") == 2 and "TZID=America/New_York" in resource["body"], "remote timezone/alarm data missing")
            h.call("events.update", {"eventRef": {"eventId": timed["id"]}, "expectedLocalRevision": timed["localRevision"],
                "clientMutationId": str(uuid.uuid4()), "guestNotificationPolicy": "none", "recurrenceScope": "series", "patch": {"summary": "Nextcloud updated", "location": "Updated room"}})
            wait(lambda: len(server.matching("Nextcloud updated")) == 1, "Nextcloud update readback")
            passed("Timed write/readback/update", "Single remote object, timezone and two VALARM entries preserved")
            all_day = create("Nextcloud multi-day", allDay=True, timeKind="all_day", startDate=start.date().isoformat(), endDate=(start + timedelta(days=3)).date().isoformat())
            require("DTSTART;VALUE=DATE:" in server.matching("Nextcloud multi-day")[0]["body"], "remote all-day representation missing")
            h.stop(); h.start(); sync()
            restored = wait(lambda: find("Nextcloud multi-day"), "all-day cache restart")
            require(restored["allDay"] and restored["endDate"] == all_day["endDate"], "exclusive all-day date changed")
            passed("Multi-day all-day and restart", "Date-only remote representation and exclusive end survive restart")
            seed_path = server.collection + "recurrence.ics"
            body = smoke.payload("Nextcloud recurring", "nextcloud-recurring", start).replace(b"END:VEVENT", b"RRULE:FREQ=DAILY;COUNT=5\r\nX-OMACALENDAR-SYNTHETIC:keep\r\nEND:VEVENT")
            require(server.request("PUT", seed_path, body)[0] == 201, "remote recurrence seed rejected")
            sync()
            series = wait(lambda: find("Nextcloud recurring"), "remote series pull")
            instances = [e for e in rows() if e["summary"] == "Nextcloud recurring"]
            require(len(instances) == 5, "Nextcloud recurrence expansion wrong")
            target = instances[1]
            current = h.call("events.get", {"eventId": series["id"]})
            h.call("events.update", {"eventRef": {"eventId": series["id"], "recurrenceId": target["recurrenceId"]},
                "expectedLocalRevision": current["localRevision"], "clientMutationId": str(uuid.uuid4()), "guestNotificationPolicy": "none", "recurrenceScope": "occurrence",
                "patch": {"summary": "Nextcloud detached", "startUtc": (start + timedelta(days=1, hours=2)).isoformat(), "endUtc": (start + timedelta(days=1, hours=3)).isoformat()}})
            wait(lambda: "SUMMARY:Nextcloud detached" in server.request("GET", seed_path)[1].decode(), "detached recurrence remote write")
            remote = server.request("GET", seed_path)[1].decode()
            require("RECURRENCE-ID" in remote and "RRULE:FREQ=DAILY;COUNT=5" in remote and "X-OMACALENDAR-SYNTHETIC:keep" in remote, "recurrence mutation lost retained data")
            h.stop(); h.start(); sync()
            wait(lambda: find("Nextcloud detached"), "detached recurrence restart")
            require(len([e for e in rows() if e["summary"] == "Nextcloud recurring"]) == 4, "detached update changed other occurrences")
            passed("Recurrence and detached update", "Five-instance remote series, explicit-date occurrence update, retained master/unknown property and restart")
            server.stop(); h.stop(); h.start()
            require(find("Nextcloud updated") is not None, "offline restart lost cached event")
            offline_id = str(uuid.uuid4())
            offline = h.call("events.create", {"clientMutationId": offline_id, "guestNotificationPolicy": "none", "event": {
                "calendarId": calendar_id, "summary": "Nextcloud offline queued", "startUtc": start.isoformat(), "endUtc": (start + timedelta(hours=1)).isoformat(), "startTimeZone": "UTC", "endTimeZone": "UTC"}})
            h.stop(); server.start(); h.start(); sync()
            wait(lambda: len(server.matching("Nextcloud offline queued")) == 1, "offline durable drain", timeout=60)
            wait(lambda: h.call("events.get", {"eventId": offline["id"]})["syncState"] == "clean",
                 "offline create local acknowledgement", timeout=60)
            h.stop(); h.start(); sync()
            require(len(server.matching("Nextcloud offline queued")) == 1, "offline create duplicated after restart")
            passed("Offline queue and server/daemon restart", "Cached read, offline mutation, both restarts, exactly one remote object after replay")
            current = h.call("events.get", {"eventId": timed["id"]})
            h.call("events.remove", {"eventRef": {"eventId": timed["id"]}, "expectedLocalRevision": current["localRevision"],
                "clientMutationId": str(uuid.uuid4()), "recurrenceScope": "series", "guestNotificationPolicy": "none"})
            wait(lambda: not server.matching("Nextcloud updated"), "delayed remote deletion", timeout=35)
            require(server.request("DELETE", seed_path)[0] in (200, 204), "remote series deletion failed")
            sync(); wait(lambda: find("Nextcloud recurring") is None and find("Nextcloud detached") is None, "remote deletion pull")
            passed("Local delayed delete and remote deletion", "Undo delay drains; externally deleted series and exception disappear locally")
            h.call("accounts.disconnect", {"accountId": account_id, "removeCachedData": False})
            wait(lambda: smoke.account(h, account_id).get("authStatus") == "disconnected", "account disconnect")
            h.call("accounts.remove", {"accountId": account_id, "removeCachedData": True})
            require(not smoke.account(h, account_id), "account removal incomplete")
            passed("Disconnect and removal", "Synthetic account disconnect, then explicit cache removal")
            h.stop()
    except (Exception, KeyboardInterrupt) as error:
        results.append({"check": "active Nextcloud scenario", "status": "FAIL", "error": server.redact(str(error)), "traceback": server.redact(traceback.format_exc())})
        print("FAIL: " + server.redact(str(error)), flush=True)
    finally:
        if h:
            h.stop()
            (args.output / "daemon.log").write_text(server.redact(h.log_text()) + "\n")
        try:
            server.cleanup()
        except Exception as error:
            results.append({"check": "cleanup", "status": "FAIL", "error": server.redact(str(error))})
        summary = {"status": "FAIL" if any(r["status"] == "FAIL" for r in results) else "PASS", "checks": results,
            "seconds": round(time.monotonic() - started, 3), "daemonSha256": hashlib.sha256((args.bin / "omacalendard").read_bytes()).hexdigest(),
            "limits": "Partial real Nextcloud 34.0.3 loopback/SQLite acceptance; synthetic Secret Service. No TLS, real keyring, guest/RSVP, this-and-future, conflict matrix, desktop workflow or hosted-account acceptance."}
        (args.output / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    raise SystemExit(0 if summary["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
