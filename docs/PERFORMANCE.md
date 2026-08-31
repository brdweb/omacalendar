# Release performance gate

OmaCalendar's release harness creates a disposable schema-2 database through the
built daemon, stops the daemon, inserts 100,000 deterministic events in one SQLite
transaction, and restarts the daemon for warm IPC measurements. It never uses the
current user's database, socket, accounts, keyring, notification bus, or XDG paths.

The dataset spans 2018–2034 across eight calendars and includes timed, all-day,
multi-day, floating, zoned, recurring, invitation, pending, conflict, read-only,
and deleted records. FTS5 receives the rows through the production triggers. The
report verifies database integrity, event/FTS row counts, and that SQLite selects
the FTS virtual-table index.

Measured gates use the nearest-rank p95 after warm-up:

- Bounded seven-day `events.list`: at most 200 ms.
- Indexed `events.search`: at most 250 ms.
- Full warm `widget.snapshot`: at most 100 ms.

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
  --warmups 5 \
  --samples 15 \
  --enforce-gates \
  --build-label "Release/reference-Omarchy" \
  --output build-performance/release-performance.json
```

Archive `release-performance.json` with the acceptance record. It contains every
sample, median/p95/max, query plans, database size, seed time, daemon hash and
reported protocol/version, source revision/dirty state, CPU, kernel, Python, and
SQLite versions. A dirty source tree or non-Release build should not be used as a
final release result even though the script records it.

## CTest modes

The default deterministic smoke test uses the same 100,000-event dataset with
three samples. It validates schema creation, seeding, query results, FTS index use,
widget snapshot behavior, and report generation, but only reports timings. It does
not fail a shared CI worker for noisy latency:

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
