# tcp_pubsub rewrite – Architecture

Status: **DRAFT for review.** Derived from [01-requirements.md](01-requirements.md) (what),
[02-wire-protocol.md](02-wire-protocol.md) (frozen bytes) and [04-api.md](04-api.md) (API shape),
and informed by the measurements in [baseline/README.md](baseline/README.md). Every design
element cites the requirement IDs it implements; section 12 is the reverse index.

An implementer (human or agent) should be able to build the library from this document plus the
three documents above. Where a detail is deliberately left to the implementer it says so.

## 1. Decisions this document is built on

| Decision | Value | Source |
|---|---|---|
| Lifetime model (OD-1) | **A**: every handle co-owns a shared *core*; the user's `Context` need not outlive publishers/subscribers | owner |
| Context | thin copyable value wrapper around `shared_ptr<Core>`; one process-wide max message size on it (OD-3, 256 MiB) | owner |
| Lifecycle | RAII only; no `cancel()`/`close()` on publisher/subscriber; `Subscription` handle ends a session | owner |
| Publisher creation | factory returning `Result<Publisher>`; a constructed publisher is always running | owner |
| Publish hot path (OD-10) | **post to the I/O loops, one post per loop**; an inline-send fast path is a *later optimisation* behind a marked seam, only if the prototype benchmark justifies it | owner |
| Low-latency busy-poll mode (OD-11) | out of scope; the design keeps one extension seam (7.5) | owner |
| Delivery | latest-wins only; default **pooled** (handler on a shared callback pool); *inline* is an opt-in seam (DEL-7) | owner |
| Callback pool (OD-12) | eCAL-style **dynamic pool**: grows on demand up to a maximum, queues beyond it (5.6) | owner |
| Subscriber slot (OD-6) | **one shared latest-wins slot per subscriber**; per-publisher isolation = one `Subscriber` per publisher (cheap: no thread per subscriber) | owner delegated, chosen for simplicity |
| Networking library (OD-8) | keep standalone Asio (header-only, `ASIO_STANDALONE`), used *only* inside `src/`; no Asio type in public headers (PLAT-4) | proposal, cf. PLAT-5 |
| Language | C++17 | owner |
| Wire protocol | unchanged v0, incl. documented new behaviour for empty messages | wire spec |

## 2. Design principles

1. **One owner per socket.** Every socket, timer and connection state machine belongs to exactly
   one *loop* (a thread running a single-threaded event loop). Everything that touches it runs on
   that thread. No strands, no per-socket mutexes, no cross-thread `close()` (TH-3). This removes the
   root cause of the races in the current implementation (defects D-1, D-2).
2. **Other threads send messages, they do not reach into connections.** User threads and pool
   threads hand work to a loop with a post; results come back through atomics and immutable
   snapshots.
3. **User code never runs on a loop and never under a library lock** (TH-4): handlers, event
   handlers and the logger are called from the callback pool, inside a *gate* (5.1), without holding
   any library lock.
4. **Immutable shared frames.** A published message becomes one immutable, reference-counted frame
   that all connections transmit (FR-PUB-8); a received message is one immutable shared buffer
   (FR-SUB-6).
5. **Latest-wins by overwriting a slot**, never by queueing (DEL-1): one slot per loop mailbox, one
   pending slot per connection, one mailbox slot per subscriber.
6. **Internal destructors never log and never call user code.** Only the explicit close protocol
   (section 8) may do so, and only before it completes (LT-1).
7. **Make the hot path lean, measure it:** acceptance numbers come from
   [05-benchmark.md](05-benchmark.md) (PERF-1/2).

## 3. Component overview

```
 user threads                                   library threads
 ───────────                                    ──────────────────────────────────────────
 Context ─┐ (thin handle, copyable)
 Publisher ├──► Core (shared)  ┌── Loop 0 … Loop L-1 : 1 thread + 1 io_context each
 Subscriber│     owns          │      owns sockets, timers, PubConn / SubConn objects
 Subscription ─┘               ├── CallbackPool 0 … C-1 : handlers, events, log delivery
                               ├── Resolver (lazy, 1 thread) : blocking name lookups
                               ├── BufferPool, LogDispatcher, Limits
                               └── thread-local "am I a library thread" marker
```

| Component | Lives in | Responsibility |
|---|---|---|
| `Core` | shared, owned by all handles | Starts/stops threads, holds options (max message size, keep-alive, timeouts), `BufferPool`, `LogDispatcher`, loop selection (round-robin) |
| `Loop` | `Core` | Thread + `asio::io_context` (concurrency hint 1) + work guard; runs everything that touches sockets; *state shared with its thread* (`LoopState`), see 8.4 |
| `CallbackPool` | `Core` | Dynamically growing thread pool (5.6) executing user handlers, events and logger calls; serial queues per object (5.2) |
| `Resolver` | `Core` | One lazily started thread for blocking host-name resolution; IP literals never use it (REC-4) |
| `PublisherState` | shared by `Publisher` handle and loops | Acceptor, loop mailboxes, registry of streaming connections, sequence counter, gate |
| `PubConn` | owned by its `Loop` | One accepted connection: handshake, write path, EOF detection |
| `SubscriberState` | shared by `Subscriber` handle and loops | Sessions, one-slot mailbox, handler, gate |
| `SubSession` / `SubConn` | owned by its `Loop` | One logical session: resolve → connect → handshake → receive, with reconnect/backoff |
| `Frame` / `Message` / `BufferPool` | `Core` | Pooled byte buffers; frame = header + payload, immutable once published (5.3) |
| `Gate` | per object | Run-down protection for user-code invocations (5.1) |
| Wire codec | `src/wire/` | Pure, I/O-free frame parser and builders; unit-tested with byte-by-byte input (WP conformance) |

## 4. Threads and execution

### 4.1 Thread budget (TH-1, TH-2, TH-5)

| Threads | Default | Configurable | Purpose |
|---|---:|---|---|
| I/O loops | **2** | `ContextOptions::io_threads` (≥ 1) | all socket work |
| Callback pool | **2** pre-started, grows on demand to a maximum of **16** (tentative) | `callback_threads` (initial, ≥ 1), `max_callback_threads` (0 = unlimited, as in eCAL) | handlers, events, logging |
| Resolver | 0–1 | – | started on first non-literal host name |
| **Total** | **4–5 when handlers are quick; up to 2 + 16 + 1 when many handlers run or block at the same time** | | not proportional to the number of publishers, subscribers, sessions and connections; the pool only grows with the number of handlers running *concurrently* (baseline for comparison: 8 threads for one subscriber, 108 for 100 subscribers, regardless of load) |

Threads are named (`tps-io-N`, `tps-cb-N`, `tps-res`) where the OS supports it (TH-5). Zero
threads is rejected by the `Context` constructor (LT-10, ROB-9).

Connections are distributed over the loops when they are created (accepted sockets and sessions:
round-robin, ties broken by lowest connection count) and **never migrate**. The acceptor of a
publisher lives on one loop; it accepts *directly into a socket that belongs to the target loop*
(`acceptor.async_accept(target_context, handler)` style) so that no hand-off is needed.

### 4.2 What runs where

| Work | Runs on |
|---|---|
| accept, read, write, connect, close, timers, handshake, reconnect/backoff, parsing frames, pending-slot replacement, ticket slot updates | the connection's **loop** |
| building the frame (one copy), enqueueing it to the loop mailboxes, atomic counters, queries | the **user thread** calling `publish()` / query |
| user message handler, event handlers, logger | **callback pool** (inside the owning object's gate) |
| `getaddrinfo` | **resolver** thread |

The only code that waits is the *close barrier* (section 8) executed by a **non-library** thread.
Library threads never wait on each other (this removes the deadlock class "A waits for B waits for A").

### 4.3 Library-thread detection (LT-2, LT-12)

Each library thread sets a `thread_local` marker at start. `isLibraryThread()` is used for exactly
two decisions: (1) a destructor running on a library thread does a *non-waiting* close
(8.2), and (2) `Core` destruction on a library thread detaches instead of joining (8.4). Blocking
user-facing helpers (`waitFor…`, `ticket.wait`) return an error on a library thread (CONN-8, 7.4).

## 5. Building blocks

### 5.1 `Gate` – run-down protection for user code (LT-1, LT-2, TH-4)

A small primitive used for **every** invocation of user code and every logger call.

```
class Gate {
  bool tryEnter();      // false once closed; otherwise active++ (atomic)
  void leave();         // active--; if closed && active==0, notify waiters
  void close();         // idempotent; after it returns tryEnter() always fails
  void waitIdle();      // blocks until active==0; MUST NOT be called on a library thread
};
struct GateSection { explicit GateSection(Gate&); bool entered; ~GateSection(); };  // RAII
```

Rules:

- Each `Publisher`, `Subscriber` and `Subscription` owns one gate. `Core` owns a gate for logging
  that is not attributable to an object.
- The callback pool runs a task as `if (GateSection s(gate); s.entered) task();` – a task of a
  closed object is silently discarded. Handlers run **without any library lock held**.
- `~X` does: `gate.close()` → close connections (8.2) → (non-library thread only) `gate.waitIdle()`.
  After `waitIdle()` returns no handler/event/logger call of that object is running or will start (LT-1).
- A destructor on a library thread (typically: a handler destroying "its own" object) closes the gate
  and does **not** wait (LT-2): the running handler returns normally, nothing new starts.
- Cost on the pool path only; the I/O loops never enter a gate.

### 5.2 Serial queues on the callback pool (DEL-6, CONN-7)

Handlers and events of one object must run serially and in order, while the pool is shared by all
objects. Pattern (used for the message mailbox, the event queue, and log delivery):

```
struct SerialQueue {            // one per object and kind
  mutex m;  bool scheduled = false;   // + the queued items
  void push(item):  lock; store item; if (!scheduled) { scheduled = true; pool.submit(drain); }
  void drain():     loop { lock; take item (or scheduled = false and return); unlock; run item under gate }
};
```

- At most one `drain` of a queue is scheduled/running at a time → calls are serial and ordered.
- One queue never occupies more than one pool thread; a slow handler blocks only that thread.
- **Message mailbox** is a `SerialQueue` with a *single-slot* policy shared by **all sessions of
  the subscriber** (OD-6): `push(source_session, msg)` overwrites an unconsumed message (latest-wins,
  DEL-3); the dropped message's buffer returns to the pool and the drop counter is incremented.
  The source session is passed on `push` so that a per-session slot policy (ready-list of sessions
  with a pending message, one slot each) can replace the single slot later without touching the
  parser, the sessions or the public API; it would cost a ready list plus removal handling when a
  session closes, and raise the memory bound from one pending message to one per session.
  **Event queue** is a normal bounded FIFO (e.g. 256 entries,
  drop-oldest with a "events lost" counter so that a stuck handler cannot grow memory).
- The pool submits with one lock + condition-variable notify (5.6). (Measured cost of a cold hand-off
  to a sleeping thread: 20–55 µs, [baseline §7](baseline/README.md); this is the price of isolation
  and is accepted for the default mode.)

### 5.3 Buffers, frames, messages (PERF-3, PERF-4, ROB-2, LT-7)

- **`Buffer`**: raw storage, **uninitialised** (`new char[]`, no zero fill; the baseline shows a fresh
  1 MiB buffer costs 13× a memcpy), with capacity ≥ size.
- **`BufferPool`** (in `Core`): size classes (powers of two from 4 KiB) with per-class free lists
  protected by a small mutex each (first implementation; a lock-free list is an optimisation to be
  justified by `Api_Publish`). Caps: total idle bytes (default 128 MiB) and idle buffers per class;
  buffers above the cap are freed instead of recycled, so one huge message cannot pin memory (ROB-2).
- **Pool lifetime (LT-7):** a pooled buffer's deleter holds a `shared_ptr<PoolState>` where
  `PoolState` is a separate small object that is *not* the `Core`. A `Message` therefore stays
  valid after the subscriber, the context and all threads are gone; returning the buffer to a pool
  nobody uses any more just frees it.
- **`Frame`** (publisher side): one pooled buffer laid out as `[12-byte header][payload]` (WP-D2),
  plus `sequence`, `max_conn_id` (5.4) and an optional ticket pointer. **Immutable after
  `publish()` returns**; shared by loops/connections through an intrusive reference count (the
  count is the only mutable part). The `Frame` object itself is pooled together with its buffer to
  avoid a `make_shared` allocation per publish (PERF-4).
- **`Message`** (subscriber side): shared pointer to a pooled buffer holding exactly the payload,
  plus `source` endpoint and `receivedAt` (FR-SUB-6/7). Empty messages are valid, non-null,
  zero-size (FR-SUB-12).
- Ownership of a published payload: **one copy** from the user's parts into the frame, none per
  subscriber (FR-PUB-4/8, PERF-3).

### 5.4 Connection ids and the streaming registry

Every connection gets a monotonic `conn_id` when it reaches *Streaming* (after the handshake
response is written, WP-H7). A frame carries `max_conn_id` = the highest id that existed when it was
published. A connection transmits only frames with `conn_id <= frame.max_conn_id`; frames published
before it became streaming are ignored (DEL-2: "frames older than the handshake MAY be dropped").

Each `PublisherState` has a **registry** (small mutex-protected vector, *never on the untracked
hot path*): `{conn_id, loop, endpoint info, shared stats block}`. Written by loops on
handshake/close, read by queries (`subscribers()`, CONN-2) and by *tracked* publishes (ticket
creation, 6.5).

Atomics for the hot path: `streaming_total` (FR-PUB-7, CONN-1) and `streaming_per_loop[l]`.

### 5.5 Published state: snapshot cells (CONN-1…CONN-6)

State that users query (session state, endpoints, counters) is owned by the loop and published as
an immutable `shared_ptr<const Info>` in a **snapshot cell** (a pointer copied/swapped under a tiny
mutex, or `std::atomic_load/store` on `shared_ptr` – both valid in C++17; the choice is an
implementation detail). Counters (messages/bytes/dropped) are relaxed atomics read directly.
Queries never touch sockets (TH-3, defect D-2). Endpoint strings are computed once at connect time.

### 5.6 `CallbackPool` – dynamic thread pool (DEL-6, TH-1, OD-12)

Modelled on eCAL's `DynamicThreadPool` (`ecal/dynamic_threadpool`, which tcp_pubsub cannot depend on:
eCAL uses tcp_pubsub, not the other way round). Behaviour of that pool, which we keep:

- `post(task)` locks, enqueues, and then either **wakes an idle worker**, or **spawns a new worker if the
  pool is below its maximum**; beyond the maximum tasks simply stay queued until a worker frees up.
- Workers are plain threads looping `wait(task or shutdown) → run task outside the lock`.
- Shutdown: no new posts accepted; queued tasks are still executed; then workers exit and are joined.

Adaptations required by this architecture:

1. **Correct idle accounting.** eCAL's pool decides with `idle_count > 0`, but `idle_count` is only
   decremented when a woken worker runs. Two quick posts can therefore both see "one idle worker",
   notify it once, and leave the second task queued although the pool is below its maximum (an
   avoidable latency/starvation case). Use `idle_count > queued_tasks` (tasks not yet claimed) as the
   "an idle worker will take it" condition; otherwise spawn if below max.
2. **Pre-started workers.** `callback_threads` workers (default 2) are started at `Core` creation, so the
   first message after start does not pay thread creation (~100 µs); growth to `max_callback_threads`
   (default 16, tentative; 0 = unlimited as in eCAL) is on demand. Idle workers are **not** retired
   (as in eCAL); an optional idle timeout is a possible later refinement.
3. **Task type and allocation.** Tasks are moved into the queue; the per-object `SerialQueue` re-uses a
   small drain task (one allocation per schedule at most), keeping the post cheap relative to the
   20–55 µs wake-up.
4. **Teardown on a library thread (LT-12).** eCAL's `Join()` would self-join if called from a worker.
   Here the worker threads share a `PoolState` with the pool object; `~CallbackPool` on a non-library
   thread joins, on a library thread it detaches all workers (8.4). Queued tasks are still executed
   after shutdown (as in eCAL): they are gate-guarded and are no-ops once their object is closed, so
   this is bounded (LT-3).
5. **Thread names and the library-thread marker** (4.3) are set by the worker start function.

Consequence for the thread budget (TH-1): the number of pool threads equals the peak number of
handlers running or blocked **at the same time** (capped by the maximum), not the number of
subscribers. With a burst of 100 subscribers receiving simultaneously the pool grows to the cap; with
quick handlers it stays at its pre-started size. A handler that blocks forever occupies one worker;
only when the cap is reached by blocked handlers can other subscribers starve (documented limit).

**Licensing note:** the eCAL file is Apache-2.0, tcp_pubsub is MIT. The semantics above are simple
enough to implement independently; copying the code verbatim needs a licence check with the
copyright holder.

## 6. Publisher design

### 6.1 Creation (FR-PUB-1/2, WP-S2/S3)

`Publisher::create(const Context&, options)`:
1. parse the address; open, set `SO_REUSEADDR` (Linux/macOS) / exclusive use (Windows, OD-5),
   `IPV6_V6ONLY=0` for `::` (WP-S3), bind, listen; **all synchronous on the calling thread**, so
   failure is returned immediately as `std::error_code` (FR-PUB-2);
2. choose the acceptor loop; post `startAccepting` to it; return the `Publisher` handle.

The publisher is accepting when `create` returns (LT-10). The bound port is stored in the state
(CONN-1; no socket query from user threads).

### 6.2 Publish path (FR-PUB-4…8, FR-PUB-12, PERF-3/4/5/7; OD-10)

```
publish(parts, options) on the USER thread:
  1. validate parts (nullptr with size>0 → InvalidArgument); total = Σ sizes; total > max → TooLarge
  2. if streaming_total == 0:        return Ok                       // FR-PUB-7: no copy, no lock
  3. frame = pool.acquire(12 + total); write header (WP-D1, LE); copy parts once
     (empty message: header only, WP-P2); frame.sequence = ++seq; frame.max_conn_id = latest_conn_id
  4. [tracked only] build ticket from the registry (6.5)
  5. for each loop l with streaming_per_loop[l] > 0:
        mailbox[l].lock;
          if (mailbox[l].latest) supersede(old)        // frame never reached any connection of l
          mailbox[l].latest = frame; need_post = !mailbox[l].drain_posted; drain_posted = true
        unlock; if (need_post) asio::post(loop[l], drainMailbox(state, l))
  6. return {Ok, sequence, ticket}
```

- The caller's cost is: validation + **one copy** + (loops × short lock + conditional post).
  With 2 loops that is ~2–35 µs cold, independent of the number of subscribers
  (baseline: raw send 180 µs, +40 µs per extra subscriber on the caller thread).
- **Latest-wins level 1 (per loop mailbox):** `latest` holds at most one frame; a newer publish
  overwrites it. Post coalescing (`drain_posted`) bounds posts to one per loop between drains.
- Several publisher threads are serialised only by the per-loop mailbox locks; messages of one
  thread stay ordered (FR-PUB-5): sequence is assigned before the mailbox insert, and the loop
  processes `latest` only (older ones are superseded, never reordered).
- **Seam for OD-10 (later):** step 5 is the only place an inline fast path would attach: for loop
  `l` and an idle `PubConn`, the *caller* could attempt a non-blocking `send` under that
  connection's mutex before falling back to the mailbox. It is deliberately not implemented first;
  see 13 (milestone M7) for the decision criterion.

### 6.3 Loop-side dispatch and connection write path (DEL-2, WP-D3, TH-3)

`drainMailbox(l)` on loop `l`: take `latest` (lock, clear `drain_posted`, unlock). For every
`PubConn` of this publisher on this loop with `state == Streaming && conn_id <= frame.max_conn_id`:
`conn.offer(frame)`.

`PubConn::offer(frame)` (loop thread only):

```
if (!writing)  { start write(frame) }                      // one async_write: header+payload contiguous
else           { if (pending) supersede(pending);          // latest-wins level 2
                 pending = frame; }
on write complete (ec):
   if ec → close(error)
   mark in-flight ticket slot Transmitted;  writing = false
   if (pending) { start write(pending); pending = null }
```

Invariants: **at most one write in flight per connection** (fixes D-1), the handshake response is
written through the same path before the connection becomes Streaming (WP-H6), a started frame is
always completed (WP-D3), and a connection holds at most two frames (in flight, pending; ROB-2).
Frames are kept alive by the connection's references only; the pool gets the buffer back when the
last connection releases it.

*Possible loop-internal optimisation (not required):* try a non-blocking `write_some` first and fall
back to `async_write` on partial write; measured as 20.6 µs vs 27.6 µs hot for a 64 B send.

Drops (DEL-8): a connection counts `dropped = (messages published while it was streaming) −
(messages it transmitted)` using sequence numbers, i.e. gaps in the sequence it was offered cover
mailbox overwrites as well as pending-slot replacements.

### 6.4 `PubConn` state machine (wire spec 5.1, WP-H*, WP-R*, ROB-4/6/7)

```
Accepted ──start──▶ ReadingHandshake ──valid request──▶ WritingResponse ──written──▶ Streaming
    │ (timeout WP-H10, bad first frame WP-H9, read error, close)                       │
    └──────────────────────────────────────▶ Closed ◀──(EOF | error | no-progress timeout | owner close)
```

- On accept: set `TCP_NODELAY`, keep-alive (WP-S3/S4); start the handshake timer (default 5 s).
  Pre-handshake connection count is capped (WP-L3); beyond the cap, accepted sockets are closed
  immediately. `EMFILE`/`ENFILE` on accept → log once, back off 100 ms…1 s, never spin, never
  stop the publisher (ROB-4).
- **Handshake read** uses the shared `FrameParser` (7.2) with the *publisher-side policy*: first
  frame must be `ProtocolHandshake` with `data_size ≥ 1`; reply version = `min(request, 0)` (WP-H3).
- **Streaming read side:** a small pending `async_read_some` into a 4 KiB scratch buffer detects
  EOF/RST immediately (WP-R1, D-5); bytes are parsed by the same parser and **discarded in constant
  memory**; zero-length frames are ignored (WP-P3). This is the only read after the handshake.
- Counts as a subscriber (CONN-1, `streaming_total`, registry) only when it enters *Streaming*
  (WP-H7); decremented before the connection's close completes.
- No-progress timeout (ROB-6, optional): a timer re-armed on every write completion while a write
  is in flight.
- **Close** (error, EOF, timeout or owner request): cancel timers, close the socket, unregister
  from registry and counters, mark in-flight and pending frames' ticket slots *Failed*, post an
  event (*subscriber disconnected*, CONN-7) to the object's event queue (5.2). Idempotent.

### 6.5 Delivery tickets (FR-PUB-14, API 4.2/4.3)

- Only when `track_delivery` is set: the publisher builds, under the registry lock, a `TicketBlock`
  with one **slot** per streaming connection (`{conn_id, loop, endpoint, atomic<state>}`), sorted by
  `conn_id`, and attaches it to the frame. Cost is paid by tracked publishes only (untracked: one
  null check).
- **Single writer per slot.** Initially the *publisher thread* owns the slots of a frame that is
  still in a loop's mailbox (so superseding a frame in `mailbox[l]` may write `Superseded` into
  the slots of loop `l` only, under that mailbox lock);
  once `drainMailbox` runs, ownership of each slot transfers to the loop that owns that connection
  (hand-over is the mailbox lock/post, which orders the writes). Loops write
  `Queued → Transmitting → Transmitted | Superseded | Failed` with release stores.
- **Terminal states always happen (FR-PUB-14.2):** during `drainMailbox`, a slot whose connection no
  longer exists on this loop is set *Failed*; connection close fails its slots; publisher close
  (8.2) drains all mailboxes and fails all remaining slots. Therefore `ticket.wait()` cannot hang.
- A ticket holds only the `TicketBlock` (shared): no reference to the publisher, loop, core or the
  frame memory (FR-PUB-14.3). Snapshot = one acquire load per slot (API 4.3).
- `ticket.wait()` uses a condition variable inside the block, notified when the last slot becomes
  terminal; calling it on a library thread returns an error (CONN-8).

## 7. Subscriber design

### 7.1 Subscriber, sessions, subscriptions (FR-SUB-1…5, FR-SUB-11)

`SubscriberState` holds the handler cell, the gate, the one-slot message mailbox **shared by all
its sessions** (OD-6), and the session table. `Subscriber::connect(candidates, options)` creates a
`SessionControl` (snapshot cell, counters,
closed flag, gate for events), chooses a loop, posts `start` to it, and returns a `Subscription`
that holds `SessionControl` and a `Core` reference. Destroying the `Subscription` closes the session
(8.2) on its loop. The loop owns the `SubSession`; the control block never touches a socket.
Destroying the `Subscriber` closes all its sessions.

**Object model for N publishers × M subscribers:** a `Subscriber` is one consumer (one handler, one
mailbox) with any number of sessions, one per publisher it listens to. Receiving from N publishers in
M consumers therefore needs N `Publisher`, M `Subscriber` and N×M sessions/TCP connections (not
N×M subscribers). A consumer that needs per-publisher isolation (separate handler or separate
latest-wins slot per publisher) uses one `Subscriber` per publisher; this costs only a mailbox and a
gate, because no thread is bound to a subscriber (5.6).

### 7.2 Receive path (PERF-3/5, WP-F*, WP-L*)

The wire layer is a pure, incremental **`FrameParser`** (no I/O, no allocation of payloads by
itself):

```
FrameParser p(max_payload);
Result r = p.feed(bytes, n);   // → events: HeaderComplete{type, data_size}, PayloadBytes{ptr,len}, FrameComplete, ProtocolError
```

It implements WP-F1…F4 (header size < 2 after LE conversion, short and long headers, skipping
unknown header bytes in constant memory), WP-M1 (unknown types skipped without delivery), WP-P1/P3,
WP-L1 (`data_size > max_payload` → `ProtocolError` *before* any allocation), and never allocates
from a peer-supplied size beyond the limit (WP-L2). It is unit-tested exhaustively with
byte-by-byte and coalesced input, independent of sockets.

`SubConn` read loop (on its loop):
1. one `async_read_some` into a **read-ahead buffer** (64 KiB, allocated while connected) – small
   messages need on average ≤ 1 read call (PERF-5; the current implementation uses 3);
2. feed the parser; on `HeaderComplete` for `RegularPayload` with `data_size > 0`, acquire a
   pooled `Buffer` of exactly that size (catching `std::bad_alloc` → close with error, ROB-3);
   bytes already in the read-ahead buffer are copied, the rest is read **directly into the payload
   buffer** (`async_read` of the remainder) for large payloads (PERF-3);
3. on `FrameComplete`: build the `Message`, `mailbox.push(msg)` (5.2), update counters, continue.
   `data_size == 0` → an empty `Message` (FR-SUB-12).

### 7.3 Session state machine and reconnect (FR-SUB-2/9/10/11, REC-1…REC-7)

```
              ┌──────────────────────────────── retry delay (Backoff) ◀──────────┐
Resolving ──▶ Connecting ──▶ Handshaking ──response ok──▶ Connected(Streaming)    │
   │ fail         │ fail         │ fail/timeout/bad version        │ EOF/error ───┘
   └──── next candidate / all failed ───▶ Backoff ──▶ Resolving …   └─▶ (retries exhausted) Closed
```

- **Candidate iteration** (FR-SUB-2): for each `(host, port)` in order, for each resolved address in
  order; first that completes TCP *and* the handshake wins; the configured pair that worked is
  recorded for `getConnectedPublisher` (as the user wrote it) together with the resolved endpoint.
- **Resolution:** IP literals are parsed inline (no thread). Other names go to the `Resolver`
  thread with a callback that posts the result back to the session's loop. A lookup in progress
  cannot be interrupted; if the session closes meanwhile, its result is discarded (REC-4). The
  resolver is never joined while busy (8.4), so teardown does not wait for DNS.
- **Handshake:** send the request (WP-H5), start the handshake timer (WP-H10/REC-5); *Connected* is
  entered only after a valid response (FR-SUB-10, WP-H8); a version greater than 0 fails the attempt
  (WP-H4).
- **Retry** (FR-SUB-11, REC-2/3): `retries_left` semantics as specified; the counter is reset after
  a successful handshake. **The delay timer is re-armed with `expires_after` for every retry** (defect
  D-3: the current code's timer fires immediately after the first use). Default 1 s, optional
  exponential backoff with cap and jitter. Cancelling the session or destroying the owner cancels
  the timer on the loop immediately (REC-3).
- On *Connected → lost*: counters/state snapshot updated, event emitted (CONN-7), then backoff.
  When retries are exhausted: state `Closed` with last error (REC-6); the session removes itself
  from the subscriber's table; the `Subscription` keeps reporting the terminal state (LT-8).
- Every attempt sets `TCP_NODELAY` and keep-alive, and applies SIGPIPE protection (10).

### 7.4 Delivery to the user (DEL-3/5/6/7/9, FR-SUB-8)

**Default (pooled) mode:** `mailbox.push(msg)` as in 5.2. The pool thread calls the handler under the
subscriber's gate, inside `try/catch` (DEL-9, ROB-5); the loop is never blocked by user code (PERF-7),
and the session keeps reading while the handler runs (DEL-4: bounded memory because the mailbox
holds ≤ 1 pending message and the receive buffer is bounded).

**Handler replacement (FR-SUB-8):** the handler lives in a `shared_ptr<const Handler>` cell. A task
copies the pointer before calling (so a replaced handler stays alive while it runs), and
`setHandler` swaps the cell under a tiny mutex and **returns without waiting** for a running call;
the old handler is never *started* again after the swap. Calling `setHandler` from inside a handler
is therefore safe (no lock is held during the call – the defect D-7 deadlock cannot occur).
Documented consequence: the old handler's captured state must stay valid until the call that is
already running has returned; destroying the *subscriber* (not just replacing the handler) does wait
(8.2).

**Events (CONN-7):** state changes and connect/disconnect notifications are pushed to a per-object
`SerialQueue` (FIFO), executed on the pool under the object's gate, never on a loop, never under a
library lock; after the owner's gate closed none is delivered (LT-1).

**Blocking helpers (CONN-8):** `waitForSubscribers(n, timeout)`, `waitUntilConnected(timeout)`,
`ticket.wait(timeout)` wait on condition variables notified by loops *outside* their locks; they
return `WrongThread` on a library thread.

### 7.5 Seams for later (OD-2/DEL-7, OD-11)

- **Inline delivery** (opt-in per subscriber): `SubConn` calls the handler directly on its loop
  inside the gate and, after it returns, drains the frames already buffered (non-blocking reads)
  and delivers only the newest, preserving latest-wins (DEL-7). It affects only step 3 of 7.2.
  Destroying objects from an inline handler uses the non-waiting close like any library thread.
- **Low-latency mode:** a per-loop option "spin for N µs before blocking" would be implemented by a
  custom `io_context` run loop (`poll()` for a bounded time, then `run_one()`); the `Loop` class
  keeps its run function in one place so this can be added without touching connections.

## 8. Lifetime and teardown

### 8.1 Ownership graph

```
Context ─┐
Publisher ──strong──▶ Core ◀──strong── Subscriber, Subscription     (option A: handles co-own the core)
Publisher ──unique──▶ PublisherState (shared with loops/pool tasks via shared_ptr)
Subscriber ─unique──▶ SubscriberState (same)
Subscription ──shared──▶ SessionControl (no pointer to a socket; weak link to SubSession)
Loop ──owns──▶ PubConn / SubSession   (shared_ptr captured by their pending async handlers)
Message, DeliveryTicket, DeliverySnapshot ──shared──▶ only their own data (+ PoolState for Message)
```

Rules: **library threads never hold a strong reference to `Core`** (otherwise the last release could
happen on a library thread and cycles would appear); loops and pool threads hold only their own
`LoopState`/`PoolState`. State objects (`PublisherState`, `SubscriberState`, `SessionControl`,
connections) hold **no** `Core` reference either: only the user-facing handles do. `Message` and
tickets hold no `Core`, so they can outlive everything (LT-7,
FR-PUB-14.3). Handles are move-only (LT-9); a moved-from handle owns nothing.

### 8.2 Close protocol (LT-1…LT-6)

`~Publisher` / `~Subscriber` / `~Subscription` (all idempotent, LT-5):

1. `gate.close()` – no user callback, event or log record of this object can start.
2. Tell the owning loops to close the object's connections: post one *close* task per affected
   loop. On the loop: cancel timers, close the acceptor/sockets, set `closed`, fail ticket slots,
   unregister counters/registry entries, discard mailbox contents.
3. **If the calling thread is not a library thread:** wait until all close tasks have run (a latch)
   and then `gate.waitIdle()` (waits only for this object's currently running handler/event/log
   task).  **If it is a library thread: do not wait** (LT-2); steps 1–2 guarantee that nothing
   further starts and a running handler returns normally.
4. Release the strong `Core` reference (may trigger 8.4).

After step 3 on a user thread: all sockets of the object are closed, no user code of it is running
or will run, and no library thread touches memory the user may free (LT-1). Operations still
pending in a loop (aborted completions, cancelled timers) only access internal state kept alive by
the `shared_ptr`s of their handlers; they check `closed` first and neither log nor call user code
(principle 6). Teardown never waits for another object's work or for an unbounded drain (LT-3).

`LT-6`: `closed` is terminal for connections, sessions and publishers; no operation restarts them.

### 8.3 Supported destruction orders (LT-4)

| Situation | Behaviour |
|---|---|
| Any order of `Publisher`, `Subscriber`, `Subscription`, `Context` copies on user threads | supported; each object closes itself; `Core` stops when the last handle is gone |
| User destroys their `Context` while publishers/subscribers exist | supported (they co-own the core, OD-1/A) |
| Handler/event handler destroys its own subscriber/publisher/subscription | supported; non-waiting close; handler returns normally (LT-2) |
| Handler holds the **last** reference to the `Core` (e.g. captured a `Context` copy) | supported; see 8.4 (LT-12), the one documented exception |
| `Subscription` outlives its `Subscriber` | supported; reports terminal state, destruction is a no-op (LT-8) |
| `Message` / ticket outlive everything | supported (LT-7, FR-PUB-14.3) |
| User *thread* uses a handle concurrently with its destruction | user's responsibility (ordinary C++ rule) |
| Use after program exit / static destruction | not supported, but no hang (LT-11); the library keeps no global objects with threads |

### 8.4 `Core` destruction (LT-3, LT-12)

`Loop` and `CallbackPool` keep everything their threads touch in a `shared_ptr`-owned
`LoopState`/`PoolState` that the thread function captures (as the current implementation already
does for the `io_context`).

`~Core` (the last handle was released):
1. All objects are closed already (their handles are gone), so every loop has only aborted
   completions and cancelled timers left. Reset the work guards and `stop()` the contexts (bounded
   work only; no draining of arbitrary timers, LT-3). Set the callback pool's stop flag and wake it
   (workers still run the already queued, gate-guarded tasks, 5.6).
2. **Non-library thread:** join all loop and pool threads. Then no library thread runs (T1).
3. **Library thread (LT-12):** a thread cannot join itself and joining others could deadlock.
   *Detach all threads.* They exit after their current handler returns; they touch only their
   `LoopState`/`PoolState`, call no user code and no logger.
4. Resolver: if a lookup is running, detach the thread (it discards its result and exits); else join.

Pending handlers that were never run are destroyed with their `io_context`; their destructors never
log or call user code (principle 6). Every object that owns Asio I/O objects (sockets, timers,
acceptor) holds a `shared_ptr<LoopState>`, so the `io_context` outlives them wherever their last
reference is dropped (this is what the old test `SubscriberSessionContextOutlivesItsAsioMembers`
protected against).

## 9. Errors, robustness, logging

- **Exception boundary** (ROB-5, PLAT-7): every asynchronous handler, task and thread function is
  wrapped by a helper that catches everything; `std::bad_alloc` while handling a connection closes
  that connection and logs once (ROB-3). Destructors are `noexcept` and never throw.
- **Malformed input:** the parser rejects, the connection closes, nothing else is affected
  (ROB-1, WP-C3). `EMFILE`: accept back-off (6.4). Socket errors map to a close reason carried in
  events and snapshots (CONN-4).
- **Limits:** `max_message_size` from the context checked by publisher (`TooLarge`) and parser; read-ahead,
  scratch and pending-handshake limits are fixed constants (ROB-2 worst case per connection:
  2 frames on the publisher side, 1 receive buffer + 1 pending message on the subscriber side).
- **Logging (LOG-1…4, TH-4, PERF-9):** `Core::log(level, lambda)` evaluates the lambda only if the level
  is enabled (no string building otherwise); `DebugVerbose` is compiled out in release builds.
  Records are queued to a `LogDispatcher` and delivered on the **callback pool** inside the owner's
  gate, so a slow logger can never block an I/O loop and no log call outlives the owner's teardown
  (LT-1). Queue bounded (e.g. 1024, drop-oldest with a "n messages dropped" record). Repeated
  identical errors (reconnect failures) are rate-limited to one per interval. Handlers expected during
  teardown (aborted operations) log nothing at Error level (baseline finding).
- Programmatic status (snapshots, counters, events) is the primary diagnostic channel; tests never
  parse log text (LOG-4).

## 10. Platform layer (PLAT-6, WP-S*)

All OS-specific socket behaviour is isolated in `src/platform/socket_options.{h,cpp}`:

| Concern | Linux | macOS | Windows |
|---|---|---|---|
| `SIGPIPE` | `MSG_NOSIGNAL` (Asio sets it) | `SO_NOSIGPIPE` on every socket (verify whether Asio does; otherwise set explicitly) | n/a |
| Listener reuse | `SO_REUSEADDR` | `SO_REUSEADDR` | `SO_EXCLUSIVEADDRUSE`, **no** `SO_REUSEADDR` (OD-5) |
| `IPV6_V6ONLY=0` for `::` | explicit | explicit | explicit |
| Keep-alive tuning | `TCP_KEEPIDLE/INTVL/CNT` | `TCP_KEEPALIVE` (+`TCP_KEEPINTVL/CNT`) | `SIO_KEEPALIVE_VALS` |
| Thread names | `pthread_setname_np` | `pthread_setname_np` (current thread) | `SetThreadDescription` |
| Event mechanism | epoll | kqueue | IOCP (each loop its own `io_context`) |
| Winsock init | n/a | n/a | handled by Asio; no global objects of ours (LT-11) |

Nothing outside this layer uses `#ifdef _WIN32`-style branching except trivial includes.

## 11. Source layout

```
tcp_pubsub/
  include/tcp_pubsub/        public headers only: context.h publisher.h subscriber.h subscription.h
                             message.h bytes.h (ConstBytes) result.h delivery.h logger.h … (no asio types)
  src/
    wire/        frame_parser.{h,cpp}  frame_builder.{h,cpp}  handshake.{h,cpp}     (pure, no I/O)
    core/        core.{h,cpp} loop.{h,cpp} callback_pool.{h,cpp} resolver.{h,cpp} gate.h serial_queue.h
                 buffer_pool.{h,cpp} frame.h message.cpp log_dispatcher.{h,cpp} snapshot_cell.h
    publisher/   publisher.cpp publisher_state.{h,cpp} pub_conn.{h,cpp} ticket.{h,cpp}
    subscriber/  subscriber.cpp subscriber_state.{h,cpp} sub_session.{h,cpp} subscription.cpp
    platform/    socket_options.{h,cpp} thread_name.{h,cpp}
tests/           unit (wire, gate, serial queue, buffer pool) · conformance (raw sockets) ·
                 functional · lifecycle · reconnect · robustness · interop (pinned old release)
benchmarks/      (exists) adapter_v2.cpp to be added
```

Dependency rule: `wire/` depends on nothing; `core/` on `wire/` and `platform/`; `publisher/` and
`subscriber/` on `core/`; only `src/` includes Asio.

## 12. Requirements traceability (reverse index)

| Requirement(s) | Design element |
|---|---|
| FR-PUB-1/2/3/13 | 6.1 creation, 6.4 `PubConn`, 4.1 acceptor on a loop |
| FR-PUB-4/5/6/7/8/9/12 | 6.2 publish path, 5.3 frames, 6.3 latest-wins levels 1+2 |
| FR-PUB-10/11 | `PublishResult`, 8.2 (RAII only) |
| FR-PUB-14 | 6.5 tickets |
| FR-SUB-1…5, 11 | 7.1, 7.3 |
| FR-SUB-6/7/12 | 5.3 `Message`, 7.2 |
| FR-SUB-8, DEL-9 | 7.4 handler cell, `try/catch` |
| FR-SUB-9/10, REC-1…7 | 7.3 state machine, resolver, timers |
| DEL-1…5 | 5.2 single slot, 6.2/6.3 slots, 7.4 |
| DEL-6/7 | 5.2 serial queues, 5.6 dynamic pool, 7.5 inline seam |
| DEL-8, CONN-1…6 | 5.5 snapshots/counters, 6.3 sequence-gap drops, 5.4 registry |
| CONN-7/8 | 5.2 event queue, 7.4 helpers |
| CONN-10, WP-S4, WP-R1 | 6.4 pending read, 10 keep-alive |
| TH-1/2/5 | 4.1, 5.6 |
| TH-3 | principle 1, 4.2 |
| TH-4 | principle 3, 5.1, 9 logging |
| TH-6 | thread-safety table in the API documentation (to write with the headers) |
| LT-1/2/5/6 | 5.1 gate, 8.2 |
| LT-3 | 8.2, 8.4 (no unbounded drain, no waiting for other objects) |
| LT-4 | 8.3 |
| LT-7/8/9 | 5.3 pool lifetime, 8.1, handles move-only |
| LT-10/11 | 4.1, 6.1, 10 |
| LT-12 | 4.3, 8.4 |
| ROB-1…9 | 7.2 parser, 9, 6.4 |
| PERF-3/4/5 | 5.3, 6.2, 7.2 |
| PERF-6 | no polling timers; only keep-alive/retry/handshake timers |
| PERF-7 | 5.2, loops never run user code |
| PLAT-1…8 | 10, 11, build files |
| WP-* | `src/wire/`, 6.4, 7.2/7.3 |

## 13. Implementation plan (for an implementing agent)

Each milestone ends green on all platforms, with TSAN/ASAN where applicable (QA-10), and ships its
tests. No milestone may disable a test by platform.

| # | Milestone | Contents | Exit criteria |
|---|---|---|---|
| M0 | Wire codec | `src/wire/`: frame builder, incremental `FrameParser`, handshake codec | all WP conformance vectors (wire spec §6, §10 items 3–9, 11) as pure unit tests incl. byte-by-byte and coalesced input; fuzz/property test against random garbage never crashes or over-allocates |
| M1 | Core primitives | `Gate`, `SerialQueue`, `BufferPool`/`Frame`/`Message`, `Loop`, dynamic `CallbackPool` (5.6), `Core` + `Context` | unit tests: gate run-down incl. self-enter, serial ordering, buffer-pool caps and lifetime after the pool owner is gone, loop start/stop, **LT-12** (last release on a library thread), callback pool: pre-start size, growth only when no idle worker, the two-quick-posts case (second task must not wait while below the maximum), cap respected and queueing beyond it, blocking handlers do not starve others below the cap, shutdown from a worker thread |
| M2 | Publisher | `Publisher::create`, `PubConn`, mailbox/post path, registry, queries | conformance with raw sockets (golden bytes, fragmentation, handshake violations, oversize), functional tests, `Api_Publish` benchmark ≥ as good as v1 and **independent of subscriber count** |
| M3 | Subscriber | `Subscriber`, `Subscription`, `SubSession` incl. parser integration, mailbox + pool delivery, handler replacement | functional tests from the existing suite on ephemeral ports; interop with pinned old release (both roles); latest-wins and slow-consumer tests |
| M4 | Reconnect and resolver | backoff, candidate lists, resolver thread, handshake timeouts | publisher-restart test, no-spin test, cancel-during-backoff test, publisher list with unresolvable first entry |
| M5 | Observability | snapshots, counters, events, wait helpers, logger dispatch | CONN-* tests; no log call after teardown (LT-1) |
| M6 | Delivery tickets | `TicketBlock`, slot protocol, snapshot/wait | tests for transmitted/superseded/failed-by-close/failed-by-teardown per peer; allocation-free when unused |
| M7 | Benchmarks and hot-path decision | `adapter_v2`, run `tcp_pubsub_benchmark` + micro-benchmarks, record baseline of the rewrite | **PERF-2 acceptance** vs [baseline](baseline/README.md) (medians of ≥ 3 runs, tolerance = baseline spread); decide OD-10: implement the inline fast path (6.2 seam) *only if* it improves end-to-end p50/p99 for W-1 by more than the baseline spread without hurting fan-out |
| M8 | CI and hardening | TSAN/ASAN/UBSAN jobs, repeat-run flake detection, 1000× create/destroy stress, old-release interop job, CI matrix | all CI jobs green, no disabled tests |
| M9 | Docs and migration | README, thread-safety table (TH-6), migration guide, changelog | DOC-1…6 |

## 14. Risks and open items

| Risk / open item | Mitigation |
|---|---|
| Hot path "post" adds a thread hop (+20–55 µs cold) vs an inline send | measured decision in M7 (seam in 6.2); the baseline shows the hop is small against a 10 ms period but not zero |
| Blocking handlers occupy callback-pool workers; once the pool maximum is reached by *blocked* handlers other subscribers can still starve; a burst of simultaneous handlers grows the pool to its maximum and workers are not retired | dynamic pool (5.6) with a configurable maximum (tentative 16); document the limit; optional idle retirement, `Inline` mode and a dedicated-thread opt-in remain later seams; tests with blocking handlers |
| One shared mailbox slot per subscriber lets a fast publisher shadow a slow one within the same subscriber | accepted (OD-6); one `Subscriber` per publisher gives isolation; per-session slots possible later behind the `push(source, msg)` seam (5.2) |
| Single registry mutex used by queries and tracked publishes | not on the untracked hot path; measured in M6 |
| Asio specifics to verify: cross-context `async_accept`, `SO_NOSIGPIPE` on macOS, `io_context` concurrency hint on IOCP | prototype tests in M1/M2 on all three OSes before relying on them |
| `std::atomic<shared_ptr>` unavailable in C++17 | mutex-protected cell or `atomic_load/store` free functions (5.5) |
| Timer/DNS behaviour differs per OS (resolver cannot be interrupted) | 7.3: result discarded, thread detached at teardown |
| Detached threads after LT-12 may outlive the process's static state | threads touch only their own state and never log/call user code (8.4); lifecycle test under TSAN |
| Latency numbers are from one Windows laptop | Linux/macOS baselines before M7 acceptance (05-benchmark §5) |

### Feedback to the other documents

- `04-api.md`: the event API (CONN-7) and the `waitFor…` helpers are not yet specified there; the
  message handler replacement semantics ("returns without waiting; running call may finish")
  must be stated in the thread-safety table; `Context` options gain `io_threads` (default 2),
  `callback_threads` (initial pool size, default 2), `max_callback_threads` (default 16, tentative;
  0 = unlimited), buffer-pool caps and log queue size; and a section on the N publishers × M subscribers
  object model (see 04-api.md §7.1).
- `01-requirements.md`: DEL-4 and QA-7 were corrected while writing this document (the subscriber keeps
  reading in pooled mode; concurrent user-thread use during destruction is the user's responsibility).
  OD-1 (A), OD-10 and OD-11 are decided as stated in section 1.
