#!/usr/bin/env python3
"""Isolated end-to-end tests for omacalendar-widgetctl."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
WIDGETCTL = REPOSITORY / "scripts" / "omacalendar-widgetctl"
PLUGIN_ID = "org.omacalendar.widget"
CLOCK_ID = "omarchy.clock"


FAKE_COMMAND = r'''#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

name = Path(sys.argv[0]).name
home = Path(os.environ["HOME"])
omarchy_path = Path(os.environ["OMARCHY_PATH"])

def fail(message):
    print(message, file=sys.stderr)
    raise SystemExit(1)

def shell_document():
    user = home / ".config/omarchy/shell.json"
    source = user if user.exists() else omarchy_path / "config/omarchy/shell.json"
    return json.loads(source.read_text())

def entry_id(value):
    return value if isinstance(value, str) else value.get("id", "") if isinstance(value, dict) else ""

def plugin_listing():
    target = home / ".config/omarchy/plugins/org.omacalendar.widget"
    if not target.is_dir():
        return []
    document = shell_document()
    layout = document.get("bar", {}).get("layout", {})
    enabled = any(
        entry_id(item) == "org.omacalendar.widget"
        for section in ("left", "center", "right")
        for item in layout.get(section, [])
    )
    return [{"id": "org.omacalendar.widget", "enabled": enabled}]

if name == "omarchy":
    if sys.argv[1:] == ["version"]:
        print(os.environ.get("FAKE_OMARCHY_VERSION", "4.0.1-1"))
    elif sys.argv[1:3] == ["plugin", "validate"]:
        plugin_path = Path(sys.argv[3])
        if os.environ.get("FAKE_FAIL_PLUGIN") == "1":
            fail("fixture plugin validation failure")
        if (os.environ.get("FAKE_FAIL_PLUGIN_TARGET_ONLY") == "1"
                and str(plugin_path).endswith("/.config/omarchy/plugins/org.omacalendar.widget")):
            fail("fixture installed plugin validation failure")
    elif sys.argv[1:] == ["plugin", "list", "--json"]:
        print(json.dumps(plugin_listing()))
    else:
        fail("unexpected omarchy arguments: " + repr(sys.argv[1:]))
elif name == "omarchy-shell":
    if os.environ.get("FAKE_FAIL_SHELL") == "1":
        fail("fixture shell failure")
    if sys.argv[1:] not in (["shell", "rescanPlugins"], ["shell", "reloadConfig"]):
        fail("unexpected omarchy-shell arguments: " + repr(sys.argv[1:]))
elif name == "hyprctl":
    if sys.argv[1:] == ["reload"]:
        if os.environ.get("FAKE_FAIL_HYPR_RELOAD") == "1":
            fail("fixture Hyprland reload failure")
    elif sys.argv[1:] == ["configerrors"]:
        if os.environ.get("FAKE_FAIL_HYPR_ERRORS") == "1":
            print("fixture invalid binding")
    else:
        fail("unexpected hyprctl arguments: " + repr(sys.argv[1:]))
elif name == "systemctl":
    if sys.argv[1:] == ["--user", "show", "--property=LoadState", "--value", "omacalendard.socket"]:
        print("loaded")
    elif sys.argv[1:] == ["--user", "enable", "--now", "omacalendard.socket"]:
        pass
    else:
        fail("unexpected systemctl arguments: " + repr(sys.argv[1:]))
else:
    fail("unexpected fixture command: " + name)
'''


class Fixture:
    def __init__(
        self,
        *,
        edge: str = "top",
        section: str = "center",
        user_shell: bool = True,
        user_bindings: bool = True,
        clock: bool = True,
        plugin_id: str = PLUGIN_ID,
    ):
        self.temporary = tempfile.TemporaryDirectory(prefix="omacalendar-widgetctl-test-")
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.omarchy_path = self.root / "omarchy"
        self.bin = self.root / "bin"
        self.source = self.root / "widget-source"
        self.shell = self.home / ".config/omarchy/shell.json"
        self.bindings = self.home / ".config/hypr/bindings.lua"
        self.plugin = self.home / ".config/omarchy/plugins" / PLUGIN_ID
        self.state = self.home / ".local/state/omacalendar/widget-activation/current"
        self.home.mkdir()
        self.bin.mkdir()
        self.source.mkdir()
        defaults = self.omarchy_path / "config/omarchy/shell.json"
        defaults.parent.mkdir(parents=True)
        layout = {
            "left": [{"id": "omarchy.menu"}],
            "center": [{"id": "omarchy.indicators"}, {"id": "omarchy.weather"}],
            "right": [{"id": "omarchy.tray"}],
        }
        if clock:
            layout[section].insert(
                1,
                {
                    "id": CLOCK_ID,
                    "format": "ddd HH:mm",
                    "customClockSetting": 17,
                },
            )
        document = {
            "version": 1,
            "fixtureUnrelated": {"preserve": True},
            "bar": {
                "position": edge,
                "transparent": False,
                "centerAnchor": CLOCK_ID,
                "layout": layout,
            },
            "plugins": [],
        }
        defaults.write_text(json.dumps(document, indent=2) + "\n")
        if user_shell:
            self.shell.parent.mkdir(parents=True)
            self.shell.write_bytes(defaults.read_bytes())
        if user_bindings:
            self.bindings.parent.mkdir(parents=True)
            self.bindings.write_text(
                '-- existing user customization\n'
                'o.bind("SUPER + U", "User action", "fixture-user-action")\n'
            )
        (self.source / "manifest.json").write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "id": plugin_id,
                    "name": "OmaCalendar",
                    "version": "0.1.0-test",
                    "kinds": ["bar-widget"],
                    "entryPoints": {"barWidget": "BarWidget.qml"},
                    "compatibility": {"minimumOmarchy": "4.0.0"},
                }
            )
            + "\n"
        )
        (self.source / "BarWidget.qml").write_text("import QtQuick\nItem {}\n")
        fake = self.bin / "fixture-command"
        fake.write_text(FAKE_COMMAND)
        fake.chmod(0o755)
        for command in ("omarchy", "omarchy-shell", "hyprctl", "systemctl"):
            (self.bin / command).symlink_to(fake)
        self.environment = os.environ.copy()
        self.environment.update(
            HOME=str(self.home),
            OMARCHY_PATH=str(self.omarchy_path),
            PATH=str(self.bin) + os.pathsep + self.environment.get("PATH", ""),
        )
        self.environment.pop("XDG_STATE_HOME", None)

    def close(self) -> None:
        self.temporary.cleanup()

    def run(
        self, command: str, *arguments: str, expected: int = 0, environment: dict[str, str] | None = None
    ) -> tuple[dict, subprocess.CompletedProcess[str]]:
        invocation = [
            sys.executable,
            str(WIDGETCTL),
            command,
            *arguments,
            "--home",
            str(self.home),
            "--omarchy-path",
            str(self.omarchy_path),
        ]
        selected_environment = self.environment.copy()
        if environment:
            selected_environment.update(environment)
        completed = subprocess.run(
            invocation,
            capture_output=True,
            text=True,
            check=False,
            timeout=20,
            env=selected_environment,
        )
        if completed.returncode != expected:
            raise AssertionError(
                f"expected {expected}, got {completed.returncode}\n"
                f"stdout: {completed.stdout}\nstderr: {completed.stderr}"
            )
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise AssertionError(f"widgetctl did not emit JSON: {completed.stdout!r}") from error
        return payload, completed

    def install(self, **kwargs) -> dict:
        payload, _ = self.run("install", "--source", str(self.source), **kwargs)
        return payload

    def restore(self, **kwargs) -> dict:
        payload, _ = self.run("restore", **kwargs)
        return payload

    def shell_document(self) -> dict:
        return json.loads(self.shell.read_text())

    @staticmethod
    def ids(document: dict, section: str) -> list[str]:
        result = []
        for item in document["bar"]["layout"][section]:
            result.append(item if isinstance(item, str) else item["id"])
        return result


class WidgetCtlTest(unittest.TestCase):
    def test_install_and_restore_preserve_unrelated_changes_on_every_bar_edge(self) -> None:
        for edge, section in (
            ("top", "center"),
            ("bottom", "right"),
            ("left", "left"),
            ("right", "center"),
        ):
            with self.subTest(edge=edge, section=section):
                fixture = Fixture(edge=edge, section=section)
                self.addCleanup(fixture.close)
                original_bindings = fixture.bindings.read_bytes()
                payload = fixture.install()
                self.assertTrue(payload["ok"])
                self.assertEqual(payload["status"], "installed")
                self.assertTrue(payload["changed"])
                document = fixture.shell_document()
                self.assertEqual(document["bar"]["position"], edge)
                self.assertEqual(document["bar"]["centerAnchor"], PLUGIN_ID)
                self.assertIn(PLUGIN_ID, fixture.ids(document, section))
                self.assertNotIn(CLOCK_ID, fixture.ids(document, section))
                widget_entry = next(
                    item
                    for item in document["bar"]["layout"][section]
                    if isinstance(item, dict) and item.get("id") == PLUGIN_ID
                )
                self.assertEqual(widget_entry["customClockSetting"], 17)
                self.assertTrue(fixture.plugin.is_dir())
                self.assertTrue((fixture.plugin / ".omacalendar-widgetctl.json").is_file())
                bindings_after_install = fixture.bindings.read_text()
                self.assertIn('hl.unbind("SUPER + CTRL + ALT + D")', bindings_after_install)
                self.assertIn(
                    'omarchy-shell shell toggle org.omacalendar.widget', bindings_after_install
                )

                # Simulate later unrelated customization; restore must make a
                # surgical inverse instead of replacing either whole file.
                document["fixtureUnrelated"]["afterActivation"] = "keep"
                document["bar"]["layout"]["left"].append({"id": "user.custom-widget"})
                fixture.shell.write_text(json.dumps(document, indent=2) + "\n")
                fixture.bindings.write_text(
                    fixture.bindings.read_text()
                    + 'o.bind("SUPER + Y", "Later user action", "fixture-later")\n'
                )
                restored = fixture.restore()
                self.assertEqual(restored["status"], "restored")
                self.assertFalse(fixture.plugin.exists())
                self.assertFalse(fixture.state.exists())
                document = fixture.shell_document()
                self.assertEqual(document["bar"]["position"], edge)
                self.assertEqual(document["bar"]["centerAnchor"], CLOCK_ID)
                self.assertIn(CLOCK_ID, fixture.ids(document, section))
                self.assertNotIn(PLUGIN_ID, fixture.ids(document, section))
                restored_clock = next(
                    item
                    for item in document["bar"]["layout"][section]
                    if isinstance(item, dict) and item.get("id") == CLOCK_ID
                )
                self.assertEqual(restored_clock["customClockSetting"], 17)
                self.assertIn("user.custom-widget", fixture.ids(document, "left"))
                self.assertEqual(document["fixtureUnrelated"]["afterActivation"], "keep")
                bindings = fixture.bindings.read_bytes()
                self.assertTrue(bindings.startswith(original_bindings))
                self.assertIn(b"fixture-later", bindings)
                self.assertNotIn(b"OMACALENDAR WIDGETCTL", bindings)

    def test_dry_run_is_read_only(self) -> None:
        fixture = Fixture()
        self.addCleanup(fixture.close)
        before_shell = fixture.shell.read_bytes()
        before_bindings = fixture.bindings.read_bytes()
        payload, _ = fixture.run(
            "install", "--source", str(fixture.source), "--dry-run"
        )
        self.assertEqual(payload["status"], "would_install")
        self.assertFalse(payload["changed"])
        self.assertEqual(fixture.shell.read_bytes(), before_shell)
        self.assertEqual(fixture.bindings.read_bytes(), before_bindings)
        self.assertFalse(fixture.plugin.exists())
        self.assertFalse(fixture.state.parent.exists())

    def test_source_must_be_trusted_and_have_exact_plugin_id(self) -> None:
        fixture = Fixture(plugin_id="attacker.lookalike")
        self.addCleanup(fixture.close)
        payload, _ = fixture.run(
            "install", "--source", str(fixture.source), expected=3
        )
        self.assertEqual(payload["error"]["code"], "wrong_plugin_id")
        self.assertFalse(fixture.plugin.exists())

        payload, _ = fixture.run(
            "install", "--source", "https://example.test/fake-widget.git", expected=3
        )
        self.assertEqual(payload["error"]["code"], "untrusted_source")

    def test_precondition_failures_do_not_touch_config(self) -> None:
        for name, fixture in (
            ("clock-missing", Fixture(clock=False)),
            ("stale-shortcut", Fixture()),
        ):
            with self.subTest(name=name):
                self.addCleanup(fixture.close)
                if name == "stale-shortcut":
                    fixture.bindings.write_text(
                        fixture.bindings.read_text()
                        + "-- BEGIN OMACALENDAR WIDGETCTL stale\n"
                    )
                before_shell = fixture.shell.read_bytes()
                before_bindings = fixture.bindings.read_bytes()
                payload, _ = fixture.run(
                    "install", "--source", str(fixture.source), expected=5
                )
                self.assertIn(
                    payload["error"]["code"],
                    {"clock_not_found", "stale_shortcut_override"},
                )
                self.assertEqual(fixture.shell.read_bytes(), before_shell)
                self.assertEqual(fixture.bindings.read_bytes(), before_bindings)
                self.assertFalse(fixture.plugin.exists())

    def test_every_post_write_validation_failure_rolls_back_exactly(self) -> None:
        failures = (
            {"FAKE_FAIL_PLUGIN_TARGET_ONLY": "1"},
            {"FAKE_FAIL_SHELL": "1"},
            {"FAKE_FAIL_HYPR_RELOAD": "1"},
            {"FAKE_FAIL_HYPR_ERRORS": "1"},
        )
        for failure in failures:
            with self.subTest(failure=failure):
                fixture = Fixture()
                self.addCleanup(fixture.close)
                before_shell = fixture.shell.read_bytes()
                before_bindings = fixture.bindings.read_bytes()
                payload, _ = fixture.run(
                    "install",
                    "--source",
                    str(fixture.source),
                    expected=4,
                    environment=failure,
                )
                self.assertFalse(payload["ok"])
                self.assertEqual(payload["status"], "rolled_back")
                self.assertEqual(fixture.shell.read_bytes(), before_shell)
                self.assertEqual(fixture.bindings.read_bytes(), before_bindings)
                self.assertFalse(fixture.plugin.exists())
                self.assertFalse(fixture.state.exists())

    def test_failed_restore_restores_the_installed_state(self) -> None:
        fixture = Fixture()
        self.addCleanup(fixture.close)
        fixture.install()
        installed_shell = fixture.shell.read_bytes()
        installed_bindings = fixture.bindings.read_bytes()
        payload, _ = fixture.run(
            "restore", expected=4, environment={"FAKE_FAIL_HYPR_ERRORS": "1"}
        )
        self.assertEqual(payload["status"], "restore_rolled_back")
        self.assertEqual(fixture.shell.read_bytes(), installed_shell)
        self.assertEqual(fixture.bindings.read_bytes(), installed_bindings)
        self.assertTrue(fixture.plugin.is_dir())
        self.assertTrue((fixture.state / "state.json").is_file())
        self.assertEqual(fixture.restore()["status"], "restored")

    def test_restore_removes_files_that_did_not_exist_before_install(self) -> None:
        fixture = Fixture(user_shell=False, user_bindings=False)
        self.addCleanup(fixture.close)
        self.assertFalse(fixture.shell.exists())
        self.assertFalse(fixture.bindings.exists())
        fixture.install()
        self.assertTrue(fixture.shell.exists())
        self.assertTrue(fixture.bindings.exists())
        fixture.restore()
        self.assertFalse(fixture.shell.exists())
        self.assertFalse(fixture.bindings.exists())

    def test_status_reports_health_and_drift(self) -> None:
        fixture = Fixture()
        self.addCleanup(fixture.close)
        payload, _ = fixture.run("status")
        self.assertEqual(payload["status"], "not_installed")
        fixture.install()
        payload, _ = fixture.run("status")
        self.assertEqual(payload["status"], "installed")
        self.assertTrue(payload["details"]["shortcutManaged"])
        document = fixture.shell_document()
        document["bar"]["centerAnchor"] = "user.anchor"
        fixture.shell.write_text(json.dumps(document) + "\n")
        payload, _ = fixture.run("status")
        self.assertEqual(payload["status"], "recovery_required")

    def test_restore_refuses_to_remove_a_plugin_with_changed_ownership(self) -> None:
        fixture = Fixture()
        self.addCleanup(fixture.close)
        fixture.install()
        installed_shell = fixture.shell.read_bytes()
        installed_bindings = fixture.bindings.read_bytes()
        marker_path = fixture.plugin / ".omacalendar-widgetctl.json"
        marker = json.loads(marker_path.read_text())
        marker["activationId"] = "someone-elses-activation"
        marker_path.write_text(json.dumps(marker) + "\n")
        payload, _ = fixture.run("restore", expected=5)
        self.assertEqual(payload["error"]["code"], "plugin_ownership_conflict")
        self.assertEqual(fixture.shell.read_bytes(), installed_shell)
        self.assertEqual(fixture.bindings.read_bytes(), installed_bindings)
        self.assertTrue(fixture.plugin.is_dir())
        self.assertTrue(fixture.state.is_dir())

    def test_install_refuses_an_unmanaged_existing_plugin(self) -> None:
        fixture = Fixture()
        self.addCleanup(fixture.close)
        fixture.plugin.mkdir(parents=True)
        (fixture.plugin / "keep.txt").write_text("unmanaged\n")
        before_shell = fixture.shell.read_bytes()
        payload, _ = fixture.run(
            "install", "--source", str(fixture.source), expected=5
        )
        self.assertEqual(payload["error"]["code"], "plugin_already_installed")
        self.assertEqual((fixture.plugin / "keep.txt").read_text(), "unmanaged\n")
        self.assertEqual(fixture.shell.read_bytes(), before_shell)

    def test_interrupted_activation_journal_is_recovered(self) -> None:
        fixture = Fixture()
        self.addCleanup(fixture.close)
        original_shell = fixture.shell.read_bytes()
        original_bindings = fixture.bindings.read_bytes()
        fixture.install()
        state_file = fixture.state / "state.json"
        state = json.loads(state_file.read_text())
        state["phase"] = "configured"
        state_file.write_text(json.dumps(state) + "\n")
        payload = fixture.restore()
        self.assertEqual(payload["status"], "recovered")
        self.assertEqual(fixture.shell.read_bytes(), original_shell)
        self.assertEqual(fixture.bindings.read_bytes(), original_bindings)
        self.assertFalse(fixture.plugin.exists())
        self.assertFalse(fixture.state.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
