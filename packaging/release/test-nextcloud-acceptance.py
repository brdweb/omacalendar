#!/usr/bin/env python3
"""Real Nextcloud CalDAV acceptance, using one disposable bounded local container.

Only the created container, private network and volume are mutated/removed.
Synthetic credentials are kept out of output; no user profile is accessed.
"""
from __future__ import annotations

import argparse
import base64
from contextlib import ExitStack
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
        self.attendee_user, self.attendee_password = "attendee", "NcTest_" + secrets.token_urlsafe(32)
        self.credentials = {self.user: self.password, self.attendee_user: self.attendee_password}
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
        for password in self.credentials.values():
            value = value.replace(password, "<synthetic-password-redacted>")
        return value

    def occ(self, *args, **kwargs):
        return self.command("exec", "--user", "www-data", self.name, "php", "occ", *args, **kwargs)

    def provision_attendee(self):
        # Keep server-side CalDAV scheduling, but disable every email invitation.
        # These are the actual Nextcloud DAV settings, not a fake scheduling endpoint.
        for key, value in (("sendInvitations", "no"), ("caldav_external_attendees_disabled", "yes"),
                           ("create_example_event", "no"), ("sendEventReminders", "no")):
            self.occ("config:app:set", "dav", key, "--value=" + value)
        self.occ("user:setting", self.user, "settings", "email", self.user + "@example.test")
        env = dict(os.environ, OC_PASS=self.attendee_password)
        self.command("exec", "--env", "OC_PASS", "--user", "www-data", self.name, "php", "occ", "user:add",
                     "--password-from-env", "--email", self.attendee_user + "@example.test", self.attendee_user, env=env)
        self.observed.update(syntheticLocalUsers=2, emailInvitations=False, externalAttendees=False)
        # Server configuration can contain a password; query only these safe values.
        require(self.occ("config:app:get", "dav", "sendInvitations") == "no", "email invitations not disabled")

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

    def request(self, method, path, body=None, headers=None, user=None):
        user = user or self.user
        require(user in self.credentials and ".." not in path and
                (path == "/remote.php/dav/" or any(path.startswith("/remote.php/dav/calendars/" + u + "/")
                 for u in self.credentials)), "unscoped Nextcloud request")
        auth = base64.b64encode((user + ":" + self.credentials[user]).encode()).decode()
        request = Request(self.base + path, data=body, method=method,
                          headers={"Authorization": "Basic " + auth, "Content-Type": "text/calendar; charset=utf-8", **(headers or {})})
        try:
            with self.opener.open(request, timeout=15) as response:
                return response.status, response.read()
        except HTTPError as error:
            return error.code, error.read()

    def resources(self, collection=None, user=None):
        query = b'<c:calendar-query xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:prop><d:getetag/><c:calendar-data/></d:prop><c:filter><c:comp-filter name="VCALENDAR"><c:comp-filter name="VEVENT"/></c:comp-filter></c:filter></c:calendar-query>'
        status, body = self.request("REPORT", collection or self.collection, query, {"Depth": "1", "Content-Type": "application/xml"}, user=user)
        require(status == 207, "Nextcloud calendar REPORT failed with " + str(status))
        root = ET.fromstring(body)
        return [{"path": r.findtext("{DAV:}href"), "etag": r.findtext(".//{DAV:}getetag"),
                 "body": r.findtext(".//{urn:ietf:params:xml:ns:caldav}calendar-data") or ""}
                for r in root.findall("{DAV:}response")]

    def matching(self, summary, collection=None, user=None):
        return [r for r in self.resources(collection, user) if "SUMMARY:" + summary in r["body"]]

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


class ExtendedSuite:
    """Two local users against the real server; failures remain explicit evidence."""

    def __init__(self, server, owner, attendee, output):
        self.server, self.h, self.attendee, self.output = server, owner, attendee, output
        self.start = (datetime.now(timezone.utc) + timedelta(days=2)).replace(hour=13, minute=0, second=0, microsecond=0)
        self.results = []
        self.account_id = self.calendar_id = self.attendee_account_id = ""

    def case(self, name, run):
        began = time.monotonic()
        try:
            result = {"check": name, "status": "PASS", "detail": run()}
        except Exception as error:
            result = {"check": name, "status": "FAIL", "error": str(error), "traceback": traceback.format_exc()}
            diagnostics = {}
            for label, h in (("owner", self.h), ("attendee", self.attendee)):
                diagnostics[label] = {}
                for method in ("sync.status", "operations.list", "conflicts.list", "calendars.list"):
                    try:
                        diagnostics[label][method] = h.call(method)
                    except Exception as diagnostic_error:
                        diagnostics[label][method] = str(diagnostic_error)
            try:
                diagnostics["remote"] = self.server.resources()
            except Exception as diagnostic_error:
                diagnostics["remote"] = str(diagnostic_error)
            (self.output / ("failure-%02d.json" % (len(self.results) + 1))).write_text(self.server.redact(json.dumps(diagnostics, indent=2)) + "\n")
        result["seconds"] = round(time.monotonic() - began, 3)
        self.results.append(result)
        (self.output / "checks.json").write_text(self.server.redact(json.dumps(self.results, indent=2)) + "\n")
        print(result["status"] + ": " + name, flush=True)
        if result["status"] == "FAIL":
            print(self.server.redact(result["error"]), flush=True)

    def sync(self, h=None, account=None):
        (h or self.h).call("sync.account", {"accountId": account or self.account_id})

    def rows(self, h=None, calendar=None):
        return (h or self.h).call("events.list", {"calendarIds": [calendar or self.calendar_id],
            "start": (self.start - timedelta(days=2)).isoformat(), "end": (self.start + timedelta(days=15)).isoformat(), "limit": 500})["events"]

    def find(self, summary, h=None, calendar=None):
        return next((e for e in self.rows(h, calendar) if e.get("summary") == summary), None)

    def get(self, event, h=None):
        return (h or self.h).call("events.get", {"eventId": event["id"]})

    def update(self, event, patch, scope="series", rid=None, policy="none", h=None):
        target = h or self.h
        ref = {"eventId": event["id"]}
        if rid:
            ref["recurrenceId"] = rid
        return target.call("events.update", {"eventRef": ref, "expectedLocalRevision": self.get(event, target)["localRevision"],
            "clientMutationId": str(uuid.uuid4()), "recurrenceScope": scope, "guestNotificationPolicy": policy, "patch": patch})

    def seed(self, name, summary, extra=()):
        body = smoke.payload(summary, "nextcloud-extended-" + name, self.start)
        body = body.replace(b"END:VEVENT", ("\r\n".join(extra) + "\r\nEND:VEVENT").encode())
        path = self.server.collection + name + ".ics"
        require(self.server.request("PUT", path, body)[0] == 201, "remote seed failed")
        self.sync()
        return wait(lambda: self.find(summary), "remote seed pull " + name), path

    def remote(self, path):
        status, body = self.server.request("GET", path)
        require(status == 200, "remote GET failed: " + str(status))
        return body.decode().replace("\r\n ", "").replace("\r\n\t", "")

    def setup(self):
        self.server.provision_attendee()
        # Sharing is server-side ACL state, not an app fixture claiming read-only.
        share = b'<o:share xmlns:d="DAV:" xmlns:o="http://owncloud.org/ns"><o:set><d:href>principal:principals/users/attendee</d:href><o:read-only/></o:set></o:share>'
        status, body = self.server.request("POST", self.server.collection, share, {"Content-Type": "application/xml"})
        require(status == 200, "read-only share rejected: " + str(status) + " " + body.decode()[:400])
        for h, user in ((self.h, self.server.user), (self.attendee, self.server.attendee_user)):
            account = h.call("accounts.addCalDav", {"endpoint": self.server.base + "/remote.php/dav/", "username": user,
                "password": self.server.credentials[user], "displayName": "Synthetic Nextcloud " + user})
            wait(lambda: len(smoke.calendars(h, account["id"])) >= 2, "two-user discovery " + user)
            if h is self.h:
                self.account_id = account["id"]
                self.calendar_id = next(c["id"] for c in smoke.calendars(h, account["id"]) if c["name"] == "Synthetic acceptance")
            else:
                self.attendee_account_id = account["id"]
        caps = {"owner": smoke.calendars(self.h, self.account_id), "attendee": smoke.calendars(self.attendee, self.attendee_account_id)}
        (self.output / "capabilities-initial.json").write_text(json.dumps(caps, indent=2) + "\n")
        return "Two synthetic local users discovered; real CalDAV sharing configured; server email invitations and external attendees disabled"

    def readonly(self):
        event, path = self.seed("readonly", "Nextcloud read-only ACL")
        shared = next(c for c in smoke.calendars(self.attendee, self.attendee_account_id) if c["name"].startswith("Synthetic acceptance"))
        home = "/remote.php/dav/calendars/" + self.server.attendee_user + "/"
        query = b'<d:propfind xmlns:d="DAV:"><d:prop><d:displayname/><d:current-user-privilege-set/></d:prop></d:propfind>'
        status, body = self.server.request("PROPFIND", home, query, {"Depth": "1", "Content-Type": "application/xml"}, user=self.server.attendee_user)
        require(status == 207, "shared calendar privilege query failed")
        (self.output / "readonly-server-privileges.xml").write_bytes(body)
        shared_response = next(r for r in ET.fromstring(body).findall("{DAV:}response")
                               if "Synthetic acceptance" in (r.findtext(".//{DAV:}displayname") or ""))
        shared_path = shared_response.findtext("{DAV:}href")
        privileges = [p[0].tag for p in shared_response.findall(".//{DAV:}privilege") if len(p)]
        resources = self.server.resources(shared_path, self.server.attendee_user)
        shared_resource = next(r for r in resources if "SUMMARY:" + event["summary"] in r["body"])
        before = self.remote(path)
        status, response = self.server.request("PUT", shared_resource["path"], before.replace("SUMMARY:Nextcloud read-only ACL", "SUMMARY:Forbidden direct write").encode(), user=self.server.attendee_user)
        create_status, create_response = self.server.request("PUT", shared_path + "forbidden.ics", before.replace("UID:nextcloud-extended-readonly", "UID:forbidden-readonly").encode(), user=self.server.attendee_user)
        (self.output / "readonly-server-evidence.json").write_text(json.dumps({"calendar": shared, "privileges": privileges,
            "independentServerPutStatus": status, "independentServerPutResponse": response.decode(),
            "independentCreateStatus": create_status, "independentCreateResponse": create_response.decode(),
            "remoteUnchanged": self.remote(path) == before}, indent=2) + "\n")
        require(status in (403, 404) and create_status in (403, 404), "server ACL did not reject independent attendee writes: " + repr((status, create_status)))
        require(shared.get("readOnly"), "App marked real read-only share writable; server rejects PUT and privileges=" + repr(privileges))
        require(all(not shared["capabilities"].get(key) for key in ("attendeeWrites", "rsvp", "serverScheduling")),
                "read-only share exposes scheduling mutation capabilities")
        self.sync(self.attendee, self.attendee_account_id)
        received = wait(lambda: self.find(event["summary"], self.attendee, shared["id"]), "read-only shared event pull")
        errors = {}
        for method, params in (
            ("events.create", {"event": {"calendarId": shared["id"], "summary": "Must reject readonly", "startUtc": self.start.isoformat(), "endUtc": (self.start + timedelta(hours=1)).isoformat()}}),
            ("events.update", {"eventRef": {"eventId": received["id"]}, "patch": {"summary": "Must reject readonly"}}),
            ("events.remove", {"eventRef": {"eventId": received["id"]}})):
            error = self.attendee.call_error(method, {**params, "clientMutationId": str(uuid.uuid4()), "guestNotificationPolicy": "none", "recurrenceScope": "series"})
            require(error.get("code") == "calendar_read_only", method + " did not reject read-only calendar")
            errors[method] = error["code"]
        require(self.remote(path) == before, "read-only attempts altered remote event")
        return {"appRejections": errors, "independentServerWrite": status, "remoteUnchanged": True}

    def conflict(self, strategy, deleted=False):
        suffix = strategy + ("-deleted" if deleted else "")
        original = "Nextcloud conflict " + suffix
        event, path = self.seed("conflict-" + suffix, original)
        before = self.remote(path)
        try:
            self.server.stop()
            self.update(event, {"summary": "Local " + suffix})
            local_modified = datetime.fromisoformat(self.get(event)["updatedAt"].replace("Z", "+00:00"))
            self.h.stop()
            self.server.start()
            if deleted:
                status, _ = self.server.request("DELETE", path)
            else:
                remote = before.replace("SUMMARY:" + original, "SUMMARY:Remote " + suffix)
                # Equal revision timestamps make neither side a safely newer edit.
                remote = remote.replace("END:VEVENT", "LAST-MODIFIED:" + local_modified.strftime("%Y%m%dT%H%M%SZ") + "\r\nEND:VEVENT")
                status, _ = self.server.request("PUT", path, remote.encode())
            require(status in (200, 201, 204), "competing remote change failed")
            self.h.start(); self.sync()
            conflict = wait(lambda: next((c for c in self.h.call("conflicts.list")["conflicts"] if c.get("eventId") == event["id"]), None), "visible conflict " + suffix, timeout=45)
            expected = "Remote " + suffix if strategy == "keep_remote" else "Local " + suffix
            params = {"id": conflict["id"], "strategy": strategy}
            if strategy == "merge":
                expected = "Merged " + suffix
                params["mergedEvent"] = {**self.get(event), "summary": expected, "description": "Merged synthetic notes"}
            self.h.call("conflicts.resolve", params)
            wait(lambda: not any(c.get("id") == conflict["id"] for c in self.h.call("conflicts.list")["conflicts"]), "conflict dismissed")
            wait(lambda: len(self.server.matching(expected)) == 1, "resolved remote value", timeout=45)
            wait(lambda: self.get(event).get("syncState") == "clean", "resolved local acknowledgement", timeout=45)
            if deleted:
                require(self.server.request("GET", path)[0] == 404, "keep-local reused deleted remote identity")
            self.h.stop(); self.h.start(); self.sync()
            require(self.find(expected) is not None, "resolution lost after restart")
            return "Real competing remote " + ("delete" if deleted else "ETag change") + ", explicit " + strategy + ", one chosen remote value, clean acknowledgement and restart"
        finally:
            self.server.start()
            if self.h.process is None:
                self.h.start()

    def future(self):
        event, path = self.seed("future", "Nextcloud future master", ["RRULE:FREQ=DAILY;COUNT=5"])
        before_resources = self.server.resources()
        self.h.call("calendars.probeThisAndFuture", {"calendarId": self.calendar_id})
        calendar = wait(lambda: next((c for c in smoke.calendars(self.h, self.account_id)
            if c["id"] == self.calendar_id and c["capabilities"].get("thisAndFutureProbeState") in ("supported", "failed")), None),
            "standalone future capability probe", timeout=45)
        (self.output / "future-probe-result.json").write_text(json.dumps(calendar, indent=2) + "\n")
        require(calendar["capabilities"].get("thisAndFutureProven"), "Nextcloud scope qualification failed: " + repr(calendar["capabilities"]))
        require(self.server.resources() == before_resources, "capability probe left or altered a server resource")
        occurrences = [e for e in self.rows() if e["id"] == event["id"]]
        self.update(event, {"summary": "Nextcloud future changed"}, "future", occurrences[2]["recurrenceId"])
        wait(lambda: "RANGE=THISANDFUTURE" in self.remote(path), "first future mutation and server qualification", timeout=45)
        calendar = next(c for c in smoke.calendars(self.h, self.account_id) if c["id"] == self.calendar_id)
        require(calendar["capabilities"].get("thisAndFutureProven"), "successful future readback did not persist proof")
        self.h.stop(); self.h.start(); self.sync()
        rows = self.rows()
        require(sum(e["summary"] == "Nextcloud future master" for e in rows) == 2 and
                sum(e["summary"] == "Nextcloud future changed" for e in rows) == 3, "future scope changed wrong occurrences")
        return "Explicit first-use server probe with no residue, persisted capability, RANGE readback and two-before/three-after recurrence presentation after restart"

    def scheduling(self):
        calendar = next(c for c in smoke.calendars(self.h, self.account_id) if c["id"] == self.calendar_id)
        require(calendar["capabilities"].get("serverScheduling"), "Nextcloud scheduling was not advertised")
        payload = {"calendarId": self.calendar_id, "summary": "Nextcloud local invitation", "startUtc": self.start.isoformat(),
            "endUtc": (self.start + timedelta(hours=1)).isoformat(), "startTimeZone": "UTC", "endTimeZone": "UTC",
            "organizer": {"email": self.server.user + "@example.test", "displayName": "Synthetic owner"},
            "attendees": [{"email": self.server.attendee_user + "@example.test", "displayName": "Synthetic attendee", "rsvp": True,
                "role": "REQ-PARTICIPANT", "responseStatus": "needsAction"}]}
        event = self.h.call("events.create", {"event": payload, "clientMutationId": str(uuid.uuid4()), "guestNotificationPolicy": "all", "recurrenceScope": "series"})
        wait(lambda: self.get(event).get("syncState") == "clean", "organizer invitation acknowledgement", timeout=45)
        self.sync(self.attendee, self.attendee_account_id)
        def invitation():
            for c in smoke.calendars(self.attendee, self.attendee_account_id):
                if not c.get("readOnly"):
                    found = self.find(payload["summary"], self.attendee, c["id"])
                    if found:
                        return found
            return None
        received = wait(invitation, "server delivery to local attendee calendar", timeout=30)
        require(any(a.get("self") for a in received["attendees"]), "attendee self identity not recognized")
        require(any(e["id"] == received["id"] for e in self.attendee.call("invitations.list")["invitations"]),
                "received invitation missing from pending list")
        owner_path = self.server.matching(payload["summary"])[0]["path"]
        for response in ("accepted", "tentative", "declined"):
            current = self.get(received, self.attendee)
            self.attendee.call("events.respond", {"eventRef": {"eventId": current["id"]}, "response": response,
                "expectedLocalRevision": current["localRevision"], "clientMutationId": str(uuid.uuid4()), "guestNotificationPolicy": "all", "recurrenceScope": "series"})
            wait(lambda: "PARTSTAT=" + response.upper() in self.remote(owner_path), "organizer receives " + response, timeout=45)
            wait(lambda: self.get(received, self.attendee).get("syncState") == "clean", "RSVP acknowledgement " + response, timeout=45)
            require(not any(e["id"] == received["id"] for e in self.attendee.call("invitations.list")["invitations"]),
                    "completed RSVP remains in pending list")
            self.sync()
            wait(lambda: any(a.get("partstat") == response.upper() for a in self.get(event)["attendees"]), "organizer app sees " + response)
        self.attendee.stop(); self.attendee.start()
        require(any(a.get("responseStatus") == "declined" for a in self.get(received, self.attendee)["attendees"]), "RSVP lost after restart")
        self.update(event, {"location": "Updated local invitation room"}, policy="all")
        self.sync(self.attendee, self.attendee_account_id)
        wait(lambda: self.get(received, self.attendee).get("location") == "Updated local invitation room", "organizer guest update delivered")
        return "App-created organizer invitation delivered to second local user's writable calendar; accepted/tentative/declined return to organizer server and app; attendee restart and organizer update; no emails"

    def scoped_scheduling(self, scope):
        summary = "Nextcloud local recurring invitation " + scope
        payload = {"calendarId": self.calendar_id, "summary": summary, "startUtc": self.start.isoformat(),
            "endUtc": (self.start + timedelta(hours=1)).isoformat(), "startTimeZone": "UTC", "endTimeZone": "UTC",
            "recurrenceRule": "FREQ=DAILY;COUNT=5", "organizer": {"email": self.server.user + "@example.test"},
            "attendees": [{"email": self.server.attendee_user + "@example.test", "rsvp": True,
                           "role": "REQ-PARTICIPANT", "responseStatus": "needsAction"}]}
        event = self.h.call("events.create", {"event": payload, "clientMutationId": str(uuid.uuid4()), "guestNotificationPolicy": "all", "recurrenceScope": "series"})
        wait(lambda: self.get(event).get("syncState") == "clean", "recurring organizer acknowledgement", timeout=45)
        self.sync(self.attendee, self.attendee_account_id)
        calendar = next(c for c in smoke.calendars(self.attendee, self.attendee_account_id) if c["name"] == "Personal")
        received = wait(lambda: self.find(summary, self.attendee, calendar["id"]), "recurring local invitation delivery")
        instances = [e for e in self.rows(self.attendee, calendar["id"]) if e.get("summary") == summary]
        require(len(instances) == 5, "recurring invitation expansion wrong")
        if scope == "future":
            self.attendee.call("calendars.probeThisAndFuture", {"calendarId": calendar["id"]})
            wait(lambda: next((c for c in smoke.calendars(self.attendee, self.attendee_account_id) if c["id"] == calendar["id"] and
                c["capabilities"].get("thisAndFutureProven")), None), "attendee calendar future qualification", timeout=45)
        target = instances[2]
        current = self.get(received, self.attendee)
        response_params = {"eventRef": {"eventId": current["id"], "recurrenceId": target["recurrenceId"]},
            "response": "accepted", "expectedLocalRevision": current["localRevision"], "clientMutationId": str(uuid.uuid4()),
            "guestNotificationPolicy": "all", "recurrenceScope": scope}
        future_rejected = scope == "future"
        if future_rejected:
            calendar = next(c for c in smoke.calendars(self.attendee, self.attendee_account_id) if c["id"] == calendar["id"])
            require(calendar["capabilities"].get("thisAndFutureProven") and
                    calendar["capabilities"].get("rsvpThisAndFuture") is False,
                    "storage proof incorrectly enables future scheduling responses")
            before_owner = self.server.resources()
            before_attendee = self.server.resources("/remote.php/dav/calendars/attendee/personal/", self.server.attendee_user)
            error = self.attendee.call_error("events.respond", response_params)
            require(error.get("code") == "recurrence_scope_unsupported", "unproven future RSVP was not rejected")
            require(self.get(received, self.attendee) == current, "rejected future RSVP altered the local event")
            require(not any(o.get("clientMutationId") == response_params["clientMutationId"]
                    for o in self.attendee.call("operations.list")["items"]), "rejected future RSVP created durable work")
            require(self.server.resources() == before_owner and
                    self.server.resources("/remote.php/dav/calendars/attendee/personal/", self.server.attendee_user) == before_attendee,
                    "rejected future RSVP changed remote data")
            (self.output / "future-rsvp-rejection.json").write_text(json.dumps({"error": error, "calendar": calendar,
                "localUnchanged": True, "outboxMutationAbsent": True, "bothRemoteCalendarsUnchanged": True}, indent=2) + "\n")
            # The supported whole-series response must remain usable after rejection.
            response_params.update(eventRef={"eventId": current["id"]}, recurrenceScope="series", clientMutationId=str(uuid.uuid4()))
        response = self.attendee.call("events.respond", response_params)
        updated = response["event"]
        wait(lambda: self.get(updated, self.attendee).get("syncState") == "clean", "scoped RSVP local acknowledgement", timeout=45)
        owner_path = self.server.matching(summary)[0]["path"]
        wait(lambda: "PARTSTAT=ACCEPTED" in self.remote(owner_path), "scoped RSVP organizer readback", timeout=45)
        self.h.stop(); self.h.start(); self.sync()
        self.attendee.stop(); self.attendee.start(); self.sync(self.attendee, self.attendee_account_id)
        expected = 5 if future_rejected else 1
        def accepted_count(h, calendar_id):
            items = [e for e in self.rows(h, calendar_id) if e.get("summary") == summary]
            return len(items), sum(any(a.get("partstat") == "ACCEPTED" for a in e["attendees"]) for e in items)
        wait(lambda: accepted_count(self.h, self.calendar_id)[1] > 0, "organizer response sync")
        details = {"scope": scope, "ownerResource": self.remote(owner_path),
            "attendeeResources": self.server.matching(summary, "/remote.php/dav/calendars/attendee/personal/", self.server.attendee_user),
            "ownerCounts": accepted_count(self.h, self.calendar_id), "attendeeCounts": accepted_count(self.attendee, calendar["id"]),
            "ownerEvents": [e for e in self.rows() if e.get("summary") == summary],
            "attendeeEvents": [e for e in self.rows(self.attendee, calendar["id"]) if e.get("summary") == summary]}
        (self.output / ("scoped-rsvp-" + scope + ".json")).write_text(json.dumps(details, indent=2) + "\n")
        wait(lambda: accepted_count(self.h, self.calendar_id) == (5, expected), "scoped RSVP organizer instance statuses")
        require(accepted_count(self.attendee, calendar["id"]) == (5, expected), "scoped RSVP attendee instance statuses wrong after restart")
        if future_rejected:
            return "Future RSVP remains unsupported despite successful storage RANGE proof; rejected before local/outbox/remote mutation; subsequent whole-series RSVP updates all five instances in both calendars across restart"
        return "Five-instance real invitation; " + scope + " RSVP updates exactly " + str(expected) + " instances in organizer and attendee calendars after restart"


def extended_main(args):
    server = Nextcloud(args.image, args.output)
    suite = None
    harnesses = []
    started = time.monotonic()
    checks = []
    try:
        server.provision()
        with ExitStack() as stack:
            for label in ("owner", "attendee"):
                root = stack.enter_context(helpers.profile(args.work, "nextcloud-extended-" + label))
                h = helpers.DaemonHarness(args.bin / "omacalendard", args.bin / "omacalendarctl", root)
                smoke.install_secret_fixture(h)
                harnesses.append(h)
                h.start()
                require(h.call("system.info")["version"] == args.version, "wrong candidate version")
            suite = ExtendedSuite(server, *harnesses, args.output)
            suite.case("Two-user setup and capabilities", suite.setup)
            if suite.results[-1]["status"] == "PASS":
                selected = args.extended_case or ["readonly", "future", "scheduling", "scoped-rsvp", "conflicts"]
                if "readonly" in selected:
                    suite.case("Read-only shared-calendar permissions", suite.readonly)
                if "future" in selected:
                    suite.case("First-use this-and-future qualification", suite.future)
                if "scheduling" in selected:
                    suite.case("Local scheduling and three RSVP responses", suite.scheduling)
                if "scoped-rsvp" in selected:
                    for scope in ("occurrence", "future"):
                        label = ("Future RSVP rejected safely; series remains supported" if scope == "future"
                                 else "Recurring invitation RSVP: occurrence")
                        suite.case(label, lambda scope=scope: suite.scoped_scheduling(scope))
                if "conflicts" in selected:
                    for strategy in ("keep_remote", "keep_local", "merge"):
                        suite.case("Competing ETag conflict: " + strategy, lambda strategy=strategy: suite.conflict(strategy))
                    suite.case("Remote deletion conflict: keep_local", lambda: suite.conflict("keep_local", deleted=True))
            checks = suite.results
            # Stop within the profile context so logs remain available.
            for label, h in zip(("owner", "attendee"), harnesses):
                h.stop()
                (args.output / (label + "-daemon.log")).write_text(server.redact(h.log_text()) + "\n")
    except (Exception, KeyboardInterrupt) as error:
        checks = (suite.results if suite else []) + [{"check": "extended setup/run", "status": "FAIL", "error": server.redact(str(error)), "traceback": server.redact(traceback.format_exc())}]
    finally:
        for h in harnesses:
            h.stop()
        try:
            server.cleanup()
        except Exception as error:
            checks.append({"check": "cleanup", "status": "FAIL", "error": server.redact(str(error))})
        summary = {"status": "FAIL" if any(c["status"] == "FAIL" for c in checks) else "PASS", "checks": checks,
            "seconds": round(time.monotonic() - started, 3), "candidateVersion": args.version,
            "daemonSha256": hashlib.sha256((args.bin / "omacalendard").read_bytes()).hexdigest(),
            "cliSha256": hashlib.sha256((args.bin / "omacalendarctl").read_bytes()).hexdigest(),
            "selectedExtendedCases": args.extended_case or ["readonly", "future", "scheduling", "scoped-rsvp", "conflicts"],
            "limits": "Real disposable Nextcloud 34.0.3; two local synthetic users; loopback HTTP/SQLite; fake Secret Service. Email invitations disabled. No real accounts, hosted infrastructure, TLS, GUI, real keyring or external mail. Future RSVP scheduling is unsupported and must reject safely even after storage RANGE qualification."}
        (args.output / "results.json").write_text(server.redact(json.dumps(summary, indent=2)) + "\n")
    return 0 if summary["status"] == "PASS" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--image", required=True, help="Digest-pinned official Nextcloud 34.0.3 apache image")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", default="1.0.0-rc.3", help="Expected candidate version; pass the exact release version")
    parser.add_argument("--extended-only", action="store_true", help="Run two-user scheduling, permissions, conflicts and scope qualification")
    parser.add_argument("--extended-case", action="append", choices=("readonly", "future", "scheduling", "scoped-rsvp", "conflicts"),
                        help="Limit extended-only to named cases; may repeat; setup always runs")
    parser.add_argument("--ack-timeout", type=float, default=65,
                        help="Seconds to await clean acknowledgement; retain failures and continue")
    args = parser.parse_args()
    require(args.image.startswith("nextcloud@sha256:"), "digest-pinned Nextcloud image required")
    args.bin, args.work, args.output = args.bin.resolve(), args.work.resolve(), args.output.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    if args.extended_only:
        raise SystemExit(extended_main(args))
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
            require(h.call("system.info")["version"] == args.version, "wrong candidate version")
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
            # Independent readback can overlap the daemon's canonical pull.
            # Open the current revision immediately before this CRUD mutation.
            timed = h.call("events.get", {"eventId": timed["id"]})
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
