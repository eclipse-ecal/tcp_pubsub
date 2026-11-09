# tcp_pubsub rewrite – Requirements

Status: **DRAFT for implementation planning.** Wire protocol is frozen and specified
in [02-wire-protocol.md](02-wire-protocol.md). The public API, the internal
architecture and the class design are **not** fixed by this document; they are to be
derived from it (follow-up documents: `03-architecture.md` – to be written, and
[04-api.md](04-api.md) – draft design notes).

**Scope rule: this document is API-neutral.** Requirements describe *capabilities and
observable behaviour* ("a message can be published", "the connection state can be
queried"), never function names, signatures, types or error-reporting style
(`send(ptr,size)`, `std::string_view`, exceptions vs. result codes, `cancel()`,
callbacks vs. handles, ...). Wherever the current API is mentioned it is only to
describe existing behaviour. Everything about *how* a capability is exposed is decided in
`04-api.md` and must trace back to the requirement IDs here.

Key words **MUST**, **SHOULD**, **MAY** are used as in RFC 2119. Every requirement has an
ID so that tests, code comments and reviews can refer to it. Requirements marked
*(existing)* describe behaviour of the current implementation that users rely on;
*(new)* means the current implementation does not provide it.

---

## 1. Purpose and scope

tcp_pubsub is a small C++ library for **low-latency one-to-many publish/subscribe of
opaque binary blobs over TCP** (a publisher accepts connections, subscribers connect
and receive the messages). It is a transport building block (the samples
`samples/ecal_to_tcp` and `samples/tcp_to_ecal` bridge it to eCAL); it deliberately does
not define a message format, discovery, QoS or history.

This rewrite replaces the **implementation** (and may replace the public API) to make
it simple, free of data races, and deterministic at teardown, while keeping the
**wire protocol bit-compatible** with all existing releases.

In scope: publisher, subscriber, reconnect, threading/execution model, lifecycle,
diagnostics, build, tests, documentation.

## 2. Goals and non-goals

**Goals**

- **G-1** Low and *predictable* latency for latest-value data (see section 3, 12).
- **G-2** No data races, no undefined behaviour, no deadlocks in any documented usage;
  verified with TSAN/ASAN in CI.
- **G-3** Deterministic, easy-to-reason-about start-up and teardown (section 10).
- **G-4** Small, readable implementation. A new contributor should be able to explain
  who owns each socket and each thread after reading one document.
- **G-5** Wire compatibility with all released versions in both directions.
- **G-6** Fixed, small number of threads independent of the number of connections.

**Non-goals**

- **NG-1** Security: no TLS, authentication, authorisation or encryption. The library
  is for trusted networks. (It still MUST be robust against malformed input, section 11.)
- **NG-2** Reliable delivery, history, replay, acknowledgements, ordering across
  publishers, topic names, discovery, schemas, compression.
- **NG-3** Queued (no-drop) delivery. Delivery is **latest-wins only** (section 7).
- **NG-4** Mobile (Android/iOS) and embedded/RTOS targets.
- **NG-5** Changing the wire protocol or adding protocol versions.
- **NG-6** Source/ABI compatibility with the current public headers. (A migration note
  is a deliverable, section 15.)

## 3. Usage context and workload

Decisions taken with the project owner:

| Item | Value |
|---|---|
| **W-1** Small messages | up to ~64 KiB at **~100 Hz** per publisher |
| **W-2** Large messages | ~1–16 MiB at **~15 Hz** per publisher (existing test uses 16 MiB) |
| **W-3** Scale | **hundreds** of concurrent connections per process (publishers' subscribers plus subscriber sessions, sum) and hundreds of publishers/subscribers objects |
| **W-4** Typical topology | local host (loopback) and LAN; many publishers per process; applications with one subscriber per topic |
| **W-5** Latency concern | *jitter and tail latency* matter more than the mean: the 10 ms period of W-1 is the budget scale, not microseconds |
| **W-6** Slow consumers | normal operating condition. A slow subscriber MUST NOT slow down the publisher's caller or other subscribers |

## 4. Platforms, toolchain, packaging

- **PLAT-1** Supported: **Windows** (x64 and win32), **Linux** (x86-64 and arm64,
  Ubuntu 22.04/24.04 toolchains), **macOS** (current runner of the repo's CI;
  Apple Silicon and Intel). The CI workflows in `.github/workflows` define the matrix;
  all of them MUST be green.
- **PLAT-2** Language level: **C++17** minimum for the library and the public headers.
  (Current: C++14.) Public headers MUST NOT require a newer standard.
- **PLAT-3** Build system: CMake (existing minimum version policy `3.13...4.0`),
  library variants **static, shared, and object** (as in CI), `find_package(tcp_pubsub)`
  config package, install rules, CPack packaging, version header. Symbol export macro
  for shared builds.
- **PLAT-4** Public headers MUST NOT expose third-party types (no `asio::` types, no
  `recycle::`) and MUST NOT include third-party headers, so that consumers need none of
  the implementation dependencies.
- **PLAT-5** Third-party dependencies MUST be header-only or vendored under
  `thirdparty/`, and keep the existing options (e.g. `TCP_PUBSUB_USE_BUILTIN_ASIO`)
  semantics where still applicable. Standalone Asio remains acceptable; the final
  choice (Asio vs. another mechanism) belongs to the architecture document. Dependencies
  may be dropped, but none added without justification.
- **PLAT-6** macOS specifics that MUST be handled: no `SIGPIPE` (WP-S1), no reliance on
  Linux-only syscalls (`epoll`, `accept4`, `MSG_NOSIGNAL`), `SO_REUSEADDR` semantics
  (WP-S2). Windows specifics: Winsock initialisation, `SO_EXCLUSIVEADDRUSE`
  (OD-5), no `poll`-only assumptions.
- **PLAT-7** Exceptions and RTTI MAY be used internally, but no exception may escape
  from a library-owned thread or a destructor (ROB-5).
- **PLAT-8** The library MUST build warning-free at the repo's existing warning level
  and pass the repo's `.clang-tidy` configuration where it passes today.

## 5. Functional requirements – Publisher

Terminology: a **message** is an opaque byte sequence of any length ≥ 0 that the user
publishes and subscribers receive as one unit (one wire frame).

- **FR-PUB-1** *(existing)* A publisher listens on a configurable local address and
  port. Port 0 means "let the OS choose"; the actual port is available to the user after
  start. The address can be an IPv4 or IPv6 literal; the default is the IPv6 wildcard
  accepting IPv4 and IPv6 clients (WP-S3).
- **FR-PUB-2** *(new)* If binding/listening fails, the failure and its **reason** (OS
  error code and message) MUST be available to the user programmatically (not only via
  logging), and the publisher is reported as not running. (Existing: only a boolean.)
- **FR-PUB-3** *(existing)* A publisher accepts any number of subscribers
  (practical limit: system limits; see W-3) at any time while running, with no action
  from the user.
- **FR-PUB-4** *(existing)* A user can publish a message that is **composed of one or more
  separate contiguous parts** (gather, e.g. user header + payload) without concatenating
  them first. The parts are sent as *one* message (concatenated in the given order). The
  data is **copied** into library-owned memory before the publish operation returns; the
  caller's memory can be released/reused immediately. Parts may be empty. *(How the
  data is passed is API design: pointer+size, view, range, ...)*
- **FR-PUB-5** *(existing)* Publishing is **thread-safe** and may happen concurrently
  from multiple threads; messages from one thread are never reordered relative to each
  other; messages from different threads are serialised in some order, never
  interleaved or corrupted on the wire (WP-D3).
- **FR-PUB-6** *(existing)* Publishing MUST be **non-blocking with respect to the
  network**: it never waits for a subscriber, a socket, or a callback. Its cost is
  bounded by copying the payload once. It MAY take internal locks that are never held
  across blocking operations.
- **FR-PUB-7** *(existing)* If there is no subscriber in the streaming state, publishing
  does nothing, does not copy the data (cheap no-op) and is **not an error**.
- **FR-PUB-8** *(existing)* One copy per message regardless of the number of subscribers:
  all connections transmit the same immutable frame buffer (header + payload in one
  contiguous buffer, WP-D2).
- **FR-PUB-9** *(existing)* Latest-wins per subscriber connection (section 7).
- **FR-PUB-10** *(new)* **Immediate publish outcome.** The user learns synchronously, at
  least, whether the message was accepted by a running publisher, or why it was not:
  publisher not running (only if the API can have such a state), message larger than the configured maximum message size
  (OD-3), invalid input. A slow or stuck subscriber is **never** a failure of the publish
  operation (that is a drop, see DEL-2/DEL-8). The failure-reporting style is API design.
- **FR-PUB-11** *(existing)* A publisher can be stopped by destroying it (section 10).
  Whether an additional explicit stop operation exists is an API decision
  ([04-api.md](04-api.md)); if it exists it MUST be idempotent and thread-safe
  (LT-5). A stopped publisher reports "not running", rejects publishing (FR-PUB-10) and has
  closed all connections.
- **FR-PUB-12** *(new)* **Empty messages are first-class.** Publishing a message of total
  length 0 is valid, is not an error and not a no-op: each streaming subscriber's
  connection receives a header-only frame (WP-P2), and it takes part in latest-wins like
  any other message. (Existing publisher already sends such frames; existing
  *subscribers* ignore them, so empty messages are only seen by subscribers built from the
  rewrite. This is the only observable difference and it is documented, WP-P3.)
- **FR-PUB-13** *(existing)* A subscriber that disconnects is removed; the publisher
  keeps running and keeps serving the others. A subscriber that fails the handshake is
  dropped without affecting others.
- **FR-PUB-14** *(new, SHOULD – nice to have)* **Delivery status of a published message.**
  A user can opt in to learn what happened to a message they published, **per message and
  preferably per subscriber connection**, as it progresses through these states:
  *queued* (accepted, waiting for the connection), *transmitting* (first byte handed to
  the OS), *transmitted* (last byte accepted by the OS socket; **not** an acknowledgement
  by the peer, the protocol has none), *superseded* (replaced by a newer message before it
  started transmission, DEL-2), *failed* (connection closed or errored before completion,
  or publisher stopped). Constraints, because latency is the primary goal:
  1. It MUST cost nothing noticeable when unused: no additional per-message allocation,
     lock or atomic on the hot path for users who do not ask (PERF-4); the opt-in
     can be at publisher level or per publish.
  2. Every tracked message reaches a terminal state (*transmitted*, *superseded* or
     *failed*) for every connection it was assigned to, including at teardown (so a
     waiting user never hangs).
  3. It MUST NOT extend the lifetime of message memory, block teardown (LT-3), or call user
     code under library locks / on I/O threads (TH-4); the status can be polled and/or
     waited on with a timeout and/or notified on the callback pool.
  4. The per-connection part is optional; a single aggregate status ("number of
     connections it was transmitted to / dropped for / failed on") is acceptable.

## 6. Functional requirements – Subscriber

- **FR-SUB-1** *(existing)* A subscriber manages a set of **sessions**; each session
  is a connection to one publisher, identified by an **ordered list of candidate
  endpoints** `(host, port)`. Host can be an IP literal or a name that the OS resolves
  (including `.local`/mDNS names, IPv4 and IPv6).
- **FR-SUB-2** *(existing)* Candidate selection: endpoints of the list are tried in
  order; every address a name resolves to is tried; the first one that completes the TCP
  connection and the handshake is used (existing test `publisher_list_test`). The
  endpoint that worked is queryable (*as given by the user*, e.g. "localhost").
- **FR-SUB-3** *(existing)* Sessions can be added at any time and from any thread,
  independently of whether a callback is set or the publisher exists yet (the session
  keeps trying per section 9).
- **FR-SUB-4** *(existing)* Several sessions (several publishers) share one subscriber and
  one callback (existing test `multiple_publishers_test`); several subscribers can
  connect to the same publisher (existing test `multiple_subscribers_test`); several
  sessions to the same publisher are allowed.
- **FR-SUB-5** *(existing)* A session can be ended individually by the user; it is then
  removed from the subscriber and never reconnects. (Whether that is done by destroying a
  handle, an explicit remove/stop operation, or both is API design.)
- **FR-SUB-6** *(existing)* Each received message is delivered as an **immutable,
  shared-ownership buffer** (no copy for the user, cheap to retain beyond the delivery
  call). The buffer content equals exactly the payload bytes of one frame. *(The concrete
  type – shared pointer to a buffer, a view plus owner token, ... – is API design; the
  zero-copy and retainability properties are the requirement, together with LT-7.)*
- **FR-SUB-7** *(new)* The delivery info SHOULD additionally provide which session /
  publisher endpoint the message came from and a receive timestamp taken from a
  monotonic clock at the moment the frame was completely received.
- **FR-SUB-8** *(existing)* The message handler (callback) is configured per subscriber
  and can be set, replaced or cleared by the user at any time. Changing it MUST be
  thread-safe, including from inside the handler itself (the existing synchronous
  implementation deadlocks there; it MUST NOT in the rewrite). After the change returns,
  the old handler is never *started* again; if it is currently running, the change either
  waits for it or documents that it does not (and the old handler must then not be
  destroyed while running). Which one is an architecture decision.
- **FR-SUB-9** *(existing)* A session that has no connection reconnects (section 9).
- **FR-SUB-10** *(new)* A session is reported **connected only after a valid handshake
  response** has been received (WP-H8), not already after TCP connect.
- **FR-SUB-11** *(existing)* Retry limit semantics: a negative limit means infinite
  retries, `0` no retry, `n > 0` at most `n` retries; the counter is reset after a
  successful connection.
- **FR-SUB-12** *(new)* **Empty messages are delivered.** A received frame of type
  *RegularPayload* with a complete header and `data_size == 0` is delivered to the handler
  as a message of length 0 (a valid, non-null empty buffer), takes part in latest-wins like
  any other message, and is counted in the statistics (CONN-5). (Existing subscribers
  silently ignore such frames; this is a deliberate, documented behaviour change, WP-P3.)
  Frames with a header too short to contain the size field are malformed and are *not*
  delivered (WP-F2).

## 7. Delivery semantics (latest-wins)

- **DEL-1** *(existing)* Delivery is **latest-wins**. Messages are never queued
  unboundedly and the library never blocks the publisher to deliver. Older messages are
  dropped when newer ones arrive before the older ones could be sent/delivered.
- **DEL-2** *(existing)* **Publisher side**, per connection: at most one frame in
  transmission and at most one frame pending (the newest). A frame in transmission is
  always completed (WP-D3); a pending frame is replaced by a newer one. The first frame a
  connection sends after the handshake is the newest pending one at that time (frames
  older than the handshake completion MAY be dropped).
- **DEL-3** *(existing)* **Subscriber side**: for one subscriber, at most one message is
  being processed by the callback and at most one is held as next. A newer message
  replaces a pending older one. All sessions of one subscriber share this one slot
  (OD-6). The callback therefore always observes the most recent data available
  and never a backlog.
- **DEL-4** *(existing)* **Back-pressure**: when the receiver (library or user callback)
  is slow, the subscriber stops/slows reading from TCP; the publisher's per-connection
  frame slots absorb this by dropping. Memory use stays bounded (ROB-2).
- **DEL-5** *(existing)* **Order**: messages of one session are delivered to the callback
  in the order received (no reordering), possibly with gaps. Across sessions no order is
  guaranteed.
- **DEL-6** *(existing)* **Callback execution**: the user callback is never invoked
  concurrently with itself for one subscriber (calls are serialised); it is allowed to
  run long (time-consuming work) without stalling other subscribers/publishers.
  **Default**: callback runs on a **shared worker pool** that is separate from network
  I/O (decision with owner). The size of that pool is small and fixed, independent of the
  number of subscribers; one slow callback occupies at most one pool thread, only
  while it runs.
- **DEL-7** *(optional, decision pending OD-2)* An opt-in mode where the callback runs
  directly on the I/O thread that received the message (lowest hand-off jitter,
  documented caveat: a slow callback delays other connections on that loop). In this
  mode the implementation MUST still honour latest-wins: when it resumes reading after
  the callback, it MUST NOT deliver stale messages that accumulated in the socket
  while the callback ran, if newer ones are available.
- **DEL-8** *(new)* Drops are counted (per publisher connection and per subscriber)
  and the counters are queryable (CONN-5).
- **DEL-9** A user callback that throws MUST NOT terminate the process or the worker
  (ROB-5); the exception is logged and the message is considered delivered.

## 8. Connection state and observability

Live queries are a requirement (decision with owner). They MUST be **cheap, lock-free or
nearly so, non-blocking, callable from any thread including from callbacks**, and MUST
NOT touch sockets from the calling thread (no `remote_endpoint()` probing from user
threads). State is maintained by the connection's owner and published as an immutable
snapshot or atomics.

- **CONN-1** *(partly existing)* Publisher: running state; bound port; number of
  subscribers in the *streaming* state (not merely accepted).
- **CONN-2** *(new)* Publisher: list of current subscriber connections with, for each,
  remote and local endpoint, state (handshaking/streaming), time connected since,
  messages and bytes sent, messages dropped.
- **CONN-3** *(partly existing)* Subscriber session: connection state as an enumeration
  (`Resolving, Connecting, Handshaking, Connected, Backoff, Closed`) from which a
  simple "is connected" indication follows, the configured candidate list, the endpoint
  currently connected to (as configured and resolved), local endpoint.
- **CONN-4** *(new)* Subscriber session: last error (code + message), number of
  reconnects, time of last message, time of last state change.
- **CONN-5** *(new)* Counters: messages and bytes received, messages dropped (DEL-8).
  They are cheap (relaxed atomics or per-owner counters).
- **CONN-6** *(new)* Subscriber: list of its sessions with the info above.
- **CONN-7** *(new)* **Event notification** (in addition to polling): optional handlers for
  *subscriber connected/disconnected* (publisher side, with remote endpoint and reason)
  and *session state changed* (subscriber side). Events for one object are delivered
  **in order**, **never on an I/O thread**, **never while holding library locks**, and
  none is delivered after the owning object's teardown completed (LT-1).
- **CONN-8** *(new)* Blocking helpers for tests/applications to avoid sleep-polling:
  wait (with timeout) until a publisher has ≥ n streaming subscribers; wait (with
  timeout) until a session is connected. Calling them from a library thread (I/O
  thread, callback pool, event handler) MUST fail fast with an error instead of
  risking a deadlock, and this is documented in the thread-safety table (TH-6).
- **CONN-9** Limits of the information: the protocol carries no subscriber identity, so
  the publisher knows only IP and port of the peer. Names/IDs are out of scope (NG-2).
- **CONN-10** Liveness detection: a peer that vanished silently MUST be detected using
  TCP keep-alive (WP-S4) and, on the publisher, a pending read (WP-R1). Intervals are
  configurable; default values SHOULD be chosen so that a dead peer is detected within
  roughly 30 s without generating noticeable traffic on idle LAN links.

## 9. Reconnect and connection establishment (subscriber)

- **REC-1** *(existing)* On connect failure, handshake failure, or connection loss the
  session retries per FR-SUB-11, after a **delay** (not immediately).
- **REC-2** *(new; existing code has a defect)* The delay MUST actually be applied for
  every retry (the existing timer is armed once with 1 s and afterwards fires
  immediately, producing a reconnect spin). Defaults: initial delay **1 s** (existing,
  documented), configurable; optional exponential backoff with a configurable cap and
  jitter.
- **REC-3** *(new)* While in backoff, cancelling the session or destroying the owner MUST
  interrupt the wait immediately (no sleeping thread, no timer left running).
- **REC-4** *(new)* Name resolution MUST NOT block an I/O thread or a callback thread for
  an unbounded time. It happens on a dedicated mechanism (e.g. resolver thread) whose
  existence is documented in the thread budget (TH-1).
- **REC-5** *(new)* A connection attempt (TCP connect and handshake) has a configurable
  timeout (default 5 s) after which the attempt counts as failed.
- **REC-6** When retries are exhausted the session reaches state `Closed` with the last
  error and (existing) is removed from its subscriber. Users observing it through a
  retained handle see the terminal state.
- **REC-7** *(existing)* The publisher need not exist when the session is created; the
  session connects as soon as it appears.

## 10. Lifecycle, threading and teardown

The architecture will define object ownership (who owns the execution context, whether a
shared context object exists, strong/weak references). **That choice is deliberately not
made here (OD-1).** Whatever is chosen MUST satisfy the following observable guarantees.

**Threading model (observable properties)**

- **TH-1** The number of threads the library creates is **fixed at start-up** and
  **independent of the number of publishers, subscribers, sessions and connections**.
  It consists of a small configurable number of I/O threads, a small configurable
  callback/event pool (DEL-6) and, at most, a documented resolver helper. A
  thread budget table is part of the architecture document and of the README.
- **TH-2** There is no thread per connection, per publisher, or per subscriber.
- **TH-3** All socket operations (connect, read, write, close, option changes) of one
  connection are performed by exactly one logical owner at a time. No two operations on
  the same socket run concurrently and no operation is initiated from an arbitrary user
  thread without that serialisation. (This is the root cause of the current races.)
- **TH-4** The library MUST NOT call user code (callbacks, event handlers, the logger)
  while holding any library-internal lock, and MUST NOT call it from a thread whose
  blocking would stall unrelated connections (except DEL-7 mode, by explicit opt-in).
- **TH-5** Threads SHOULD be named (where the OS supports it) for debugging.
- **TH-6** A thread-safety table is part of the API documentation: for every public
  function, "thread-safe", "externally synchronised" or "must not be called from a
  callback".

**Teardown guarantees**

- **LT-1** After a publisher, subscriber or session has been **destroyed or (if the API
  offers it) explicitly closed**, and that operation *returned* (on any non-library
  thread): all its sockets are closed, **no user callback, event handler or logger call
  belonging to it is running or will ever start**, and no library-owned thread continues
  to touch memory the user may free (e.g. objects captured by callbacks or the logger).
  The "no further logger call" guarantee is existing and is tested by
  `ExecutorShutdown.DestructorReturnsOnlyAfterEverythingIsTornDown`.
- **LT-2** Teardown from **inside a user callback or event handler** (including destroying
  the last handle, and destroying the object the callback belongs to) is supported
  and MUST NOT deadlock, abort or access freed memory. In that case the call cannot wait
  for the running callback; it guarantees instead that the callback returns normally and
  no further call starts.
- **LT-3** Teardown MUST NOT wait for **unrelated** work (other publishers/subscribers,
  or timers). It may wait only for (a) the object's own running callback(s) and (b)
  bounded socket close operations. Specifically, it MUST NOT contain a "drain until the
  I/O context runs out of work" step with an unbounded duration. *(The existing test
  `PendingWorkLongerThanFiveSecondsIsDrained` encodes an implementation artifact and is
  not a requirement.)*
- **LT-4** Teardown order flexibility: the architecture document MUST list the supported
  destruction orders of {publisher, subscriber, retained session handle, shared
  execution context/executor, user logger} and for each unsupported order the behaviour
  (documented, preferably safe/inert rather than undefined). At minimum supported:
  users destroying everything on one thread in any order of handles and then the
  context; and destruction of handles on callback/event threads (LT-2).
- **LT-5** **Destruction alone MUST be sufficient** to stop and release everything (RAII);
  no use case may require an explicit stop call. If the API additionally offers an
  explicit stop/close operation, it is idempotent, may be called repeatedly and
  concurrently from multiple threads, and a later destruction is a no-op.
- **LT-6** A **cancelled/closed object stays closed**: it can never be restarted or
  reconnect (existing test `CancelledPublisherSessionCannotBeRestarted`).
- **LT-7** A message buffer handed to the user remains valid for as long as the user holds
  it, **even after** the subscriber, its context, and all sessions are destroyed. Any
  buffer recycling mechanism MUST be safe against a recycle target that no longer
  exists.
- **LT-8** Any handle or reference the library gives to the user that refers to a
  library-managed object (e.g. a session) remains **safe to use** after the object's owner
  has been destroyed or the object has been closed: queries report a terminal state and
  operations are no-ops (existing tests `RetainedCancelledSession...`, with the
  relaxation that it need not keep a logger or any thread alive).
- **LT-9** Ownership of a running publisher or subscriber MUST be **transferable**
  (returned from functions, stored in containers) without interrupting connections,
  sessions or the handler (existing tests `Moved...Preserves...`). Copying is not
  supported. A transferred-from object may only be destroyed or assigned.
- **LT-10** Starting is cheap and immediate: a publisher accepts when its creation
  returns; a session starts connecting without further user action. Invalid
  configuration (e.g. zero worker threads) is rejected deterministically (existing test
  `ZeroWorkerThreadsAreRejected`, see ROB-9).
- **LT-11** No static-initialisation-order or destruction-order hazards: no non-trivial
  global/static objects with threads or sockets. Use at program exit (static
  destruction) is not supported but MUST NOT hang forever if all objects were destroyed.
- **LT-12** **Last release on a library thread.** If the last owner of the library's
  shared execution resources is released on one of the library's own threads (e.g. a
  message handler destroys the last publisher), the library cannot wait for its threads
  (a thread cannot join itself). In that case it MUST still not deadlock or leak, the
  threads MUST wind down on their own without calling any user code (handlers, event
  handlers, logger) and without touching memory they do not own, and the exception to
  "no library thread runs after the last destructor returned" MUST be documented
  (existing test `LastReferenceIsReleasedOnWorkerThread`).

## 11. Robustness and resource bounds

- **ROB-1** Malformed or hostile input (section 7/9/10 of the wire spec) closes the
  affected connection only; it MUST NOT crash, hang, or leak (WP-C3, WP-L*).
- **ROB-2** **Bounded memory.** Worst-case memory per connection is bounded by
  `max_payload_size` (configurable, wire spec WP-L1; default OD-3) × a documented small
  constant, plus fixed per-connection state. In particular: per publisher connection at
  most 2 frames referenced (in transmission, pending), per subscriber at most 1 receive
  buffer being filled + 1 pending + those the user retains. Recycled buffers MUST NOT
  accumulate without bound (pool with size/idle cap; a single large message must not
  pin its memory forever).
- **ROB-3** `std::bad_alloc` while handling a connection closes that connection and is
  logged; it does not terminate the process.
- **ROB-4** Socket errors, `ECONNRESET`, `EPIPE`, `ECONNABORTED`, `EMFILE` etc. are handled;
  `EMFILE`/`ENFILE` on `accept` MUST NOT busy-loop (back off) and MUST NOT stop the
  publisher.
- **ROB-5** **No exception escapes** a library-owned thread, a destructor, or an I/O
  completion handler. User callbacks, event handlers and the logger are called inside
  `try`/`catch` (DEL-9).
- **ROB-6** A **stuck subscriber** (peer connected, not reading, frame in transmission not
  progressing) SHOULD be disconnected after a configurable no-progress timeout (default
  off or a generous value, OD-9). Without it, memory is still bounded (ROB-2).
- **ROB-7** Handshake timeout (WP-H10) and pre-handshake connection limit (WP-L3).
- **ROB-8** Behaviour under file-descriptor exhaustion, `fork` and signals is not
  specified beyond: no `SIGPIPE`-induced termination (WP-S1), no signal handlers
  installed by the library.
- **ROB-9** Valid use for the full range of API arguments: invalid arguments (null with
  non-zero size, empty publisher list, zero thread count, etc.) produce a documented,
  deterministic error (exception or result code), never UB.

## 12. Performance requirements

The primary objective is low and *stable* latency for workloads W-1/W-2.

- **PERF-1** **Baseline first.** Before the rewrite is accepted, a benchmark MUST exist (extend
  `samples/performance_*` or add a dedicated benchmark target) that measures, over loopback
  and for W-1 and W-2: end-to-end latency (publish call → handler entry) as
  p50/p99/p99.9/max, throughput (messages/s and MB/s for a subscriber that keeps up), and CPU
  usage. The baseline of the current implementation is recorded in the repo's docs.
- **PERF-2** *Acceptance:* the rewrite is **not worse than the baseline** in p50 and p99 for W-1
  and W-2 on every supported OS, with the default settings. Proposed stretch target (to be
  confirmed after the baseline exists): W-1 loopback p99 end-to-end < 1 ms on an idle Linux
  machine with a single publisher and subscriber; no regression of p99 with 200 idle
  connections added.
- **PERF-3** **Copies.** Publisher: exactly one copy of the user's payload into the frame
  buffer per publish, none per subscriber. Subscriber: payload is read from the socket
  directly into the buffer that is delivered to the user (no intermediate copy for
  payloads larger than the read-ahead size; small messages may be copied out of a
  read-ahead buffer if cheaper).
- **PERF-4** **Allocations.** In steady state (messages of similar size) the hot paths
  (publish, receive, delivery) perform **no heap allocation**: buffers are recycled
  (subject to ROB-2), no per-message `shared_ptr` header objects, no per-message
  `std::function` copies/allocations, no string formatting when the log level is
  disabled. Optional features such as delivery status (FR-PUB-14) are exempt only when
  the user opts in.
- **PERF-5** **System calls.** A small message costs at most one `write`-type call per
  subscriber on the publisher side (header+payload contiguous, WP-D2) and, on average,
  not more than one `read`-type call per message on the subscriber (read-ahead for small
  messages; the existing implementation issues three reads per message).
- **PERF-6** **Idle cost.** With no traffic the library consumes ~0 CPU: no polling
  timers, no busy loops, no periodic wake-ups other than the configured keep-alive
  and reconnect timers.
- **PERF-7** **Isolation.** A slow subscriber or a slow callback does not increase the
  latency of publishing for the caller (FR-PUB-6) and does not delay other publisher
  connections or other subscribers' callbacks (except DEL-7 opt-in).
- **PERF-8** Socket buffer sizes: for W-2 the defaults MUST NOT throttle 16 MiB messages at
  15 Hz on loopback and a 1 Gbit LAN; configuration knobs for `SO_SNDBUF`/`SO_RCVBUF`
  SHOULD be provided.
- **PERF-9** Logging at disabled levels costs no more than an integer comparison; verbose
  per-message logging is compiled out in release builds (existing macros
  `TCP_PUBSUB_LOG_DEBUG_*_ENABLED`) or equivalently cheap.

## 13. Logging and diagnostics

- **LOG-1** *(existing)* User-supplied logger `(LogLevel, message)` with levels
  `DebugVerbose, Debug, Info, Warning, Error, Fatal`, and the provided default loggers
  (`default_logger`, `logger_no_verbose_debug`) or equivalents.
- **LOG-2** The logger may be called from any library thread, concurrently. The
  documentation says so. It is never called while holding library locks (TH-4) and never
  after teardown of the owning object completed (LT-1).
- **LOG-3** Log content: connection established/closed with endpoints and reason; errors with
  the OS error text; first occurrence of drops (not every drop); no per-message logging
  above `DebugVerbose`; rate limiting for repeated identical errors (e.g. reconnect
  failures every second) SHOULD be applied.
- **LOG-4** Programmatic status (CONN-*) is the primary diagnostic channel; tests and
  applications MUST NOT need to parse log text. (Existing test
  `AcceptedConnectionCannotEscapeCancellation` does so; the rewrite replaces it by a
  behaviour test.)

## 14. Quality, testing and CI

- **QA-1** Tests use GoogleTest as today (`thirdparty/gtest`), registered with CTest.
- **QA-2** Tests exercise the **public API** (and raw sockets for wire tests). Tests
  MUST NOT include private headers or depend on class names inside the implementation.
- **QA-3** Wire conformance tests as listed in
  [02-wire-protocol.md](02-wire-protocol.md) section 10, using raw sockets
  independent from the library, plus interop tests against a pinned release of the old
  implementation (both roles).
- **QA-4** Tests use **ephemeral ports** (`port 0`) and the wait helpers (CONN-8) —
  no fixed port numbers (existing tests use 1588/1589) and no sleep-polling for
  state. Time-based assertions use generous timeouts and are not sensitive to CI load.
- **QA-5** Functional tests (existing ones to be retained in behaviour): basic publish/receive of
  two messages, 16 MiB message, two publishers → one subscriber, one publisher → two
  subscribers, publisher candidate list with unresolvable first entry. New: empty
  messages are delivered as non-null empty buffers (FR-PUB-12, FR-SUB-12), including
  an empty message following a non-empty one and a burst mixing both; delivery status
  (FR-PUB-14, if implemented) reaches a terminal state for every message in the cases
  *transmitted*, *superseded*, *failed by peer close* and *failed by teardown*.
- **QA-6** Latest-wins tests: slow callback sees only the newest of a burst; slow subscriber
  does not slow the publisher or others; no unbounded memory growth under a fast
  publisher and slow consumer (bounded RSS/allocation counters).
- **QA-7** Lifecycle tests: the existing `ExecutorShutdown.*` scenarios re-expressed as
  behaviour tests of LT-1..LT-12, including: destruction from within callbacks and event
  handlers; destruction while connecting/handshaking/in backoff/mid-frame; destroy
  while another thread publishes/queries/closes; destruction in each supported
  order; repeated create/destroy loops (≥ 1000 iterations) with traffic.
- **QA-8** Reconnect tests: publisher restart (same port) → session reconnects; retry
  counting; no spin (measure attempts per second ≤ 1/s with the default); cancel during
  backoff is immediate.
- **QA-9** Robustness tests: wire fuzz / garbage input, oversize frames, handshake
  violations, abrupt resets, `EMFILE`-style behaviour where feasible.
- **QA-10** **TSAN** and **ASAN/UBSAN** jobs on Linux (and Clang+TSAN on macOS if
  feasible) run the whole suite in CI; the suite MUST be green in all CI jobs.
  **No test is disabled by platform** (the current `disable_racy_tests.cmake` mechanism
  and the disabled `tcp_pubsub.*` tests on Linux must be gone).
- **QA-11** Tests are deterministic: stress tests run with a bounded iteration count or time
  and fixed seeds; a CI flake-detection run (e.g. `--gtest_repeat`) is part of the
  definition of done for the lifecycle tests.
- **QA-12** Code coverage for the protocol parsing and state machines SHOULD be measured;
  all state transitions of both state machines (wire spec 5.1) are covered by tests.

## 15. Documentation deliverables

- **DOC-1** README updated: usage, thread budget, lifecycle/teardown rules, platform notes.
- **DOC-2** Public headers documented (Doxygen style as today), including the thread-safety
  table (TH-6) and the delivery semantics (section 7).
- **DOC-3** `docs/rewrite/03-architecture.md` (ownership, threads, state machines, data
  flow, teardown protocol) and `04-api.md` (public API) as follow-ups that must trace to
  requirement IDs.
- **DOC-4** Migration guide from the current API, and a changelog entry.
- **DOC-5** README's protocol section stays in sync with the wire specification (single
  source of truth: `02-wire-protocol.md`; the README links to it or mirrors it).
- **DOC-6** Recorded baseline benchmark results (PERF-1) and the final comparison.

## 16. Open decisions

| ID | Question | Default / recommendation |
|---|---|---|
| **OD-1** | Context/executor lifetime model: (A) handles co-own the context, (B) user must keep the context alive, (C) handles own their cores and only weakly reference the context, which closes everything on its destruction. The architecture document decides and must satisfy LT-1..LT-12. | **Owner preference: the context need not outlive publishers/subscribers → A**, with the context as a thin value wrapper over a shared core ([04-api.md](04-api.md) section 6.1). Remaining design problem: last release on a library thread (LT-12). Confirm in `03-architecture.md`. |
| **OD-2** | Keep the opt-in "inline on I/O thread" delivery (DEL-7)? | Optional; only if the benchmark shows jitter of the pool hand-off matters. Expected gain: tens of µs on average, mostly lower jitter. |
| **OD-3** | Default `max_payload_size`. | **Agreed: default 256 MiB** (≥ 16 MiB is mandatory). **One generic limit, configured once on the shared execution context** (no per-object override). It applies on the receiving side (WP-L1) and the publisher rejects larger messages up front (FR-PUB-10). Consider growing the receive buffer proportionally to received bytes for sizes above a few MiB to limit hostile pre-allocation. |
| **OD-4** | Failure reporting of publisher start (FR-PUB-2): throw, error state, or factory result. | **API decision** → [04-api.md](04-api.md) section 8 (factory result proposed). |
| **OD-5** | Windows listener socket options (`SO_REUSEADDR` vs `SO_EXCLUSIVEADDRUSE`). | Exclusive use on Windows; verify that rebind after close works for the tests. |
| **OD-6** | One latest-wins slot per subscriber shared by all its sessions (existing), or one per session. | Keep existing (shared); document that fast publishers can shadow slow ones. |
| **OD-7** | Logger type: keep `std::function<void(LogLevel, const std::string&)>`, or `string_view`. | **API decision** → [04-api.md](04-api.md). |
| **OD-8** | Internal networking library (Asio vs. raw sockets). | Asio stays acceptable (PLAT-5). |
| **OD-9** | Default of the stuck-subscriber timeout (ROB-6), keep-alive intervals (CONN-10), handshake/connect timeouts (WP-H10, REC-5). | As given in the respective requirement; all configurable. |
| **OD-10** | **Publish hot path** (architecture): (a) the caller thread performs a non-blocking send itself (lowest latency, charges the caller ~20–180 µs per subscriber), (b) the caller only posts to the I/O loops (caller pays 1–17 µs, one extra thread hop of ~20–55 µs when the system is idle), or (c) hybrid. FR-PUB-6 allows (a) and (b) as long as the call never waits for the network. | Decide with a prototype against the micro-benchmarks, see [baseline/README.md](baseline/README.md) sections 7–8. Fan-out must in any case not be serial on one thread. |
| **OD-11** | Optional **low-latency mode** (short busy-poll / keep-warm on I/O threads) to avoid wake-up latency on sparse streams. Costs CPU and power. | Not required; decide after the Linux/macOS baselines show whether wake-ups dominate there too. |

## 17. Traceability and defects of the current implementation

### 17.1 Existing tests → requirements

| Existing test | Requirement(s) | Keep as |
|---|---|---|
| `tcp_pubsub.basic_test` | FR-PUB-4, FR-SUB-6, DEL-5 | functional, ephemeral port |
| `tcp_pubsub.large_message_test` (16 MiB) | W-2, ROB-2, PERF-8 | functional |
| `tcp_pubsub.multiple_publishers_test` | FR-SUB-4 | functional |
| `tcp_pubsub.multiple_subscribers_test` | FR-PUB-3, FR-SUB-4 | functional |
| `tcp_pubsub.publisher_list_test` | FR-SUB-2 | functional |
| `ExecutorShutdown.DestructorReturnsOnlyAfterEverythingIsTornDown` | LT-1 | lifecycle |
| `...UserReleasesExecutorHandleBeforePublisherAndSubscriber` | LT-4 | lifecycle (outcome depends on OD-1) |
| `...LastReferenceIsReleasedOnWorkerThread` | LT-2, LT-12 | lifecycle |
| `...MovedSubscriberPreservesSessionsAndCallback`, `...MovedPublisherPreservesConnections` | LT-9 | lifecycle |
| `...RetainedCancelledSessionKeepsLoggerButNotWorkersAlive` | LT-8 | lifecycle (relaxed) |
| `...PendingWorkLongerThanFiveSecondsIsDrained` | – (implementation artifact, LT-3 rejects it) | **drop** |
| `...SubscriberSessionContextOutlivesItsAsioMembers` | – (private classes) | **drop** (replaced by LT-7/LT-8 behaviour tests) |
| `...RunningCallbackFinishesBeforeShutdownReturns` | LT-1 | lifecycle |
| `...AcceptedConnectionCannotEscapeCancellation` | LT-1, LOG-4 | lifecycle, without log parsing |
| `...CancelledPublisherSessionCannotBeRestarted` | LT-6 | lifecycle (public API) |
| `...ZeroWorkerThreadsAreRejected` | LT-10 | API validation |
| `...ExecutorHandleOwnership` (move/copy traits) | LT-9 | compile-time checks |

On Linux, the current build disables all `tcp_pubsub.*` tests and eight
`ExecutorShutdown.*` tests (`tests/tcp_pubsub_test/disable_racy_tests.cmake`),
"until socket cancellation is serialized with asynchronous socket operations". The
rewrite is complete only when **all** of them (re-expressed) run and pass on all platforms
(QA-10).

### 17.2 Defects found in the current implementation (to be fixed by design, not copied)

| # | Defect | Fixed by |
|---|--------|----------|
| D-1 | Two `async_write`s on one socket can be in flight (handshake response vs. first data frame; completion handler hand-off without `sending_in_progress_`). | TH-3, WP-D3 |
| D-2 | `close()`/`cancel()` called from user threads concurrently with handlers on the strand; `isConnected()`, endpoint strings call socket accessors from user threads. | TH-3, CONN-* |
| D-3 | Retry timer armed only once, retries then spin without delay. | REC-2 |
| D-4 | Unbounded allocation from the peer-supplied size before validation; `bad_alloc` escapes into an Asio worker. | WP-L1/L2, ROB-1/3/5 |
| D-5 | Publisher never notices vanished peers until a write fails; subscriber counted before handshake. | WP-R1/H7, CONN-10 |
| D-6 | Reference cycles between owner objects and sessions are broken only by `cancel()`; last release may happen on a worker, requiring detached threads. | LT-2/3, TH-3 (architecture) |
| D-7 | `setCallback` is not synchronised with the callback thread; synchronous callbacks run under a mutex so calling `setCallback` inside a callback deadlocks. | FR-SUB-8, TH-4 |
| D-8 | One extra thread per subscriber with an asynchronous callback. | TH-1/2, DEL-6 |
| D-9 | Three reads per message, per-message `make_shared` header allocations, additional `post` hops. | PERF-3/4/5 |
| D-10 | `IPV6_V6ONLY`, `SIGPIPE`, keep-alive not handled. | WP-S1/S3/S4, PLAT-6 |
| D-11 | Tests disabled on Linux, fixed ports, log-parsing tests, private-header tests. | QA-2/4/10, LOG-4 |

## 18. Glossary

- **Publisher** – accepts TCP connections and sends messages to all connected subscribers.
- **Subscriber** – object owning sessions and one callback.
- **Session** – one connection (with reconnect logic) from a subscriber to one publisher.
- **Connection** – one accepted TCP connection on the publisher side.
- **Frame** – header + payload as defined in the wire specification.
- **Streaming** – connection state after a completed handshake.
- **Latest-wins** – a newer message replaces an older one that has not yet been sent or processed.
- **I/O thread** – library thread that performs socket operations.
- **Callback pool** – library threads on which user callbacks and event handlers run.
