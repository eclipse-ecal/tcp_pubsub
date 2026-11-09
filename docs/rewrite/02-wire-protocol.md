# tcp_pubsub rewrite – Wire Protocol Specification (normative)

Status: **FROZEN.** The rewrite MUST remain wire-compatible with every released
version of tcp_pubsub (protocol version 0). A new implementation must
interoperate in both directions with the existing implementation
(`tcp_pubsub/src/`), in particular a new publisher with an old subscriber and
vice versa.

Companion document: [01-requirements.md](01-requirements.md).

Key words **MUST**, **MUST NOT**, **SHOULD**, **MAY** are used as in RFC 2119.

Sources of truth for the existing behaviour:
`tcp_pubsub/src/tcp_header.h`, `tcp_pubsub/src/protocol_handshake_message.h`,
`tcp_pubsub/src/publisher_session.cpp`, `tcp_pubsub/src/subscriber_session_impl.cpp`,
README section "The Protocol (Version 0)".

Where the existing implementation is stricter, looser or buggy compared to this
document, section 9 lists the deviations.

---

## 1. Transport

- **WP-T1** TCP stream, IPv4 or IPv6. There is no TLS, authentication or
  encryption (non-goal, see requirements NG-1).
- **WP-T2** The **publisher** listens and accepts; the **subscriber** connects.
  Direction of connection establishment is fixed.
- **WP-T3** Both ends MUST set `TCP_NODELAY` on every connection.
- **WP-T4** One TCP connection carries one publisher → subscriber stream.
  There is no multiplexing and no topic/channel name on the wire. A publisher
  is identified only by `(address, port)`.
- **WP-T5** All multi-byte integers on the wire are **little-endian**,
  regardless of host byte order. Implementations MUST convert explicitly and
  MUST NOT rely on struct layout of the host.
- **WP-T6** Messages are delimited only by the header's length fields. There
  are no delimiters, checksums, sequence numbers or timestamps.

## 2. Frame layout

Every message (handshake or data) is one frame: `header` followed by `payload`.

### 2.1 Header (version 0: 12 bytes)

| Offset | Size | Field         | Type | Meaning |
|-------:|-----:|---------------|------|---------|
| 0      | 2    | `header_size` | u16  | Total size of this header in bytes, including this field itself. Senders MUST send `12`. |
| 2      | 1    | `type`        | u8   | Message type, see 2.3. |
| 3      | 1    | `reserved`    | u8   | Senders MUST send `0`. Receivers MUST ignore the value. |
| 4      | 8    | `data_size`   | u64  | Number of payload bytes that follow the header. |

Total header size is 12 bytes, no padding. The structure is `#pragma pack(1)`
in the existing implementation.

### 2.2 Forward/backward compatible header parsing (MUST for receivers)

Receivers read the header in two steps:

1. Read the first 2 bytes → `remote_header_size` (little-endian).
2. Read `min(remote_header_size, 12) - 2` further bytes into the header.
   If `remote_header_size > 12`, read and **discard** `remote_header_size - 12`
   additional bytes before the payload.
   Header bytes that were not sent (because `remote_header_size < 12`) are
   interpreted as `0`.

Rules:

- **WP-F1** `remote_header_size < 2` is a protocol error → close the connection.
- **WP-F2** `remote_header_size` in `[2, 11]` is accepted and parsed per the
  zero-fill rule above, but such a header does not contain the complete
  `data_size` field and is therefore **malformed for `RegularPayload`**: the frame
  carries no payload and MUST NOT be delivered to the user as a message (in
  particular not as an *empty* message, WP-P3). The connection is not closed.
  Receivers MUST NOT treat a short header as an error beyond what WP-F1/WP-P*
  demand.
- **WP-F3** `remote_header_size > 12` is accepted; unknown header bytes are
  skipped. The skipped byte count is not bounded by the protocol (max 65535) and
  therefore needs no allocation beyond a small scratch buffer.
- **WP-F4** The comparison `remote_header_size < 2` MUST be performed **after**
  converting from little-endian to host order.

### 2.3 Message types

| Value | Name                | Payload |
|------:|---------------------|---------|
| 0     | `RegularPayload`    | Opaque user bytes (any content, any length ≥ 0; length 0 = *empty message*, WP-P2/P3). |
| 1     | `ProtocolHandshake` | `ProtocolHandshakeMessage`, see 3. |
| 2–255 | reserved            | Future use. |

- **WP-M1** A receiver that gets an **unknown `type`** in a position where it
  accepts data frames MUST consume and discard the payload and continue with
  the next frame (it MUST NOT close the connection, MUST NOT deliver it to the
  user). Rationale: forward compatibility, "old subscribers never interpret
  future message types as payload".

### 2.4 Payload and size rules

- **WP-P1** `data_size` is a u64. Implementations MUST guard all arithmetic and
  allocation against overflow and absurd values (see 7, "Limits").
- **WP-P2** **Empty messages.** A `RegularPayload` frame with `data_size = 0`
  (a complete 12-byte header and no payload bytes) is a valid **empty message**.
  A publisher MUST send an empty user message as exactly such a frame (the
  existing publisher already does). Wire bytes are unchanged compared to the
  existing implementation.
- **WP-P3** Receiving a `RegularPayload` frame with a complete header and
  `data_size == 0`:
  - by a **subscriber**: MUST be delivered to the user as an empty message
    (latest-wins applies as for any message). **This is a deliberate behaviour
    change:** existing subscribers silently ignore such frames. Consequently an
    empty message is only observable by subscribers built from the rewrite (old
    publisher → new subscriber works as well); an old subscriber loses the
    message without any error. Documented limitation, no wire change.
  - by a **publisher** (i.e. from a subscriber, after the handshake): MUST be
    ignored (see 5.4). The existing publisher closes the connection; this only
    matters for new-subscriber → old-publisher, and a new subscriber never
    sends anything after the handshake (WP-D5).
  - A zero-length frame of an **unknown** type is discarded like any other
    frame of that type (WP-M1), never delivered.
- **WP-P4** Payload bytes are opaque. The library neither inspects nor
  transforms them.

## 3. Handshake

`ProtocolHandshakeMessage` (the payload of a `ProtocolHandshake` frame):

| Offset | Size | Field              | Type | Meaning |
|-------:|-----:|--------------------|------|---------|
| 0      | 1    | `protocol_version` | u8   | Protocol version. Currently only `0` exists. |

- **WP-H1** Request and response use the **same layout**. Both are a
  complete frame: header (`type = 1`, `data_size = 1`) + 1 payload byte.
  Total length 13 bytes.
- **WP-H2** Receivers of a handshake payload MUST accept payloads longer than 1
  byte (forward compatibility: additional bytes are ignored; use only byte 0)
  and treat a payload of fewer bytes than the struct as zero-filled.
  (With `data_size == 0` see WP-P3.)
- **WP-H3** Version negotiation: the **subscriber** states the maximum protocol
  version it supports (today `0`). The **publisher** answers with the version
  that will be used, which MUST be `≤` the requested one and `≤` the highest
  version the publisher supports. Today this is always `0`.
- **WP-H4** If a subscriber receives a response with `protocol_version` greater
  than the highest version it supports, it MUST treat the connection as failed
  (close it, report an error, follow the reconnect policy). This is the only
  handshake failure reported by a subscriber.

### 3.1 Message flow

```
Subscriber                          Publisher
    | --- TCP connect ----------------> |
    | --- Handshake(request, v0) -----> |   subscriber speaks first
    | <-- Handshake(response, v0) ----- |   publisher answers once
    | <-- RegularPayload -------------- |   from here on: data only,
    | <-- RegularPayload -------------- |   publisher → subscriber
    |            ...                    |
```

- **WP-H5** The subscriber MUST send the handshake request first, immediately
  after the TCP connection is established, before sending anything else.
- **WP-H6** The publisher MUST NOT send any frame before it has received and
  validated the handshake request. Its first frame MUST be the handshake
  response. It MUST NOT send `RegularPayload` frames before the response frame
  has been completely written.
- **WP-H7** A publisher MUST treat a connection as *streaming* (eligible for
  data, and counted as a subscriber) only after it has sent the response.
- **WP-H8** A subscriber MUST treat a connection as *established* only after it
  has received a valid handshake response (WP-H4 satisfied). The existing
  implementation considers itself connected as soon as TCP connects; the rewrite
  SHOULD NOT reproduce that, see requirements FR-SUB-10 and section 9.
- **WP-H9** Publisher handling of the *first* frame of a new connection:
  - `type == ProtocolHandshake` (and `data_size ≥ 1`) → send response.
  - Any other type, or `data_size == 0` → close the connection (no response).
  The publisher MUST NOT wait for or require more than one handshake per
  connection.
- **WP-H10** Handshake timeout (new, not in the existing implementation, MUST
  NOT change bytes on the wire): a publisher SHOULD close connections that have
  not completed the handshake within a configurable time (default 5 s). A
  subscriber SHOULD fail the attempt if no response arrives within a
  configurable time (default 5 s).

## 4. Data phase

- **WP-D1** After the handshake the publisher sends `RegularPayload` frames,
  each a complete 12-byte header (`header_size = 12`, `type = 0`,
  `reserved = 0`, `data_size = N`) followed by exactly `N` payload bytes.
- **WP-D2** Header and payload SHOULD be written as **one contiguous write**
  (one buffer, one `send` where possible) to minimise latency and segments.
- **WP-D3** Frames are delivered in the order sent per connection.
  Frames MAY be omitted entirely (latest-wins dropping is a library policy,
  see requirements DEL-*), but a frame that is started MUST be completed;
  a partially written frame MUST NOT be abandoned or interleaved with
  another frame. The only way to drop an in-flight frame is to close the
  connection.
- **WP-D4** The subscriber reads frames strictly in sequence and MAY pause
  reading at any time (TCP flow control provides back-pressure).
- **WP-D5** The subscriber MUST NOT send any further bytes after the handshake
  request. (Existing publishers never read after the handshake.)

## 5. Connection lifecycle and error handling

### 5.1 State machines (informative for implementers, behaviour normative)

Publisher side, per accepted connection:

```
Accepted ──(valid handshake request)──▶ Streaming ──(error|EOF|local close)──▶ Closed
   │
   └─(invalid first frame | read error | timeout)──▶ Closed
```

Subscriber side, per session:

```
Resolving ─▶ Connecting ─▶ Handshaking ─▶ Streaming ─(error|EOF)─▶ Backoff ─▶ Resolving …
                                 └─(invalid/unsupported response)──▶ Backoff
```

### 5.2 Closing

- **WP-C1** Either side closes by closing the TCP connection. There is no
  goodbye message.
- **WP-C2** Any protocol error (WP-F1, WP-H4, WP-H9, oversized payload per
  section 7, read/write error, EOF in the middle of a frame) closes the
  connection. EOF *between* frames is a normal close.
- **WP-C3** Receiving garbage MUST NOT crash, hang, leak or allocate an
  unbounded amount of memory; the connection is closed instead.

### 5.3 Socket-level requirements (not visible in bytes, but required for correct behaviour)

- **WP-S1** Writing to a connection closed by the peer MUST NOT raise
  `SIGPIPE` (use `MSG_NOSIGNAL` on Linux, `SO_NOSIGPIPE` on macOS, nothing on
  Windows).
- **WP-S2** Listener: `SO_REUSEADDR` MUST be enabled on Linux and macOS so that
  a restarted publisher can re-bind a port in `TIME_WAIT`. On Windows the
  existing implementation enables `SO_REUSEADDR`, which has different semantics
  (allows port sharing). The rewrite SHOULD NOT allow two live listeners on the
  same port on Windows (use `SO_EXCLUSIVEADDRUSE` / no reuse), unless tests
  demonstrate that re-binding after close then fails; see open decision OD-5 in
  the requirements.
- **WP-S3** Listener bound to the IPv6 wildcard `::` MUST explicitly set
  `IPV6_V6ONLY = 0` so IPv4 clients are accepted on all supported platforms (the
  public documentation promises this; the existing code relies on OS defaults,
  which differ on Windows).
- **WP-S4** TCP keep-alive SHOULD be enabled on both sides, with interval values
  configurable. It is the only mechanism for detecting a silently vanished
  peer, since the protocol has no heartbeat (see 5.4 and 8).

### 5.4 Reading on the publisher side after the handshake (compatible extension)

The existing publisher stops reading after the handshake and therefore only
notices a dead subscriber when a write fails. A new publisher SHOULD keep a read
pending on every streaming connection so that EOF/RST is detected immediately.

- **WP-R1** Bytes received after the handshake are parsed as frames
  (WP-F*), the payload is **discarded in constant memory** (streamed through a
  small scratch buffer, never allocated in full), and the frame is ignored.
- **WP-R2** Such frames MUST NOT cause the connection to be closed, except for
  WP-F1 and read errors.

## 6. Byte-exact examples (use as conformance vectors)

All values hexadecimal, bytes in wire order.

**Handshake request/response, version 0** (13 bytes):
```
0C 00 | 01 | 00 | 01 00 00 00 00 00 00 00 | 00
header_size=12, type=Handshake, reserved=0, data_size=1, protocol_version=0
```

**Data frame carrying the 2 bytes "Hi":**
```
0C 00 | 00 | 00 | 02 00 00 00 00 00 00 00 | 48 69
```

**Data frame with a 4 GiB + 5 byte size field (must be rejected by the limit, no allocation):**
```
0C 00 | 00 | 00 | 05 00 00 00 01 00 00 00 | ...
```

**Header extended by a future version (header_size = 16, 4 unknown bytes `DE AD BE EF`) – receiver must skip them:**
```
10 00 | 00 | 00 | 02 00 00 00 00 00 00 00 | DE AD BE EF | 48 69
```

**Unknown message type 7 – receiver must discard payload and keep the connection:**
```
0C 00 | 07 | 00 | 03 00 00 00 00 00 00 00 | 01 02 03
```

**Empty message (12 bytes, header only) – subscriber delivers an empty message (WP-P3):**
```
0C 00 | 00 | 00 | 00 00 00 00 00 00 00 00
```

**Short header (header_size = 4: no `data_size` field → malformed, no payload, NOT delivered, connection stays open; WP-F2):**
```
04 00 | 00 | 00
```

## 7. Limits (implementation policy – not on the wire)

- **WP-L1** The receiver MUST validate `data_size` against a configurable
  `max_payload_size` **before** allocating or reading the payload and MUST close
  the connection if it is exceeded. The default is defined in the requirements
  (OD-3). The value must be at least 16 MiB (existing test) and MUST be
  expressible up to the platform's `size_t` range.
- **WP-L2** Never trust a peer-supplied size for pre-allocation beyond the
  configured limit. A payload SHOULD be allocated only when its size has been
  validated, and the allocation failure (`std::bad_alloc`) MUST be caught and
  treated like any other protocol error for that connection.
- **WP-L3** A publisher SHOULD limit the number of connections that are in the
  *Accepted* (pre-handshake) state (default: a few hundred) to protect itself
  from connection floods.

## 8. What the protocol does NOT provide

- No topic, type or schema information; no subscriber identity (a publisher only
  knows the remote IP and port).
- No acknowledgements, sequence numbers, retransmission, replay or history.
- No heartbeat; liveness relies on TCP (keep-alive, reads, write errors).
- No compression, fragmentation or multi-part messages. A multi-buffer user send
  is concatenated by the publisher into one payload.

Any addition would be a new message type or a new protocol version negotiated by
the handshake (WP-H3) and is out of scope for this rewrite.

## 9. Deviations of the existing implementation (do NOT reproduce)

| # | Existing behaviour | Required behaviour |
|---|--------------------|--------------------|
| D-W1 | `header_size < 2` is checked before `le16toh`. | WP-F4. |
| D-W2 | Payload buffer is allocated from `data_size` before the message type is looked at; no upper limit. | WP-L1/L2. |
| D-W3 | Publisher answers with version 0 regardless of the request. | Same today, but implement `min(request, supported)` (WP-H3). |
| D-W4 | Publisher counts a connection as subscriber from `accept`, i.e. before the handshake. | WP-H7. |
| D-W5 | Subscriber is "connected" after TCP connect, before the response. | WP-H8. |
| D-W6 | Publisher never reads after the handshake. | WP-R1 (SHOULD). |
| D-W7 | Publisher closes on a zero-length frame from a subscriber; subscriber silently ignores zero-length data frames (empty user messages are lost). | WP-P2/P3: empty messages are delivered by the subscriber; publisher ignores zero-length frames. |
| D-W8 | The handshake response write is not serialised with the first data write (two `async_write`s can be in flight on one socket). | WP-D3: exactly one write in flight per connection, frames never interleave. |
| D-W9 | No handshake timeout. | WP-H10. |
| D-W10 | No `SIGPIPE` protection, no keep-alive, `IPV6_V6ONLY` not set. | WP-S1, S3, S4. |

## 10. Conformance tests required for this specification

Each item must be an automated test using raw sockets (independent of the
library under test) in addition to library-to-library tests. See
requirements section 14 (QA-3).

1. Golden bytes: library publisher emits exactly the response and frame bytes
   of section 6 (sniff with a raw client).
2. Golden bytes: library subscriber emits exactly the request of section 6.
3. Fragmentation: the library subscriber/publisher works when every byte of the
   handshake and a frame arrives in a separate TCP segment (1-byte writes).
4. Coalescing: multiple frames in a single TCP segment are delivered separately
   and in order.
5. `header_size > 12` and `< 12` handling (section 6 vectors).
6. Unknown message type is skipped (subscriber) and ignored (publisher
   post-handshake).
7. Oversize `data_size` → connection closed, process memory not grown.
8. Handshake violations: first frame not a handshake, zero-length first frame,
   `header_size = 0/1`, unsupported response version → connection closed.
9. Peer closes mid-frame → no delivery of the partial frame, no crash.
10. Old-implementation interop: the existing library (pinned release) as peer
    in both roles (can be a CI job that builds a pinned tag). Expected
    differences are limited to empty messages (item 11).
11. Empty messages: new publisher → raw client sees the 12-byte header-only frame;
    raw server sending it → new subscriber delivers an empty message; mixed
    bursts of empty and non-empty frames; new publisher → old subscriber does
    not crash or disconnect (message is silently not delivered); a zero-length
    frame sent by a subscriber after the handshake does not close a new
    publisher's connection; short-header `RegularPayload` is not delivered.
