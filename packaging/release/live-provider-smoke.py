#!/usr/bin/env python3
"""Exercise real Radicale and authenticated HTTPS ICS with disposable local data.

Requires a Radicale 3 executable, openssl, and bubblewrap for ICS TLS. Only loopback servers created by
this process are used. Secret Service is replaced with a disposable fixture;
this is provider transport/restart evidence, not real keyring or account acceptance.
"""

from __future__ import annotations

import argparse
import base64
from contextlib import ExitStack
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shlex
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Callable
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from test_daemon_contract import ContractError, DaemonHarness, require


FIXTURE_USER = "smoke"
FIXTURE_PASSWORD = "disposable-smoke-fixture"


def wait_for(check: Callable[[], Any], description: str, timeout: float = 20) -> Any:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.1)
    raise ContractError("timed out waiting for " + description)


def payload(summary: str, uid: str, start: datetime) -> bytes:
    return ("\r\n".join((
        "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//OmaCalendar//Isolated acceptance//EN",
        "BEGIN:VEVENT", "UID:" + uid, "DTSTAMP:" + start.strftime("%Y%m%dT%H%M%SZ"),
        "DTSTART:" + start.strftime("%Y%m%dT%H%M%SZ"),
        "DTEND:" + (start + timedelta(hours=1)).strftime("%Y%m%dT%H%M%SZ"),
        "SUMMARY:" + summary, "END:VEVENT", "END:VCALENDAR", "",
    ))).encode("utf-8")


def authorization(password: str = FIXTURE_PASSWORD) -> str:
    return "Basic " + base64.b64encode(f"{FIXTURE_USER}:{password}".encode()).decode()


class RadicaleServer:
    def __init__(self, executable: Path, root: Path) -> None:
        self.executable = executable
        self.root = root
        self.process: subprocess.Popen | None = None
        self.log = None
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        self.url = f"http://127.0.0.1:{self.port}"
        self.collection = "/smoke/acceptance/"
        root.mkdir(mode=0o700)
        (root / "users").write_text(f"{FIXTURE_USER}:{FIXTURE_PASSWORD}\n")
        (root / "config").write_text(
            f"[server]\nhosts = 127.0.0.1:{self.port}\n"
            f"[auth]\ntype = htpasswd\nhtpasswd_filename = {root / 'users'}\n"
            "htpasswd_encryption = plain\ndelay = 0\n"
            "[rights]\ntype = owner_only\n"
            f"[storage]\nfilesystem_folder = {root / 'collections'}\n"
            "[logging]\nlevel = warning\n"
        )

    def start(self) -> None:
        self.log = (self.root / "server.log").open("a")
        self.process = subprocess.Popen(
            [str(self.executable), "--config", str(self.root / "config")],
            stdout=self.log, stderr=self.log,
        )

        def ready() -> bool:
            require(self.process is not None and self.process.poll() is None,
                    "isolated Radicale exited during startup")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return True
            except OSError:
                return False

        wait_for(ready, "Radicale startup")

    def stop(self) -> None:
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None

    def request(self, method: str, path: str, data: bytes | None = None,
                content_type: str = "text/calendar") -> tuple[int, bytes]:
        require(path.startswith(self.collection) or path == "/smoke/", "unsafe fixture path")
        request = urllib.request.Request(self.url + path, data=data, method=method, headers={
            "Authorization": authorization(), "Content-Type": content_type,
        })
        # No ambient proxy configuration can redirect a local test to a remote server.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(request, timeout=5) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()

    def seed(self, start: datetime) -> None:
        status, _ = self.request("MKCOL", "/smoke/")
        require(status in (201, 405), "could not create Radicale test principal")
        status, _ = self.request("MKCALENDAR", self.collection, b'''<?xml version="1.0"?>
<c:mkcalendar xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
<d:set><d:prop><d:displayname>Isolated acceptance</d:displayname></d:prop></d:set>
</c:mkcalendar>''', "application/xml")
        require(status == 201, "could not create Radicale test calendar")
        status, _ = self.request("PUT", self.collection + "seed.ics",
                                 payload("Radicale seed", "smoke-seed", start))
        require(status == 201, "could not seed Radicale test calendar")

    def contains(self, summary: str) -> bool:
        status, body = self.request("GET", self.collection)
        require(status == 200, "Radicale calendar readback failed")
        return ("SUMMARY:" + summary).encode() in body


class AuthenticatedIcsServer:
    def __init__(self, root: Path, start: datetime) -> None:
        root.mkdir(mode=0o700)
        self.certificate = root / "fixture-ca.pem"
        private_key = root / "fixture-key.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-days", "1", "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-keyout", str(private_key), "-out", str(self.certificate),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        private_key.chmod(0o600)
        self.trust_directory = root / "trust"
        self.trust_directory.mkdir(mode=0o700)
        certificate_hash = subprocess.check_output([
            "openssl", "x509", "-in", str(self.certificate), "-noout", "-hash",
        ], text=True).strip()
        (self.trust_directory / f"{certificate_hash}.0").symlink_to(self.certificate)
        (self.trust_directory / "fixture-ca.pem").symlink_to(self.certificate)
        self.password = FIXTURE_PASSWORD
        self.generation = 1
        self.authorized_requests = 0
        self.rejected_requests = 0
        self.not_modified_requests = 0
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, _format: str, *_args: Any) -> None:
                pass

            def do_GET(self) -> None:
                if self.path != "/fixture.ics":
                    self.send_error(404)
                    return
                if self.headers.get("Authorization") != authorization(fixture.password):
                    fixture.rejected_requests += 1
                    self.send_response(401)
                    self.send_header("WWW-Authenticate", 'Basic realm="isolated-fixture"')
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                fixture.authorized_requests += 1
                etag = f'"fixture-{fixture.generation}"'
                if self.headers.get("If-None-Match") == etag:
                    fixture.not_modified_requests += 1
                    self.send_response(304)
                    self.send_header("ETag", etag)
                    self.end_headers()
                    return
                body = payload(f"Authenticated ICS {fixture.generation}", "smoke-ics", start)
                self.send_response(200)
                self.send_header("Content-Type", "text/calendar; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("ETag", etag)
                self.end_headers()
                self.wfile.write(body)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.certificate), str(private_key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.url = f"https://127.0.0.1:{self.server.server_port}/fixture.ics"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def install_secret_fixture(harness: DaemonHarness) -> None:
    fixture_bin = harness.root / "fixture-bin"
    fixture_bin.mkdir(mode=0o700)
    secret_root = harness.root / "synthetic-secrets"
    secret_root.mkdir(mode=0o700)
    executable = fixture_bin / "secret-tool"
    executable.write_text(f"#!{sys.executable}\n" + '''import hashlib, os, pathlib, sys
arguments = [value for value in sys.argv[2:] if not value.startswith("--label=")]
key = hashlib.sha256("\\0".join(arguments).encode()).hexdigest()
path = pathlib.Path(os.environ["OMACALENDAR_SMOKE_SECRET_DIR"]) / key
command = sys.argv[1]
if command == "store":
    os.umask(0o077)
    path.write_text(sys.stdin.read())
elif command == "lookup":
    if not path.is_file():
        raise SystemExit(1)
    sys.stdout.write(path.read_text())
elif command == "clear":
    path.unlink(missing_ok=True)
else:
    raise SystemExit(2)
''')
    executable.chmod(0o700)
    harness.env["PATH"] = str(fixture_bin)
    harness.env["OMACALENDAR_SMOKE_SECRET_DIR"] = str(secret_root)
    for key in tuple(harness.env):
        if key.lower().endswith("_proxy"):
            del harness.env[key]


def calendars(harness: DaemonHarness, account_id: str) -> list[dict[str, Any]]:
    return [item for item in harness.call("calendars.list")["calendars"]
            if item.get("accountId") == account_id]


def account(harness: DaemonHarness, account_id: str) -> dict[str, Any]:
    return next((item for item in harness.call("accounts.list")["accounts"]
                 if item.get("id") == account_id), {})


def events(harness: DaemonHarness, calendar_id: str, start: datetime) -> list[dict[str, Any]]:
    return harness.call("events.list", {
        "calendarIds": [calendar_id],
        "start": (start - timedelta(days=1)).isoformat(),
        "end": (start + timedelta(days=7)).isoformat(),
    })["events"]


def has_event(harness: DaemonHarness, calendar_id: str, start: datetime, summary: str) -> bool:
    return any(item.get("summary") == summary for item in events(harness, calendar_id, start))


def run_radicale(harness: DaemonHarness, server: RadicaleServer, start: datetime) -> None:
    added = harness.call("accounts.addCalDav", {
        "endpoint": server.url + "/smoke/", "username": FIXTURE_USER,
        "password": FIXTURE_PASSWORD, "displayName": "Isolated Radicale",
    })
    account_id = added["id"]
    calendar = wait_for(lambda: next(iter(calendars(harness, account_id)), None), "Radicale discovery")
    calendar_id = calendar["id"]
    wait_for(lambda: has_event(harness, calendar_id, start, "Radicale seed"), "Radicale remote seed pull")
    created = harness.call("events.create", {
        "clientMutationId": "live-smoke-create", "guestNotificationPolicy": "none",
        "event": {"calendarId": calendar_id, "summary": "Radicale created",
                  "startUtc": start.isoformat(), "endUtc": (start + timedelta(hours=1)).isoformat(),
                  "startTimeZone": "UTC", "endTimeZone": "UTC"},
    })
    wait_for(lambda: server.contains("Radicale created"), "Radicale create readback")
    current = harness.call("events.get", {"eventId": created["id"]})
    harness.call("events.update", {
        "eventRef": {"eventId": created["id"]}, "expectedLocalRevision": current["localRevision"],
        "clientMutationId": "live-smoke-update", "patch": {"summary": "Radicale updated"},
        "guestNotificationPolicy": "none", "recurrenceScope": "series",
    })
    wait_for(lambda: server.contains("Radicale updated"), "Radicale update readback")
    server.stop()
    harness.stop()
    harness.start()
    require(has_event(harness, calendar_id, start, "Radicale updated"), "offline restart lost CalDAV cache")
    offline = harness.call("events.create", {
        "clientMutationId": "live-smoke-offline", "guestNotificationPolicy": "none",
        "event": {"calendarId": calendar_id, "summary": "Radicale offline queued",
                  "startUtc": start.isoformat(), "endUtc": (start + timedelta(hours=1)).isoformat(),
                  "startTimeZone": "UTC", "endTimeZone": "UTC"},
    })
    require(offline.get("id"), "offline mutation was not accepted locally")
    harness.stop()
    server.start()
    harness.start()
    harness.call("sync.account", {"accountId": account_id})
    wait_for(lambda: server.contains("Radicale offline queued"), "offline queue drain after restarts", timeout=45)
    current = harness.call("events.get", {"eventId": created["id"]})
    harness.call("events.remove", {
        "eventRef": {"eventId": created["id"]}, "expectedLocalRevision": current["localRevision"],
        "clientMutationId": "live-smoke-remove", "guestNotificationPolicy": "none", "recurrenceScope": "series",
    })
    wait_for(lambda: not server.contains("Radicale updated"), "Radicale delayed-delete readback", timeout=30)
    status, _ = server.request("PUT", server.collection + "remote.ics", payload("Radicale remote added", "smoke-remote", start))
    require(status == 201, "remote insertion failed")
    harness.call("sync.account", {"accountId": account_id})
    wait_for(lambda: has_event(harness, calendar_id, start, "Radicale remote added"), "remote insertion pull")
    status, _ = server.request("DELETE", server.collection + "remote.ics")
    require(status in (200, 204), "remote deletion failed")
    harness.call("sync.account", {"accountId": account_id})
    wait_for(lambda: not has_event(harness, calendar_id, start, "Radicale remote added"), "remote deletion pull")
    print("PASS: real Radicale discovery, remote pull, create/update/delete readback, offline queue and server/daemon restart")


def run_ics(harness: DaemonHarness, server: AuthenticatedIcsServer, start: datetime) -> None:
    added = harness.call("accounts.addIcs", {
        "url": server.url, "displayName": "Isolated authenticated ICS", "refreshSeconds": 90,
        "username": FIXTURE_USER, "password": FIXTURE_PASSWORD,
    })
    account_id = added["account"]["id"]
    calendar = wait_for(lambda: next(iter(calendars(harness, account_id)), None), "ICS calendar")
    calendar_id = calendar["id"]
    try:
        wait_for(lambda: has_event(harness, calendar_id, start, "Authenticated ICS 1"), "authenticated HTTPS ICS initial sync")
    except ContractError as error:
        status = harness.call("sync.status", {"accountId": account_id})
        raise ContractError(
            f"{error}; state={status.get('state')}, code={status.get('errorCode')}, "
            f"detail={status.get('errorMessage')}"
        ) from error
    require(server.authorized_requests > 0, "ICS fixture saw no authenticated request")
    harness.stop()
    harness.start()
    require(has_event(harness, calendar_id, start, "Authenticated ICS 1"), "restart lost ICS cache")
    wait_for(lambda: harness.call("sync.status", {"accountId": account_id}).get("state") != "refreshing", "restart ICS refresh")
    before = server.not_modified_requests
    harness.call("ics.refresh", {"accountId": account_id})
    wait_for(lambda: server.not_modified_requests > before, "conditional ICS refresh after credential reload")
    wait_for(lambda: harness.call("sync.status", {"accountId": account_id}).get("state") != "refreshing", "ICS refresh completion")
    server.password = "rotated-disposable-fixture"
    server.generation = 2
    harness.call("ics.refresh", {"accountId": account_id})
    wait_for(lambda: server.rejected_requests > 0, "ICS 401 rejection")
    wait_for(lambda: harness.call("sync.status", {"accountId": account_id}).get("state") == "error", "ICS auth error state")
    require(has_event(harness, calendar_id, start, "Authenticated ICS 1"), "401 response discarded cached ICS events")
    harness.call("accounts.update", {"accountId": account_id, "username": FIXTURE_USER, "password": server.password})
    wait_for(lambda: has_event(harness, calendar_id, start, "Authenticated ICS 2"), "ICS refresh after credential rotation")
    harness.call("accounts.disconnect", {"accountId": account_id})
    wait_for(lambda: account(harness, account_id).get("authStatus") == "disconnected", "ICS disconnect")
    require(has_event(harness, calendar_id, start, "Authenticated ICS 2"), "disconnect discarded ICS cache")
    server.generation = 3
    harness.call("accounts.update", {"accountId": account_id, "username": FIXTURE_USER, "password": server.password})
    wait_for(lambda: has_event(harness, calendar_id, start, "Authenticated ICS 3"), "ICS reconnect")
    harness.call("accounts.remove", {"accountId": account_id})
    wait_for(lambda: not account(harness, account_id), "ICS removal")
    require(not calendars(harness, account_id), "ICS removal retained its calendar")
    print("PASS: authenticated HTTPS ICS, restart credential reload, conditional refresh, 401 cached retention, rotation, disconnect/reconnect/removal")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--radicale", type=Path, default=shutil.which("radicale"))
    parser.add_argument("--provider", choices=("all", "radicale", "ics"), default="all")
    args = parser.parse_args()
    require(args.daemon.is_file() and args.cli.is_file(), "daemon and CLI executables are required")
    if args.provider in ("all", "radicale"):
        require(args.radicale is not None and args.radicale.is_file(), "install pinned Radicale in a disposable environment and pass --radicale")
    require(shutil.which("openssl") is not None, "openssl is required")
    if args.provider in ("all", "ics"):
        require(shutil.which("bwrap") is not None, "bubblewrap is required for isolated HTTPS fixture trust")
    start = (datetime.now(timezone.utc) + timedelta(days=1)).replace(hour=13, minute=0, second=0, microsecond=0)
    with tempfile.TemporaryDirectory(prefix="omacalendar-provider-smoke-") as directory, ExitStack() as cleanup:
        root = Path(directory)
        daemon_root = root / "daemon"
        daemon_root.mkdir(mode=0o700)
        harness = DaemonHarness(args.daemon.resolve(), args.cli.resolve(), daemon_root)
        install_secret_fixture(harness)
        ics = None
        if args.provider in ("all", "ics"):
            ics = AuthenticatedIcsServer(root / "ics", start)
            cleanup.callback(ics.stop)
            # Qt's Unix CA directories are fixed; SSL_CERT_FILE is insufficient.
            # Mount the fixture trust directory only in the daemon namespace.
            # Its XDG root is writable, the host filesystem stays read-only,
            # and TLS certificate/hostname verification remains enabled.
            launcher = daemon_root / "launch-with-fixture-trust"
            command = [shutil.which("bwrap"), "--die-with-parent", "--unshare-pid",
                       "--ro-bind", "/", "/", "--bind", str(daemon_root), str(daemon_root),
                       "--ro-bind", str(ics.trust_directory), "/etc/ssl/certs",
                       "--proc", "/proc", "--", str(args.daemon.resolve())]
            launcher.write_text("#!/bin/sh\nexec " + shlex.join(command) + '\n')
            launcher.chmod(0o700)
            harness.daemon = launcher
        radicale = None
        if args.provider in ("all", "radicale"):
            radicale = RadicaleServer(args.radicale.resolve(), root / "radicale")
            cleanup.callback(radicale.stop)
            radicale.start()
            radicale.seed(start)
        cleanup.callback(harness.stop)
        harness.start()
        print("Testing OmaCalendar", harness.call("system.info")["version"], "with isolated provider fixtures")
        if radicale is not None:
            run_radicale(harness, radicale, start)
        if ics is not None:
            run_ics(harness, ics, start)
    print("Provider smoke passed; all disposable state and servers removed. Real Secret Service and external provider acceptance remain separate.")


if __name__ == "__main__":
    def interrupted(signum: int, _frame: Any) -> None:
        raise InterruptedError(f"received signal {signum}")

    previous_sigterm = signal.signal(signal.SIGTERM, interrupted)
    previous_sigint = signal.signal(signal.SIGINT, interrupted)
    try:
        main()
    except (ContractError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(f"provider smoke failed: {error}") from error
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
        signal.signal(signal.SIGINT, previous_sigint)
