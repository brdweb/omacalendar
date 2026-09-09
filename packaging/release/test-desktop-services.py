#!/usr/bin/env python3
"""Test an exact native candidate with private real Secret Service and D-Bus.

Uses only disposable profiles and loopback providers. The system user session,
keyring, notification server and systemd manager are never accessed. Requires
gnome-keyring-daemon, secret-tool, dbus-daemon, bubblewrap and Python Gio.
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
import shlex
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from test_daemon_contract import ContractError, DaemonHarness, JsonSocket, require

spec = importlib.util.spec_from_file_location(
    "live_provider_smoke", Path(__file__).with_name("live-provider-smoke.py"))
provider = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provider)


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def lookup(env: dict[str, str], account_id: str, kind: str,
           application: str = "omacalendar") -> str | None:
    result = subprocess.run([
        "/usr/bin/secret-tool", "lookup", "application", application,
        "account", account_id, "kind", kind,
    ], env=env, capture_output=True, text=True, timeout=10)
    require(result.returncode in (0, 1), "private keyring lookup failed")
    return result.stdout.rstrip("\r\n") if result.returncode == 0 else None


class PrivateKeyring:
    def __init__(self, env: dict[str, str], root: Path) -> None:
        self.env = env
        self.root = root
        self.process = None
        self.log = (root / "keyring.log").open("w")
        self.password = "disposable-private-keyring-fixture"

    def start(self) -> None:
        self.process = subprocess.Popen([
            "/usr/bin/gnome-keyring-daemon", "--foreground", "--unlock",
            "--components=secrets", "--control-directory", str(self.root / "control"),
        ], env=self.env, stdin=subprocess.PIPE, stdout=self.log, stderr=self.log,
            text=True)
        self.process.stdin.write(self.password)
        self.process.stdin.close()
        self.process.stdin = None
        def ready():
            require(self.process.poll() is None, "private gnome-keyring exited")
            result = subprocess.run([
                "/usr/bin/gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                "--object-path", "/org/freedesktop/DBus", "--method",
                "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.secrets",
            ], env=self.env, capture_output=True, text=True, timeout=3)
            return "true" in result.stdout
        provider.wait_for(ready, "private real Secret Service")

    def stop(self) -> None:
        if self.process is not None:
            stop(self.process)
            self.process = None

    def close(self) -> None:
        self.stop()
        self.log.close()


class NotificationRecorder:
    """A private D-Bus notification server; records protocol, never displays UI."""

    def __init__(self, env):
        from gi.repository import Gio, GLib
        self.GLib = GLib
        self.records = []
        self.fail_next = False
        self.failures = 0
        self.loop = GLib.MainLoop()
        self.connection = Gio.DBusConnection.new_for_address_sync(
            env["DBUS_SESSION_BUS_ADDRESS"],
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None, None)
        info = Gio.DBusNodeInfo.new_for_xml('''<node>
<interface name="org.freedesktop.Notifications">
<method name="GetCapabilities"><arg type="as" direction="out"/></method>
<method name="GetServerInformation"><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/></method>
<method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/></method>
<method name="CloseNotification"><arg type="u" direction="in"/></method>
<signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
<signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
</interface></node>''')
        self.registration = self.connection.register_object(
            "/org/freedesktop/Notifications", info.interfaces[0], self.handle, None, None)
        result = self.connection.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName",
            GLib.Variant("(su)", ("org.freedesktop.Notifications", 0)),
            GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 5000, None)
        require(result.unpack()[0] == 1, "private notification service name was unavailable")
        self.thread = threading.Thread(target=self.loop.run, daemon=True)
        self.thread.start()

    def handle(self, connection, sender, path, interface, method, parameters, invocation):
        if method == "Notify":
            if self.fail_next:
                self.fail_next = False
                self.failures += 1
                invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", "Isolated delivery failure")
                return
            app, replacement, icon, summary, body, actions, hints, timeout = parameters.unpack()
            notification_id = len(self.records) + 1
            self.records.append({"id": notification_id, "app": app, "summary": summary,
                "body": body, "actions": actions, "hints": hints, "timeout": timeout})
            invocation.return_value(self.GLib.Variant("(u)", (notification_id,)))
        elif method == "GetCapabilities":
            invocation.return_value(self.GLib.Variant("(as)", (["actions", "body", "body-markup"],)))
        elif method == "GetServerInformation":
            invocation.return_value(self.GLib.Variant("(ssss)", ("Isolated recorder", "Acceptance", "1", "1.2")))
        elif method == "CloseNotification":
            invocation.return_value(None)

    def action(self, notification_id, action):
        self.connection.emit_signal(None, "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "ActionInvoked",
            self.GLib.Variant("(us)", (notification_id, action)))
        self.connection.flush_sync(None)

    def close(self):
        self.connection.unregister_object(self.registration)
        self.connection.close_sync(None)
        self.loop.quit()
        self.thread.join(timeout=5)


def check_notifications(harness, recorder):
    def reminder(event_id):
        return next((r for r in harness.call("reminders.list")["reminders"]
                     if r["eventId"] == event_id), {})
    def stored_state(event_id):
        # Dismissed reminders intentionally disappear from reminders.list.
        with sqlite3.connect(f"file:{harness.database_path}?mode=ro", uri=True) as db:
            row = db.execute("SELECT state FROM reminder_jobs WHERE event_id=?", (event_id,)).fetchone()
            return row[0] if row else None
    created = []
    for privacy in ("generic", "title_only", "full_details"):
        harness.call("settings.set", {"key": "notificationPrivacy", "value": privacy})
        now = datetime.now(timezone.utc).replace(microsecond=0)
        before = len(recorder.records)
        event_id = harness.call("events.create", {"clientMutationId": "notify-" + privacy,
            "event": {"calendarId": "local-default", "summary": "<b>Fixture & appointment</b>",
                "description": "Secret fixture notes", "location": "<i>Private & room</i>",
                "startUtc": (now + timedelta(minutes=1)).isoformat(),
                "endUtc": (now + timedelta(minutes=61)).isoformat(),
                "startTimeZone": "UTC", "endTimeZone": "UTC", "reminders": [2]}})["id"]
        provider.wait_for(lambda: len(recorder.records) > before, "real D-Bus reminder delivery")
        record = recorder.records[-1]
        provider.wait_for(lambda: reminder(event_id).get("state") == "delivered", "acknowledged reminder delivery")
        require(record["app"] == "OmaCalendar", "notification app identity missing")
        require("snooze5" in record["actions"] and "dismiss" in record["actions"], "reminder actions absent")
        require(record["hints"].get("category") == "x-omacalendar.reminder", "reminder hint missing")
        require("Secret fixture notes" not in json.dumps(record), "notes leaked into notification")
        if privacy == "generic":
            require(record["summary"] == "Calendar reminder" and "Private" not in record["body"],
                    "generic notification privacy failed")
        elif privacy == "title_only":
            require(record["summary"] == "&lt;b&gt;Fixture &amp; appointment&lt;/b&gt;" and not record["body"],
                    "title-only privacy or markup escaping failed")
        else:
            require("&lt;i&gt;Private &amp; room&lt;/i&gt;" in record["body"], "location was not escaped")
        created.append((event_id, record["id"]))
    print("PASS: actual freedesktop Notify calls, acknowledgment, actions, three privacy modes and markup escaping", flush=True)
    recorder.action(created[0][1], "snooze5")
    provider.wait_for(lambda: reminder(created[0][0]).get("state") == "snoozed", "D-Bus snooze action")
    require(reminder(created[0][0]).get("snoozedUntil"), "snooze time absent")
    recorder.action(created[1][1], "dismiss")
    provider.wait_for(lambda: stored_state(created[1][0]) == "dismissed", "D-Bus dismiss action")
    require(not reminder(created[1][0]), "dismissed reminder remained in active list")
    before = len(recorder.records)
    harness.stop()
    harness.start()
    time.sleep(0.5)
    require(len(recorder.records) == before, "restart duplicated acknowledged reminder")
    require(reminder(created[0][0]).get("state") == "snoozed", "restart lost snooze state")
    require(stored_state(created[1][0]) == "dismissed", "restart lost dismissed state")
    print("PASS: D-Bus ActionInvoked snooze/dismiss round trips; restart retains states without duplicate delivery", flush=True)
    recorder.fail_next = True
    now = datetime.now(timezone.utc).replace(microsecond=0)
    event_id = harness.call("events.create", {"clientMutationId": "notify-retry",
        "event": {"calendarId": "local-default", "summary": "Retry fixture",
            "startUtc": (now + timedelta(minutes=1)).isoformat(),
            "endUtc": (now + timedelta(minutes=61)).isoformat(),
            "startTimeZone": "UTC", "endTimeZone": "UTC", "reminders": [2]}})["id"]
    provider.wait_for(lambda: recorder.failures == 1, "injected notification service failure")
    provider.wait_for(lambda: stored_state(event_id) == "pending", "failed notification released for retry")
    harness.stop()
    harness.start()
    provider.wait_for(lambda: stored_state(event_id) == "delivered", "notification retry after restart")
    print("PASS: real D-Bus Notify error releases delivery for retry; daemon restart delivers it successfully", flush=True)


def check_socket_activation(daemon, cli, root, env):
    (root / "activated").mkdir(mode=0o700)
    harness = DaemonHarness(daemon, cli, root / "activated")
    harness.env.update({k: v for k, v in env.items() if not k.startswith("XDG_")})
    harness.socket_path.parent.mkdir(mode=0o700)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(harness.socket_path))
    listener.listen(128)
    harness.socket_path.chmod(0o600)
    original_inode = harness.socket_path.stat().st_ino
    exec_code = ("import os,sys; fd=int(sys.argv[1]); os.dup2(fd,3); os.set_inheritable(3,True); "
        "os.environ.update(LISTEN_PID=str(os.getpid()),LISTEN_FDS='1',LISTEN_FDNAMES='omacalendar'); "
        "os.execve(sys.argv[2],[sys.argv[2]],os.environ)")
    try:
        for attempt in range(2):
            queued = JsonSocket(harness.socket_path)
            queued.send("activated", "system.ping")
            harness.process = subprocess.Popen([
                "/usr/bin/python3", "-c", exec_code, str(listener.fileno()), str(daemon)],
                pass_fds=(listener.fileno(),), env=harness.env, cwd=harness.root,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                require(queued.receive(timeout=10).get("result", {}).get("ok") is True,
                        "activated daemon failed queued connection")
                require(harness.call("system.health").get("ok") is True, "activated daemon health failed")
                require(harness.socket_path.stat().st_ino == original_inode, "daemon replaced inherited socket")
            finally:
                queued.close()
                harness.stop()
            require(harness.socket_path.exists(), "daemon removed externally owned activation socket")
        require((harness.socket_path.stat().st_mode & 0o777) == 0o600, "activation socket mode changed")
        print("PASS: LISTEN_FDS socket handoff, queued client, health, preserved inode/0600 permissions and daemon restart", flush=True)
    finally:
        harness.stop()
        listener.close()


def check_credentials(harness, keyring, radicale, ics, start):
    account_id = harness.call("accounts.addCalDav", {
        "endpoint": radicale.url + "/smoke/", "username": provider.FIXTURE_USER,
        "password": provider.FIXTURE_PASSWORD, "displayName": "Private keyring Radicale",
    })["id"]
    calendar = provider.wait_for(
        lambda: next(iter(provider.calendars(harness, account_id)), None), "CalDAV discovery")
    calendar_id = calendar["id"]
    provider.wait_for(lambda: provider.has_event(harness, calendar_id, start, "Radicale seed"),
                      "CalDAV authenticated initial pull")
    require(lookup(harness.env, account_id, "password") == provider.FIXTURE_PASSWORD,
            "CalDAV did not save its fixture password in the real private keyring")
    print("PASS: CalDAV authenticated discovery and password stored in real Secret Service", flush=True)

    ics_id = harness.call("accounts.addIcs", {
        "url": ics.url, "displayName": "Private keyring ICS", "refreshSeconds": 90,
        "username": provider.FIXTURE_USER, "password": provider.FIXTURE_PASSWORD,
    })["account"]["id"]
    ics_calendar = provider.wait_for(
        lambda: next(iter(provider.calendars(harness, ics_id)), None), "ICS discovery")["id"]
    provider.wait_for(lambda: provider.has_event(harness, ics_calendar, start, "Authenticated ICS 1"),
                      "ICS authenticated initial pull")
    require(lookup(harness.env, ics_id, "ics_password") == provider.FIXTURE_PASSWORD,
            "ICS did not store its password in real Secret Service")
    print("PASS: HTTPS ICS authentication and password stored in real Secret Service", flush=True)
    for aid, kind in ((account_id, "password"), (ics_id, "ics_password")):
        result = subprocess.run(["/usr/bin/secret-tool", "store", "--label=Disposable profile isolation",
            "application", "org.omacalendar.OmaCalendar", "account", aid, "kind", kind],
            input="disposable-other-profile-sentinel\n", env=harness.env, capture_output=True,
            text=True, timeout=10)
        require(result.returncode == 0, "could not store isolated profile sentinel")

    harness.stop()
    keyring.stop()
    keyring.start()
    require(lookup(harness.env, account_id, "password") == provider.FIXTURE_PASSWORD,
            "CalDAV credential lost after Secret Service restart")
    require(lookup(harness.env, ics_id, "ics_password") == provider.FIXTURE_PASSWORD,
            "ICS credential lost after Secret Service restart")
    status, _ = radicale.request("PUT", radicale.collection + "after-keyring.ics",
        provider.payload("After private keyring restart", "private-keyring-restart", start))
    require(status == 201, "could not insert fixture update")
    ics.generation = 2
    harness.start()
    harness.call("sync.account", {"accountId": account_id})
    harness.call("ics.refresh", {"accountId": ics_id})
    provider.wait_for(lambda: provider.has_event(harness, calendar_id, start, "After private keyring restart"),
                      "CalDAV fresh pull after daemon and real keyring restart")
    provider.wait_for(lambda: provider.has_event(harness, ics_calendar, start, "Authenticated ICS 2"),
                      "ICS fresh pull after daemon and real keyring restart")
    print("PASS: real keyring and daemon restarts preserve credentials; both providers fetch new remote events", flush=True)

    ics.password = "rotated-disposable-desktop-fixture"
    ics.generation = 3
    harness.call("ics.refresh", {"accountId": ics_id})
    provider.wait_for(lambda: provider.account(harness, ics_id).get("authStatus") == "error"
        or harness.call("sync.status", {"accountId": ics_id}).get("state") == "error", "ICS revoked credential error")
    require(provider.has_event(harness, ics_calendar, start, "Authenticated ICS 2"), "auth error lost cache")
    harness.call("accounts.update", {"accountId": ics_id,
        "username": provider.FIXTURE_USER, "password": ics.password})
    provider.wait_for(lambda: provider.has_event(harness, ics_calendar, start, "Authenticated ICS 3"), "rotated ICS sync")
    require(lookup(harness.env, ics_id, "ics_password") == ics.password,
            "real keyring did not replace rotated credential")
    print("PASS: ICS credential rejection retains cache; rotation updates keyring and resumes fetch", flush=True)

    for aid, kind, cid, summary in (
        (account_id, "password", calendar_id, "After private keyring restart"),
        (ics_id, "ics_password", ics_calendar, "Authenticated ICS 3"),
    ):
        harness.call("accounts.disconnect", {"accountId": aid})
        provider.wait_for(lambda: provider.account(harness, aid).get("authStatus") == "disconnected", "account disconnect")
        provider.wait_for(lambda: lookup(harness.env, aid, kind) is None, "private credential deletion")
        require(lookup(harness.env, aid, kind, "org.omacalendar.OmaCalendar") ==
                "disposable-other-profile-sentinel", "native disconnect erased another profile's credential")
        require(provider.has_event(harness, cid, start, summary), "disconnect lost cached events")
    harness.stop()
    harness.start()
    for aid in (account_id, ics_id):
        require(provider.account(harness, aid).get("authStatus") == "disconnected", "restart reconnected disconnected account")
    print("PASS: CalDAV and ICS disconnect erase only fixture credentials, retain cache, and persist after restart", flush=True)
    print("PASS: native disconnect preserves real-keyring items with identical account IDs under the Flatpak application namespace", flush=True)

    harness.call("accounts.update", {"accountId": ics_id,
        "username": provider.FIXTURE_USER, "password": ics.password})
    ics.generation = 4
    harness.call("ics.refresh", {"accountId": ics_id})
    provider.wait_for(lambda: provider.has_event(harness, ics_calendar, start, "Authenticated ICS 4"), "ICS reconnect")
    harness.call("accounts.remove", {"accountId": ics_id})
    provider.wait_for(lambda: not provider.account(harness, ics_id), "ICS account removal")
    provider.wait_for(lambda: lookup(harness.env, ics_id, "ics_password") is None, "removed ICS credential absent")
    require(not provider.calendars(harness, ics_id), "account removal retained calendars")
    print("PASS: ICS reconnect and account removal update real Secret Service and calendar state", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--radicale", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    args.work.mkdir(parents=True, exist_ok=True)
    daemon, cli = args.daemon.resolve(), args.cli.resolve()
    require(daemon.is_file() and cli.is_file(), "candidate binaries required")
    print("Candidate daemon SHA256:", hashlib.sha256(daemon.read_bytes()).hexdigest(), flush=True)
    print("Candidate CLI SHA256:", hashlib.sha256(cli.read_bytes()).hexdigest(), flush=True)
    # The only /tmp data is a short directory plus symlink for Unix socket paths.
    # All provider data, certificates, profiles and logs live under --work.
    with tempfile.TemporaryDirectory(prefix="ocal-ds-") as short_dir, ExitStack() as cleanup:
        disk_root = Path(tempfile.mkdtemp(prefix="run-", dir=args.work.resolve()))
        cleanup.callback(shutil.rmtree, disk_root, True)
        root = Path(short_dir) / "w"
        root.symlink_to(disk_root, target_is_directory=True)
        for name in ("home", "data", "config", "state", "cache", "runtime", "control", "daemon"):
            (root / name).mkdir(mode=0o700)
        env = {
            "PATH": "/usr/bin:/bin", "HOME": str(root / "home"), "LANG": "C.UTF-8",
            "XDG_DATA_HOME": str(root / "data"), "XDG_CONFIG_HOME": str(root / "config"),
            "XDG_CACHE_HOME": str(root / "cache"), "XDG_STATE_HOME": str(root / "state"),
            "XDG_RUNTIME_DIR": str(root / "runtime"),
            "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=" + str(root / "no-system-bus"),
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=" + str(root / "runtime" / "bus"),
            "GNOME_KEYRING_CONTROL": str(root / "control"),
        }
        bus_log = (root / "bus.log").open("w")
        cleanup.callback(bus_log.close)
        bus = subprocess.Popen(["/usr/bin/dbus-daemon", "--session", "--nofork",
            "--address=" + env["DBUS_SESSION_BUS_ADDRESS"]], env=env, stdout=bus_log, stderr=bus_log)
        cleanup.callback(stop, bus)
        provider.wait_for(lambda: (root / "runtime" / "bus").exists(), "private D-Bus socket")
        keyring = PrivateKeyring(env, root)
        cleanup.callback(keyring.close)
        keyring.start()
        recorder = NotificationRecorder(env)
        cleanup.callback(recorder.close)
        start = (datetime.now(timezone.utc) + timedelta(days=1)).replace(hour=13, minute=0, second=0, microsecond=0)
        radicale = provider.RadicaleServer(args.radicale.resolve(), root / "radicale")
        cleanup.callback(radicale.stop)
        radicale.start()
        radicale.seed(start)
        ics = provider.AuthenticatedIcsServer(root / "ics", start)
        cleanup.callback(ics.stop)
        harness = DaemonHarness(daemon, cli, root / "daemon")
        harness.env.update({k: v for k, v in env.items() if not k.startswith("XDG_")})
        launcher = root / "daemon" / "launch-with-fixture-trust"
        command = ["/usr/bin/bwrap", "--die-with-parent", "--unshare-pid",
                   "--ro-bind", "/", "/", "--bind", str(disk_root), str(disk_root),
                   "--ro-bind", str(ics.trust_directory), "/etc/ssl/certs",
                   "--proc", "/proc", "--", str(daemon)]
        launcher.write_text("#!/bin/sh\nexec " + shlex.join(command) + "\n")
        launcher.chmod(0o700)
        harness.daemon = launcher
        cleanup.callback(harness.stop)
        try:
            harness.start()
            print("Testing OmaCalendar", harness.call("system.info")["version"], flush=True)
            check_credentials(harness, keyring, radicale, ics, start)
            check_notifications(harness, recorder)
            check_socket_activation(daemon, cli, root, env)
        finally:
            harness.stop()
            (args.work / "daemon.log").write_text(harness.log_text())
        for fixture_secret in (provider.FIXTURE_PASSWORD, ics.password, keyring.password):
            require(fixture_secret not in harness.log_text(), "fixture credential appeared in daemon logs")
            for suffix in ("", "-wal", "-shm"):
                candidate_file = Path(str(harness.database_path) + suffix)
                if candidate_file.exists():
                    require(fixture_secret.encode() not in candidate_file.read_bytes(),
                            "fixture credential appeared in the application database")
        print("PASS: fixture credentials absent from application SQLite/WAL and daemon logs", flush=True)
        print("Private desktop service tests passed; no personal session or provider accounts accessed.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (ContractError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit("desktop service acceptance failed: " + str(error)) from error
