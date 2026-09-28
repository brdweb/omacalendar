#!/usr/bin/env python3
"""Measure full widget.snapshot IPC while 20,000 local CalDAV events sync.

Only a disposable daemon, loopback HTTP fixture, and fake secret-tool are used.
No installed daemon, keyring, user database, or external provider is contacted.
"""

from __future__ import annotations

import argparse
from datetime import UTC, datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sqlite3
import statistics
import tempfile
import threading
import time

from release_performance import (
    HarnessError,
    IsolatedDaemon,
    JsonSocket,
    WIDGET_GATE_MS,
    percentile,
    require,
)

EVENT_COUNT = 20_000
SYNC_TIMEOUT_SECONDS = 600


def resource(index: int) -> str:
    start = datetime(2025, 1, 1, 9, tzinfo=UTC) + timedelta(days=index % 2190)
    end = start + timedelta(minutes=30)
    return (
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
        "PRODID:-//OmaCalendar local performance fixture//EN\r\n"
        "BEGIN:VEVENT\r\n"
        f"UID:perf-{index}@example.test\r\n"
        "DTSTAMP:20260901T000000Z\r\n"
        f"DTSTART:{start:%Y%m%dT%H%M%SZ}\r\n"
        f"DTEND:{end:%Y%m%dT%H%M%SZ}\r\n"
        f"SUMMARY:Sync fixture {index}\r\n"
        "END:VEVENT\r\nEND:VCALENDAR\r\n"
    )


def multistatus(events: int) -> bytes:
    parts = [
        '<?xml version="1.0"?><d:multistatus xmlns:d="DAV:" '
        'xmlns:c="urn:ietf:params:xml:ns:caldav">'
    ]
    for index in range(events):
        parts.append(
            f"<d:response><d:href>/calendar/{index}.ics</d:href>"
            '<d:propstat><d:prop><d:getetag>"perf-1"</d:getetag>'
            f"<c:calendar-data><![CDATA[{resource(index)}]]></c:calendar-data>"
            '</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>'
            '</d:response>'
        )
    parts.append("<d:sync-token>perf-sync-1</d:sync-token></d:multistatus>")
    return "".join(parts).encode()


class FixtureHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    fixture_response: bytes

    def do_PROPFIND(self) -> None:
        if self.path == "/dav/":
            body = (
                '<?xml version="1.0"?><d:multistatus xmlns:d="DAV:">'
                '<d:response><d:href>/dav/</d:href><d:propstat><d:prop>'
                '<d:current-user-principal><d:href>/principal/</d:href>'
                '</d:current-user-principal></d:prop>'
                '<d:status>HTTP/1.1 200 OK</d:status></d:propstat>'
                '</d:response></d:multistatus>'
            )
        elif self.path == "/principal/":
            body = (
                '<?xml version="1.0"?><d:multistatus xmlns:d="DAV:" '
                'xmlns:c="urn:ietf:params:xml:ns:caldav">'
                '<d:response><d:href>/principal/</d:href><d:propstat><d:prop>'
                '<c:calendar-home-set><d:href>/home/</d:href>'
                '</c:calendar-home-set></d:prop>'
                '<d:status>HTTP/1.1 200 OK</d:status></d:propstat>'
                '</d:response></d:multistatus>'
            )
        elif self.path == "/home/":
            body = (
                '<?xml version="1.0"?><d:multistatus xmlns:d="DAV:" '
                'xmlns:c="urn:ietf:params:xml:ns:caldav">'
                '<d:response><d:href>/calendar/</d:href><d:propstat><d:prop>'
                '<d:resourcetype><d:collection/><c:calendar/></d:resourcetype>'
                '<d:displayname>Local performance fixture</d:displayname>'
                '<d:sync-token>perf-sync-1</d:sync-token>'
                '<d:current-user-privilege-set><d:privilege><d:read/>'
                '</d:privilege></d:current-user-privilege-set>'
                '</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>'
                '</d:response></d:multistatus>'
            )
        else:
            self.send_error(404)
            return
        self.respond(body.encode())

    def do_REPORT(self) -> None:
        if self.path != "/calendar/":
            self.send_error(404)
            return
        self.respond(self.fixture_response)

    def respond(self, body: bytes) -> None:
        self.send_response(207)
        self.send_header("Content-Type", "application/xml; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.close_connection = True
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args: object) -> None:
        pass


def install_fake_secret_tool(root: Path) -> None:
    helper = root / "empty-bin" / "secret-tool"
    helper.write_text(
        "#!/bin/sh\n"
        'case "$1" in\n'
        "  store) IFS= read -r ignored; exit 0 ;;\n"
        "  lookup) printf '%s\\n' 'fixture-password'; exit 0 ;;\n"
        "  clear) exit 0 ;;\n"
        "  *) exit 2 ;;\n"
        "esac\n",
        encoding="utf-8",
    )
    helper.chmod(0o700)


def run(daemon_executable: Path, output: Path | None, enforce: bool) -> dict:
    require(daemon_executable.is_file(), f"daemon not found: {daemon_executable}")
    FixtureHandler.fixture_response = multistatus(EVENT_COUNT)
    server = ThreadingHTTPServer(("127.0.0.1", 0), FixtureHandler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="omacalendar-sync-perf-") as temp:
            isolated = IsolatedDaemon(daemon_executable, Path(temp))
            install_fake_secret_tool(Path(temp))
            isolated.start()
            try:
                client = JsonSocket(isolated.socket_path, timeout=15.0)
                try:
                    for _ in range(5):
                        client.call("widget.snapshot")
                    _, account = client.call(
                        "accounts.addCalDav",
                        {
                            "endpoint": f"http://127.0.0.1:{server.server_port}/dav/",
                            "username": "fixture-user",
                            "password": "fixture-password",
                            "displayName": "Isolated sync latency fixture",
                        },
                    )
                    account_id = account["id"]
                    deadline = time.monotonic() + SYNC_TIMEOUT_SECONDS
                    samples: list[float] = []
                    saw_syncing = False
                    state = ""
                    last_poll = 0.0
                    while time.monotonic() < deadline:
                        latency, snapshot = client.call("widget.snapshot")
                        require(isinstance(snapshot, dict), "widget result is invalid")
                        samples.append(latency)
                        now = time.monotonic()
                        if now - last_poll >= 0.5:
                            _, status = client.call(
                                "sync.status", {"accountId": account_id}
                            )
                            state = status.get("state")
                            last_poll = now
                            saw_syncing |= state == "syncing"
                            if state in ("error", "reauthorization_required"):
                                raise HarnessError(f"fixture sync failed: {status}")
                            if saw_syncing and state == "idle":
                                break
                        time.sleep(0.02)
                    require(saw_syncing and state == "idle", "sync timed out")
                    with sqlite3.connect(isolated.database_path) as database:
                        count = database.execute(
                            "SELECT COUNT(*) FROM events e JOIN calendars c "
                            "ON c.id=e.calendar_id WHERE c.account_id=?",
                            (account_id,),
                        ).fetchone()[0]
                        tokens = database.execute(
                            "SELECT sync_token FROM calendars WHERE account_id=?",
                            (account_id,),
                        ).fetchall()
                    require(count == EVENT_COUNT, f"only {count} events applied")
                    require(
                        len(tokens) == 1 and tokens[0][0] == "perf-sync-1",
                        f"final sync token missing: {tokens}",
                    )
                    result = {
                        "fixture": "isolated-local-caldav",
                        "events": count,
                        "samples_ms": samples,
                        "median_ms": statistics.median(samples),
                        "p95_ms": percentile(samples, 0.95),
                        "max_ms": max(samples),
                        "gate_ms": WIDGET_GATE_MS,
                        "p95_pass": percentile(samples, 0.95) <= WIDGET_GATE_MS,
                        "max_pass": max(samples) <= WIDGET_GATE_MS,
                    }
                    if output is not None:
                        output.parent.mkdir(parents=True, exist_ok=True)
                        output.write_text(json.dumps(result, indent=2) + "\n")
                    print(json.dumps({key: value for key, value in result.items()
                                      if key != "samples_ms"}, indent=2))
                    if enforce:
                        require(
                            result["p95_pass"] and result["max_pass"],
                            "widget.snapshot exceeded the 100 ms sync latency gate",
                        )
                    return result
                finally:
                    client.close()
            finally:
                isolated.stop()
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--enforce-gate", action="store_true")
    args = parser.parse_args()
    try:
        run(args.daemon, args.output, args.enforce_gate)
    except (HarnessError, OSError, TimeoutError, KeyError) as error:
        parser.exit(1, f"sync latency harness: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
