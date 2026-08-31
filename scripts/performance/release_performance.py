#!/usr/bin/env python3
"""Create an isolated 100k-event store and measure OmaCalendar release gates.

The daemon creates the disposable schema, then remains stopped while this script
loads deterministic fixture rows in one transaction. Measurements use one warm
IPC connection to the restarted daemon, so they include the production database,
recurrence, JSON serialization, and socket paths without CLI process startup.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import socket
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import UTC, date, datetime, timedelta
from typing import Any, Iterable


SCHEMA_VERSION = 2
PROTOCOL_MAJOR = 2
AGENDA_GATE_MS = 200.0
SEARCH_GATE_MS = 250.0
WIDGET_GATE_MS = 100.0
REFERENCE_START = datetime(2026, 8, 17, tzinfo=UTC)
REFERENCE_END = REFERENCE_START + timedelta(days=7)
SEARCH_MARKER = "release gate marker"


class HarnessError(RuntimeError):
    """Raised when fixture construction or a measured contract is invalid."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise HarnessError(message)


def iso_utc(value: datetime) -> str:
    return value.astimezone(UTC).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def percentile(values: list[float], fraction: float) -> float:
    require(bool(values), "cannot calculate a percentile without samples")
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


class JsonSocket:
    def __init__(self, path: Path, timeout: float = 10.0) -> None:
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.settimeout(timeout)
        self._socket.connect(str(path))
        self._buffer = b""
        self._next_id = 0

    def close(self) -> None:
        self._socket.close()

    def _receive(self) -> dict[str, Any]:
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                frame = self._buffer[:newline]
                self._buffer = self._buffer[newline + 1 :]
                value = json.loads(frame.decode("utf-8"))
                require(isinstance(value, dict), "daemon response is not an object")
                return value
            chunk = self._socket.recv(65536)
            if not chunk:
                raise HarnessError("daemon closed the benchmark socket")
            self._buffer += chunk

    def call(self, method: str, params: dict[str, Any] | None = None) -> tuple[float, Any]:
        self._next_id += 1
        request_id = f"perf-{self._next_id}"
        request = {
            "id": request_id,
            "protocolMajor": PROTOCOL_MAJOR,
            "method": method,
            "params": params or {},
        }
        payload = json.dumps(request, separators=(",", ":")).encode("utf-8") + b"\n"
        started = time.perf_counter_ns()
        self._socket.sendall(payload)
        while True:
            response = self._receive()
            if response.get("id") != request_id:
                continue
            elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000.0
            if "error" in response:
                raise HarnessError(f"{method} failed: {response['error']}")
            require("result" in response, f"{method} returned no result")
            return elapsed_ms, response["result"]


class IsolatedDaemon:
    def __init__(self, executable: Path, root: Path) -> None:
        self.executable = executable.resolve()
        self.root = root
        self.socket_path = root / "runtime" / "omacalendar" / "daemon.sock"
        self.database_path = root / "data" / "omacalendar" / "calendar.sqlite3"
        self.log_path = root / "daemon.log"
        self.process: subprocess.Popen[bytes] | None = None
        self._log_file: Any = None
        self.env = os.environ.copy()

        for name, relative in {
            "XDG_DATA_HOME": "data",
            "XDG_CONFIG_HOME": "config",
            "XDG_CACHE_HOME": "cache",
            "XDG_STATE_HOME": "state",
            "XDG_RUNTIME_DIR": "runtime",
        }.items():
            directory = root / relative
            directory.mkdir(mode=0o700, parents=True, exist_ok=True)
            os.chmod(directory, 0o700)
            self.env[name] = str(directory)
        empty_bin = root / "empty-bin"
        empty_bin.mkdir(mode=0o700, exist_ok=True)
        self.env["PATH"] = str(empty_bin)
        self.env["DBUS_SESSION_BUS_ADDRESS"] = (
            f"unix:path={root / 'runtime' / 'no-session-bus'}"
        )
        for name in (
            "GNOME_KEYRING_CONTROL",
            "OMACALENDAR_GOOGLE_CLIENT_ID",
            "OMACALENDAR_GOOGLE_CLIENT_SECRET",
        ):
            self.env.pop(name, None)

    def start(self) -> None:
        require(self.process is None, "isolated daemon is already running")
        self._log_file = self.log_path.open("ab")
        self.process = subprocess.Popen(
            [str(self.executable)],
            cwd=self.root,
            env=self.env,
            stdin=subprocess.DEVNULL,
            stdout=self._log_file,
            stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 15.0
        last_error = "socket was not created"
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.stop()
                raise HarnessError(f"daemon exited during startup: {self.logs()}")
            if self.socket_path.exists():
                client: JsonSocket | None = None
                try:
                    client = JsonSocket(self.socket_path, timeout=0.5)
                    _, result = client.call("system.ping")
                    if isinstance(result, dict) and result.get("ok") is True:
                        return
                    last_error = f"startup ping returned {result!r}"
                except (OSError, HarnessError, socket.timeout) as error:
                    last_error = str(error)
                finally:
                    if client is not None:
                        client.close()
            time.sleep(0.05)
        self.stop()
        raise HarnessError(f"daemon did not become ready: {last_error}; {self.logs()}")

    def stop(self) -> None:
        process = self.process
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5.0)
        self.process = None
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None

    def logs(self) -> str:
        if not self.log_path.exists():
            return "(no daemon output)"
        return self.log_path.read_text(encoding="utf-8", errors="replace")[-4000:]


def event_rows(event_count: int, calendar_ids: list[str]) -> Iterable[tuple[Any, ...]]:
    range_start = datetime(2018, 1, 1, tzinfo=UTC)
    range_days = (datetime(2035, 1, 1, tzinfo=UTC) - range_start).days
    created = "2026-08-28T00:00:00.000Z"
    locations = ("Studio", "Office", "Remote", "Auditorium", "")

    for index in range(event_count):
        calendar_id = calendar_ids[index % len(calendar_ids)]
        day = range_start + timedelta(days=(index * 37) % range_days)
        all_day = index % 13 == 0
        multi_day = index % 41 == 0
        marker = f" {SEARCH_MARKER}" if index % 997 == 0 else ""
        summary = f"Calendar fixture {index % 4096}{marker}"
        description = (
            f"Deterministic release benchmark event {index}; "
            f"project cohort {index % 113}."
        )
        location = locations[index % len(locations)]

        if all_day:
            start_utc = ""
            end_utc = ""
            start_date = day.date().isoformat()
            end_date = (day.date() + timedelta(days=3 if multi_day else 1)).isoformat()
            start_timezone = ""
            end_timezone = ""
            time_kind = "all_day"
        else:
            start = day + timedelta(minutes=120 + (index * 47) % 1200)
            duration = timedelta(days=3 if multi_day else 0, minutes=30 * (1 + index % 5))
            end = start + duration
            start_utc = iso_utc(start)
            end_utc = iso_utc(end)
            start_date = ""
            end_date = ""
            floating = index % 19 == 0
            start_timezone = "" if floating else "UTC"
            end_timezone = start_timezone
            time_kind = "floating" if floating else "zoned"

        recurrence_rule = ""
        if not all_day and index % 211 == 0:
            recurrence_rule = "RRULE:FREQ=WEEKLY;INTERVAL=2;UNTIL=20361231T235959Z"

        has_attendees = index % 17 == 0
        organizer = (
            {"displayName": "Fixture Organizer", "email": "organizer@example.test"}
            if has_attendees
            else {}
        )
        attendees = (
            [
                {
                    "email": "owner@example.test",
                    "self": True,
                    "responseStatus": "accepted",
                    "partstat": "ACCEPTED",
                },
                {"email": f"guest-{index % 97}@example.test", "partstat": "NEEDS-ACTION"},
            ]
            if has_attendees
            else []
        )
        sync_state = (
            "conflict"
            if index % 877 == 0
            else "pending" if index % 503 == 0 else "clean"
        )
        deleted = 1 if index % 389 == 0 else 0
        dirty = 1 if sync_state != "clean" else 0

        yield (
            f"perf-event-{index:06d}",
            calendar_id,
            "",
            f"perf-uid-{index:06d}@omacalendar.test",
            "",
            summary,
            description,
            location,
            "",
            "https://meet.google.com/abc-defg-hij" if index % 251 == 0 else "",
            start_utc,
            end_utc,
            start_date,
            end_date,
            start_timezone,
            end_timezone,
            int(all_day),
            time_kind,
            "confirmed",
            "transparent" if index % 29 == 0 else "opaque",
            "private" if index % 31 == 0 else "default",
            recurrence_rule,
            "",
            index % 4,
            json.dumps(organizer, separators=(",", ":")),
            json.dumps(attendees, separators=(",", ":")),
            "[]",
            "",
            "",
            dirty,
            deleted,
            index % 7,
            sync_state,
            created,
            created,
        )


def batched(values: Iterable[tuple[Any, ...]], size: int) -> Iterable[list[tuple[Any, ...]]]:
    batch: list[tuple[Any, ...]] = []
    for value in values:
        batch.append(value)
        if len(batch) == size:
            yield batch
            batch = []
    if batch:
        yield batch


def seed_database(database_path: Path, event_count: int) -> dict[str, Any]:
    started = time.perf_counter()
    connection = sqlite3.connect(database_path)
    connection.execute("PRAGMA foreign_keys=ON")
    connection.execute("PRAGMA synchronous=OFF")
    connection.execute("PRAGMA temp_store=MEMORY")
    schema_version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    require(schema_version == SCHEMA_VERSION, f"expected schema 2, found {schema_version}")

    event_columns = [
        "id", "calendar_id", "remote_id", "uid", "etag", "summary",
        "description", "location", "url", "conference_url", "start_utc",
        "end_utc", "start_date", "end_date", "start_timezone", "end_timezone",
        "all_day", "time_kind", "status", "transparency", "visibility",
        "recurrence_rule", "recurrence_id", "sequence", "organizer_json",
        "attendees_json", "reminders_json", "raw_payload", "raw_format", "dirty",
        "deleted", "local_revision", "sync_state", "created_at", "updated_at",
    ]
    actual_columns = {
        str(row[1]) for row in connection.execute("PRAGMA table_info(events)")
    }
    missing = sorted(set(event_columns) - actual_columns)
    require(not missing, f"schema-2 events table is missing columns: {missing}")

    calendar_ids = ["local-default"] + [f"perf-calendar-{index}" for index in range(1, 8)]
    insert_event = (
        f"INSERT INTO events ({','.join(event_columns)}) VALUES "
        f"({','.join('?' for _ in event_columns)})"
    )
    with connection:
        for index, calendar_id in enumerate(calendar_ids[1:], 1):
            connection.execute(
                """
                INSERT INTO calendars
                  (id,account_id,name,color,timezone,read_only,enabled,position,
                   capabilities_json)
                VALUES (?,?,?,?,?,?,?,?,?)
                """,
                (
                    calendar_id,
                    "local-account",
                    f"Performance {index}",
                    ("#7aa2f7", "#9ece6a", "#bb9af7", "#e0af68")[index % 4],
                    "UTC",
                    1 if index == 7 else 0,
                    1,
                    index,
                    '{"provider":"local","createEvent":true,"updateEvent":true,"removeEvent":true}',
                ),
            )
            connection.execute(
                "INSERT INTO calendar_set_members(set_id,calendar_id,position) VALUES (?,?,?)",
                ("all-calendars", calendar_id, index),
            )
        for batch in batched(event_rows(event_count, calendar_ids), 2500):
            connection.executemany(insert_event, batch)
        connection.execute(
            "UPDATE metadata SET value=? WHERE key='change_revision'", (str(event_count),)
        )

    connection.execute("ANALYZE")
    connection.execute("PRAGMA optimize")
    integrity = str(connection.execute("PRAGMA integrity_check").fetchone()[0])
    stored = int(connection.execute("SELECT count(*) FROM events").fetchone()[0])
    indexed = int(connection.execute("SELECT count(*) FROM events_fts").fetchone()[0])
    connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    connection.close()
    require(integrity == "ok", f"seeded database integrity check failed: {integrity}")
    require(stored == event_count, f"stored {stored} events, expected {event_count}")
    require(indexed == event_count, f"FTS indexed {indexed} events, expected {event_count}")
    os.chmod(database_path, 0o600)
    return {
        "eventCount": stored,
        "ftsRowCount": indexed,
        "calendarCount": len(calendar_ids),
        "seedSeconds": round(time.perf_counter() - started, 3),
    }


def query_plans(database_path: Path, calendar_ids: list[str]) -> dict[str, Any]:
    connection = sqlite3.connect(f"{database_path.as_uri()}?mode=ro", uri=True)
    placeholders = ",".join("?" for _ in calendar_ids)
    agenda_sql = f"""
        SELECT * FROM (
          SELECT e.* FROM events AS e
          WHERE e.deleted=0 AND e.all_day=0
            AND e.recurrence_rule='' AND e.recurrence_id=''
            AND e.end_utc>? AND e.start_utc<?
            AND e.calendar_id IN ({placeholders})
          UNION ALL
          SELECT e.* FROM events AS e
          WHERE e.deleted=0 AND e.all_day=1
            AND e.recurrence_rule='' AND e.recurrence_id=''
            AND e.end_date>? AND e.start_date<?
            AND e.calendar_id IN ({placeholders})
          UNION ALL
          SELECT e.* FROM events AS e
          WHERE e.deleted=0
            AND (e.recurrence_rule<>'' OR e.recurrence_id<>'')
            AND e.calendar_id IN ({placeholders})
        ) AS bounded_events
        ORDER BY all_day DESC, COALESCE(NULLIF(start_utc,''),start_date),id
    """
    agenda_bindings: list[Any] = [
        iso_utc(REFERENCE_START),
        iso_utc(REFERENCE_END),
        *calendar_ids,
        REFERENCE_START.date().isoformat(),
        REFERENCE_END.date().isoformat(),
        *calendar_ids,
        *calendar_ids,
    ]
    search_sql = f"""
        SELECT e.* FROM events e
        JOIN events_fts ON events_fts.event_id=e.id
        WHERE e.deleted=0 AND events_fts MATCH ?
          AND e.calendar_id IN ({placeholders})
        ORDER BY e.start_utc DESC,e.start_date DESC LIMIT ? OFFSET ?
    """
    search_bindings: list[Any] = [f'"{SEARCH_MARKER}"', *calendar_ids, 100, 0]
    agenda = [
        str(row[3])
        for row in connection.execute("EXPLAIN QUERY PLAN " + agenda_sql, agenda_bindings)
    ]
    search = [
        str(row[3])
        for row in connection.execute("EXPLAIN QUERY PLAN " + search_sql, search_bindings)
    ]
    connection.close()
    uses_fts = any("VIRTUAL TABLE INDEX" in step.upper() for step in search)
    require(uses_fts, f"search query did not use the FTS virtual index: {search}")
    unindexed_event_scans = [
        step for step in agenda
        if re.search(r"\bSCAN (?:E|EVENTS)\b", step.upper())
        and "USING INDEX" not in step.upper()
    ]
    uses_range_indexes = not unindexed_event_scans and any(
        "EVENTS_TIMED_RANGE_ACTIVE_INDEX" in step.upper()
        or "EVENTS_TIME_INDEX" in step.upper()
        for step in agenda
    ) and any(
        "EVENTS_ALL_DAY_RANGE_ACTIVE_INDEX" in step.upper()
        or "EVENTS_DATE_INDEX" in step.upper()
        for step in agenda
    ) and any(
        "EVENTS_RECURRENCE_ACTIVE_INDEX" in step.upper()
        for step in agenda
    )
    require(
        uses_range_indexes,
        f"bounded agenda query did not use every event-kind range index: {agenda}",
    )
    return {
        "agenda": agenda,
        "search": search,
        "agendaUsesRangeIndexes": uses_range_indexes,
        "agendaUnindexedEventScans": unindexed_event_scans,
        "searchUsesFtsIndex": uses_fts,
    }


def measure(
    client: JsonSocket,
    method: str,
    params: dict[str, Any],
    warmups: int,
    samples: int,
    gate_ms: float | None,
) -> tuple[dict[str, Any], Any]:
    result: Any = None
    for _ in range(warmups):
        _, result = client.call(method, params)
    durations: list[float] = []
    for _ in range(samples):
        elapsed, result = client.call(method, params)
        durations.append(elapsed)
    p95 = percentile(durations, 0.95)
    summary: dict[str, Any] = {
        "samplesMs": [round(value, 3) for value in durations],
        "medianMs": round(statistics.median(durations), 3),
        "p95Ms": round(p95, 3),
        "maxMs": round(max(durations), 3),
    }
    if gate_ms is not None:
        summary["gateMs"] = gate_ms
        summary["passesGate"] = p95 <= gate_ms
    return summary, result


def file_size(database_path: Path) -> int:
    return sum(
        path.stat().st_size
        for path in (database_path, Path(str(database_path) + "-wal"), Path(str(database_path) + "-shm"))
        if path.exists()
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cpu_model() -> str:
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        match = re.search(r"^model name\s*:\s*(.+)$", cpuinfo.read_text(), re.MULTILINE)
        if match:
            return match.group(1).strip()
    return platform.processor() or "unknown"


def git_metadata(source_root: Path) -> dict[str, Any]:
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=source_root, text=True,
            capture_output=True, timeout=3.0, check=True,
        ).stdout.strip()
        dirty = bool(
            subprocess.run(
                ["git", "status", "--porcelain", "--untracked-files=normal"],
                cwd=source_root, text=True, capture_output=True, timeout=3.0,
                check=True,
            ).stdout.strip()
        )
        return {"commit": commit, "dirty": dirty}
    except (OSError, subprocess.SubprocessError):
        return {"commit": "unknown", "dirty": None}


def benchmark(args: argparse.Namespace, root: Path) -> dict[str, Any]:
    daemon = IsolatedDaemon(args.daemon, root)
    daemon.start()
    daemon.stop()
    require(daemon.database_path.is_file(), "daemon did not initialize schema 2")

    seed = seed_database(daemon.database_path, args.events)
    calendar_ids = ["local-default"] + [f"perf-calendar-{index}" for index in range(1, 8)]
    plans = query_plans(daemon.database_path, calendar_ids)

    daemon.start()
    client = JsonSocket(daemon.socket_path)
    try:
        _, system_info = client.call("system.info")
        require(
            isinstance(system_info, dict)
            and system_info.get("schemaVersion") == SCHEMA_VERSION,
            "benchmark daemon does not report schema 2",
        )
        agenda_params = {
            "start": iso_utc(REFERENCE_START),
            "end": iso_utc(REFERENCE_END),
            "calendarIds": calendar_ids,
            "limit": 500,
        }
        search_params = {
            "query": SEARCH_MARKER,
            "calendarIds": calendar_ids,
            "limit": 100,
            "offset": 0,
        }
        widget_params = {
            "start": iso_utc(REFERENCE_START),
            "end": iso_utc(REFERENCE_END),
            "calendarSetId": "all-calendars",
        }

        agenda, agenda_result = measure(
            client, "events.list", agenda_params, args.warmups, args.samples,
            AGENDA_GATE_MS,
        )
        require(
            isinstance(agenda_result, dict)
            and int(agenda_result.get("total", 0)) > 0,
            "bounded agenda query returned no representative events",
        )
        agenda["resultCount"] = int(agenda_result["total"])

        search, search_result = measure(
            client, "events.search", search_params, args.warmups, args.samples,
            SEARCH_GATE_MS,
        )
        require(
            isinstance(search_result, dict)
            and int(search_result.get("total", 0)) > 0,
            "indexed search returned no marker events",
        )
        search["resultCount"] = int(search_result["total"])

        widget, widget_result = measure(
            client, "widget.snapshot", widget_params, args.warmups, args.samples,
            WIDGET_GATE_MS,
        )
        require(
            isinstance(widget_result, dict)
            and isinstance(widget_result.get("events"), list)
            and len(widget_result["events"]) > 0,
            "widget snapshot returned no representative events",
        )
        widget["resultCount"] = len(widget_result["events"])
        invitations = widget_result.get("invitations")
        require(
            isinstance(invitations, list) and len(invitations) <= 100,
            "widget snapshot did not enforce its 100-invitation compact bound",
        )
        invitation_count = widget_result.get("invitationCount")
        require(
            isinstance(invitation_count, int)
            and invitation_count >= len(invitations),
            "widget snapshot returned invalid invitation count metadata",
        )
        require(
            widget_result.get("invitationsTruncated")
            is (invitation_count > len(invitations)),
            "widget invitation truncation metadata disagrees with its total",
        )
        private_event_fields = {"remoteId", "etag", "rawPayload", "rawFormat"}
        compact_events = [*widget_result["events"], *invitations]
        for key in ("currentEvent", "upNext"):
            item = widget_result.get(key)
            require(isinstance(item, dict), f"widget snapshot {key} is not an object")
            if item:
                compact_events.append(item)
        for item in compact_events:
            require(
                isinstance(item, dict)
                and private_event_fields.isdisjoint(item),
                "widget snapshot exposed daemon-private provider event fields",
            )
        widget["invitationResultCount"] = len(invitations)
        widget["invitationTotal"] = invitation_count
        widget["encodedResultBytes"] = len(
            json.dumps(widget_result, separators=(",", ":")).encode("utf-8")
        )
        require(
            widget["encodedResultBytes"] < 1024 * 1024,
            "widget snapshot result exceeded the one-MiB IPC frame budget",
        )
        widget_revision = int(widget_result.get("revision", -1))
        require(widget_revision >= 0, "widget snapshot omitted its revision")
        unchanged, unchanged_result = measure(
            client,
            "widget.snapshot",
            {"sinceRevision": widget_revision},
            args.warmups,
            args.samples,
            None,
        )
        require(
            isinstance(unchanged_result, dict)
            and unchanged_result.get("unchanged") is True,
            "revision-based widget snapshot did not take the unchanged fast path",
        )
    finally:
        client.close()
        daemon.stop()

    gates = {
        "agendaP95": bool(agenda["passesGate"]),
        "searchP95": bool(search["passesGate"]),
        "widgetP95": bool(widget["passesGate"]),
    }
    report = {
        "formatVersion": 1,
        "hardwareGateEnforced": bool(args.enforce_gates),
        "allPerformanceGatesPassed": all(gates.values()),
        "gates": gates,
        "dataset": {
            **seed,
            "schemaVersion": SCHEMA_VERSION,
            "databaseBytes": file_size(daemon.database_path),
            "referenceRange": {
                "start": iso_utc(REFERENCE_START),
                "end": iso_utc(REFERENCE_END),
            },
            "searchMarker": SEARCH_MARKER,
        },
        "measurements": {
            "boundedAgenda": agenda,
            "indexedSearch": search,
            "widgetSnapshot": widget,
            "widgetUnchangedSnapshot": unchanged,
        },
        "queryPlans": plans,
        "build": {
            "label": args.build_label,
            "daemon": str(args.daemon.resolve()),
            "daemonSha256": sha256_file(args.daemon),
            "systemInfo": system_info,
            "source": git_metadata(args.source_root),
        },
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "cpu": cpu_model(),
            "logicalCpuCount": os.cpu_count(),
            "python": platform.python_version(),
            "sqlite": sqlite3.sqlite_version,
        },
    }
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", required=True, type=Path,
                        help="Path to the built omacalendard executable")
    parser.add_argument("--events", type=int, default=100_000,
                        help="Number of stored events to seed (default: 100000)")
    parser.add_argument("--warmups", type=int, default=3,
                        help="Warm calls before each measurement")
    parser.add_argument("--samples", type=int, default=11,
                        help="Measured calls used for median/p95/max")
    parser.add_argument("--enforce-gates", action="store_true",
                        help="Fail when a measured p95 exceeds a release gate")
    parser.add_argument("--output", type=Path,
                        help="Optional path for the JSON report")
    parser.add_argument("--build-label", default="unspecified",
                        help="Human-readable build description stored in the report")
    parser.add_argument(
        "--source-root", type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Source checkout used for git metadata",
    )
    args = parser.parse_args()
    require(args.daemon.is_file(), f"daemon executable not found: {args.daemon}")
    require(args.events >= 1000, "--events must be at least 1000")
    require(args.warmups >= 0, "--warmups cannot be negative")
    require(args.samples >= 3, "--samples must be at least 3")
    return args


def main() -> int:
    try:
        args = parse_args()
        with tempfile.TemporaryDirectory(prefix="omacalendar-release-performance-") as value:
            report = benchmark(args, Path(value))
        encoded = json.dumps(report, indent=2, sort_keys=True)
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded + "\n", encoding="utf-8")
        print(encoded)
        if args.enforce_gates and not report["allPerformanceGatesPassed"]:
            failed = ", ".join(
                name for name, passed in report["gates"].items() if not passed
            )
            print(f"release performance gates failed: {failed}", file=sys.stderr)
            return 1
        return 0
    except (HarnessError, OSError, sqlite3.Error, subprocess.SubprocessError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, indent=2), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
