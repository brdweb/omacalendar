# Release performance gate

OmaCalendar's release harness creates a disposable schema-3 database through the
built daemon, stops the daemon, inserts 100,000 deterministic events in one SQLite
transaction, and restarts the daemon for warm IPC measurements. It then adds
20,000 finished historical recurring series, restarts the daemon, and repeats the
seven-day `events.list` measurement. It never uses the current user's database,
socket, accounts, keyring, notification bus, or XDG paths.

The base dataset spans 2018–2034 across eight calendars and includes timed,
all-day, multi-day, floating, zoned, recurring, invitation, pending, conflict,
read-only, and deleted records. The additional historical series ended between
2000 and 2011; their COUNT-based UTC/date bounds are stored just as schema 3
stores them for daemon writes and upgrades. FTS5 receives all rows through the
production triggers. The report verifies database integrity, event/FTS row
counts, and that SQLite selects the bounded-series indexes.

Measured gates use the nearest-rank p95 after warm-up:

- Bounded seven-day `events.list`: at most 200 ms.
- History-size regression: after adding 20,000 finished series, seven-day
  `events.list` p95 stays within the larger of 1.25× baseline or baseline
  plus 10 ms (allowing for host noise).
- Indexed `events.search`: at most 250 ms.
- Full warm `widget.snapshot`: at most 100 ms.

The report includes `boundedAgendaWithoutHistory` and `boundedAgenda` samples,
their p95 ratio, and the historical series count. The two calls must return the
same total: finished series cannot appear in the contemporary week. The
relative p95 gate above detects history-size sensitivity in addition to the
absolute 200 ms limit. Use `--historical-series 0` to skip the comparison or
increase it to stress larger archives. Warm samples exercise the per-series
expansion cache; SQLite range indexes exclude finished series before hydration
and expansion.

The report also measures the widget's revision-based unchanged fast path, but that
shortcut is not substituted for the full-snapshot gate.

## Reference Omarchy hardware run

Use a clean Release build and keep the machine on AC power with its normal release
CPU governor. Close compilers, games, VM workloads, and other sustained CPU or disk
jobs. The daemon started by the harness is isolated, so a normally installed user
daemon does not need to be stopped.

```sh
cmake -S . -B build-performance -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOMACALENDAR_BUILD_TESTS=ON
cmake --build build-performance --target omacalendard
python3 scripts/performance/release_performance.py \
  --daemon build-performance/omacalendard \
  --events 100000 \
  --historical-series 20000 \
  --warmups 5 \
  --samples 15 \
  --enforce-gates \
  --build-label "Release/reference-Omarchy" \
  --output build-performance/release-performance.json
```

Archive `release-performance.json` with the acceptance record. It contains every
sample (including the before/after history comparison), median/p95/max, query
plans, database size, seed time, daemon hash and reported protocol/version,
source revision/dirty state, CPU, kernel, Python, and SQLite versions. A dirty
source tree or non-Release build should not be used as a final release result
even though the script records it.

## CTest modes

The default deterministic smoke test uses the same 100,000-event base dataset
and 20,000 finished series with three samples. It validates schema creation,
seeding, seven-day result parity, FTS index use, widget snapshot behavior, and
report generation, but only reports timings. It does not fail a shared CI
worker for noisy latency:

```sh
ctest --test-dir build-performance \
  --output-on-failure -R '^release_performance_smoke$'
```

The timing-enforced CTest is deliberately opt-in and runs serially:

```sh
cmake -S . -B build-performance \
  -DCMAKE_BUILD_TYPE=Release \
  -DOMACALENDAR_BUILD_TESTS=ON \
  -DOMACALENDAR_ENABLE_HARDWARE_PERF_TESTS=ON
cmake --build build-performance --target omacalendard
ctest --test-dir build-performance \
  --output-on-failure -R '^release_performance_hardware_gate$'
```

A failed hardware gate is a release blocker until a repeat run on the same idle
reference machine confirms whether the cause is a regression or host contention.

## Sync-time widget latency

`sync_latency.py` runs an isolated daemon against a deterministic 20,000-event
CalDAV response served on loopback. A disposable `secret-tool` stand-in lives
only in the harness's temporary `PATH`; neither the real keyring nor any external
provider is contacted. The script samples full `widget.snapshot` calls during
provider parsing and chunked database apply, verifies the final event count and
sync token, and reports every sample plus nearest-rank p95 and max against the
existing 100 ms widget gate. The script exits nonzero on either latency failure
when `--enforce-gate` is supplied. Run it on idle reference hardware:

```sh
python3 scripts/performance/sync_latency.py \
  --daemon build-performance/omacalendard \
  --output build-performance/sync-latency.json \
  --enforce-gate
```

This measures the real daemon IPC path while applying remote events, rather
than timing offline SQLite fixture insertion. The HTTP fixture binds only to
127.0.0.1 and the daemon uses separate temporary XDG directories and socket.

## Desktop view benchmark

The daemon gate above does not cover the desktop app, which can spend far
longer turning a response into pixels than the daemon spends producing it.
`tests/qml-bench/tst_ViewBenchmark.qml` renders the agenda, day, week, month
and year views with deterministic synthetic events spread over one year and
measures, per view:

- `render`: creating the view with the full event list until layout settles.
- `update`: replacing the list with a copy in which one event changed, as the
  controller does after a single edit or sync change.

It also measures `all-views update`: the same one-event change delivered to all
five views at once, which is what happens when every view stays alive.

The report is informational. Timings are not asserted, because shared CI
workers are too noisy to gate on; compare runs on the same machine instead.

```sh
python3 scripts/performance/ui_benchmark.py \
  --qmltestrunner "$(qmake6 -query QT_INSTALL_BINS)/qmltestrunner" \
  --events 500 5000 50000 \
  --output build/ui-benchmark.json
```

`--events` selects the dataset sizes (500, 5,000 and 50,000). The default,
also used by the `ui_benchmark_smoke` CTest, is 500 and 5,000; the
50,000-event rows take several minutes and are for manual comparisons. The
JSON report records every sample, the median, the source revision and the
machine.
