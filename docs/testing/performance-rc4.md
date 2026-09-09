# RC4 performance qualification — 2026-09-09

The frozen local RC4 development daemon passes all three enforced performance
gates in one controlled comparison with the prior acceptance daemon. No runtime
code or performance threshold was changed during this investigation. Downloaded
release artifacts still need their own qualification.

| Executable | SHA-256 |
| --- | --- |
| Prior acceptance daemon | `b4191bd0898acd767c5c4a602ea4eb9d1fd16a69232315839213d1af542cd932` |
| RC4 development daemon | `15c06c654225f2c1e1e502a22b65316a749c65b6c6769d0237bd90c2c2a5d205` |

Both runs used `scripts/performance/release_performance.py`, 100,000 deterministic
events, five warmups, 15 samples, and `--enforce-gates`. Each initialized its own
disposable database and used production database, recurrence, serialization, and
IPC code. Gate values, fixture, timezone, and measurement logic were unchanged.

A three-second `/proc/stat` sample across the process's allowed CPUs 0–5 selected
CPU 1, the least busy at 12.71%. `taskset --cpu-list 1` applied only to each test
harness and its child daemon. Their actual affinities were read back as `[1]`.
No user process or service configuration changed. The prior daemon ran first,
then RC4, exactly once each; both exited successfully.

| Measurement | Gate p95 | Prior p95 | RC4 p95 |
| --- | ---: | ---: | ---: |
| Bounded agenda | 200 ms | 37.474 ms | 38.801 ms |
| Indexed search | 250 ms | 4.082 ms | 3.973 ms |
| Widget snapshot | 100 ms | 67.688 ms | 70.624 ms |

RC4 returned the expected 225 agenda/widget events and 100 search results. Range
and FTS index checks and compact widget payload checks also passed. Lifecycle
observations of each daemon's `/proc/<pid>/schedstat`, taken outside request
timing, recorded 30.037 ms cumulative runnable wait for the prior daemon and
24.073 ms for RC4 across the benchmark's warmup and measured calls.

Earlier runs on the shared desktop remain failures: RC4 widget p95 was
125.086 ms in the initial sequential control and 105.311 ms in a subsequent
official qualification attempt. Additional alternating RC4/prior/RC4/prior
controls using the same database and 30 samples had widget p95 values of
88.596/89.484/94.927/111.196 ms. In the prior binary's 111.196 ms request,
22.459 ms was scheduler wait and 87.213 ms was CPU time. These observations show
that host contention can also make the previously passing binary fail. They do
not establish a substantial RC4 code regression, and the passing affinity pair
does not erase those failures or promise latency under arbitrary system load.

Local raw reports, scheduler observations, CPU selection, and the observer
wrapper are retained under
`artifacts-rc4-release/performance-investigation/`: `affinity-prior.json`,
`affinity-rc4.json`, their `.scheduler.json` files and logs,
`affinity-selection.json`, `affinity-official.py`, `control.json`, and
`official-current.json`. The observer wrapper calls the unchanged official
harness and records scheduler state only at daemon start/stop boundaries.
