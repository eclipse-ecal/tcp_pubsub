# tcp_pubsub rewrite – Benchmark (PERF-1)

Status: end-to-end harness **and** Google Benchmark micro-benchmarks implemented; the **Windows
baseline of the current implementation is recorded** ([baseline/README.md](baseline/README.md)).
Linux and macOS baselines are still missing.
Requirements this serves: PERF-1 (baseline first), PERF-2 (not worse than baseline),
PERF-3/4/5/7, QA-6, ROB-2 in [01-requirements.md](01-requirements.md).

## 1. What exists

| Part | Location | Purpose |
|---|---|---|
| End-to-end harness | `benchmarks/tcp_pubsub_benchmark/` | Paced latency, throughput, fan-out, scale, idle cost, slow consumer, resources. Custom code, no benchmark library: Google Benchmark's iteration model cannot express paced multi-thread scenarios or per-message percentiles. |
| Micro-benchmarks | `benchmarks/tcp_pubsub_micro_benchmark/` | Google Benchmark (submodule `thirdparty/benchmark/benchmark`, pinned to v1.9.5). Per-call cost of the library API (`Api_*`) and implementation-independent reference floors (`Floor_*`) that put the numbers into context. See section 7. |
| Library adapter | `tcp_pubsub_benchmark/src/adapter.h`, `adapter_v1.cpp` | The **only** code that knows a concrete tcp_pubsub API, shared by both benchmark programs. Benchmarking the rewrite means adding `adapter_<name>.cpp` and building with `-DTCP_PUBSUB_BENCHMARK_ADAPTER=<name>`; scenarios and benchmarks stay identical, so results are comparable. |
| Baseline results | `docs/rewrite/baseline/` | Recorded numbers of the current implementation (JSON + interpretation). |

## 2. Building and running

Always use a **Release** build, on a quiet machine (close other applications, AC power,
no antivirus scan; on laptops use a high-performance power plan).

```powershell
cmake -S . -B _build/bench -G "Visual Studio 17 2022" -A x64 `
      -DTCP_PUBSUB_BUILD_BENCHMARKS=ON -DTCP_PUBSUB_BUILD_SAMPLES=OFF -DTCP_PUBSUB_BUILD_TESTS=OFF
cmake --build _build/bench --config Release --target tcp_pubsub_benchmark tcp_pubsub_micro_benchmark
_build/bench/benchmarks/tcp_pubsub_benchmark/Release/tcp_pubsub_benchmark.exe --list
_build/bench/benchmarks/tcp_pubsub_benchmark/Release/tcp_pubsub_benchmark.exe `
      --scenario all --duration 10 --repeat 3 --json result.json --label "my run"
_build/bench/benchmarks/tcp_pubsub_micro_benchmark/Release/tcp_pubsub_micro_benchmark.exe `
      --benchmark_repetitions=3 --benchmark_out=micro.json --benchmark_out_format=json
```

`TCP_PUBSUB_USE_BUILTIN_BENCHMARK=OFF` uses a system-installed Google Benchmark instead of the
submodule. On Linux/macOS use a single-config generator with `-DCMAKE_BUILD_TYPE=Release`.

Options: `--scenario <name|group|all>` (repeatable), `--duration`, `--warmup`, `--repeat`,
`--io-threads` (worker threads per side, default 2), `--delivery pooled|inline`,
`--label`, `--json`, `--verbose` (prints sequence gaps).

## 3. Scenarios

Topology: *pairs* independent publishers, each with *fanout* subscribers. Every subscriber is
its own `Subscriber` object with one connection. Publisher and subscribers live in one process
but use **separate execution contexts**; traffic is real TCP over loopback (`127.0.0.1`).

| Scenario | Workload (requirement) | Message | Rate | Topology |
|---|---|---|---|---|
| `small-64B`, `small-1KiB`, `small-64KiB` | W-1 | 64 B / 1 KiB / 64 KiB | 100 Hz | 1 → 1 |
| `large-1MiB`, `large-4MiB`, `large-16MiB` | W-2 | 1 / 4 / 16 MiB | 15 Hz | 1 → 1 |
| `fanout-10`, `fanout-100` | W-3 | 64 KiB / 16 KiB | 100 Hz | 1 → 10 / 100 |
| `pairs-100` | W-3 | 1 KiB | 100 Hz each | 100 publishers × 1 subscriber |
| `idle-100` | PERF-6 | – | none | 100 pairs connected |
| `throughput-64KiB`, `throughput-1MiB` | max rate | 64 KiB / 1 MiB | unpaced | 1 → 1 |
| `slow-consumer` | DEL-3/4, PERF-7, ROB-2 | 1 MiB | 100 Hz | handler sleeps 50 ms |

## 4. What is measured and how

- **End-to-end latency**: time from immediately before `publish()` (stamped into the message
  with `steady_clock`) to the moment the handler is entered, per received message, all
  subscribers merged. Reported as mean, min, p50, p90, p99, p99.9, max (nearest rank).
  Publisher and subscriber share the clock because they are in one process; this cannot
  measure clock offset between hosts and is not a network benchmark.
- **`publish()` call duration**: cost of the call itself on the publishing thread (FR-PUB-6).
- **Delivered ratio**: received / (sent × fanout). Below 100 % is *expected* in the slow-consumer
  and unpaced scenarios (latest-wins drops) and a defect elsewhere.
- **Throughput**: received messages and MiB per second inside the window.
- **CPU**: whole-process CPU time (user + kernel) over the window, in percent of one core
  (both sides, harness included). **Threads**, **RSS** at window end, peak, and growth over the
  window (bounded-memory check, ROB-2).
- **Lifecycle**: time to connect all subscribers, time to tear everything down (LT-3).
- Warm-up traffic is not measured. After the window the harness waits for in-flight messages
  before tearing down.

### Harness health

- The pacing thread skips slots it missed by more than one period instead of sending a burst
  (a burst would be dropped by latest-wins and be mistaken for message loss). The number of
  skipped slots is reported as `pacing_overruns`; any non-zero value means the machine was
  not quiet enough and the run should be repeated.
- The harness cost itself is small but not zero: handlers record into pre-sized vectors, no
  allocation or I/O happens in the measured path. Process-wide thread enumeration (slow on
  Windows) is done only after the window.
- A latency histogram with fixed memory (instead of storing every sample) is a possible
  later refinement for hour-long runs.

## 5. Comparing implementations (PERF-2)

Acceptance rule from the requirements: the rewrite is not worse than the baseline in p50 and
p99 for W-1 and W-2 on every supported OS with default settings. Procedure:

1. Run the same scenarios with the same options, same machine, same build type, **at least 3
   repetitions**, for both adapters.
2. Compare medians of the repetitions. Differences below the run-to-run spread of the baseline
   (reported in section 6) are noise.
3. Record the comparison next to the baseline JSON and in the README.

Run the baseline on every platform of the CI matrix (Windows, Linux x86-64/arm64, macOS) once
the rewrite exists; only Windows has been recorded so far.

## 6. Baseline of the current implementation

Recorded; see [baseline/README.md](baseline/README.md) for tables, interpretation, noise levels
and the library issues observed. Peak RSS in the JSON is a process-wide peak (a large scenario
raises it for all later scenarios in the same run): compare `rss_mib_end` / `rss_mib_growth`, or
run a scenario alone to get its own peak.

## 7. Micro-benchmarks (Google Benchmark)

Every benchmark is **manually timed** (the measured region is only the operation of interest;
setup, waiting for completion and the idle gap are excluded) and uses a **fixed iteration count**
(so that the 100 Hz variants do not run for minutes). The argument `gap_us` is the idle time
before each operation: `0` = back to back (hot caches, threads running) and `10000` = a 100 Hz
stream (threads asleep, the situation of workload W-1). The cold/hot difference is the
interesting part: the end-to-end baseline showed that sparse streams are dominated by wake-ups.

### 7.1 Library benchmarks (`Api_*`, through the adapter)

| Benchmark | Arguments | Measures |
|---|---|---|
| `Api_Publish_NoSubscriber` | `size` | `publish()` while nobody is connected (FR-PUB-7: must be nearly free) |
| `Api_Publish` | `size`, `subs`, `gap_us` | duration of one `publish()` call on the caller's thread with `subs` connected subscribers; counter `delivered` = fraction of messages the handlers received (latest-wins drops are expected when publishing back to back) |

### 7.2 Reference floors (`Floor_*`, no tcp_pubsub code)

| Benchmark | Measures | Why it matters |
|---|---|---|
| `Floor_Memcpy`, `Floor_AllocZeroCopyFree` | copying a payload; allocating + zero-filling + copying + freeing | lower bound of the one-copy rule (PERF-3) and the value of a buffer pool (PERF-4) |
| `Floor_Handoff_spin`, `Floor_Handoff_condvar` | one-way thread hand-off: spinning (best case) and sleeping with a condition variable | cost of an extra thread hop, e.g. pooled vs inline delivery |
| `Floor_PostToHandler`, `Floor_PostCall` | `asio::post` to an idle loop thread: time until the handler runs, and the cost of the post call itself | latency of "hand off to the connection's loop" and the cost of the publish hot path in a post-based design |
| `Floor_TcpPingPong` | blocking echo over loopback with `TCP_NODELAY`, reported as one-way = round trip / 2 | the latency floor of TCP on this machine |
| `Floor_TcpSyncWrite`, `Floor_TcpAsyncWriteInitiate` | one blocking send; initiating one `async_write` on the calling thread (completion waited outside the measured time) | what the caller thread pays per subscriber today vs the cheapest alternative |

Reading the results: compare `Api_Publish` with `Floor_TcpAsyncWriteInitiate` (the library should
be close to the OS floor per subscriber) and with `Floor_PostCall` (the cost a post-based design
would have).

### 7.3 Running

```powershell
tcp_pubsub_micro_benchmark.exe --benchmark_list_tests=true
tcp_pubsub_micro_benchmark.exe --benchmark_filter=Api_ --benchmark_repetitions=3
tcp_pubsub_micro_benchmark.exe --benchmark_filter=Floor_Tcp --benchmark_out=floor.json --benchmark_out_format=json
```

Stderr contains the library's own teardown log lines (see baseline/README.md section 5);
redirect it (`2>$null`) when reading the table. Repetitions add mean/median/stddev/cv
aggregates; use the **median** for comparisons. Google Benchmark's `compare.py`
(in `thirdparty/benchmark/benchmark/tools/`) can compare two JSON files directly.
