# Baseline of tcp_pubsub v2.0.1

Recorded with the end-to-end harness described in [../05-benchmark.md](../05-benchmark.md).
These numbers are the reference for PERF-2 ("the rewrite is not worse than the baseline").

The historical `v1` adapter label and JSON file names below refer to the **2.0.1** API, not
release 1.x. The adapter is now named `v2`; the recorded JSON remains unchanged. The measured
library is preserved in [legacy/v2/](../../../legacy/v2/README.md).

| | |
|---|---|
| Implementation | current tcp_pubsub (commit `971382e`, adapter `v1`) |
| Machine | AMD Ryzen 7 PRO 7735U (8 cores / 16 threads), Windows 11 Enterprise 10.0.26200, **laptop, not a dedicated benchmark machine** |
| Build | MSVC 19.44, Release (`/O2`), x64, 2 worker threads per side |
| Run | 10 s measured + 1 s warm-up per scenario, **3 repetitions**; tables show the **median** of the repetitions and (min–max) for p50/p99 |
| Data | [baseline-v1-windows-pooled.json](baseline-v1-windows-pooled.json), [baseline-v1-windows-inline.json](baseline-v1-windows-inline.json) |
| Pacing health | 0 skipped publish slots in all runs |

Traffic is real TCP over loopback; publisher and subscriber are in one process with separate
executors. Latency = just before `publish()` → handler entry (µs).

**Platforms:** sections 1–9 describe the **Windows** baseline. The **Linux baseline (WSL2)** recorded
on the same laptop is in [section 10](#10-linux-baseline-wsl2); it changes some conclusions
(small versus large messages, cost of thread wake-ups), so read both before drawing design
consequences.

## 1. Default configuration (asynchronous callback thread, "pooled")

### Latency and throughput

| Scenario | p50 (min–max) | p99 (min–max) | p99.9 | Received | Delivered |
|---|---:|---:|---:|---:|---:|
| small-64B (100 Hz) | 227 (222–233) | 455 (441–459) | 576 | 99 msg/s | 100 % |
| small-1KiB (100 Hz) | 251 (229–268) | 524 (414–571) | 712 | 99 msg/s | 100 % |
| small-64KiB (100 Hz) | 630 (612–663) | 961 (923–1087) | 1 272 | 6.2 MiB/s | 100 % |
| large-1MiB (15 Hz) | 4 051 (3 961–4 174) | 6 343 (5 957–6 554) | 7 208 | 14.9 MiB/s | 100 % |
| large-4MiB (15 Hz) | 12 245 (11 812–13 552) | 17 440 (15 551–23 482) | 18 097 | 59.6 MiB/s | 100 % |
| large-16MiB (15 Hz) | 41 987 (41 421–42 528) | 74 533 (53 073–107 271) | 77 377 | 238.7 MiB/s | 99.3–100 % |
| fanout-10 (64 KiB, 100 Hz) | 1 452 (1 433–1 476) | 2 590 (2 573–2 640) | 3 105 | 62.1 MiB/s | 100 % |
| fanout-100 (16 KiB, 100 Hz) | 3 094 (3 035–3 094) | 5 982 (5 783–6 107) | 8 522 | 155.4 MiB/s | 100 % |
| pairs-100 (1 KiB, 100 Hz each) | 68 (68–69) | 163 (157–165) | 231 | 9 948 msg/s | 100 % |
| throughput-64KiB (unpaced) | 178 (177–180) | 460 (455–480) | 577 | 4 806 msg/s, 300 MiB/s | 4 % (latest-wins) |
| throughput-1MiB (unpaced) | 2 936 (2 855–2 965) | 5 075 (4 677–5 278) | 6 423 | 371 msg/s, 371 MiB/s | 10 % (latest-wins) |
| slow-consumer (1 MiB, 100 Hz, 50 ms handler) | 8 394 (8 204–8 434) | 14 993 (14 369–15 466) | 17 924 | 20 msg/s | 19.9 % (latest-wins) |

### Cost and resources

| Scenario | `publish()` mean | CPU (1 core = 100 %) | Threads | RSS at end (growth in window) | Connect | Teardown |
|---|---:|---:|---:|---:|---:|---:|
| small-64B | 162 µs | 2 % | 8–11 | 6 MiB (+0.0) | 7 ms | 0.6 ms |
| small-1KiB | 180 µs | 3 % | 8 | 6 MiB (+0.0) | 6 ms | 0.7 ms |
| small-64KiB | 508 µs | 9 % | 8 | 6 MiB (+0.1) | 7 ms | 0.7 ms |
| large-1MiB | 895 µs | 7 % | 8 | 9 MiB (+2.1) | 6 ms | 0.8 ms |
| large-4MiB | 962 µs | 19 % | 8 | 18 MiB (+8.0) | 7 ms | 1.4 ms |
| large-16MiB | 2 078 µs | 66 % | 8 | 70 MiB (+48.1) | 7 ms | 2.1 ms |
| fanout-10 | 2 141 µs | 37 % | 17 | 7 MiB (+0.8) | 9 ms | 3.5 ms |
| fanout-100 | 5 455 µs | 97 % | **107** | 12 MiB (+2.5) | 36 ms | 23 ms |
| pairs-100 | 42 µs | 88 % | **108** | 12 MiB (+1.9) | 40 ms | 25 ms |
| idle-100 (no traffic) | – | **0 %** | **108** | 10 MiB | 35 ms | 26 ms |
| throughput-1MiB | 278 µs | 202 % | 8 | 15 MiB (+6.4) | 6 ms | 0.8 ms |
| slow-consumer | 823 µs | 39 % | 8 | 12 MiB (+4.2) | 6 ms | 0.8 ms |

Peak RSS is not tabulated: it is a *process-wide* peak and the 16 MiB scenario sets it to 87 MiB
for every scenario that runs after it in the same process. Run a scenario alone to get its peak.

## 2. Inline delivery (the library's "synchronous" mode; handler runs on the I/O side)

Same machine, same settings, 3 repetitions.

| Scenario | p50 (min–max) | vs. pooled | p99 (min–max) | vs. pooled |
|---|---:|---:|---:|---:|
| small-64B | 212 (211–218) | −15 µs (−7 %) | 399 (398–413) | −56 µs (−12 %) |
| small-1KiB | 212 (207–215) | −39 µs (−16 %) | 395 (370–469) | −129 µs (−25 %) |
| small-64KiB | 553 (521–587) | −77 µs (−12 %) | 861 (811–863) | −100 µs (−10 %) |
| large-1MiB | 4 256 (4 235–4 347) | +205 µs (+5 %) | 5 952 (5 916–6 281) | −391 µs (−6 %) |
| fanout-10 | 1 697 (1 648–1 702) | **+245 µs (+17 %)** | 3 043 (2 996–3 103) | +453 µs (+17 %) |

## 3. What the baseline shows

1. **Latency floor.** A 64-byte message at 100 Hz takes ~225 µs p50 and ~450 µs p99 on this
   machine. That is ~2 % of the 10 ms period (W-1): the absolute numbers are not the problem,
   the *variance* is (p99 ≈ 2× p50, p99.9 ≈ 2.5× p50).
2. **Idle versus busy.** The same code path with 100 publishers (`pairs-100`) is **3× faster at p50
   (68 µs)** than a single idle pair (227 µs): the work is identical, but with a single 100 Hz
   stream every message pays thread wake-ups (and CPU power-state exits) twice – I/O thread, then
   the callback thread. The latency of a sparse stream is dominated by wake-ups, not by TCP.
3. **Inline delivery** saves 15–40 µs p50 (7–16 %) and 10–25 % at p99 for small messages –
   removing one hop. Both modes sit in the same order of magnitude, and the run-to-run spread of
   the pooled baseline (e.g. 229–268 µs for 1 KiB) is of the same size as the gain. For large
   messages and fan-out inline is neutral or **worse** (+17 % at fan-out 10) because handlers of
   all subscribers serialise on the I/O threads. Consistent with the earlier estimate (tens of µs,
   mostly jitter): inline is not worth making the default.
4. **`publish()` is not cheap and scales with subscribers.** 160–180 µs for a 64 B–1 KiB message
   to one subscriber, 508 µs for 64 KiB, and **5.5 ms for 100 subscribers** (≈ 55 µs per subscriber).
   The caller thread performs the per-connection socket work, which conflicts with the intent of
   FR-PUB-6 and PERF-5/7. The micro-benchmarks (section 7) show that this cost is the operating
   system's send path after an idle period, not library code: a raw `send()` costs the same.
5. **Threads grow with subscribers: 8 threads for one subscriber, 108 for 100** – one extra
   thread per `Subscriber` with an asynchronous callback (defect D-8 / TH-1/TH-2, measured).
6. **Idle cost is fine in CPU (0 %), expensive in threads and teardown:** 100 idle pairs hold
   108 threads and need 25 ms to tear down; connecting takes ~35 ms.
7. **Large messages are throughput-bound:** 16 MiB at 15 Hz is 239 MiB/s through 66 % of a core
   (both sides). Latency p50 ≈ 42 ms is one copy per side plus the loopback transfer; p99 is up to
   2.5× that and varies most between runs (53–107 ms). One 16 MiB scenario run lost 0.7 % of messages
   (latest-wins drop when the receiver was late); memory grows by ~48 MiB in the window
   (buffer pool plus in-flight buffers, about 3–4 messages' worth).
8. **Latest-wins works as designed:** the slow consumer receives 19.9 % of the messages (the 50 ms
   handler allows 20 of 100 per second) with latency bounded by one handler duration, and memory
   stays bounded (+4 MiB).
9. **Throughput ceiling** of the current implementation over loopback: ~370 MiB/s for 1 MiB
   messages, ~300 MiB/s for 64 KiB messages (one subscriber, 2 + 2 worker threads), using 1.5–2
   cores in total.

## 4. Noise and how to read differences

Run-to-run spread of the *same* build on this laptop: small messages ±5–15 % at p50 and up to
±25 % at p99; large messages ±5 % at p50, p99 of the 16 MiB case varies by 2×. **Only differences
clearly larger than that spread are meaningful.** The acceptance check for the rewrite should
therefore compare medians of ≥ 3 runs, use the p50 and p99 columns above, and allow the p99
comparison a tolerance equal to the baseline spread.

## 5. Library issues observed while benchmarking

- At every normal teardown the library logs `Error`-level lines such as *"Publisher ?: Error while
  waiting for subsriber: The I/O operation has been aborted…"* and *"SubscriberSession …: Error
  reading header length: The network connection was aborted by the local system."* These are expected
  consequences of cancelling operations, not errors (LOG-3 asks for exactly this to be avoided).
  Endpoint strings print as `?` for the same reason (socket already closed when logging).
- The first-message artifacts a naive harness could produce are avoided: connection and handshake
  settle before measuring and the measurement window starts after the warm-up.
- **The current implementation crashes (SIGSEGV) on Linux when topologies are torn down repeatedly.**
  Reproduced under gdb with the harness creating and destroying 100 publisher/subscriber pairs back
  to back (`tcp_pubsub_benchmark --scenario idle-100 --duration 1 --warmup 0 --repeat 30`); it also
  hit one micro-benchmark run (`Api_Publish/size:64/subs:100/gap_us:10000`) and a first, interrupted
  attempt of the micro suite. Backtrace of the crashing thread (an I/O worker):
  `asio::detail::epoll_reactor::deregister_descriptor` ← `SubscriberSession_Impl::connectionFailedHandler()`
  ← completion handler of an `async_read` on the session's strand. The worker closes the session's socket
  while a user thread's `cancel()` closes/cancels the same socket: the unserialised-socket race behind the
  disabled Linux tests (`disable_racy_tests.cmake`), defect D-2 in the requirements. The runner therefore
  executes every scenario in its own process and retries after a crash (section 10.1).

## 6. Micro-benchmarks (Google Benchmark)

Same machine and build as above, 3 repetitions, medians; cv (run-to-run) is 0–13 % unless
noted. Data: [micro-v1-windows.json](micro-v1-windows.json). Description of every benchmark:
[../05-benchmark.md](../05-benchmark.md) section 7. `gap` = idle time before each operation:
**hot** = back to back, **cold** = after 10 ms idle (a 100 Hz stream).

### 6.1 Cold versus hot: the same primitives, 10 ms later

| Primitive | hot | cold (after 10 ms idle) | factor |
|---|---:|---:|---:|
| hand-off between two threads, spinning (best case) | 46 ns | – | – |
| hand-off between two threads, condition variable (one way) | 0.54 µs | 22.6 µs | 42× |
| `asio::post` call on the calling thread | 1.0 µs | 16.7 µs | 17× |
| `asio::post` until the handler runs on an idle loop thread | 1.9 µs | 54.4 µs | 29× |
| blocking `send` of 64 B on loopback | 20.6 µs | 182 µs | 8.8× |
| initiating an `async_write` of 64 B on the caller thread | 27.6 µs | 194 µs | 7.0× |
| TCP echo, `TCP_NODELAY`, 64 B, **one way** | 24.4 µs | 117.5 µs | 4.8× |
| TCP echo, 64 KiB, one way | 135 µs | 487 µs | 3.6× |

Everything, including a trivial `post()` call, is 5–40× slower after a pause. On this machine the
cost of a 100 Hz stream is dominated by the system waking up (CPU power states, cold caches and
scheduler wake-ups), not by TCP or by library code.

### 6.2 Library: `Api_Publish` (duration of one `publish()` call on the caller's thread)

| Size | Subscribers | back to back (`delivered`) | 100 Hz stream (cold) | raw `async_write` initiation, cold | raw blocking `send`, cold |
|---|---:|---:|---:|---:|---:|
| 64 B | 0 | 49 ns | – | – | – |
| 64 B | 1 | 5.8 µs (16 %) | **180 µs** | 194 µs | 182 µs |
| 64 B | 10 | 244 µs (99 %) | 606 µs | | |
| 64 B | 100 | 2.56 ms (100 %) | 4.04 ms | | |
| 64 KiB | 0 | 50 ns | – | – | – |
| 64 KiB | 1 | 16 µs (6 %) | 476 µs | 456 µs | 423 µs |
| 64 KiB | 10 | 1.39 ms (100 %) | 2.32 ms | | |
| 1 MiB | 1 | 300 µs (10 %) | 870 µs | 419 µs | 2.75 ms |

*`delivered` is the fraction of messages that reached the handlers. Back to back with few
subscribers, most messages replace the pending one (latest-wins) and no write happens, so those
rows measure the buffer-handling path, not the send.*

### 6.3 Memory

| Size | `memcpy` | allocate + zero-fill + copy + free (a buffer without pool) |
|---|---:|---:|
| 64 B | 4 ns | 40 ns |
| 64 KiB | 1.0 µs (63 GiB/s) | 4.8 µs |
| 1 MiB | 20 µs (48 GiB/s) | **261 µs** (13×) |
| 16 MiB | 1.2 ms (13 GiB/s) | 4.8 ms (4×) |

## 7. What the micro-benchmarks show

1. **The library adds almost nothing to the OS send cost for a single subscriber.** 180 µs for a
   64 B `publish()` versus 182 µs for a raw blocking send and 194 µs for a raw `async_write`
   initiation; 476 µs versus 423–456 µs at 64 KiB. The 160–180 µs `publish()` seen in the end-to-end
   run is the operating system's cold send path (the loopback send includes delivering to the
   receiver and waking it), executed on the caller's thread.
2. **That send is most of the end-to-end latency at 100 Hz.** A 64 B message needs ~227 µs p50
   end to end; ~160–180 µs of it is spent *inside* `publish()`. The rest (~50–65 µs) is the receive
   side: waking the I/O thread and hopping to the callback thread. The raw cold TCP floor is 117 µs
   one way, so the current implementation is about 1.9× the floor.
3. **Per-subscriber cost is serial on the caller's thread.** The first subscriber is cold (180 µs),
   each additional one costs ≈ 40 µs (64 B) and ≈ 205 µs (64 KiB), against 21–28 µs and 136 µs for a
   raw hot send. 100 subscribers cost the caller 4 ms, and the last subscriber receives its
   message ~4 ms after the first: this is why `fanout-100` has p50 ≈ 3 ms. Spreading the sends over
   the I/O threads and returning early would fix both.
4. **Library overhead per publish is visible where it should not be.** With nobody connected a
   `publish()` costs ~50 ns for any size (FR-PUB-7 holds: nothing is copied). But when a message
   only replaces the pending one (no write), it still costs **5.8 µs at 64 B and 16 µs at 64 KiB**, against 4 ns / 1 µs
   for the copy itself: locks, buffer-pool allocation and shared-pointer handling. A lean hot path
   should be well below 1 µs.
5. **The buffer pool matters for large messages.** A fresh buffer costs 261 µs per MiB
   (13× a memcpy: page faults and zero-filling). The 1 MiB back-to-back `publish()` takes 300 µs,
   which is what a non-pooled buffer costs, so reuse is not working there, probably because buffers
   are still in flight. This also explains part of the 870 µs for 1 MiB at 100 Hz (raw initiation 419 µs).
6. **Thread hops are cheap hot and expensive cold:** 0.5 µs vs 22.6 µs (condition variable), 1.9 µs
   vs 54 µs (`asio::post` to an idle loop). One extra hop in a sparse stream therefore costs
   20–55 µs; on busy systems it is almost free. This is the quantitative basis for the
   pooled-versus-inline discussion (section 3, item 3, shows the same ~15–40 µs effect end to end).

## 8. Consequences for the rewrite (hypotheses to validate with a prototype)

These follow from the numbers; they are inputs for `03-architecture.md`, not decisions.

- **Publish hot path: inline send versus post.** Posting the frame to the connection's loop makes
  the caller's cost 1–17 µs (`Floor_PostCall`) instead of 180 µs, but adds a hop (+20–55 µs cold)
  before the send even starts, so the **end-to-end latency would not improve and may get worse by
  tens of µs**. A non-blocking send from the caller's thread has the lowest latency but charges the
  caller (180 µs cold, 40 µs per further subscriber). The earlier proposal to drop the caller-thread
  write because "100 Hz has a millisecond budget" needs to be revisited with this data. A hybrid is
  plausible: first connection(s) written inline, the rest posted per I/O loop (not per connection).
- **Fan-out must not be serial on one thread** (item 3): with hundreds of subscribers, one post
  per I/O loop that then sends to its connections in parallel.
- **Keep a buffer pool, avoid zero-fill** (item 5), and make the replace-slot path lock-light (item 4).
- **Low-latency mode (open):** because wake-ups dominate, an optional short busy-poll or
  keep-warm on the I/O thread could cut latency substantially for sparse streams. It costs CPU and
  power and is not in the requirements; whether to offer it is a product decision.
- **Absolute numbers are machine-specific** (see 8.1); the *direction* (cold ≫ hot, send ≫ library
  overhead) is what to carry over.

### 8.1 Caveats of this run

- AMD Ryzen 7 PRO 7735U laptop on AC power with the vendor scheme *HP Optimized (Modern
  Standby)*, not a high-performance plan; deep idle states are likely a large part of the
  cold penalty. Repeat on a desktop/server and with a high-performance plan, and on Linux and macOS,
  before treating the cold/hot factors as general.
- Loopback only: a real NIC adds interrupt moderation and DMA latency of its own.
- Publisher and subscriber share a process; the benchmark threads, the library threads and the
  harness compete for the same 16 hardware threads.

## 9. Not yet recorded

- macOS and Linux **arm64** baselines, and a **native** Linux x86-64 baseline (section 10 is a WSL2 VM).
- A baseline taken on a dedicated, quiet machine with a fixed power plan.
- Micro-benchmarks for frame building and buffer-pool internals (they need access to library
  internals and are only meaningful for the implementation under test); gather publish and ticket
  snapshot benchmarks belong to the rewrite.

## 10. Linux baseline (WSL2)

| | |
|---|---|
| Machine | the **same laptop** as above, Linux in WSL2 (a Hyper-V virtual machine): Ubuntu 24.04, kernel 6.18.33.2-microsoft-standard-WSL2, 16 virtual CPUs |
| Build | gcc 13.3, CMake 3.28, Ninja, Release (`-O3 -DNDEBUG`), sources built on the VM's own ext4 file system |
| Run | same parameters as Windows (10 s + 1 s warm-up, 3 repetitions, 2 worker threads per side); **one process per scenario**, so *peak RSS is per scenario* here (it is process-wide on Windows) |
| Data | [baseline-v1-linux-wsl2-pooled.json](baseline-v1-linux-wsl2-pooled.json), [baseline-v1-linux-wsl2-inline.json](baseline-v1-linux-wsl2-inline.json), [micro-v1-linux-wsl2.json](micro-v1-linux-wsl2.json) |
| Pacing health | 0 skipped publish slots in the reported runs (one `pairs-100` run was flagged and repeated) |

**Read this first:** WSL2 is virtualised. Waking a blocked thread (futex/epoll wake-up) can require
hypervisor involvement, which makes cross-thread wake-ups much more expensive than on native Linux.
Treat the absolute small-message latencies as an upper bound for Linux, and the *relative* findings
(where the time goes) as the useful result. A native Linux baseline is still missing.

### 10.1 How the run was made

- The first Linux attempt was **discarded**: the laptop went to sleep/hibernation during the run
  (wall time 53 minutes instead of ~10, multi-second stalls, flagged by the harness's pacing check).
  The final run keeps the system awake (`SetThreadExecutionState`) and took 18 minutes in total.
- `benchmarks/tools/run_baseline.sh` runs each scenario/benchmark in its own process, **retries up
  to 3 times** if the process crashes (see section 5) or the run reports pacing overruns, and logs
  every such event. In the reported run: 1 crash (`Api_Publish/size:64/subs:100/gap_us:10000`, attempt 1)
  and 1 noisy run (`pairs-100`, attempt 1), both succeeded on the next attempt.

### 10.2 End-to-end, pooled delivery (median of 3; Windows for comparison)

| Scenario | p50 Win | p50 Linux (min–max) | p99 Win | p99 Linux (min–max) | Linux throughput | `publish()` Win / Linux | CPU Win / Linux |
|---|---:|---:|---:|---:|---:|---:|---:|
| small-64B | 227 | **622** (577–631) | 455 | **1 032** (951–1 070) | 100 msg/s | 162 / 104 µs | 2 / 4 % |
| small-1KiB | 251 | **643** (638–664) | 524 | **1 058** (1 022–1 085) | 100 msg/s | 180 / 107 µs | 3 / 4 % |
| small-64KiB | 630 | 880 (829–980) | 961 | 1 278 (1 223–1 566) | 6.2 MiB/s | 508 / 128 µs | 9 / 5 % |
| large-1MiB | 4 051 | **2 063** (2 022–2 146) | 6 343 | 3 185 (2 845–3 708) | 15.0 MiB/s | 895 / 296 µs | 7 / 3 % |
| large-4MiB | 12 245 | 7 081 (6 623–8 012) | 17 440 | 13 268 (8 976–18 249) | 60 MiB/s | 962 / 1 012 µs | 19 / 10 % |
| large-16MiB | 41 987 | **21 907** (21 486–25 722) | 74 533 | 40 368 (33 880–61 515) | 240 MiB/s | 2 078 / 3 489 µs | 66 / 35 % |
| fanout-10 | 1 452 | 1 299 (1 189–1 315) | 2 590 | 2 196 (2 093–2 242) | 62.5 MiB/s | 2 141 / 954 µs | 37 / 30 % |
| fanout-100 | 3 094 | 3 380 (3 222–3 505) | 5 982 | 7 735 (6 709–7 984) | 156 MiB/s | 5 455 / 5 679 µs | 97 / 179 % |
| pairs-100 | 68 | **298** (294–318) | 163 | **813** (702–2 338) | 9.8 MiB/s | 42 / 52 µs | 88 / **191 %** |
| throughput-64KiB | 178 | 2 868 (995–3 395) | 460 | 6 423 | **546 MiB/s**, 8 730 msg/s, 10.6 % delivered | 8 / 12 µs | 147 / 250 % |
| throughput-1MiB | 2 936 | 5 732 | 5 075 | 9 444 | **745 MiB/s**, 6.4 % delivered | 278 / 86 µs | 202 / 205 % |
| slow-consumer | 8 394 | 6 484 | 14 993 | 12 085 | 20 msg/s, 20.1 % delivered | 823 / 277 µs | 39 / 16 % |
| idle-100 | – | – | – | – | 0 % CPU, 106 threads (Windows 108), connect 25 ms (35), teardown 22 ms (25) | – | 0 / 0 % |

Latencies in µs. Threads: 7 for one subscriber (Windows 8–11), 106 for 100 subscribers (Windows 108).

### 10.3 Inline delivery (Linux, median of 3)

| Scenario | p50 inline | vs pooled (Linux) | vs pooled (Windows) | p99 inline | vs pooled (Linux) |
|---|---:|---:|---:|---:|---:|
| small-64B | 543 | −79 µs (−13 %) | −15 µs (−7 %) | 1 022 | −10 µs (−1 %) |
| small-1KiB | 565 | −78 µs (−12 %) | −39 µs (−16 %) | 1 126 | +68 µs (+6 %) |
| small-64KiB | 932 | +52 µs (+6 %) | −77 µs (−12 %) | 1 709 | +431 µs (+34 %) |
| large-1MiB | 2 054 | ≈ 0 | +205 µs (+5 %) | 3 669 | +484 µs (+15 %) |
| fanout-10 | 1 222 | −77 µs (−6 %) | +245 µs (+17 %) | 2 193 | ≈ 0 |

### 10.4 Micro-benchmarks (median of 3)

| Primitive (hot = back to back, cold = after 10 ms idle) | Windows hot | Windows cold | Linux/WSL2 hot | Linux/WSL2 cold |
|---|---:|---:|---:|---:|
| condition-variable hand-off between two threads (one way) | 0.54 µs | 22.6 µs | **28.0 µs** | 65.3 µs |
| `asio::post` call on the calling thread | 1.0 µs | 16.7 µs | 0.43 µs | 43.7 µs |
| `asio::post` until the handler runs on an idle loop thread | 1.9 µs | 54.4 µs | 0.88 µs | **84.0 µs** |
| blocking `send` of 64 B on loopback | 20.6 µs | 182 µs | 1.1 µs | **51.8 µs** |
| `async_write` initiation of 64 B on the caller thread | 27.6 µs | 194 µs | 25.0 µs | 97.9 µs |
| TCP echo, `TCP_NODELAY`, 64 B, one way | 24.4 µs | 117.5 µs | **150 µs** | **389 µs** |
| TCP echo, 64 KiB, one way | 135 µs | 487 µs | 283 µs | 547 µs |
| blocking `send` of 1 MiB | 2.09 ms | 2.75 ms | 0.87 ms | 0.48 ms |

| `Api_Publish` (caller's `publish()` duration), cold, 100 Hz stream | Windows | Linux/WSL2 |
|---|---:|---:|
| 64 B → 1 subscriber | 180 µs | 110 µs |
| 64 B → 10 subscribers | 606 µs | 695 µs |
| 64 B → 100 subscribers | 4.04 ms | 3.94 ms |
| 64 KiB → 1 subscriber | 476 µs | 148 µs |
| 64 KiB → 10 subscribers | 2.32 ms | 966 µs |
| 1 MiB → 1 subscriber | 870 µs | 280 µs |
| no subscriber (any size) | 49–53 ns | 20–23 ns |

Memory benchmarks on WSL2 are **not used**: `Floor_Memcpy/65536` takes 17 µs (3.6 GiB/s) while
1 MiB takes 24 µs (40 GiB/s), stable across repetitions but non-monotonic. This looks like an artifact
of how the two 64 KiB buffers are placed (cache-line/4K aliasing in the allocator) rather than a platform
property; the benchmark should align its buffers before these floors are interpreted. The Windows
memory numbers are consistent.

### 10.5 What the Linux baseline adds

1. **The send syscall is cheap on Linux, waking a thread is expensive (in this VM).** A cold 64 B
   `send` costs 52 µs (Windows 182 µs), but handing work to a sleeping thread costs 65–84 µs (Windows
   23–54 µs) and even a *hot* condition-variable hand-off costs 28 µs (Windows 0.5 µs). The latency of a
   sparse small-message stream is therefore (number of thread wake-ups) × (wake-up cost): 622 µs p50
   against a raw cold TCP floor of 389 µs one way.
2. **Library overhead relative to the raw floor is similar on both systems:** 1.6× the cold TCP
   one-way time on Linux (622 / 389), 1.9× on Windows (227 / 117.5).
3. **Large messages are twice as fast on Linux** (1 MiB 2.1 ms vs 4.1 ms, 16 MiB 22 ms vs 42 ms) at half
   the CPU, and the maximum throughput doubles (745 vs 371 MiB/s for 1 MiB messages).
4. **Inline delivery helps small messages more on Linux in relative terms** (−13 %, ≈ −80 µs = one
   avoided wake-up) but is neutral or worse for 64 KiB, 1 MiB and its tail latency.
5. **One thread per subscriber seems to hurt Linux (VM) much more:** `pairs-100` is 4.4× slower at p50
   (298 vs 68 µs) and 5× at p99 with 191 % CPU (Windows 88 %). This is consistent with the 100
   callback threads of the current design competing for the VM's scheduler and paying the expensive
   wake-ups of finding 1; it was not isolated further. A dynamic pool (OD-12) that only grows when
   handlers actually overlap addresses this by design.
6. **`publish()` on the caller thread is cheaper on Linux** (110 µs vs 180 µs for 64 B, 280 µs vs
   870 µs for 1 MiB) but still scales linearly with subscribers (≈ 39 µs each at 100).

**Effect on the open decision OD-10 (publish hot path):** on Linux an inline non-blocking send costs
the caller ~52 µs cold (a few µs hot), while the post-to-loop design adds a cold hop of ~84 µs
(~13 % of the small-message latency, matching the inline-delivery gain above). On Windows the same
trade is 180 µs on the caller versus a 54 µs hop. Both platforms therefore favour *at least trying* the
inline fast path for the first connection(s) once the prototype exists; the data supports the plan
in [03-architecture.md](../03-architecture.md) §6.2 (post first, inline seam, decide in milestone M7)
but makes it likely that the seam will be used. Fan-out (≈ 39–40 µs per extra subscriber on the caller
in both systems) still argues against sending serially on the caller beyond the first few connections.
