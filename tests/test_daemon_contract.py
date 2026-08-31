#!/usr/bin/env python3
"""Black-box IPC 2.0 release-foundation contract.

The test launches the built daemon with disposable XDG roots and drives it
through the built CLI plus one raw Unix-socket subscriber. It must never read
or write the user's real OmaCalendar state.
"""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import stat
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any


PROTOCOL_MAJOR = 2
SCHEMA_VERSION = 2


class ContractError(AssertionError):
    """Raised when the daemon violates the release-foundation contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def revision(value: dict[str, Any], context: str) -> int:
    result = value.get("revision")
    require(isinstance(result, int) and result >= 0, f"{context}: invalid revision")
    return result


def json_objects(text: str) -> list[dict[str, Any]]:
    objects: list[dict[str, Any]] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            objects.append(value)
    return objects


class JsonSocket:
    def __init__(self, path: Path, timeout: float = 2.0) -> None:
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.settimeout(timeout)
        self._socket.connect(str(path))
        self._buffer = b""

    def close(self) -> None:
        self._socket.close()

    def send(
        self,
        request_id: str,
        method: str,
        params: dict[str, Any] | None = None,
        protocol_major: int = PROTOCOL_MAJOR,
    ) -> None:
        request = {
            "id": request_id,
            "protocolMajor": protocol_major,
            "method": method,
            "params": params or {},
        }
        self.send_value(request)

    def send_value(self, request: Any) -> None:
        payload = json.dumps(request, separators=(",", ":")).encode("utf-8") + b"\n"
        self._socket.sendall(payload)

    def receive(self, timeout: float = 3.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                frame = self._buffer[:newline]
                self._buffer = self._buffer[newline + 1 :]
                value = json.loads(frame.decode("utf-8"))
                require(isinstance(value, dict), "socket response is not an object")
                return value
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ContractError("timed out waiting for daemon socket response")
            self._socket.settimeout(remaining)
            chunk = self._socket.recv(65536)
            if not chunk:
                raise ContractError("daemon closed the socket before replying")
            self._buffer += chunk

    def receive_matching(
        self, predicate: Any, description: str, timeout: float = 3.0
    ) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            message = self.receive(max(0.05, deadline - time.monotonic()))
            if predicate(message):
                return message
        raise ContractError(f"timed out waiting for {description}")

    def require_no_message(self, description: str, timeout: float = 0.2) -> None:
        try:
            message = self.receive(timeout)
        except socket.timeout:
            return
        raise ContractError(f"{description}: unexpected message {message}")


class LoopbackProviderFixture:
    """Serves deterministic ICS and CalDAV data on loopback only."""

    caldav_etag = '"contract-private-caldav-etag"'
    caldav_sync_token = "contract-private-caldav-sync-token"
    caldav_raw_marker = "contract-private-caldav-raw-payload"
    ics_etag = '"contract-private-ics-etag"'
    ics_raw_marker = "contract-private-ics-raw-payload"

    def __init__(self) -> None:
        fixture = self
        self._lock = threading.Lock()
        self._report_bodies: list[str] = []
        self._delete_count = 0
        self._caldav_deleted = False
        self._created_paths: set[str] = set()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, _format: str, *_args: Any) -> None:
                return

            def read_body(self) -> bytes:
                length = int(self.headers.get("Content-Length", "0"))
                return self.rfile.read(length) if length > 0 else b""

            def reply(
                self,
                status: int,
                body: str,
                content_type: str,
                headers: dict[str, str] | None = None,
            ) -> None:
                payload = body.encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Connection", "close")
                for name, value in (headers or {}).items():
                    self.send_header(name, value)
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self) -> None:  # noqa: N802 - HTTP verb hook
                self.read_body()
                if self.path != "/fixture.ics":
                    self.reply(404, "not found", "text/plain")
                    return
                self.reply(
                    200,
                    "\r\n".join(
                        (
                            "BEGIN:VCALENDAR",
                            "VERSION:2.0",
                            "PRODID:-//OmaCalendar//Contract fixture//EN",
                            "BEGIN:VEVENT",
                            "UID:contract-ics-event",
                            "DTSTAMP:20260829T120000Z",
                            "DTSTART:20260904T150000Z",
                            "DTEND:20260904T160000Z",
                            "SUMMARY:Cached ICS contract event",
                            f"X-PRIVATE-MARKER:{fixture.ics_raw_marker}",
                            "END:VEVENT",
                            "END:VCALENDAR",
                            "",
                        )
                    ),
                    "text/calendar; charset=utf-8",
                    {"ETag": fixture.ics_etag},
                )

            def do_PROPFIND(self) -> None:  # noqa: N802 - HTTP verb hook
                self.read_body()
                if self.path == "/dav/":
                    body = """<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:">
  <d:response><d:href>/dav/</d:href><d:propstat><d:prop>
    <d:current-user-principal><d:href>/principals/fixture/</d:href></d:current-user-principal>
  </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>
</d:multistatus>"""
                elif self.path == "/principals/fixture/":
                    body = """<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:response><d:href>/principals/fixture/</d:href><d:propstat><d:prop>
    <c:calendar-home-set><d:href>/calendars/fixture/</d:href></c:calendar-home-set>
  </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>
</d:multistatus>"""
                elif self.path == "/calendars/fixture/":
                    body = f"""<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"
               xmlns:cs="http://calendarserver.org/ns/">
  <d:response><d:href>/calendars/fixture/calendar/</d:href><d:propstat><d:prop>
    <d:resourcetype><d:collection/><c:calendar/></d:resourcetype>
    <d:displayname>Contract CalDAV cache</d:displayname>
    <cs:getctag>contract-private-caldav-ctag</cs:getctag>
    <d:sync-token>{fixture.caldav_sync_token}</d:sync-token>
    <d:current-user-privilege-set><d:privilege><d:read/></d:privilege><d:privilege><d:write-content/></d:privilege></d:current-user-privilege-set>
  </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>
</d:multistatus>"""
                else:
                    self.reply(404, "not found", "text/plain")
                    return
                self.reply(207, body, "application/xml; charset=utf-8")

            def do_REPORT(self) -> None:  # noqa: N802 - HTTP verb hook
                request_body = self.read_body().decode("utf-8", errors="replace")
                if self.path != "/calendars/fixture/calendar/":
                    self.reply(404, "not found", "text/plain")
                    return
                with fixture._lock:
                    fixture._report_bodies.append(request_body)
                    deleted = fixture._caldav_deleted
                if deleted:
                    body = f"""<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:sync-token>{fixture.caldav_sync_token}-deleted</d:sync-token>
</d:multistatus>"""
                    self.reply(207, body, "application/xml; charset=utf-8")
                    return
                calendar_data = "\r\n".join(
                    (
                        "BEGIN:VCALENDAR",
                        "VERSION:2.0",
                        "PRODID:-//OmaCalendar//Contract fixture//EN",
                        "BEGIN:VEVENT",
                        "UID:contract-caldav-event",
                        "DTSTAMP:20260829T120000Z",
                        "DTSTART:20260902T150000Z",
                        "DTEND:20260902T160000Z",
                        "SUMMARY:Cached CalDAV contract event",
                        f"X-PRIVATE-MARKER:{fixture.caldav_raw_marker}",
                        "END:VEVENT",
                        "END:VCALENDAR",
                        "",
                    )
                )
                # A bounded historical query deliberately returns a different
                # collection token. The daemon must not adopt it because the
                # response did not contain changes outside that time range.
                response_sync_token = (
                    f"{fixture.caldav_sync_token}-history"
                    if "20100101T000000Z" in request_body
                    else f"{fixture.caldav_sync_token}-next"
                )
                body = f"""<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:response><d:href>/calendars/fixture/calendar/event.ics</d:href><d:propstat><d:prop>
    <d:getetag>{fixture.caldav_etag.replace('&', '&amp;').replace('<', '&lt;')}</d:getetag>
    <c:calendar-data><![CDATA[{calendar_data}]]></c:calendar-data>
  </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>
  <d:sync-token>{response_sync_token}</d:sync-token>
</d:multistatus>"""
                self.reply(207, body, "application/xml; charset=utf-8")

            def do_DELETE(self) -> None:  # noqa: N802 - HTTP verb hook
                self.read_body()
                if not self.path.startswith("/calendars/fixture/calendar/"):
                    self.reply(404, "not found", "text/plain")
                    return
                with fixture._lock:
                    fixture._delete_count += 1
                    if self.path == "/calendars/fixture/calendar/event.ics":
                        fixture._caldav_deleted = True
                    fixture._created_paths.discard(self.path)
                self.reply(204, "", "text/plain")

            def do_PUT(self) -> None:  # noqa: N802 - HTTP verb hook
                self.read_body()
                if not self.path.startswith("/calendars/fixture/calendar/"):
                    self.reply(404, "not found", "text/plain")
                    return
                with fixture._lock:
                    fixture._created_paths.add(self.path)
                self.reply(
                    201,
                    "",
                    "text/calendar",
                    {"ETag": '"contract-created-etag"'},
                )

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._server.daemon_threads = True
        self._thread = threading.Thread(
            target=self._server.serve_forever,
            name="omacalendar-contract-provider-fixture",
            daemon=True,
        )

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self._server.server_port}"

    @property
    def report_bodies(self) -> list[str]:
        with self._lock:
            return list(self._report_bodies)

    @property
    def delete_count(self) -> int:
        with self._lock:
            return self._delete_count

    def __enter__(self) -> "LoopbackProviderFixture":
        self._thread.start()
        return self

    def __exit__(self, *_args: Any) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=2.0)


class DaemonHarness:
    def __init__(self, daemon: Path, cli: Path, root: Path) -> None:
        self.daemon = daemon
        self.cli = cli
        self.root = root
        self.socket_path = root / "runtime" / "omacalendar" / "daemon.sock"
        self.database_path = root / "data" / "omacalendar" / "calendar.sqlite3"
        self.process: subprocess.Popen[str] | None = None
        self.logs: list[str] = []

        self.env = os.environ.copy()
        xdg_roots = {
            "XDG_DATA_HOME": root / "data",
            "XDG_CONFIG_HOME": root / "config",
            "XDG_CACHE_HOME": root / "cache",
            "XDG_STATE_HOME": root / "state",
            "XDG_RUNTIME_DIR": root / "runtime",
        }
        for key, directory in xdg_roots.items():
            directory.mkdir(mode=0o700)
            os.chmod(directory, 0o700)
            self.env[key] = str(directory)
        empty_bin = root / "empty-bin"
        empty_bin.mkdir(mode=0o700)
        self.env["PATH"] = str(empty_bin)
        for key in (
            "GNOME_KEYRING_CONTROL",
            "OMACALENDAR_GOOGLE_CLIENT_ID",
            "OMACALENDAR_GOOGLE_CLIENT_SECRET",
        ):
            self.env.pop(key, None)
        # Do not let this isolated test reach a logged-in user's notification or
        # Secret Service session bus or discover secret-tool on the user's PATH.
        # No fake HOME is needed because every path OmaCalendar owns is
        # redirected through XDG.
        self.env["DBUS_SESSION_BUS_ADDRESS"] = (
            f"unix:path={root / 'runtime' / 'no-session-bus'}"
        )

    def start(self) -> None:
        require(self.process is None, "daemon is already running")
        self.process = subprocess.Popen(
            [str(self.daemon)],
            cwd=str(self.root),
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.monotonic() + 10.0
        last_error = "socket not created"
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self._collect_logs()
                self.process = None
                raise ContractError("daemon exited during startup")
            if self.socket_path.exists():
                connection: JsonSocket | None = None
                try:
                    connection = JsonSocket(self.socket_path, timeout=0.25)
                    connection.send("startup-ping", "system.ping")
                    response = connection.receive(timeout=0.5)
                    if response.get("result", {}).get("ok") is True:
                        return
                    last_error = f"startup ping was rejected: {response}"
                except (ConnectionError, OSError, ContractError, socket.timeout) as exc:
                    last_error = str(exc)
                finally:
                    if connection is not None:
                        connection.close()
            time.sleep(0.05)
        self.stop()
        raise ContractError(f"daemon did not become ready: {last_error}")

    def stop(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5.0)
        self._collect_logs()
        self.process = None

    def _collect_logs(self) -> None:
        if self.process is None or self.process.stdout is None or self.process.stderr is None:
            return
        stdout, stderr = self.process.communicate(timeout=1.0)
        combined = "\n".join(part.strip() for part in (stdout, stderr) if part.strip())
        if combined:
            self.logs.append(combined)

    def log_text(self) -> str:
        return "\n--- daemon run ---\n".join(self.logs) or "(no daemon output)"

    def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        completed = subprocess.run(
            [str(self.cli), method, json.dumps(params or {}, separators=(",", ":"))],
            cwd=str(self.root),
            env=self.env,
            capture_output=True,
            text=True,
            timeout=10.0,
            check=False,
        )
        messages = json_objects(completed.stdout) + json_objects(completed.stderr)
        responses = [item for item in messages if "result" in item or "error" in item]
        if completed.returncode != 0 or not responses or "error" in responses[-1]:
            raise ContractError(
                f"CLI call {method} failed (exit {completed.returncode})\n"
                f"stdout: {completed.stdout.strip()}\n"
                f"stderr: {completed.stderr.strip()}"
            )
        return responses[-1]["result"]

    def call_error(
        self, method: str, params: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        completed = subprocess.run(
            [str(self.cli), method, json.dumps(params or {}, separators=(",", ":"))],
            cwd=str(self.root),
            env=self.env,
            capture_output=True,
            text=True,
            timeout=10.0,
            check=False,
        )
        messages = json_objects(completed.stdout) + json_objects(completed.stderr)
        responses = [item for item in messages if "result" in item or "error" in item]
        require(completed.returncode != 0, f"CLI call {method} unexpectedly succeeded")
        require(responses and "error" in responses[-1], f"{method} returned no IPC error")
        error = responses[-1]["error"]
        require(isinstance(error, dict), f"{method} returned a malformed IPC error")
        return error


def assert_private_fields_hidden(
    value: dict[str, Any],
    context: str,
    forbidden_values: tuple[str, ...] = (),
) -> None:
    private_fields = {"remoteId", "href", "etag", "syncToken", "rawPayload"}
    leaked = sorted(private_fields.intersection(value))
    require(not leaked, f"{context} leaked daemon-private fields: {leaked}")
    serialized = json.dumps(value, sort_keys=True)
    for forbidden in forbidden_values:
        require(forbidden not in serialized, f"{context} leaked a private value")


def read_schema(database_path: Path) -> tuple[int, int]:
    require(database_path.is_file(), "fresh startup did not create the database")
    connection = sqlite3.connect(f"{database_path.as_uri()}?mode=ro", uri=True)
    try:
        schema = int(connection.execute("PRAGMA user_version").fetchone()[0])
        row = connection.execute(
            "SELECT value FROM metadata WHERE key='change_revision'"
        ).fetchone()
        require(row is not None, "change_revision metadata is missing")
        return schema, int(row[0])
    finally:
        connection.close()


def outbox_state(database_path: Path) -> tuple[int, int]:
    """Return a compact durable-mutation snapshot for rejection assertions."""
    connection = sqlite3.connect(f"{database_path.as_uri()}?mode=ro", uri=True)
    try:
        row = connection.execute(
            "SELECT COUNT(*), COALESCE(MAX(id), 0) FROM outbox"
        ).fetchone()
        require(row is not None, "outbox table is missing")
        return int(row[0]), int(row[1])
    finally:
        connection.close()


def assert_mode(path: Path, expected: int) -> None:
    actual = stat.S_IMODE(path.stat().st_mode)
    require(actual == expected, f"{path} mode is {oct(actual)}, expected {oct(expected)}")


def event_ids(result: dict[str, Any]) -> set[str]:
    events = result.get("events")
    require(isinstance(events, list), "events.list did not return an events array")
    return {event["id"] for event in events if isinstance(event, dict) and "id" in event}


def assert_ipc_error(
    error: dict[str, Any],
    code: str,
    context: str,
    forbidden_values: tuple[str, ...] = (),
) -> None:
    require(error.get("code") == code, f"{context} returned the wrong error: {error}")
    require(
        isinstance(error.get("message"), str) and error["message"],
        f"{context} returned no safe error message",
    )
    require(error.get("retryable") is False, f"{context} is unexpectedly retryable")
    assert_private_fields_hidden(error, f"{context} error", forbidden_values)


def run_account_lifecycle_contract(
    harness: DaemonHarness,
    methods: set[str],
    initial_revision: int,
) -> None:
    require("accounts.addLocal" not in methods, "device-only account add was advertised")
    local_add_error = harness.call_error(
        "accounts.addLocal", {"displayName": "Second local account"}
    )
    assert_ipc_error(
        local_add_error,
        "method_not_found",
        "unsupported local-account add",
    )

    account_items = assert_account_list_private(
        harness.call("accounts.list"), "initial accounts.list"
    )
    local_account = next(
        (item for item in account_items if item.get("provider") == "local"), None
    )
    require(local_account is not None, "fresh schema did not seed a local account")
    require(
        local_account.get("id") == "local-account"
        and local_account.get("authStatus") == "connected"
        and local_account.get("enabled") is True,
        "seeded device-only account is not connected",
    )

    local_update = harness.call(
        "accounts.update",
        {
            "accountId": local_account["id"],
            "displayName": "Contract device account",
        },
    )
    require(local_update.get("accepted") is True, "local account rename was rejected")
    require(
        revision(local_update, "local account rename") > initial_revision,
        "local account rename did not advance revision",
    )
    renamed_local = local_update.get("account")
    require(isinstance(renamed_local, dict), "local account rename returned no DTO")
    assert_account_fields_hidden(renamed_local, "renamed local account DTO")
    require(
        renamed_local.get("displayName") == "Contract device account",
        "local account displayName update did not persist",
    )
    local_empty_name = harness.call_error(
        "accounts.update", {"accountId": local_account["id"], "displayName": "  "}
    )
    assert_ipc_error(
        local_empty_name,
        "invalid_params",
        "empty local-account displayName",
    )
    reauthorize_error = harness.call_error(
        "accounts.reauthorize", {"accountId": local_account["id"]}
    )
    assert_ipc_error(
        reauthorize_error,
        "unsupported",
        "local-account reauthorization",
    )
    update_error = harness.call_error(
        "accounts.update",
        {
            "accountId": local_account["id"],
            "username": "not-applicable",
            "password": "not-applicable",
        },
    )
    assert_ipc_error(
        update_error,
        "unsupported",
        "local-account credential update",
        ("not-applicable",),
    )
    disconnect_error = harness.call_error(
        "accounts.disconnect",
        {"accountId": local_account["id"], "removeCachedData": True},
    )
    assert_ipc_error(
        disconnect_error,
        "unsupported",
        "local-account disconnect",
    )
    remove_error = harness.call_error(
        "accounts.remove", {"accountId": local_account["id"]}
    )
    assert_ipc_error(remove_error, "unsupported", "local-account remove")
    local_after_rejections = account_by_id(
        assert_account_list_private(
            harness.call("accounts.list"), "accounts.list after local rejections"
        ),
        local_account["id"],
    )
    require(
        local_after_rejections is not None
        and local_after_rejections.get("displayName") == "Contract device account"
        and local_after_rejections.get("authStatus") == "connected"
        and local_after_rejections.get("enabled") is True,
        "rejected local lifecycle operations damaged the device-only account",
    )

    google_error = harness.call_error(
        "accounts.addGoogle", {"displayName": "Fixture Google"}
    )
    assert_ipc_error(
        google_error,
        "google_oauth_start_failed",
        "accounts.addGoogle without OAuth configuration",
    )
    google_alias_error = harness.call_error(
        "google.oauthStart", {"displayName": "Fixture Google alias"}
    )
    assert_ipc_error(
        google_alias_error,
        "google_oauth_start_failed",
        "google.oauthStart compatibility alias",
    )

    with LoopbackProviderFixture() as fixture:
        harness.install_secret_tool(delay=0.2)
        caldav_endpoint = f"{fixture.base_url}/dav/"
        caldav_private = (
            caldav_endpoint,
            "fixture-password",
            "replacement-password",
            "reconnect-password",
            fixture.caldav_etag,
            fixture.caldav_sync_token,
            fixture.caldav_raw_marker,
            "contract-private-caldav-ctag",
        )
        caldav_account, caldav_add_elapsed = harness.call_timed(
            "accounts.addCalDav",
            {
                "endpoint": caldav_endpoint,
                "username": "fixture-user",
                "password": "fixture-password",
                "displayName": "Contract CalDAV",
            },
        )
        require(
            caldav_add_elapsed < 1.0,
            "accounts.addCalDav blocked on secret-tool store",
        )
        require(
            isinstance(caldav_account, dict) and caldav_account.get("id"),
            "CalDAV add did not return an account",
        )
        assert_account_fields_hidden(
            caldav_account,
            "CalDAV add account DTO",
            caldav_private,
        )
        require(
            caldav_account.get("provider") == "caldav"
            and caldav_account.get("principal") == "fixture-user"
            and caldav_account.get("authStatus") == "credential_storage_pending"
            and caldav_account.get("enabled") is True,
            "CalDAV add did not present the pending-credential state",
        )
        caldav_id = caldav_account["id"]
        _ = wait_for_account(
            harness,
            caldav_id,
            lambda value: value.get("authStatus")
            == "credential_storage_pending",
            forbidden_values=caldav_private,
        )
        caldav_connected = wait_for_account(
            harness,
            caldav_id,
            lambda value: value.get("authStatus") == "connected"
            and value.get("enabled") is True,
            timeout=4.0,
            forbidden_values=caldav_private,
        )
        assert_account_fields_hidden(
            caldav_connected,
            "connected CalDAV account DTO",
            caldav_private,
        )
        caldav_calendars = wait_for_calendars(
            harness,
            caldav_id,
            timeout=4.0,
            forbidden_values=caldav_private,
        )
        caldav_calendar = next(
            (
                item
                for item in caldav_calendars
                if item.get("name") == "Contract CalDAV cache"
            ),
            None,
        )
        require(caldav_calendar is not None, "CalDAV discovery cached no calendar")
        caldav_calendar_id = caldav_calendar["id"]
        caldav_event = wait_for_cached_event(
            harness,
            caldav_calendar_id,
            "Cached CalDAV contract event",
            timeout=4.0,
            forbidden_values=caldav_private,
        )
        caldav_event_id = caldav_event["id"]

        initial_range = harness.call(
            "events.list",
            {
                "start": "2026-09-01T00:00:00Z",
                "end": "2026-09-08T00:00:00Z",
                "calendarIds": [caldav_calendar_id],
            },
        )
        require(
            initial_range.get("coverage", {}).get("complete") is True,
            "initial CalDAV hydration did not record durable coverage",
        )
        historical_params = {
            "start": "2010-01-01T00:00:00Z",
            "end": "2011-01-01T00:00:00Z",
            "calendarIds": [caldav_calendar_id],
        }
        historical, historical_elapsed = harness.call_timed(
            "events.list", historical_params
        )
        require(
            historical_elapsed < 1.0,
            "uncovered events.list blocked on CalDAV network hydration",
        )
        require(
            historical.get("coverage", {}).get("complete") is False
            and historical.get("coverage", {}).get("hydrationScheduled") is True,
            "uncovered events.list did not schedule CalDAV hydration",
        )
        hydration_deadline = time.monotonic() + 5.0
        while time.monotonic() < hydration_deadline:
            historical = harness.call("events.list", historical_params)
            if historical.get("coverage", {}).get("complete") is True:
                break
            time.sleep(0.05)
        require(
            historical.get("coverage", {}).get("complete") is True,
            "bounded CalDAV hydration never became covered",
        )
        require(
            any(
                "20100101T000000Z" in body and "20110101T000000Z" in body
                for body in fixture.report_bodies
            ),
            "CalDAV hydration did not issue the requested time-range query",
        )
        reports_before_incremental_check = len(fixture.report_bodies)
        harness.call("sync.account", {"accountId": caldav_id})
        incremental_deadline = time.monotonic() + 5.0
        incremental_requests: list[str] = []
        while time.monotonic() < incremental_deadline:
            incremental_requests = [
                body
                for body in fixture.report_bodies[reports_before_incremental_check:]
                if "<d:sync-collection" in body
            ]
            if incremental_requests:
                break
            time.sleep(0.05)
        require(
            incremental_requests
            and (
                "<d:sync-token>"
                f"{fixture.caldav_sync_token}-next"
                "</d:sync-token>"
            )
            in incremental_requests[0]
            and f"{fixture.caldav_sync_token}-history"
            not in incremental_requests[0],
            "bounded CalDAV hydration incorrectly advanced the collection sync token",
        )
        sync_idle_deadline = time.monotonic() + 5.0
        while time.monotonic() < sync_idle_deadline:
            if (
                harness.call("sync.status", {"accountId": caldav_id}).get("state")
                == "idle"
            ):
                break
            time.sleep(0.05)
        require(
            harness.call("sync.status", {"accountId": caldav_id}).get("state")
            == "idle",
            "CalDAV incremental verification sync did not finish",
        )

        delayed_event = harness.call(
            "events.create",
            {
                "clientMutationId": "contract-caldav-delayed-create",
                "expectedLocalRevision": 0,
                "recurrenceScope": "series",
                "guestNotificationPolicy": "none",
                "event": {
                    "calendarId": caldav_calendar_id,
                    "summary": "CalDAV delayed delete contract event",
                    "startUtc": "2026-09-06T13:00:00Z",
                    "endUtc": "2026-09-06T14:00:00Z",
                    "startTimeZone": "UTC",
                    "endTimeZone": "UTC",
                    "allDay": False,
                    "timeKind": "zoned",
                },
            },
        )

        def operation_state(client_mutation_id: str) -> str | None:
            operations = harness.call("operations.list", {"limit": 100}).get(
                "items", []
            )
            for operation in operations:
                if operation.get("clientMutationId") == client_mutation_id:
                    return operation.get("state")
            return None

        create_deadline = time.monotonic() + 5.0
        while time.monotonic() < create_deadline:
            if operation_state("contract-caldav-delayed-create") == "done":
                break
            time.sleep(0.05)
        require(
            operation_state("contract-caldav-delayed-create") == "done",
            "CalDAV setup create did not drain",
        )
        delayed_event = harness.call(
            "events.get", {"eventId": delayed_event["id"]}
        )
        delete_count_before = fixture.delete_count
        _ = harness.call(
            "events.remove",
            {
                "eventRef": {"eventId": delayed_event["id"]},
                "expectedLocalRevision": delayed_event["localRevision"],
                "clientMutationId": "contract-caldav-delayed-delete",
                "recurrenceScope": "series",
                "guestNotificationPolicy": "none",
            },
        )
        require(
            operation_state("contract-caldav-delayed-delete") == "pending",
            "CalDAV delete ignored its undo delay",
        )
        historical_report_count = sum(
            "20100101T000000Z" in body and "20110101T000000Z" in body
            for body in fixture.report_bodies
        )
        harness.stop()
        harness.start()
        require(
            operation_state("contract-caldav-delayed-delete") == "pending",
            "CalDAV deferred delete was lost across daemon restart",
        )
        restarted_historical = harness.call("events.list", historical_params)
        require(
            restarted_historical.get("coverage", {}).get("complete") is True,
            "completed CalDAV coverage did not survive daemon restart",
        )
        require(
            sum(
                "20100101T000000Z" in body and "20110101T000000Z" in body
                for body in fixture.report_bodies
            )
            == historical_report_count,
            "covered CalDAV range was redundantly rehydrated after restart",
        )
        delete_deadline = time.monotonic() + 14.0
        while time.monotonic() < delete_deadline:
            delete_state = operation_state("contract-caldav-delayed-delete")
            if delete_state == "done" or (
                delete_state is None and fixture.delete_count > delete_count_before
            ):
                break
            time.sleep(0.1)
        final_delete_state = operation_state("contract-caldav-delayed-delete")
        require(
            final_delete_state in (None, "done")
            and fixture.delete_count == delete_count_before + 1,
            "CalDAV delayed delete did not wake and drain without manual sync "
            f"(state={final_delete_state!r}, deletes={fixture.delete_count}, "
            f"before={delete_count_before})",
        )
        assert_ipc_error(
            harness.call_error("events.get", {"eventId": delayed_event["id"]}),
            "not_found",
            "CalDAV event after deferred remote delete",
        )
        require(
            operation_state("contract-caldav-delayed-delete") is None,
            "cascaded CalDAV delete left an active operation",
        )
        assert_ipc_error(
            harness.call_error("events.get", {"eventId": delayed_event["id"]}),
            "not_found",
            "CalDAV event after deferred remote delete",
        )
        require(
            operation_state("contract-caldav-delayed-delete") is None,
            "cascaded CalDAV delete left an active operation",
        )

        caldav_partial_update = harness.call_error(
            "accounts.update",
            {"accountId": caldav_id, "username": "missing-password"},
        )
        assert_ipc_error(
            caldav_partial_update,
            "account_update_failed",
            "partial CalDAV credential update",
            caldav_private,
        )
        caldav_after_rejection = wait_for_account(
            harness,
            caldav_id,
            lambda value: value.get("principal") == "fixture-user",
            forbidden_values=caldav_private,
        )
        require(
            caldav_after_rejection.get("authStatus") == "connected",
            "rejected CalDAV credential update changed authentication state",
        )

        caldav_update, caldav_update_elapsed = harness.call_timed(
            "accounts.update",
            {
                "accountId": caldav_id,
                "username": "replacement-user",
                "password": "replacement-password",
                "displayName": "Contract CalDAV updated",
            },
        )
        require(
            caldav_update_elapsed < 1.0,
            "accounts.update blocked on CalDAV secret operations",
        )
        require(
            caldav_update.get("accepted") is True,
            "CalDAV account update was not accepted",
        )
        caldav_updated = caldav_update.get("account")
        require(
            isinstance(caldav_updated, dict),
            "accounts.update did not return an updated CalDAV account",
        )
        assert_account_fields_hidden(
            caldav_updated,
            "updated CalDAV account DTO",
            caldav_private,
        )
        require(
            caldav_updated.get("principal") == "replacement-user"
            and caldav_updated.get("displayName") == "Contract CalDAV updated"
            and caldav_updated.get("authStatus") == "credential_storage_pending",
            "CalDAV update did not return its pending replacement state",
        )
        require(
            revision(caldav_update, "CalDAV update")
            > revision(local_update, "local update before CalDAV"),
            "CalDAV account update did not advance revision",
        )
        _ = wait_for_account(
            harness,
            caldav_id,
            lambda value: value.get("authStatus") == "connected"
            and value.get("principal") == "replacement-user",
            timeout=4.0,
            forbidden_values=caldav_private,
        )

        caldav_disconnect = harness.call(
            "accounts.disconnect",
            {"accountId": caldav_id, "removeCachedData": True},
        )
        require(
            caldav_disconnect.get("cachedDataRemoved") is False,
            "accounts.disconnect honored a cache-delete override",
        )
        caldav_cached = wait_for_account(
            harness,
            caldav_id,
            lambda value: value.get("authStatus") == "disconnected"
            and value.get("enabled") is False,
            forbidden_values=caldav_private,
        )
        assert_account_fields_hidden(
            caldav_cached,
            "disconnected CalDAV account DTO",
            caldav_private,
        )
        retained_caldav_calendars = wait_for_calendars(
            harness,
            caldav_id,
            forbidden_values=caldav_private,
        )
        require(
            any(item.get("id") == caldav_calendar_id for item in retained_caldav_calendars),
            "CalDAV disconnect discarded its cached calendar",
        )
        retained_caldav_event = harness.call(
            "events.get", {"eventId": caldav_event_id}
        )
        assert_private_fields_hidden(
            retained_caldav_event,
            "retained CalDAV event DTO",
            caldav_private,
        )

        caldav_keep_remove = harness.call(
            "accounts.remove",
            {"accountId": caldav_id, "removeCachedData": False},
        )
        require(
            caldav_keep_remove.get("cachedDataRemoved") is False,
            "accounts.remove did not honor CalDAV cache retention",
        )
        _ = wait_for_account(
            harness,
            caldav_id,
            lambda value: value.get("authStatus") == "disconnected"
            and value.get("enabled") is False,
            forbidden_values=caldav_private,
        )
        require(
            harness.call("events.get", {"eventId": caldav_event_id}).get("summary")
            == "Cached CalDAV contract event",
            "CalDAV remove-with-cache-retention discarded cached events",
        )

        caldav_reconnect = harness.call(
            "accounts.update",
            {
                "accountId": caldav_id,
                "username": "reconnect-user",
                "password": "reconnect-password",
            },
        )
        reconnected_caldav_account = caldav_reconnect.get("account")
        require(
            isinstance(reconnected_caldav_account, dict)
            and reconnected_caldav_account.get("enabled") is True
            and reconnected_caldav_account.get("authStatus")
            == "credential_storage_pending",
            "CalDAV credential update did not reconnect a retained account",
        )
        assert_account_fields_hidden(
            reconnected_caldav_account,
            "reconnecting CalDAV account DTO",
            caldav_private,
        )
        _ = wait_for_account(
            harness,
            caldav_id,
            lambda value: value.get("authStatus") == "connected",
            timeout=4.0,
            forbidden_values=caldav_private,
        )
        caldav_removed = harness.call(
            "accounts.remove",
            {"accountId": caldav_id, "removeCachedData": True},
        )
        require(
            caldav_removed.get("cachedDataRemoved") is True,
            "accounts.remove did not remove cached CalDAV data",
        )
        remaining_after_caldav_remove = assert_account_list_private(
            harness.call("accounts.list"),
            "accounts.list after CalDAV removal",
            caldav_private,
        )
        require(
            account_by_id(remaining_after_caldav_remove, caldav_id) is None,
            "CalDAV account remained after cached-data removal",
        )
        require(
            harness.call("calendars.list", {"accountId": caldav_id}).get("count")
            == 0,
            "CalDAV calendars remained after cached-data removal",
        )
        assert_ipc_error(
            harness.call_error("events.get", {"eventId": caldav_event_id}),
            "not_found",
            "CalDAV cached event after account removal",
            caldav_private,
        )

        alias_password = "alias-fixture-password"
        alias_account, alias_add_elapsed = harness.call_timed(
            "accounts.createCalDav",
            {
                "endpoint": caldav_endpoint,
                "username": "alias-fixture-user",
                "password": alias_password,
                "displayName": "Contract CalDAV alias",
            },
        )
        require(
            alias_add_elapsed < 1.0,
            "accounts.createCalDav alias blocked on credential storage",
        )
        require(
            isinstance(alias_account, dict)
            and alias_account.get("provider") == "caldav"
            and alias_account.get("authStatus") == "credential_storage_pending",
            "accounts.createCalDav did not behave like accounts.addCalDav",
        )
        assert_account_fields_hidden(
            alias_account,
            "accounts.createCalDav alias DTO",
            caldav_private + (alias_password,),
        )
        alias_id = alias_account["id"]
        alias_removed = harness.call("accounts.remove", {"accountId": alias_id})
        require(
            alias_removed.get("cachedDataRemoved") is True,
            "accounts.remove default did not delete alias-created CalDAV cache",
        )
        require(
            account_by_id(
                assert_account_list_private(
                    harness.call("accounts.list"),
                    "accounts.list after alias CalDAV removal",
                    caldav_private + (alias_password,),
                ),
                alias_id,
            )
            is None,
            "alias-created CalDAV account survived removal",
        )

        # ICS intentionally requires HTTPS. Use the local discard port so this
        # validates lifecycle state without trusting a certificate or reaching
        # any external network; addSubscription still creates its cached
        # presentation calendar synchronously.
        ics_url = "https://127.0.0.1:9/fixture.ics"
        ics_private = (
            ics_url,
            "ics-password",
            "ics-replacement-password",
        )
        ics_account_result, ics_add_elapsed = harness.call_timed(
            "accounts.addIcs",
            {
                "url": ics_url,
                "displayName": "Contract ICS",
                "refreshSeconds": 90,
                "username": "ics-user",
                "password": "ics-password",
            },
        )
        require(
            ics_add_elapsed < 1.0,
            "accounts.addIcs blocked on ICS secret operations",
        )
        require(
            isinstance(ics_account_result, dict)
            and isinstance(ics_account_result.get("account"), dict)
            and ics_account_result["account"].get("id"),
            "ICS add did not return an account",
        )
        ics_account = ics_account_result["account"]
        assert_account_fields_hidden(
            ics_account,
            "ICS add account DTO",
            ics_private,
        )
        require(
            ics_account.get("provider") == "ics"
            and ics_account.get("authStatus") == "credential_storage_pending"
            and ics_account.get("enabled") is True,
            "ICS add did not present the pending-credential state",
        )
        require(
            ics_account_result.get("refreshing") is True,
            "ICS add response did not advertise refresh",
        )
        ics_id = ics_account["id"]
        _ = wait_for_account(
            harness,
            ics_id,
            lambda value: value.get("authStatus")
            == "credential_storage_pending",
            forbidden_values=ics_private,
        )
        _ = wait_for_account(
            harness,
            ics_id,
            lambda value: value.get("authStatus") == "connected"
            and value.get("enabled") is True,
            timeout=4.0,
            forbidden_values=ics_private,
        )
        ics_calendars = wait_for_calendars(
            harness,
            ics_id,
            forbidden_values=ics_private,
        )
        require(len(ics_calendars) == 1, "ICS add created an unexpected calendar set")
        ics_calendar_id = ics_calendars[0]["id"]

        ics_partial_update = harness.call_error(
            "accounts.update", {"accountId": ics_id, "password": "missing-user"}
        )
        assert_ipc_error(
            ics_partial_update,
            "account_update_failed",
            "partial ICS credential update",
            ics_private + ("missing-user",),
        )
        ics_display_update = harness.call(
            "accounts.update",
            {"accountId": ics_id, "displayName": "Contract ICS updated"},
        )
        ics_display_account = ics_display_update.get("account")
        require(
            isinstance(ics_display_account, dict)
            and ics_display_account.get("displayName") == "Contract ICS updated"
            and ics_display_account.get("authStatus") == "connected",
            "ICS displayName-only update changed authentication state",
        )
        assert_account_fields_hidden(
            ics_display_account,
            "renamed ICS account DTO",
            ics_private,
        )
        ics_update, ics_update_elapsed = harness.call_timed(
            "accounts.update",
            {
                "accountId": ics_id,
                "username": "ics-replacement",
                "password": "ics-replacement-password",
            },
        )
        require(ics_update_elapsed < 1.0, "ICS credentials update blocked")
        ics_updated = ics_update.get("account")
        require(
            isinstance(ics_updated, dict)
            and ics_updated.get("provider") == "ics"
            and ics_updated.get("authStatus") == "credential_storage_pending",
            "ICS credentials update did not return its pending state",
        )
        assert_account_fields_hidden(
            ics_updated,
            "updated ICS account DTO",
            ics_private,
        )
        _ = wait_for_account(
            harness,
            ics_id,
            lambda value: value.get("authStatus") == "connected",
            timeout=4.0,
            forbidden_values=ics_private,
        )
        ics_cleared = harness.call(
            "accounts.update",
            {"accountId": ics_id, "username": "", "password": ""},
        )
        ics_cleared_account = ics_cleared.get("account")
        require(
            isinstance(ics_cleared_account, dict)
            and ics_cleared_account.get("authStatus") == "connected"
            and ics_cleared_account.get("enabled") is True,
            "ICS credential clearing did not preserve anonymous connectivity",
        )
        assert_account_fields_hidden(
            ics_cleared_account,
            "credential-cleared ICS account DTO",
            ics_private,
        )

        ics_disconnect = harness.call(
            "accounts.disconnect",
            {"accountId": ics_id, "removeCachedData": True},
        )
        require(
            ics_disconnect.get("cachedDataRemoved") is False,
            "ICS disconnect honored a cache-delete override",
        )
        ics_cached = wait_for_account(
            harness,
            ics_id,
            lambda value: value.get("authStatus") == "disconnected"
            and value.get("enabled") is False,
            forbidden_values=ics_private,
        )
        assert_account_fields_hidden(
            ics_cached,
            "disconnected ICS account DTO",
            ics_private,
        )
        retained_ics_calendars = wait_for_calendars(
            harness,
            ics_id,
            forbidden_values=ics_private,
        )
        require(
            retained_ics_calendars[0].get("id") == ics_calendar_id,
            "ICS disconnect discarded its cached calendar",
        )

        ics_keep_remove = harness.call(
            "accounts.remove",
            {"accountId": ics_id, "removeCachedData": False},
        )
        require(
            ics_keep_remove.get("cachedDataRemoved") is False,
            "accounts.remove did not honor ICS cache retention",
        )
        _ = wait_for_account(
            harness,
            ics_id,
            lambda value: value.get("authStatus") == "disconnected"
            and value.get("enabled") is False,
            forbidden_values=ics_private,
        )
        retained_after_remove = wait_for_calendars(
            harness,
            ics_id,
            forbidden_values=ics_private,
        )
        require(
            retained_after_remove[0].get("id") == ics_calendar_id,
            "ICS remove-with-cache-retention discarded its cached calendar",
        )
        ics_reconnect = harness.call(
            "accounts.update",
            {"accountId": ics_id, "username": "", "password": ""},
        )
        reconnected_ics_account = ics_reconnect.get("account")
        require(
            isinstance(reconnected_ics_account, dict)
            and reconnected_ics_account.get("authStatus") == "connected"
            and reconnected_ics_account.get("enabled") is True,
            "ICS anonymous update did not reconnect a retained subscription",
        )
        assert_account_fields_hidden(
            reconnected_ics_account,
            "reconnected ICS account DTO",
            ics_private,
        )
        ics_removed = harness.call("accounts.remove", {"accountId": ics_id})
        require(
            ics_removed.get("cachedDataRemoved") is True,
            "accounts.remove default did not delete cached ICS data",
        )
        final_accounts = assert_account_list_private(
            harness.call("accounts.list"),
            "accounts.list after remote-account removals",
            caldav_private + ics_private + (alias_password,),
        )
        require(
            account_by_id(final_accounts, ics_id) is None,
            "ICS account remained after cached-data removal",
        )
        require(
            harness.call("calendars.list", {"accountId": ics_id}).get("count")
            == 0,
            "ICS calendar remained after cached-data removal",
        )

    final_accounts = assert_account_list_private(
        harness.call("accounts.list"), "final account lifecycle list"
    )
    require(
        len(final_accounts) == 1
        and final_accounts[0].get("id") == local_account["id"]
        and final_accounts[0].get("authStatus") == "connected"
        and final_accounts[0].get("enabled") is True,
        "remote account lifecycle damaged the seeded local account",
    )
    local_calendars = harness.call(
        "calendars.list", {"accountId": local_account["id"]}
    ).get("calendars", [])
    require(
        any(item.get("id") == "local-default" for item in local_calendars),
        "remote account cleanup damaged the default device calendar",
    )


def run_contract(harness: DaemonHarness) -> None:
    require(not harness.database_path.exists(), "test did not begin with fresh state")
    harness.start()

    info = harness.call("system.info")
    require(isinstance(info, dict), "system.info result is not an object")
    require(info.get("server") == "omacalendard", "unexpected server identity")
    require(info.get("protocolMajor") == PROTOCOL_MAJOR, "wrong IPC protocol major")
    require(isinstance(info.get("protocolMinor"), int), "missing protocol minor")
    require(info.get("schemaVersion") == SCHEMA_VERSION, "wrong schema version")
    require(isinstance(info.get("version"), str) and info["version"], "missing version")
    required_methods = {
        "system.info",
        "system.health",
        "system.subscribe",
        "accounts.reauthorize",
        "calendars.list",
        "calendars.upsert",
        "calendars.remove",
        "events.list",
        "events.get",
        "events.create",
        "events.update",
        "events.move",
        "events.remove",
        "events.undo",
    }
    methods = info.get("methods")
    require(isinstance(methods, list), "system.info methods is not an array")
    require(required_methods.issubset(set(methods)), "required IPC methods are absent")
    initial_revision = revision(info, "system.info")

    health = harness.call("system.health")
    require(
        isinstance(health, dict)
        and health.get("ok") is True
        and health.get("database") is True
        and health.get("socket") is True,
        "system.health did not report a healthy database and socket",
    )
    require(revision(health, "system.health") == initial_revision, "health revision drift")

    account_items = harness.call("accounts.list").get("accounts", [])
    local_account = next(
        (item for item in account_items if item.get("provider") == "local"), None
    )
    require(local_account is not None, "fresh schema did not seed a local account")
    reauthorize_error = harness.call_error(
        "accounts.reauthorize", {"accountId": local_account["id"]}
    )
    require(
        reauthorize_error.get("code") == "unsupported",
        "accounts.reauthorize did not reject a non-Google account safely",
    )

    schema, database_revision = read_schema(harness.database_path)
    require(schema == SCHEMA_VERSION, "database PRAGMA user_version is not schema 2")
    require(database_revision == initial_revision, "database/API revision mismatch")
    for owned_directory in (
        harness.root / "data" / "omacalendar",
        harness.root / "config" / "omacalendar",
        harness.root / "cache" / "omacalendar",
        harness.root / "runtime" / "omacalendar",
    ):
        assert_mode(owned_directory, 0o700)
    assert_mode(harness.database_path, 0o600)
    for suffix in ("-wal", "-shm"):
        sidecar = Path(str(harness.database_path) + suffix)
        if sidecar.exists():
            assert_mode(sidecar, 0o600)
    assert_mode(harness.socket_path, 0o600)

    calendars = harness.call("calendars.list")
    calendar_items = calendars.get("calendars", [])
    default_calendar = next(
        (item for item in calendar_items if item.get("id") == "local-default"), None
    )
    require(default_calendar is not None, "fresh schema did not seed local-default")
    require(default_calendar.get("readOnly") is False, "seed calendar is read-only")
    for item in calendar_items:
        assert_private_fields_hidden(item, "calendar DTO")

    contract_calendar = harness.call(
        "calendars.upsert",
        {
            "calendar": {
                "id": "contract-local",
                "accountId": "local-account",
                "name": "IPC Contract",
                "description": "Disposable contract calendar",
                "color": "#89b4fa",
                "timeZone": "UTC",
                "enabled": True,
                "position": 1,
            }
        },
    )
    require(contract_calendar.get("id") == "contract-local", "calendar ID changed")
    require(contract_calendar.get("readOnly") is False, "local calendar is read-only")
    assert_private_fields_hidden(contract_calendar, "created calendar DTO")
    calendar_create_revision = revision(contract_calendar, "calendar create")
    require(
        calendar_create_revision > initial_revision,
        "calendar creation did not advance revision",
    )
    updated_calendar = harness.call(
        "calendars.upsert",
        {
            "calendar": {
                "id": "contract-local",
                "accountId": "local-account",
                "name": "IPC Contract Updated",
                "description": "Disposable contract calendar",
                "color": "#89b4fa",
                "timeZone": "UTC",
                "enabled": True,
                "position": 1,
            }
        },
    )
    require(updated_calendar.get("name") == "IPC Contract Updated", "calendar update failed")
    after_calendar = revision(updated_calendar, "calendar update")
    require(
        after_calendar > calendar_create_revision,
        "calendar update did not advance revision",
    )
    move_calendar = harness.call(
        "calendars.upsert",
        {
            "calendar": {
                "id": "contract-move-target",
                "accountId": "local-account",
                "name": "IPC Move Target",
                "description": "Second device-only calendar",
                "color": "#a6e3a1",
                "timeZone": "UTC",
                "enabled": True,
                "position": 2,
            }
        },
    )
    move_calendar_revision = revision(move_calendar, "move calendar create")
    require(
        move_calendar_revision > after_calendar,
        "move target calendar creation did not advance revision",
    )
    after_calendar = move_calendar_revision

    current_subscription = harness.call(
        "system.subscribe", {"sinceRevision": after_calendar}
    )
    require(current_subscription.get("catchUpRequired") is False, "false catch-up signal")
    require(
        current_subscription.get("topics") == ["*"],
        "omitted subscription topics did not retain wildcard compatibility",
    )
    invalid_subscription = harness.call_error(
        "system.subscribe", {"topics": ["events", "providerSecrets"]}
    )
    require(
        invalid_subscription.get("code") == "invalid_params",
        "unsupported subscription topic was accepted",
    )

    subscriber = JsonSocket(harness.socket_path)
    calendar_only_subscriber = JsonSocket(harness.socket_path)
    unsubscribed_socket = JsonSocket(harness.socket_path)
    try:
        subscriber.send(
            "live-subscription",
            "system.subscribe",
            {"topics": ["events"], "sinceRevision": after_calendar},
        )
        subscribed = subscriber.receive_matching(
            lambda value: value.get("id") == "live-subscription",
            "subscription acknowledgement",
        )
        require(subscribed.get("result", {}).get("subscribed") is True, "subscribe failed")
        require(
            subscribed.get("result", {}).get("topics") == ["events"],
            "subscription acknowledgement did not return accepted topics",
        )
        calendar_only_subscriber.send(
            "calendar-only-subscription",
            "system.subscribe",
            {"topics": ["calendars"], "sinceRevision": after_calendar},
        )
        calendar_only_subscriber.receive_matching(
            lambda value: value.get("id") == "calendar-only-subscription",
            "calendar subscription acknowledgement",
        )

        timed = harness.call(
            "events.create",
            {
                "clientMutationId": "contract-timed-create",
                "expectedLocalRevision": 0,
                "recurrenceScope": "series",
                "guestNotificationPolicy": "none",
                "event": {
                    "calendarId": "contract-local",
                    "summary": "Timed contract event",
                    "description": "Created through IPC 2.0",
                    "location": "Local",
                    "startUtc": "2026-09-01T13:00:00Z",
                    "endUtc": "2026-09-01T14:00:00Z",
                    "startTimeZone": "UTC",
                    "endTimeZone": "UTC",
                    "allDay": False,
                    "timeKind": "zoned",
                },
            },
        )
        timed_revision = revision(timed, "timed create")
        require(timed_revision > after_calendar, "timed create did not advance revision")
        changed = subscriber.receive_matching(
            lambda value: value.get("event") == "events.changed",
            "events.changed notification",
        )
        require(
            changed.get("data", {}).get("revision") == timed_revision,
            "live notification revision did not match the committed mutation",
        )
        calendar_only_subscriber.require_no_message(
            "calendar topic received an event-family notification"
        )
        unsubscribed_socket.require_no_message(
            "pre-subscription connection received a domain notification"
        )
    finally:
        subscriber.close()
        calendar_only_subscriber.close()
        unsubscribed_socket.close()

    timed_id = timed.get("id")
    require(isinstance(timed_id, str) and timed_id, "timed event has no ID")
    require(timed.get("allDay") is False, "timed event became all-day")
    require(timed.get("syncState") == "clean", "local event is not immediately clean")
    assert_private_fields_hidden(timed, "timed event DTO")

    all_day = harness.call(
        "events.create",
        {
            "clientMutationId": "contract-all-day-create",
            "event": {
                "calendarId": "contract-local",
                "summary": "All-day contract event",
                "startDate": "2026-09-03",
                "endDate": "2026-09-05",
                "allDay": True,
                "timeKind": "all_day",
            },
        },
    )
    all_day_id = all_day.get("id")
    require(isinstance(all_day_id, str) and all_day_id, "all-day event has no ID")
    require(all_day.get("allDay") is True, "all-day flag was not retained")
    require(all_day.get("startDate") == "2026-09-03", "all-day start changed")
    require(all_day.get("endDate") == "2026-09-05", "all-day end changed")
    all_day_revision = revision(all_day, "all-day create")
    require(all_day_revision > timed_revision, "all-day create did not advance revision")

    recurring = harness.call(
        "events.create",
        {
            "clientMutationId": "contract-recurring-create",
            "recurrenceScope": "series",
            "guestNotificationPolicy": "none",
            "event": {
                "calendarId": "contract-local",
                "summary": "Recurring contract event",
                "startUtc": "2026-09-05T13:00:00Z",
                "endUtc": "2026-09-05T13:30:00Z",
                "startTimeZone": "UTC",
                "endTimeZone": "UTC",
                "allDay": False,
                "timeKind": "zoned",
                "recurrenceRule": "FREQ=DAILY;COUNT=3",
            },
        },
    )

    def reject_without_durable_mutation(
        method: str,
        params: dict[str, Any],
        expected_code: str,
        context: str,
    ) -> None:
        before_revision = revision(harness.call("system.info"), f"{context} before")
        before_outbox = outbox_state(harness.database_path)
        assert_ipc_error(
            harness.call_error(method, params), expected_code, context
        )
        require(
            revision(harness.call("system.info"), f"{context} after")
            == before_revision,
            f"{context} advanced the database change revision",
        )
        require(
            outbox_state(harness.database_path) == before_outbox,
            f"{context} created or changed a durable outbox mutation",
        )

    reject_without_durable_mutation(
        "events.create",
        {
            "clientMutationId": "contract-invalid-create-occurrence-scope",
            "recurrenceScope": "occurrence",
            "guestNotificationPolicy": "none",
            "event": {
                "calendarId": "contract-local",
                "summary": "Invalid occurrence create",
                "startUtc": "2026-09-05T15:00:00Z",
                "endUtc": "2026-09-05T15:30:00Z",
                "startTimeZone": "UTC",
                "endTimeZone": "UTC",
                "allDay": False,
                "timeKind": "zoned",
            },
        },
        "invalid_recurrence_scope",
        "occurrence-scope create rejection",
    )
    reject_without_durable_mutation(
        "events.create",
        {
            "clientMutationId": "contract-invalid-create-exception",
            "recurrenceScope": "series",
            "guestNotificationPolicy": "none",
            "event": {
                "calendarId": "contract-local",
                "summary": "Invalid exception create",
                "startUtc": "2026-09-05T15:00:00Z",
                "endUtc": "2026-09-05T15:30:00Z",
                "startTimeZone": "UTC",
                "endTimeZone": "UTC",
                "allDay": False,
                "timeKind": "zoned",
                "recurrenceId": "2026-09-05T15:00:00.000Z",
            },
        },
        "invalid_recurrence_creation",
        "client exception create rejection",
    )
    reject_without_durable_mutation(
        "events.update",
        {
            "eventRef": {
                "eventId": timed_id,
                "recurrenceId": "2026-09-01T13:00:00.000Z",
            },
            "expectedLocalRevision": timed["localRevision"],
            "clientMutationId": "contract-invalid-nonrecurring-occurrence-update",
            "recurrenceScope": "occurrence",
            "guestNotificationPolicy": "none",
            "patch": {
                "summary": "Invalid non-recurring occurrence update",
                "recurrenceId": "2026-09-09T13:00:00.000Z",
            },
        },
        "recurrence_scope_requires_recurring_event",
        "non-recurring occurrence update rejection",
    )
    reject_without_durable_mutation(
        "events.update",
        {
            "eventRef": {
                "eventId": timed_id,
                "recurrenceId": "2026-09-01T13:00:00.000Z",
            },
            "expectedLocalRevision": timed["localRevision"],
            "clientMutationId": "contract-invalid-nonrecurring-future-update",
            "recurrenceScope": "future",
            "guestNotificationPolicy": "none",
            "patch": {
                "summary": "Invalid non-recurring future update",
                "recurrenceId": "2026-09-09T13:00:00.000Z",
            },
        },
        "recurrence_scope_requires_recurring_event",
        "non-recurring future update rejection",
    )
    occurrence_draft_identity = harness.call(
        "events.update",
        {
            "eventRef": {
                "eventId": recurring["id"],
                "recurrenceId": "2026-09-05T13:00:00.000Z",
            },
            "expectedLocalRevision": recurring["localRevision"],
            "clientMutationId": "contract-occurrence-reference-authority",
            "recurrenceScope": "occurrence",
            "guestNotificationPolicy": "none",
            "patch": {
                "summary": "Occurrence reference is authoritative",
                "recurrenceId": "2026-09-07T13:00:00.000Z",
            },
        },
    )
    require(
        occurrence_draft_identity.get("recurrenceId")
        == "2026-09-05T13:00:00.000Z",
        "events.update accepted recurrence identity from the editable draft",
    )
    reject_without_durable_mutation(
        "events.update",
        {
            "eventRef": {
                "eventId": occurrence_draft_identity["id"],
                "recurrenceId": "2026-09-06T13:00:00.000Z",
            },
            "expectedLocalRevision": occurrence_draft_identity["localRevision"],
            "clientMutationId": "contract-detached-occurrence-ref-mismatch",
            "recurrenceScope": "occurrence",
            "guestNotificationPolicy": "none",
            "patch": {"summary": "Mismatched detached recurrence identity"},
        },
        "occurrence_identity_mismatch",
        "detached occurrence eventRef mismatch rejection",
    )
    reject_without_durable_mutation(
        "events.remove",
        {
            "eventRef": {
                "eventId": recurring["id"],
                "recurrenceId": "2026-09-06T13:00:00.000Z",
            },
            "expectedLocalRevision": recurring["localRevision"],
            "clientMutationId": "contract-local-unsupported-future-delete",
            "recurrenceScope": "future",
            "guestNotificationPolicy": "none",
        },
        "recurrence_scope_unsupported",
        "unsupported local future delete rejection",
    )

    listed = harness.call(
        "events.list",
        {
            "start": "2026-09-01T00:00:00Z",
            "end": "2026-09-08T00:00:00Z",
            "calendarIds": ["contract-local"],
        },
    )
    require(
        {timed_id, all_day_id}.issubset(event_ids(listed)),
        "timed/all-day events were not readable from their local calendar",
    )
    fetched_timed = harness.call("events.get", {"eventId": timed_id})
    require(fetched_timed.get("summary") == "Timed contract event", "event read failed")

    updated_timed = harness.call(
        "events.update",
        {
            "eventRef": {"eventId": timed_id},
            "expectedLocalRevision": fetched_timed["localRevision"],
            "clientMutationId": "contract-timed-update",
            "recurrenceScope": "series",
            "guestNotificationPolicy": "none",
            "patch": {
                "summary": "Timed contract event updated",
                "location": "Updated locally",
                "endUtc": "2026-09-01T14:30:00Z",
            },
        },
    )
    updated_revision = revision(updated_timed, "timed update")
    require(updated_revision > all_day_revision, "timed update did not advance revision")
    require(updated_timed.get("summary", "").endswith("updated"), "update was lost")
    require(
        updated_timed.get("localRevision") > fetched_timed.get("localRevision"),
        "event-local revision did not advance",
    )

    moved_timed = harness.call(
        "events.move",
        {
            "eventRef": {"eventId": timed_id},
            "targetCalendarId": "contract-move-target",
            "expectedLocalRevision": updated_timed["localRevision"],
            "clientMutationId": "contract-timed-move-out",
            "recurrenceScope": "series",
            "guestNotificationPolicy": "none",
        },
    )
    moved_event = moved_timed.get("event", {})
    require(moved_event.get("id") == timed_id, "same-account move changed event ID")
    require(
        moved_event.get("calendarId") == "contract-move-target",
        "same-account move did not update the canonical calendar",
    )
    require(
        moved_timed.get("state") == "complete"
        and moved_event.get("syncState") == "clean",
        "device-only move did not complete immediately",
    )
    moved_revision = revision(moved_timed, "timed move out")
    require(moved_revision > updated_revision, "move did not advance revision")

    stale_move = harness.call_error(
        "events.move",
        {
            "eventRef": {"eventId": timed_id},
            "targetCalendarId": "contract-local",
            "expectedLocalRevision": updated_timed["localRevision"],
            "clientMutationId": "contract-stale-move",
            "recurrenceScope": "series",
            "guestNotificationPolicy": "none",
        },
    )
    require(
        stale_move.get("code") == "stale_local_revision",
        "events.move did not enforce expectedLocalRevision",
    )
    moved_back = harness.call(
        "events.move",
        {
            "eventRef": {"eventId": timed_id},
            "targetCalendarId": "contract-local",
            "expectedLocalRevision": moved_event["localRevision"],
            "clientMutationId": "contract-timed-move-back",
            "recurrenceScope": "series",
            "guestNotificationPolicy": "none",
        },
    )
    require(
        moved_back.get("event", {}).get("id") == timed_id
        and moved_back.get("event", {}).get("calendarId") == "contract-local",
        "same-account move back did not preserve identity and routing",
    )
    updated_revision = revision(moved_back, "timed move back")
    require(updated_revision > moved_revision, "move back did not advance revision")
    updated_timed = moved_back["event"]

    removed_timed = harness.call(
        "events.remove",
        {
            "eventRef": {"eventId": timed_id},
            "expectedLocalRevision": updated_timed["localRevision"],
            "clientMutationId": "contract-timed-delete",
            "recurrenceScope": "series",
            "guestNotificationPolicy": "none",
        },
    )
    removed_revision = revision(removed_timed, "timed remove")
    require(removed_revision > updated_revision, "timed delete did not advance revision")
    undo_token = removed_timed.get("undoToken")
    require(isinstance(undo_token, str) and undo_token, "delete returned no undoToken")
    undone = harness.call("events.undo", {"undoToken": undo_token})
    undo_revision = revision(undone, "timed undo")
    require(undone.get("undone") is True, "delete undo was not acknowledged")
    require(undo_revision > removed_revision, "undo did not advance revision")
    require(undone.get("event", {}).get("deleted") is False, "undo left tombstone")

    stale_subscription = harness.call(
        "system.subscribe", {"sinceRevision": after_calendar}
    )
    require(
        stale_subscription.get("catchUpRequired") is True
        and revision(stale_subscription, "stale subscribe") == undo_revision,
        "missed-revision recovery was not signaled",
    )

    harness.stop()
    harness.start()

    restarted_info = harness.call("system.info")
    restarted_revision = revision(restarted_info, "restart")
    require(restarted_revision >= undo_revision, "restart regressed change revision")
    schema, persisted_revision = read_schema(harness.database_path)
    require(schema == SCHEMA_VERSION, "restart changed schema version")
    require(persisted_revision == restarted_revision, "restart revision was not persisted")

    restarted_health = harness.call("system.health")
    require(restarted_health.get("ok") is True, "daemon did not reconnect healthy")
    restarted_calendars = harness.call("calendars.list")
    require(
        any(
            item.get("id") == "contract-local"
            for item in restarted_calendars.get("calendars", [])
        ),
        "local calendar did not persist across daemon restart",
    )
    restarted_events = harness.call(
        "events.list",
        {
            "start": "2026-09-01T00:00:00Z",
            "end": "2026-09-08T00:00:00Z",
            "calendarIds": ["contract-local"],
        },
    )
    require(
        {timed_id, all_day_id}.issubset(event_ids(restarted_events)),
        "events did not persist across daemon restart",
    )
    require(
        harness.call("events.get", {"eventId": timed_id}).get("summary")
        == "Timed contract event updated",
        "updated event did not survive restart",
    )
    current_after_restart = harness.call(
        "system.subscribe", {"sinceRevision": restarted_revision}
    )
    require(
        current_after_restart.get("catchUpRequired") is False,
        "restart incorrectly required catch-up at current revision",
    )
    missed_during_restart = harness.call(
        "system.subscribe", {"sinceRevision": after_calendar}
    )
    require(
        missed_during_restart.get("catchUpRequired") is True
        and revision(missed_during_restart, "restart catch-up")
        == restarted_revision,
        "persisted revision did not recover a client's missed changes after restart",
    )

    protocol_client = JsonSocket(harness.socket_path)
    try:
        protocol_client.send(
            "protocol-mismatch", "system.info", protocol_major=PROTOCOL_MAJOR - 1
        )
        mismatch = protocol_client.receive()
        protocol_client.send_value(
            {
                "protocolMajor": PROTOCOL_MAJOR,
                "method": "accounts.list",
                "params": {},
            }
        )
        missing_id = protocol_client.receive()
        protocol_client.send_value(
            {
                "id": "invalid-account-params",
                "protocolMajor": PROTOCOL_MAJOR,
                "method": "accounts.list",
                "params": [],
            }
        )
        invalid_params = protocol_client.receive()
        protocol_client.send_value(
            {
                "id": "unknown-account-method",
                "protocolMajor": PROTOCOL_MAJOR,
                "method": "accounts.addCaldav",
                "params": {},
            }
        )
        unknown_method = protocol_client.receive()
        protocol_client.send_value(["not", "an", "object"])
        non_object = protocol_client.receive()
    finally:
        protocol_client.close()
    require(mismatch.get("id") == "protocol-mismatch", "mismatch response lost ID")
    assert_ipc_error(
        mismatch.get("error", {}),
        "incompatible_protocol",
        "protocol-major mismatch",
    )
    require(
        missing_id.get("id") is None,
        "missing request ID did not preserve a null response ID",
    )
    assert_ipc_error(
        missing_id.get("error", {}),
        "invalid_request",
        "missing request ID",
    )
    require(
        invalid_params.get("id") == "invalid-account-params",
        "invalid-params response lost its request ID",
    )
    assert_ipc_error(
        invalid_params.get("error", {}),
        "invalid_params",
        "non-object account params",
    )
    require(
        unknown_method.get("id") == "unknown-account-method",
        "unknown-method response lost its request ID",
    )
    assert_ipc_error(
        unknown_method.get("error", {}),
        "method_not_found",
        "case-mismatched account method",
    )
    require(
        non_object.get("id") is None,
        "non-object frame did not preserve a null response ID",
    )
    assert_ipc_error(
        non_object.get("error", {}),
        "parse_error",
        "non-object JSON frame",
    )

    fetched_all_day = harness.call("events.get", {"eventId": all_day_id})
    updated_all_day = harness.call(
        "events.update",
        {
            "eventId": all_day_id,
            "expectedLocalRevision": fetched_all_day["localRevision"],
            "clientMutationId": "contract-all-day-update",
            "patch": {
                "summary": "All-day contract event updated",
                "endDate": "2026-09-06",
            },
        },
    )
    require(updated_all_day.get("endDate") == "2026-09-06", "all-day update failed")
    removed_all_day = harness.call(
        "events.remove",
        {
            "eventId": all_day_id,
            "expectedLocalRevision": updated_all_day["localRevision"],
            "clientMutationId": "contract-all-day-delete",
        },
    )
    require(removed_all_day.get("deleted") is True, "all-day delete made no tombstone")
    after_delete = harness.call(
        "events.list",
        {
            "start": "2026-09-01T00:00:00Z",
            "end": "2026-09-08T00:00:00Z",
            "calendarIds": ["contract-local"],
        },
    )
    require(all_day_id not in event_ids(after_delete), "deleted all-day event is listed")
    require(timed_id in event_ids(after_delete), "unrelated timed event disappeared")
    all_day_remove_revision = revision(removed_all_day, "all-day remove")
    require(
        all_day_remove_revision > restarted_revision,
        "post-restart mutation did not advance revision",
    )

    unconfirmed_remove = harness.call_error(
        "calendars.remove", {"calendarId": "contract-local"}
    )
    require(
        unconfirmed_remove.get("code") == "confirmation_required",
        "local calendar removal did not require explicit confirmation",
    )
    protected_remove = harness.call_error(
        "calendars.remove", {"calendarId": "local-default", "confirmed": True}
    )
    require(
        protected_remove.get("code") == "protected_calendar",
        "built-in device calendar was not protected",
    )
    calendar_subscriber = JsonSocket(harness.socket_path)
    try:
        calendar_subscriber.send(
            "calendar-subscription",
            "system.subscribe",
            {
                "topics": ["calendars"],
                "sinceRevision": all_day_remove_revision,
            },
        )
        calendar_subscriber.receive_matching(
            lambda value: value.get("id") == "calendar-subscription",
            "calendar subscription acknowledgement",
        )
        removed_calendar = harness.call(
            "calendars.remove", {"calendarId": "contract-local", "confirmed": True}
        )
        calendar_changed = calendar_subscriber.receive_matching(
            lambda value: value.get("event") == "calendars.changed",
            "calendars.changed notification",
        )
    finally:
        calendar_subscriber.close()
    require(removed_calendar.get("removed") is True, "calendar removal failed")
    require(
        revision(removed_calendar, "calendar remove")
        > all_day_remove_revision,
        "calendar removal did not advance revision",
    )
    require(
        calendar_changed.get("data", {}).get("revision")
        == revision(removed_calendar, "calendar remove notification"),
        "calendar removal broadcast the wrong revision",
    )
    remaining_calendars = harness.call("calendars.list").get("calendars", [])
    require(
        all(item.get("id") != "contract-local" for item in remaining_calendars),
        "removed local calendar is still listed",
    )
    require(
        any(item.get("id") == "local-default" for item in remaining_calendars),
        "removing a custom calendar damaged the built-in calendar",
    )
    cascaded_event = harness.call_error("events.get", {"eventId": timed_id})
    require(
        cascaded_event.get("code") == "not_found",
        f"calendar removal did not expose an event miss: {cascaded_event}",
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    daemon = args.daemon.resolve()
    cli = args.cli.resolve()
    require(daemon.is_file(), f"daemon executable not found: {daemon}")
    require(cli.is_file(), f"CLI executable not found: {cli}")

    def interrupt(signum: int, _frame: Any) -> None:
        raise InterruptedError(f"received signal {signum}")

    previous_sigterm = signal.signal(signal.SIGTERM, interrupt)
    previous_sigint = signal.signal(signal.SIGINT, interrupt)
    try:
        with tempfile.TemporaryDirectory(
            prefix="omacalendar-ipc-contract-"
        ) as directory:
            harness = DaemonHarness(daemon, cli, Path(directory))
            try:
                run_contract(harness)
            except Exception as error:  # noqa: BLE001 - show diagnostics
                harness.stop()
                print(f"daemon contract failed: {error}", file=sys.stderr)
                print(harness.log_text(), file=sys.stderr)
                return 1
            finally:
                harness.stop()
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
        signal.signal(signal.SIGINT, previous_sigint)
    print("daemon IPC 2.0 release-foundation contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
