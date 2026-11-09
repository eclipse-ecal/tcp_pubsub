# tcp_pubsub rewrite – API design notes (DRAFT, non-normative)

Status: **discussion draft.** Nothing here is a requirement. Requirements live in
[01-requirements.md](01-requirements.md) and are API-neutral by design; this document
is where *how a capability is exposed* is decided. Every API element below should cite
the requirement ID it implements. The wire protocol
([02-wire-protocol.md](02-wire-protocol.md)) is frozen; the API is free to change
(NG-6).

## 1. Design principles

1. **RAII is the lifecycle.** Creating an object starts it, destroying it stops it and
   releases everything (LT-1, LT-5). No use case may require an explicit stop call.
2. **Hot path without allocation or exceptions** (PERF-4, FR-PUB-6): publishing and
   receiving use no `new`, no `std::function` construction, and report problems by
   return value.
3. **No third-party types in public headers** (PLAT-4); C++17 only (PLAT-2).
4. **Observe with snapshots, control with handles.** Queries return plain value types
   (copyable, never touch sockets, CONN-*); control operations are on RAII handles.
5. **Make misuse hard:** `[[nodiscard]]` on handles and results where ignoring them is
   almost certainly a bug; invalid arguments are rejected deterministically (ROB-9).

## 2. Decisions taken so far

| Topic | Decision | Source |
|---|---|---|
| FR/API split | Requirements are API-neutral; API shape is discussed here. | owner |
| Message input | `ConstBytes` view with implicit conversion from `std::string_view` and friends; not `(const char*, size_t)` (section 3). | owner |
| Empty messages | Supported by default on both sides (FR-PUB-12, FR-SUB-12). Requires a documented behaviour change for subscribers, WP-P3. | owner |
| Publish result | Immediate, `[[nodiscard]]` result with a reason on failure (FR-PUB-10). Users who do not want it can discard it explicitly (`std::ignore =`). | owner |
| Delivery status | Opt-in **ticket object** that can take value-type snapshots without user locking, combined with the structured publish result (section 4.2). Details deliberately deferred. | owner |
| Stop operations | **RAII only**: no `cancel()`/`close()` on publisher/subscriber. An idempotent `close()` can be added later without breaking anything (section 5). | owner |
| Sessions | Move-only RAII `Subscription` handle, `[[nodiscard]]`; no weak handle, nothing touches the handle on the hot path (section 5). | owner delegated, decided here |
| Max message size | **One generic limit on the context**, default 256 MiB (OD-3); no per-object override. It is a single number checked per frame, so resource usage is a non-issue (section 6). | owner |
| Context | Thin **value-type wrapper** (copyable, cheap) around a shared execution resource. Publishers/subscribers take it by reference and keep the resource alive themselves, so **the context need not outlive them** (section 6). | owner, assumption stated in section 6 |
| Delivery ticket | **Per-peer detail from the first version.** No pooling of ticket state initially (section 4.2). | owner |
| Publisher start | Proposed: **factory returning a result**; a constructed `Publisher` is always running (section 8). | proposal, owner leaning |
| Gather publish | Separate name **`publishParts`**, taking any iterable of `ConstBytes`-convertible elements plus an `initializer_list` overload; no variadic template (section 3). | owner / decided |

## 3. Passing message data in (FR-PUB-4, FR-PUB-12)

Options:

| Option | Pros | Cons |
|---|---|---|
| `std::string_view` | In the standard, implicit from `std::string`/literals. | Says "text"; binary data from `vector<uint8_t>`, `std::byte*`, structs needs `reinterpret_cast`/`.data()` at every call site. |
| `std::span<const std::byte>` | The right abstraction. | C++20 only (PLAT-2 says C++17). |
| **Own tiny `ConstBytes` view (recommended)** | Implicit from `std::string_view`, `std::string`, and contiguous containers/arrays/C arrays whose element type is **byte-like** (`char`, `signed/unsigned char`, `std::byte`), plus `(const void*, size_t)`; trivially replaceable by `std::span` when C++20 is the floor. Other element types (e.g. `float`, structs) need an explicit `bytesOf(x)`. | One more type to document (10 lines of code). |

Rules for `ConstBytes`: default-constructed and `(nullptr, 0)` are valid empty views;
`(nullptr, n > 0)` is an invalid argument and makes the publish call return
`InvalidArgument`; the view is only read during the call (data is copied before return,
FR-PUB-4).

**Why implicit conversion is limited to byte-like element types.** A conversion from any
`std::vector<T>` of a trivially copyable `T` would make `std::vector<std::string_view>` or
`std::vector<ConstBytes>` (both trivially copyable!) silently convert into "the raw bytes of
the view objects", i.e. a message full of pointers. Restricting the implicit constructors
to byte-like elements and requiring `bytesOf(vec_of_floats)` for everything else turns that
trap into a compile error.

### Publishing several buffers as one message ("gather", FR-PUB-4)

Gather means publishing several separate buffers as **one** message (for example your own
header plus a payload) without first concatenating them yourself. Decided shape: a
**separate name, `publishParts`**, which also removes every overload-resolution doubt
against `publish(ConstBytes)` (and the vector-of-views trap above):

```cpp
publish     (ConstBytes message,                          PublishOptions = {});   // one buffer

// any iterable (vector, array, list, ...) whose elements convert to ConstBytes
template <class Range>   // constrained: begin()/end() exist, element convertible to ConstBytes
publishParts(const Range& parts,                          PublishOptions = {});

publishParts(std::initializer_list<ConstBytes> parts,     PublishOptions = {});   // braced literal list
```

Usage:

```cpp
pub.publishParts({header, payload});                      // braced list
pub.publishParts(std::vector<std::string>{a, b, c});      // iterable of strings
pub.publishParts(std::list<ConstBytes>{...});             // non-contiguous containers work too
```

Why an iterable plus an `initializer_list`, and not a variadic template:

- A braced list such as `{header, payload}` cannot be deduced by a template parameter, so the
  `initializer_list` overload is needed anyway; the iterable template covers every container
  the user already has.
- A variadic `publishParts(a, b, c, options)` collides with the trailing `PublishOptions`
  argument (is the last argument a part or the options?) and needs extra SFINAE to
  disambiguate; it also instantiates once per arity. It adds nothing the braced list does not
  already give. It can be layered on later as a pure convenience if somebody really wants it.
- Implementation: the template is a thin inline shim. The publisher needs the **total size
  before copying** (the header contains `data_size`, the frame buffer is sized once), so the
  shim collects the parts into a small on-stack array (e.g. 8 parts, no allocation) and calls
  one non-template core function `(const ConstBytes*, size_t, options)`. Only a part list
  longer than the on-stack capacity allocates. Iterables are traversed once by the shim, so
  even input-iterator-only ranges would work; elements are only *read* during the call.

## 4. Publish outcome and delivery status (FR-PUB-10, FR-PUB-14)

### 4.1 Immediate result (required)

```cpp
enum class PublishStatus { Ok, TooLarge, InvalidArgument };   // no "NotRunning": see section 8

struct [[nodiscard]] PublishResult {
  PublishStatus  status   = PublishStatus::Ok;
  uint64_t       sequence = 0;     // monotonically increasing per publisher; 0 if not accepted
  DeliveryTicket ticket;           // empty unless tracking was requested (section 4.2)
  explicit operator bool() const noexcept { return status == PublishStatus::Ok; }
};
```

Errors are return values: `publish` is called at 100 Hz from latency-sensitive threads and
the failure modes are ordinary conditions (nobody subscribed is *not* one: it is `Ok`,
FR-PUB-7). The result is `[[nodiscard]]` (decided); a user who really does not care writes
`std::ignore = pub.publish(...)`. The empty `DeliveryTicket` is a null `shared_ptr`:
returning and destroying it costs no atomic operation.

### 4.2 Delivery status: ticket object (decided in principle, details deferred)

Direction decided with the owner: delivery status is an **opt-in ticket** returned as part
of the structured publish result. It can take **snapshots** of the delivery state at any
time, from any thread, **without the user locking anything**; a snapshot is a plain value.

```cpp
struct PublishOptions { bool track_delivery = false; };

PublishResult Publisher::publish(ConstBytes message, PublishOptions = {});

enum class DeliveryState { Queued, Transmitting, Transmitted, Superseded, Failed };

struct PeerDelivery   { Endpoint peer; DeliveryState state; };
struct DeliverySnapshot {                       // value type, safe to keep and copy
  size_t queued, transmitting, transmitted, superseded, failed;
  bool   complete;                              // every connection reached a terminal state
  std::vector<PeerDelivery> peers;              // per-peer detail (optional part, see below)
};

class DeliveryTicket {                          // cheap to copy (shared state), may be empty
 public:
  explicit operator bool() const noexcept;      // false if tracking was not requested
  DeliverySnapshot snapshot() const;            // lock-free read of per-connection atomics
  bool wait(std::chrono::milliseconds timeout) const;   // until `complete`; optional convenience
};
```

Why a ticket, not just a sequence number: the owner prefers an object that carries its own
state and can be passed around and snapshotted; this also keeps the publisher's API small.
The alternative that was considered (sequence numbers plus a fixed-size per-connection
history queried through the publisher, no allocation at all, but only the last N messages
answerable) stays on file as the fall-back if the ticket turns out too costly.

Design constraints for the later detailed design (this is the part that is "hard to get
right", so it is written down here instead of decided):

1. **Zero cost when unused.** With `track_delivery == false` no ticket state exists; the hot
   path pays one null-pointer check per connection. Allocation of the shared ticket state
   happens only for tracked publishes (pooled if it matters).
2. **Snapshot without locks.** The ticket holds a block with one small atomic state per
   connection *that was streaming when the message was published*; connections write their
   slot with a single store at each transition; `snapshot()` only loads. Connections that join
   later are not part of the ticket.
3. **Terminal states always happen.** Every slot ends in *Transmitted*, *Superseded* or
   *Failed*, including at connection loss and at publisher teardown (FR-PUB-14.2), so
   `wait()` can never hang forever.
4. **No lifetime coupling.** A ticket never keeps the publisher, the context or the message
   memory alive; it can outlive all of them and then just shows terminal states
   (FR-PUB-14.3, LT-3).
5. **Per-peer detail is part of the first version** (decided). Each connection owns its own
   slot, so one peer can end *Transmitted* while another ends *Superseded* (its pending
   message was overwritten by a newer one) or *Failed* (connection lost). The peer endpoint
   is copied into the slot when the tracked message is published (only then, never for
   untracked messages), so the snapshot never refers to a connection that is gone.
   Mechanism: a connection's pending-message slot carries a pointer to the ticket slot (null
   for untracked messages); when the message is replaced or sent, the owning I/O thread
   writes the slot state. Single writer per slot, any number of snapshot readers.
6. **No pooling of the ticket block in the first version** (decided). "Pooling" would mean
   keeping used ticket blocks on a free list and reusing them instead of calling the
   allocator per tracked publish, like the existing buffer pool does for message buffers. It
   only pays off if the benchmark (constraint 8) shows that the allocation per *tracked*
   publish matters; untracked publishes never allocate for tickets. Revisit with numbers.
7. **`wait()` must not be callable on a library thread** (CONN-8 rule: fail fast instead of
   risking a deadlock).
8. *Transmitted* means "handed to the OS", not "received by the peer"; the protocol has no
   acknowledgement (wire spec section 8). The documentation must say so prominently.
9. Per-message tracking at 100 Hz with hundreds of connections must stay within the
   allocation and CPU budget of PERF-4/PERF-2 when enabled; to be validated by the
   benchmark before the feature is declared done.

### 4.3 Snapshot semantics, by example

What the user writes (no locks, any thread, any time):

```cpp
auto r = pub.publish(frame, {.track_delivery = true});   // 3 subscribers connected: A, B, C
...
tcp_pubsub::DeliverySnapshot s = r.ticket.snapshot();     // plain value, cheap, never blocks
for (const auto& p : s.peers)
  std::cout << p.peer << ": " << toString(p.state) << "\n";
if (s.complete) { /* every peer is Transmitted, Superseded or Failed */ }
std::cout << s.transmitted << " of " << s.peers.size() << " sent, " << s.superseded << " dropped\n";
```

What the library does inside `snapshot()` (one load per peer, nothing else):

```cpp
struct Slot { Endpoint peer; std::atomic<DeliveryState> state{DeliveryState::Queued}; };

DeliverySnapshot DeliveryTicket::snapshot() const {
  DeliverySnapshot s{};
  for (const Slot& slot : block_->slots) {
    const DeliveryState st = slot.state.load(std::memory_order_acquire);   // the only read of the slot
    s.peers.push_back({slot.peer, st});
    count(s, st);                         // counters are derived from the SAME loaded value
  }
  s.complete = all_terminal(s.peers);     // also derived from the loaded values
  return s;                               // fully self-consistent value; nothing refers back to the block
}
```

Each slot is written only by the I/O thread that owns that connection (`Queued → Transmitting →
Transmitted`, or `Queued → Superseded`, or any state `→ Failed`). Because the counters and
`complete` are computed from the values that were actually loaded, a snapshot never
contradicts itself (the counts always add up to `peers.size()`).

What is **not** guaranteed: that all peers are read at the *same instant*. Example, with
three real events happening while the snapshot loop runs:

```text
real time   event                              
t1          A: Queued        B: Transmitting
t2          A: Transmitted   B: Transmitting      <- A finishes
t3          A: Transmitted   B: Transmitted       <- B finishes
snapshot reads slot A at t1 ("Queued"), slot B at t3 ("Transmitted")
=> the snapshot says (A: Queued, B: Transmitted), a combination that never existed at one instant
```

This is harmless because peers are independent: there is no rule that relates the state of one
peer to another. What does hold, and is enough for monitoring, waiting and tests:

- every peer's reported state was true at some instant during the `snapshot()` call;
- states only move forward, so a later snapshot is never "behind" an earlier one for any peer;
- `complete == true` is final: terminal states never change, so once seen it stays true;
- a ticket that was complete at publisher teardown reports `Failed`/terminal states, it never
  stays at `Queued` forever (constraint 3).

If a transactionally consistent view across peers were ever needed, it would require a lock
or a sequence-lock on the block and writers that wait for readers, which conflicts with the
"no user locking, no cost on the hot path" goal. That is why the weaker guarantee is proposed.

## 5. Do we need `cancel()`? (LT-5)

**Decision: no explicit `cancel()`/`close()` on `Publisher` and `Subscriber` in the
first version; RAII only.** Reasons:

- LT-5 already demands that destruction alone is sufficient, so the internal idempotent
  close exists regardless; exposing it only adds a second public path to test (every API
  call after `close()`, concurrent `close()`/destroy, ...).
- With a factory (section 8) a constructed `Publisher` is running for its whole life, so
  there is no "stopped" state that `cancel()` would have to introduce or that needs to be
  tested.
- Typical reasons for wanting `cancel()` are handled otherwise: early stop → destroy or
  `reset()` the object (hold it in `std::optional`/`unique_ptr` if it must be
  re-creatable); destruction inside a callback is supported (LT-2); two-phase shutdown
  (stop receiving, then finish processing) → `Subscriber::setHandler({})` (FR-SUB-8) or
  destroying the `Subscription`s.
- Adding `close()` later is a non-breaking change; removing it is not.

Arguments *for* keeping one: objects held by `shared_ptr` with unclear last release (e.g.
captured in many lambdas), bindings to languages without deterministic destruction, and
inspecting final statistics after stopping. If any of these is a real use case for you,
add only an idempotent, thread-safe `close()` (LT-5) – nothing else.

**Sessions** (the per-publisher connections of a subscriber, FR-SUB-5) need *some* way to
end individually. **Decided: RAII `Subscription`** – `[[nodiscard]] Subscription
Subscriber::connect(...)`, move-only, destroying it ends that session. There is no weak
handle and no `remove()`.

Question raised: does a weak handle cost performance on the hot path? Answer: **no handle
is ever touched on the receive path in either design.** Per message, the I/O thread works on
the connection object it owns; the user-facing handle is only used for (a) queries and
(b) teardown, both on user threads. The reasons for choosing the RAII handle anyway:

- *Ownership is visible.* The session lives exactly as long as the `Subscription`; no
  hidden list inside `Subscriber`, no explicit `remove()`, no stale `SessionRef` that must
  be validated by `weak_ptr::lock()` (an atomic compare-exchange per query) and may
  already be gone.
- *Symmetric with `Publisher`/`Subscriber`* (RAII only), and simpler to test (LT-5).
- `[[nodiscard]]` covers the one behavioural change from today, where the return value of
  `addSession` could be ignored ("you don't need to store it"): ignoring it now disconnects
  immediately, and the compiler says so.

Implementation guidance (architecture document to confirm): the `Subscription` holds a
`shared_ptr` to a small **control block** (published state snapshot, counters, closed flag,
link to the owner). The loop-owned connection object is a *different* object; the hot path
must not copy `shared_ptr`s per message (no atomic reference counting per read). The control
block outliving the subscriber/context is what makes LT-8 trivial: queries return the terminal
state and destruction becomes a no-op.

`Subscriber::sessions()` returns snapshots (`SessionInfo`, plain values), so observing never
needs a handle (CONN-3, CONN-6).

## 6. Context and configuration

### 6.1 Context as a thin value wrapper (decided direction)

Owner preference: value semantics, and the context should not have to outlive publishers
and subscribers. *(Assumption, please correct if misread: "must not outlive" is read as
"must not be **required** to outlive", i.e. the user may destroy their `Context` object
while publishers/subscribers still exist.)*

```cpp
class Context {                       // copyable, cheap: one shared_ptr to the shared core
 public:
  explicit Context(ContextOptions = {});       // starts the threads; throws on invalid options / thread creation failure
  // copy/move/destroy: default
 private:
  std::shared_ptr<detail::Core> core_;         // threads, loops, callback pool, logger, limits
};

Publisher::create(const Context&, PublisherOptions)   // copies the Context (one shared_ptr copy at creation)
Subscriber(const Context&, SubscriberOptions)         // same
```

At the call site it looks like a reference; internally every publisher/subscriber stores a
copy of the thin handle, which keeps the shared core alive. This corresponds to option A of
OD-1 (handles co-own the execution resource) and is **not expensive**:

- **Hot path: no cost.** The copy happens once at creation. The I/O threads work on
  loop-owned connection objects and never touch the `Context` or its reference count per
  message.
- **Teardown** stays deterministic in the normal case: whoever releases the last handle
  (user `Context`, publisher, subscriber, subscription) on an ordinary thread runs the core
  destructor, which stops and joins all library threads before returning.
- **The one real complication** is the last release happening *on a library thread* (e.g.
  a message handler destroys the last publisher, or a handler holds the last copy of a
  `Context`). A thread cannot join itself. Handling (to be specified in the architecture
  document): the core destructor detects this, moves the thread objects into a short-lived
  helper thread that owns nothing else, and returns. The library threads wind down on their
  own; they call no user code (handlers, logger) and touch only memory they own. The
  guarantee "no library thread is running when the last destructor returns" is therefore
  given for all cases except this one, which is documented (same as today's README).
- It avoids the surprising behaviours of the alternatives: no dangling pointer when the
  context is destroyed early (option B), and no handles silently turning inert (option C).

### 6.2 Options

```cpp
struct ContextOptions {
  size_t   io_threads         = 2;               // TH-1: I/O loops
  size_t   callback_threads   = 2;               // DEL-6: pre-started workers of the callback pool
  size_t   max_callback_threads = 16;            // DEL-6 / OD-12: the pool grows on demand up to this (0 = unlimited); tentative default
  Logger   logger             = defaultLogger;   // std::function<void(LogLevel, std::string_view)>  (OD-7)
  size_t   max_message_size   = 256u << 20;      // OD-3: ONE generic limit, applies to all publishers and subscribers
  KeepAlive keep_alive        = {};              // CONN-10
};
```

- **The maximum message size lives only on the context** (decided): one number, checked per
  frame on the receiving side (WP-L1) and before copying on the publishing side
  (FR-PUB-10). Resource-wise it costs nothing beyond what ROB-2 already bounds. The price of
  not having per-object limits is that one process cannot give a trusted publisher/subscriber
  a bigger limit than an untrusted one; if that ever matters, a second context with other
  options is the answer.
- Per-object options (`PublisherOptions`, `SubscriberOptions`) carry only per-object
  things: bind address and port, handler, delivery mode (inline vs. pooled, DEL-7),
  reconnect policy, timeouts.
- Invalid configuration (e.g. zero threads) is a programmer error → deterministic rejection
  by exception from the `Context` constructor; it happens once at start-up, not on the hot
  path.

## 7. Receive side

```cpp
class Message {                         // cheap to copy: shares the buffer (FR-SUB-6, LT-7)
 public:
  ConstBytes  bytes()      const noexcept;      // may be empty (FR-SUB-12)
  std::string_view text()  const noexcept;      // convenience view over the same bytes
  size_t      size()       const noexcept;
  bool        empty()      const noexcept;
  std::chrono::steady_clock::time_point receivedAt() const noexcept;   // FR-SUB-7
  const Endpoint& source() const noexcept;                              // FR-SUB-7
};
using MessageHandler = std::function<void(const Message&)>;
```

- A default-constructed or empty received `Message` is a valid object whose `bytes()` is
  an empty view; users never need a null check for empty messages (FR-SUB-12).
- Handler setting: in `SubscriberOptions` at construction and `setHandler()` at runtime
  (FR-SUB-8). Exceptions thrown by the handler are caught and logged (DEL-9).
- `Message` must remain valid after the `Subscriber`/`Context` is gone (LT-7).

### 7.1 Object model: N publishers, M subscribers

A `Subscriber` is **not** limited to one publisher: it is one consumer (one handler) with any
number of `Subscription`s, one per publisher it listens to.

```cpp
// N publishers (here 3, in some other process or in this one) and M = 2 consumers
tcp_pubsub::Subscriber a(ctx, {.handler = handlerA});
tcp_pubsub::Subscriber b(ctx, {.handler = handlerB});

std::vector<tcp_pubsub::Subscription> links;
for (const auto& endpoint : publisher_endpoints) {       // N endpoints
  links.push_back(a.connect({endpoint}));                 // N sessions in a
  links.push_back(b.connect({endpoint}));                 // N sessions in b
}
// Objects: N publishers + M subscribers.  TCP connections: N x M (one per session).
```

| Question | Answer |
|---|---|
| Do I need N×M `Subscriber` objects? | No: M subscribers, each with N subscriptions. |
| How does a handler know the publisher? | `Message::source()` (FR-SUB-7); per-publisher state/statistics via the `Subscription`. |
| What do several publishers in one `Subscriber` share? | The handler, its serial execution, and **one latest-wins slot** (OD-6): under overload the newest message from *any* of them wins. |
| How do I get per-publisher handlers or per-publisher latest-wins? | One `Subscriber` per publisher; cheap in the rewrite (no thread is bound to a subscriber). |
| What is `connect(candidates)` for? | The candidate list contains alternative addresses of the **same** publisher (e.g. host name, `.local`, IPv4/IPv6), not several publishers. |

A convenience that connects to a list of publishers in one call and returns a group handle can be
added later without changing the model.

## 8. Error reporting at start-up (FR-PUB-2, OD-4)

Question: should a publisher that cannot bind (port in use, bad address) be reported by a
**factory result** instead of a constructor plus a `Failed` state? Candidates:

| | A. Constructor + `Failed` state (today's style) | B. Throwing constructor | C. Factory returning `Result<Publisher>` (**proposed**) |
|---|---|---|---|
| Shape | `Publisher p(ctx, opts); if (p.state() != Running) …` | `try { Publisher p(ctx, opts); } catch (std::system_error&) …` | `auto r = Publisher::create(ctx, opts); if (!r) { r.error(); } Publisher& p = *r;` |
| Invalid object possible | **Yes**: every `Publisher` may be non-functional; `publish()` needs a `NotRunning` result, tests and docs must cover the state. | No | **No**: a `Publisher` that exists is running for its whole life (RAII only, no `close()`). |
| Failure is hard to ignore | No (the user can forget to check). | Yes | Yes if `Result` is `[[nodiscard]]`. |
| Ordinary condition handled as an exception | No | **Yes**: "port busy" is routine in port-probing code; also awkward with exceptions disabled (PLAT-7 only says exceptions *may* be used internally). | No |
| Error details | `error_code` accessor. | `std::system_error` carries the code. | `std::error_code` in the result. |
| Call-site ergonomics | Best (plain object). | Good. | More verbose; needs `std::optional`/`Result` handling to store in members. |
| C++17 support | Trivial. | Trivial. | Needs a small home-grown `Result<T>` (about 40 lines; later an alias for `std::expected`). `std::variant`/`std::optional` + out-parameter are clumsy alternatives. |
| Complexity inside the library | Extra state, extra branches on the publish path, extra tests. | Small. | Smallest object state machine. |
| Consistency | Differs from `Context` (throws). | Same as `Context`. | Different from `Context`, but justified: the context fails for system/programmer reasons once, a publisher fails for runtime reasons (port in use) that applications expect. |

Proposal: **C**, with `Result<T>::value()` throwing `std::system_error` for users who prefer
exceptions (so B is available as `Publisher::create(...).value()`). Consequences: no `state()`,
`isRunning()` or `PublishStatus::NotRunning`; `Subscriber` creation cannot fail at runtime
(it only allocates), so it keeps a plain constructor; `Subscriber::connect` returns a
`Subscription` and reports connection problems through the session state (CONN-3/CONN-4),
because connecting is asynchronous and retried (section 9 of the requirements).
Programmer errors (e.g. an empty candidate list) throw `std::invalid_argument`.

## 9. Illustrative sketch (non-binding)

```cpp
tcp_pubsub::Context ctx;                                   // thin value wrapper; may be destroyed before pub/sub (section 6)

auto made = tcp_pubsub::Publisher::create(ctx, {.address = "::", .port = 0});
if (!made) { log(made.error().message()); return; }        // e.g. port in use
tcp_pubsub::Publisher pub = std::move(made).value();       // always running while it exists

tcp_pubsub::Subscriber sub(ctx, {.handler = [](const tcp_pubsub::Message& m) { use(m.bytes()); }});
auto link = sub.connect({{"localhost", pub.port()}});     // Subscription, RAII, [[nodiscard]]
pub.waitForSubscribers(1, std::chrono::seconds(2));

auto r = pub.publish("hello");                             // string_view → ConstBytes, [[nodiscard]]
std::ignore = pub.publishParts({header_bytes, payload_bytes});  // gather, result deliberately discarded
std::ignore = pub.publish(tcp_pubsub::ConstBytes{});       // empty message (valid)

auto tracked = pub.publish("important", {.track_delivery = true});
auto snap    = tracked.ticket.snapshot();                  // value type, no user locking (FR-PUB-14)
```

(Designated initializers above are C++20; with C++17 use option structs with setters or
aggregate initialisation in member order.)

## 10. Open questions for the owner

Settled so far: RAII only (no `cancel()`); RAII `Subscription` (no weak handle);
ticket-based delivery status with per-peer detail and no pooling initially; `[[nodiscard]]`
result; `ConstBytes` (implicit only for byte-like element types); `publishParts` for gather;
factory returning `Result<Publisher>` (home-grown `Result<T>` until C++23); one max message
size on the context; context as a thin value wrapper that need not outlive
publishers/subscribers (option A, reading confirmed), with the documented exception when the
last release happens on a library thread; delivery-ticket snapshots are exact per peer and
monotonic but not one atomic instant across all peers (section 4.3, confirmed).

Still open:

1. Ticket slot layout and the remaining details are for the architecture document
   (`03-architecture.md`, not yet written).
