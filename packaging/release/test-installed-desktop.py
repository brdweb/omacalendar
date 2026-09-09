#!/usr/bin/env python3
"""Exercise an unmodified installed app on private Xvfb through real AT-SPI.

Requires Python/PyGObject/Atspi, Xvfb, xauth, ImageMagick, and D-Bus.
Synthetic local data only. Does not qualify physical monitors, Wayland,
real screen-reader interaction, or live provider accounts.
"""
from __future__ import annotations
import argparse
import ctypes
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.parse import quote
from zoneinfo import ZoneInfo


def wait_for(check, description, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError("Timed out: " + description)


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def child(args, root):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
    from test_daemon_contract import DaemonHarness
    processes, checks = [], []
    harness = None
    reference_day = datetime.now().date()
    report = {"scale": args.scale, "version": args.version, "referenceDate": str(reference_day),
              "checks": checks, "findings": []}
    try:
        address = "unix:path=" + str(root / "runtime/a11y.sock")
        bus_log = (args.output / "accessibility.log").open("w")
        processes.append(subprocess.Popen([
            "/usr/bin/dbus-daemon",
            "--config-file=/usr/share/defaults/at-spi2/accessibility.conf",
            "--address=" + address, "--nofork"], stdout=bus_log, stderr=bus_log))
        os.environ["AT_SPI_BUS_ADDRESS"] = address
        wait_for(lambda: (root / "runtime/a11y.sock").exists(), "private accessibility bus")
        processes.append(subprocess.Popen(["/usr/lib/at-spi2-registryd"],
                                         stdout=bus_log, stderr=bus_log))
        import gi
        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi
        profile = root / "profile"
        profile.mkdir(mode=0o700)
        harness = DaemonHarness(args.bin / "omacalendard", args.bin / "omacalendarctl", profile)
        harness.env["DBUS_SESSION_BUS_ADDRESS"] = os.environ["DBUS_SESSION_BUS_ADDRESS"]
        harness.start()
        assert harness.call("system.info")["version"] == args.version
        ui_env = dict(harness.env)
        ui_env["PATH"] = "/usr/bin:/bin"
        app_log = (args.output / "application.log").open("w")

        def launch(*arguments):
            return subprocess.Popen([str(args.bin / "omacalendar"), *arguments],
                                    env=ui_env, stdout=app_log, stderr=app_log)

        app = launch()
        processes.append(app)
        desktop = Atspi.get_desktop(0)

        def walk(node, depth=0):
            if node is None or depth > 14:
                return
            yield node
            for index in range(node.get_child_count()):
                yield from walk(node.get_child_at_index(index), depth + 1)

        def application():
            for index in range(desktop.get_child_count()):
                node = desktop.get_child_at_index(index)
                if node.get_name() == "OmaCalendar":
                    return node
            return None

        def find(name):
            app_node = application()
            match = None
            if app_node is not None:
                for node in walk(app_node):
                    if (node.get_name() == name
                            and node.get_state_set().contains(Atspi.StateType.SHOWING)):
                        # Popup children follow the underlying view. A modal
                        # search result can share its name with that view's event.
                        match = node
            return match

        def dump_visible(label):
            app_node = application()
            rows = []
            if app_node is not None:
                for node in walk(app_node):
                    if node.get_state_set().contains(Atspi.StateType.SHOWING):
                        rows.append({"name": node.get_name(), "role": node.get_role_name()})
            (args.output / (label + ".json")).write_text(json.dumps(rows, indent=2) + "\n")

        def press(name):
            node = wait_for(lambda: find(name), "visible control " + name)
            action = node.get_action_iface()
            assert action is not None, "No accessible action: " + name
            actions = [action.get_action_name(i).lower() for i in range(action.get_n_actions())]
            index = next(i for i, label in enumerate(actions) if label in ("press", "toggle"))
            assert action.do_action(index), "Action failed: " + name
            time.sleep(0.12)

        def fill(name, value):
            node = wait_for(lambda: find(name), "editable control " + name)
            editable = node.get_editable_text_iface()
            assert editable is not None, "Not editable: " + name
            assert editable.set_text_contents(value), "Could not set " + name
            time.sleep(0.05)

        def screenshot(label):
            subprocess.run(["/usr/bin/import", "-window", "root",
                            str(args.output / (label + ".png"))],
                           env=ui_env, check=True, timeout=15)

        def events():
            return harness.call("events.list", {
                "start": str(reference_day - timedelta(days=1)) + "T00:00:00Z",
                "end": str(reference_day + timedelta(days=30)) + "T00:00:00Z",
                "limit": 1000})["events"]

        def saved(title):
            return next((event for event in events() if event["summary"] == title), None)

        def drag(x, y, dx, dy):
            # Explicitly target this harness's authenticated virtual display.
            # Qt's AT-SPI extents use logical pixels; XTest uses device pixels.
            factor = float(args.scale)
            x, y, dx, dy = (value * factor for value in (x, y, dx, dy))
            x11 = ctypes.CDLL("libX11.so.6")
            xtest = ctypes.CDLL("libXtst.so.6")
            x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
            x11.XOpenDisplay.restype = ctypes.c_void_p
            x11.XFlush.argtypes = [ctypes.c_void_p]
            x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
            xtest.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                                  ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
            xtest.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                                  ctypes.c_int, ctypes.c_ulong]
            display = x11.XOpenDisplay(ui_env["DISPLAY"].encode())
            assert display, "Cannot open the private gesture display"
            try:
                xtest.XTestFakeMotionEvent(display, -1, int(x), int(y), 0)
                x11.XFlush(display)
                time.sleep(0.1)
                xtest.XTestFakeButtonEvent(display, 1, 1, 0)
                x11.XFlush(display)
                for step in range(1, 13):
                    xtest.XTestFakeMotionEvent(display, -1, int(x + dx * step / 12),
                                              int(y + dy * step / 12), 0)
                    x11.XFlush(display)
                    time.sleep(0.025)
                xtest.XTestFakeButtonEvent(display, 1, 0, 0)
                x11.XFlush(display)
                time.sleep(0.2)
            finally:
                x11.XCloseDisplay(display)

        def keys(*names):
            x11 = ctypes.CDLL("libX11.so.6")
            xtest = ctypes.CDLL("libXtst.so.6")
            x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
            x11.XOpenDisplay.restype = ctypes.c_void_p
            x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
            x11.XStringToKeysym.restype = ctypes.c_ulong
            x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            x11.XKeysymToKeycode.restype = ctypes.c_uint
            x11.XFlush.argtypes = [ctypes.c_void_p]
            x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
            xtest.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                               ctypes.c_int, ctypes.c_ulong]
            display = x11.XOpenDisplay(ui_env["DISPLAY"].encode())
            assert display, "Cannot open private keyboard display"
            try:
                codes = [x11.XKeysymToKeycode(display, x11.XStringToKeysym(name.encode()))
                         for name in names]
                assert all(codes), "Unknown key"
                for code in codes:
                    xtest.XTestFakeKeyEvent(display, code, 1, 0)
                for code in reversed(codes):
                    xtest.XTestFakeKeyEvent(display, code, 0, 0)
                x11.XFlush(display)
                time.sleep(0.2)
            finally:
                x11.XCloseDisplay(display)

        wait_for(lambda: find("Connected"), "installed app connected to exact daemon")
        checks.append("Installed app launched and connected to exact-version daemon")
        screenshot("01-start")
        created = []
        for index, view in enumerate(("Agenda", "Day", "Week", "Month", "Year")):
            press(view)
            press("New event")
            title = f"Acceptance UI {view} scale {args.scale}"
            fill("Event title", title)
            fill("Start date", str(reference_day))
            fill("End date", str(reference_day))
            fill("Start time", f"{9 + index:02}:00")
            fill("End time", f"{10 + index:02}:00")
            fill("Location", "Synthetic test room")
            press("Add event")
            event = wait_for(lambda: saved(title), "UI create persisted: " + view)
            assert event["location"] == "Synthetic test room" and not event["allDay"]
            zone = ZoneInfo(event["startTimeZone"])
            expected_start = datetime(reference_day.year, reference_day.month, reference_day.day,
                                      9 + index, tzinfo=zone).astimezone(timezone.utc)
            expected_end = datetime(reference_day.year, reference_day.month, reference_day.day,
                                    10 + index, tzinfo=zone).astimezone(timezone.utc)
            assert datetime.fromisoformat(event["startUtc"].replace("Z", "+00:00")) == expected_start
            assert datetime.fromisoformat(event["endUtc"].replace("Z", "+00:00")) == expected_end
            created.append(event)
            checks.append(view + ": toolbar creation with exact title/location/time readback")
            screenshot("view-" + view.lower())

        press("Day")
        gesture_event = created[1]
        node = wait_for(lambda: find(gesture_event["summary"]), "day timeline event")
        rect = node.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
        report["timelineGeometry"] = {"x": rect.x, "y": rect.y, "width": rect.width,
                                      "height": rect.height, "deviceScale": args.scale}
        hour_pixels = rect.height + 2
        original_start = datetime.fromisoformat(gesture_event["startUtc"].replace("Z", "+00:00"))
        drag(rect.x + rect.width / 2, rect.y + rect.height / 2, 0, hour_pixels)
        moved = wait_for(lambda: (value if value["startUtc"] != gesture_event["startUtc"] else None)
                         if (value := saved(gesture_event["summary"])) else None, "timeline drag persisted")
        moved_start = datetime.fromisoformat(moved["startUtc"].replace("Z", "+00:00"))
        assert (moved_start - original_start).total_seconds() == 3600
        checks.append("Real pointer drag shifts a timed event by one hour")
        node = wait_for(lambda: find(gesture_event["summary"]), "moved timeline event")
        rect = node.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
        drag(rect.x + rect.width / 2, rect.y + rect.height - 3, 0, hour_pixels / 2)
        resized = wait_for(lambda: (value if value["endUtc"] != moved["endUtc"] else None)
                           if (value := saved(gesture_event["summary"])) else None, "timeline resize persisted")
        assert (datetime.fromisoformat(resized["endUtc"].replace("Z", "+00:00"))
                - datetime.fromisoformat(moved["endUtc"].replace("Z", "+00:00"))).total_seconds() == 1800
        assert resized["startUtc"] == moved["startUtc"]
        checks.append("Real pointer end-resize adds 30 minutes without changing start")
        screenshot("timeline-gestures")
        keys("Control_L", "z")
        undone = wait_for(lambda: (value if value["endUtc"] == moved["endUtc"] else None)
                          if (value := saved(gesture_event["summary"])) else None,
                          "keyboard undo restores resize")
        assert undone["startUtc"] == moved["startUtc"]
        checks.append("Ctrl+Z undoes the resize and preserves the preceding move")

        event = created[0]
        route = "omacalendar://event/" + quote(event["id"], safe="")
        secondary = launch(route)
        assert secondary.wait(timeout=10) == 0 and app.poll() is None
        wait_for(lambda: find("Save changes"), "deep link opened editor in primary app")
        fill("Event title", "Acceptance UI edited")
        fill("Location", "Updated synthetic room")
        press("Save changes")
        updated = wait_for(lambda: saved("Acceptance UI edited"), "UI edit persisted")
        assert updated["id"] == event["id"] and updated["location"] == "Updated synthetic room"
        checks.append("Single-instance deep link opened and edited existing event")

        press("New event")
        fill("Event title", "Acceptance UI multi-day")
        press("All-day event")
        fill("Start date", str(reference_day + timedelta(days=1)))
        fill("End date", str(reference_day + timedelta(days=3)))
        press("Add event")
        all_day = wait_for(lambda: saved("Acceptance UI multi-day"), "UI all-day create")
        assert all_day["allDay"] and all_day["startDate"] == str(reference_day + timedelta(days=1))
        assert all_day["endDate"] == str(reference_day + timedelta(days=4)), "Inclusive UI end must store exclusive end"
        checks.append("Multi-day all-day UI creation preserves inclusive/exclusive dates")
        keys("Control_L", "f")
        fill("Search all events", "Acceptance UI edited")
        checks.append("Ctrl+F opens the searchable activity dialog")
        time.sleep(1)
        dump_visible("search-accessibility")
        accessible_result = any("Acceptance UI edited" in node.get_name()
                                and node.get_role_name() not in ("text", "entry")
                                and node.get_state_set().contains(Atspi.StateType.SHOWING)
                                for node in walk(application()))
        if not accessible_result:
            report["findings"].append("Search result renders but has no accessible event name")
        screenshot("search-result")
        if accessible_result:
            press("Acceptance UI edited")
            wait_for(lambda: find("Save changes"), "accessible search result opens its event")
            keys("Escape")
            wait_for(lambda: find("Save changes") is None, "Escape closes event editor")
            checks.append("Escape closes the event editor")
            checks.append("Named accessible search result opens the edited event")
        secondary = launch(route)
        assert secondary.wait(timeout=10) == 0
        press("Delete")
        press("Confirm delete")
        wait_for(lambda: saved("Acceptance UI edited") is None, "UI deletion")
        checks.append("UI deletion with confirmation removes selected event")
        before = {e["id"]: e["summary"] for e in events()}
        stop(app)
        harness.stop()
        harness.start()
        app = launch()
        processes.append(app)
        wait_for(lambda: find("Connected"), "app/daemon restart reconnect")
        assert before == {e["id"]: e["summary"] for e in events()}
        checks.append("Installed app and daemon restart preserve remaining synthetic events")
        screenshot("restart")
        assert not any(marker in (args.output / "application.log").read_text()
                       for marker in ("ReferenceError:", "TypeError:", "failed to load component"))
        assert not report["findings"], "; ".join(report["findings"])
        report["status"] = "PASS"
    except Exception as error:
        report.update(status="FAIL", error=type(error).__name__ + ": " + str(error))
        raise
    finally:
        for process in reversed(processes):
            stop(process)
        if harness:
            harness.stop()
        (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--xvfb", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scale", default="1", choices=("1", "1.25", "2"))
    parser.add_argument("--version", default="1.0.0-rc.4")
    parser.add_argument("--child-root", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.bin, args.xvfb, args.output = args.bin.resolve(), args.xvfb.resolve(), args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.child_root:
        child(args, args.child_root)
        return
    identities = {name: hashlib.sha256((args.bin / name).read_bytes()).hexdigest()
                  for name in ("omacalendar", "omacalendard", "omacalendarctl")}
    (args.output / "binaries.json").write_text(json.dumps(identities, indent=2) + "\n")
    with tempfile.TemporaryDirectory(prefix="omacal-ui-") as temporary:
        root, env = Path(temporary), dict(os.environ)
        # Freeze executable bytes so a concurrent development build cannot
        # change the binary exercised during the daemon/app restart checks.
        run_bin = root / "bin"
        run_bin.mkdir(mode=0o700)
        for name, digest in identities.items():
            shutil.copy2(args.bin / name, run_bin / name)
            assert hashlib.sha256((run_bin / name).read_bytes()).hexdigest() == digest
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "GNOME_KEYRING_CONTROL",
                    "AT_SPI_BUS_ADDRESS", "HYPRLAND_INSTANCE_SIGNATURE", "SWAYSOCK", "I3SOCK",
                    "OMACALENDAR_GOOGLE_CLIENT_ID", "OMACALENDAR_GOOGLE_CLIENT_SECRET"):
            env.pop(key, None)
        for key, name in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                          ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state"), ("XDG_RUNTIME_DIR", "runtime")):
            directory = root / name
            directory.mkdir(mode=0o700)
            env[key] = str(directory)
        env.update(QT_QPA_PLATFORM="xcb", QT_QPA_PLATFORMTHEME="", QT_SCALE_FACTOR=args.scale,
                   QT_QUICK_BACKEND="software", QSG_RHI_BACKEND="software", QML_DISABLE_DISK_CACHE="1",
                   QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1", QT_ACCESSIBILITY="1",
                   GDK_BACKEND="x11", XDG_SESSION_TYPE="x11", XDG_CURRENT_DESKTOP="X-Generic")
        env["DBUS_SYSTEM_BUS_ADDRESS"] = "unix:path=" + str(root / "no-system-bus")
        authority, cookie = root / "xauthority", secrets.token_hex(16)
        subprocess.run(["/usr/bin/xauth", "-f", str(authority), "add", ":0", ".", cookie],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        authority.chmod(0o600)
        read_fd, write_fd = os.pipe()
        display_log = (args.output / "xvfb.log").open("w")
        xvfb = subprocess.Popen([str(args.xvfb), "-displayfd", str(write_fd), "-screen", "0", "2880x1800x24",
                                 "-nolisten", "tcp", "-auth", str(authority)], pass_fds=(write_fd,),
                                stdout=display_log, stderr=display_log)
        os.close(write_fd)
        try:
            with os.fdopen(read_fd) as stream:
                number = stream.readline().strip()
            assert number.isdigit(), "Xvfb did not allocate a private display"
            env.update(DISPLAY=":" + number, XAUTHORITY=str(authority))
            subprocess.run(["/usr/bin/xauth", "-f", str(authority), "add", env["DISPLAY"], ".", cookie],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            result = subprocess.run([
                "/usr/bin/dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                "--bin", str(run_bin), "--xvfb", str(args.xvfb), "--output", str(args.output),
                "--scale", args.scale, "--version", args.version, "--child-root", str(root)],
                env=env, timeout=240)
            if result.returncode:
                raise SystemExit(result.returncode)
        finally:
            stop(xvfb)


if __name__ == "__main__":
    main()
