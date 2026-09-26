#!/usr/bin/env python3
"""Run the desktop view benchmark and report its timings.

The benchmark lives in tests/qml-bench/tst_ViewBenchmark.qml. It prints one
OMACALENDAR_BENCH line per measurement; this script selects the event-count
rows to run, collects those lines, prints a table and optionally writes a JSON
report. Timings are informational: nothing is enforced.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import platform
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BENCH_INPUT = ROOT / "tests" / "qml-bench"
BENCH_IMPORTS = ROOT / "tests" / "qml" / "imports"
MARKER = "OMACALENDAR_BENCH "
SIZES = ("500", "5000", "50000")
FUNCTIONS = ("test_view_render_and_update", "test_all_views_live_update")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmltestrunner", required=True,
                        help="path to Qt's qmltestrunner")
    parser.add_argument("--events", nargs="+", choices=SIZES,
                        default=["500", "5000"],
                        help="event-count rows to run (default: 500 5000)")
    parser.add_argument("--output", type=pathlib.Path,
                        help="write the JSON report to this path")
    parser.add_argument("--timeout", type=int, default=1800,
                        help="seconds before the benchmark run is abandoned")
    return parser.parse_args()


def benchmark_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment.update({
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "QT_QPA_PLATFORM": "offscreen",
        "QT_QUICK_CONTROLS_STYLE": "Basic",
        "QT_QUICK_BACKEND": "software",
        "QSG_RHI_BACKEND": "software",
        "QML_DISABLE_DISK_CACHE": "1",
    })
    return environment


def git_revision() -> str:
    try:
        result = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    return result.stdout.strip()


def main() -> int:
    args = parse_args()
    selections = [f"ViewBenchmark::{function}:{size}"
                  for size in args.events for function in FUNCTIONS]
    command = [args.qmltestrunner, "-input", str(BENCH_INPUT),
               "-import", str(BENCH_IMPORTS), *selections]
    completed = subprocess.run(command, capture_output=True, text=True,
                               env=benchmark_environment(), timeout=args.timeout)
    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        sys.stderr.write(output)
        return completed.returncode

    results = []
    for line in output.splitlines():
        marker = line.find(MARKER)
        if marker >= 0:
            results.append(json.loads(line[marker + len(MARKER):]))
    if not results:
        sys.stderr.write(output)
        sys.stderr.write("benchmark produced no measurements\n")
        return 1

    print(f"{'view':<10} {'operation':<9} {'events':>7} {'median ms':>10}  samples")
    for result in results:
        print(f"{result['view']:<10} {result['operation']:<9} "
              f"{result['events']:>7} {result['medianMs']:>10}  "
              f"{result['samplesMs']}")

    if args.output:
        report = {
            "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
            "revision": git_revision(),
            "machine": platform.machine(),
            "platform": platform.platform(),
            "python": platform.python_version(),
            "results": results,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n",
                               encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
