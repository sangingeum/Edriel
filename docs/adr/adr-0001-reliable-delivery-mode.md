---
title: "ADR-0001: Reliable Delivery Mode for Edriel"
status: "Proposed"
date: "2026-08-24"
authors: "solomon (architect) — decision proposal for owner"
---

# Context

Edriel is a C++20 multicast pub/sub over ASIO, using protobuf envelopes stamped
with a 0xED75E1ED magic header. Delivery today is strictly **best-effort**
UDP multicast:

- fire-and-forget `sendMessage()`: serialize, prepend magic, `async_send_to`
  the (config-derived) group, notify local subscribers after the strand returns.
- no retransmission, no ACK/NAK, no sequence numbers, no ordering guarantee,
  no dedup. Each receiver sees an independent, unordered, possibly-lossy cursor.
- payload hard-capped at the ~1500 B path MTU; larger sends are rejected at
  send time.
- participant discovery via 2 s heartbeats, 10 s alive timeout, 5 s cleanup;
  the registry keyed on `(pid, tid, uid)` already models every live peer.

The owner asks for a serious treatment of how to add a **reliable** mode —
the DDS QoS pair being best-effort vs. reliable. This record lays out the
option space, assigns each option a cost/ordering/backward-compat posture, and
makes one default recommendation for the owner to accept or reject. It is a
design-investment document, not an implementation plan.

A design tension should be stated up front, because it shapes everything below:

> **Reliable delivery and raw multicast fight each other in the general case.**
> Multicast is efficient *because* it is 1-to-N with no per-receiver state.
> Reliability is a *per-receiver* property (each receiver has an independent
> loss/ordering picture). Consequently, no honest reliable mode avoids
> per-receiver state — the design question is only *where* that state lives:
> as unicast control/repair lanes beside a retained multicast data plane, or
> as a fully unicast reliable transport that simply does not use multicast for
> the reliable path.

Anyone who believes a purely group-multicast NACK scheme delivers industrial
reliability under *heterogeneous* receiver loss is trading a real reliability
guarantee for a cheaper fiction. Under uniform loss on a controlled link it is
fine; under diverse receivers (different NICs, Wi-Fi vs. wired, separate
subnets) it degrades toward the unicast model anyway.

---

# Options

All take the form "**mechanism** → **when** → **cost** → **ordering** →
**back-compat**". Options A–D keep multicast on the data plane and differ in
where per-receiver state appears. Option E is the "drop multicast for the
reliable path" family.

## Option A — Group-level NAK-based reliability (NORM-family, e.g. PGM/tpgm)

**Mechanism.** Keep single-group multicast for *all* data transport. The
sender stamps a per-(publisher,topic) strictly increasing sequence number
(`tid` in the existing `Identifier` is recycled here). Each receiver detects a
gap against its own next-expected counter and responds over multicast with a
NAK naming the missing sequence. The sender caches the tail of outgoing
datagrams (a bounded per-topic history buffer) and, on any NAK, retransmits
the named range to the group. An SCR (sender current status) announcement each
κ interval bounds how long a receiver waits before NAK'ing so late-but-not-lost
datagrams are not spuriously repaired. No unicast lane required at all.

**When.** Homogeneous receivers on a single, hard-wired / controlled link where
loss is bursty *and* common to all members (e.g. one switch, one collision
domain). Best when multicast fan-out is the whole point and no receiver has
stricter needs than the group as a whole.

**Cost.**
- **Buffer:** bounded per-publisher per-topic send window; NAK horizon governed
  by window size. Bounded, not unbounded.
- **Latency:** a gap waits for the NAK round-trip + SCRM cadence before repair
  surfaces → multi-ms to tens-of-ms tail latency under loss; near-zero
  overhead when no loss (stepped NAK, not per-message ACK).
- **Throughput:** repair traffic is *broadcast* to every receiver even when only
  one needs it — 1-faulted repair costs all receivers. Repair amplification is
  the cardinal scaling tax.
- **Wire change:** no protocol change (magic header + envelope reused); needs a
  new NAK control *packet* and a NAK-capable receive path.
- **Code complexity:** a reorder/dedup buffer + sender window cache + NAK
  handler. Moderate, contained entirely in the multicast plane.
- **Ordering/dedup:** exactly-once and in-order *per (publisher, topic) per
  receiver*, but that guarantee is *per-receiver* — a fresh joiner cannot
  retroactively receive dropped history.

**Backward compatibility:** strongest of all plain options — any modern Edriel
node still speaks plain magic-numbered multicast; reliable nodes decode, the
best-effort path and existing message format are untouched. A node that never
learns NAK simply behaves as best-effort.

## Option B. Unicast-per-receiver selective reliability (DDS-RTPS-style): control + repair lane

**Mechanism.** Multicast remains the *data* plane for fan-out, but each live
participant establishes a **unicast reliability/control channel** (UDP
datagram ACK/NAK, optionally TCP if ordered-request semantics are wanted).
When the control lane is up, the publisher keeps per-receiver state: expected
sequence → gap map, flow-window, and a small NAK-window cache. On loss, the
**repair is sent unicast only to the receiver that missed**, not to the group.
Matches the "redundant unicast lane" pattern of production DDS RTPS
transports.

**When.** Groups with heterogeneous receivers (different link loss, a receiver
killed/restarted mid-stream, mixed wired+Wi-Fi), where group-NACK would wobble
and where you actually need per-receiver guarantees. This is the mainstream
honest-reliable pub/sub shape.

**Cost.**
- **Sender: per-receiver state grows linearly with N** live participants
  (unbounded; must bound with a max-receiver cap or flow control).
- **Latency:** NAK→unicast-repair path is fast (single RTT + one ACK); lower
  worst-case tail than A because repair is not queued behind everyone.
- **Throughput:** the happy path (no loss) is multicast + silent ACK — near
  best-effort fan-out; only the *faulting* receiver pays a unicast copy.
- **Wire change:** requires **a new unicast control socket per peer** and a
  new control-protocol session. This is the architectural change the brief
  flags: *the transport is no longer multicast-only.* It is also the right
  price to pay for real per-receiver reliability.
- **Ordering/dedup:** exactly-once, in-order per (publisher, receiver). New
  joiner gets no history unless paired with D.
- **Back-compat:** the multicast datagram format is *unchanged*; negotiation
  is additive at the application layer. A peer that never opens a control
  lane silently remains best-effort.

## Option C. Additive repair-only hybrid (recommended default)

**Mechanism.** A *throttled* hybrid of A and B that keeps B's per-receiver
ackbookkeeping cost low while still getting per-receiver order/repair:

- Data stays on multicast (unchanged envelope, magic header).
- Each receiver maintains a receive-reorder/dedup window and announces its
  cursor. Repair requests for a gap are throttled *and grouped*: if gaps
  affect many receivers, the sender multicasts the repair once (amortize the
  copy); if only one receiver is missing, it responds **unicast** to that one.
- The sender keeps a bounded retention window of recent (topic, seq) payloads
  (a "topic history" buffer, § D below, reused).

This is not a fourth protocol — it's an engineering realization that lets B
degrade gracefully: repair copy sits between the group-wide worst case (A) and
the fully-per-recipient case (B). **The recommended default.**

**Cost.**
- **Sender:** window cache (bounded) + per-receiver "which seq is fully ack'd"
  set; NOT a full per-recipient gap map unless loss is actually occurring.
- **Latency:** tail latency is minimized because most recovery is single-fault
  → unicast; hybrid lets a common-loss event multicast-fan the repair once.
- **Throughput:** rep assembly only under fault; silent otherwise.
- **Wire change:** the same new control/NAK packet as B — requires a unicast
  control socket per peer *for the control traffic* (the payload bytes never
  leave multicast except for repairs).
- **Order/dedup:** exactly-once, in-order per (publisher, topic) per receiver
  via the reorder/dedup maintenance window.
- **Back-compat:** unchanged for best-effort; reliable is opt-in (`reliable`
  QoS on a topic).

## Option D. Topic-history persistence + replay

**Mechanism.** The publisher journals the last-K payloads per topic/type into
a bounded ring (topic history). New or restarted subscribers send a
**replay-request** on their control lane; the publisher replays missed frames.
Ordering is by `tid`; delivery is at-least-once with dedup at the receiver
(the receiver keeps a ring of recently-seen `tid`).

**When.** The *late joiner / re-join* case: node crashes and restarts, or
subscribes mid-stream and legitimately may want the last-K it had under the
highest tier — "latest-wins flake subscribers."

**Cost.** Sender-side bounded ring buffer per topic. Latency of a join =
round-trip replay. No ongoing per-message cost — emptiest mode to maintain
for the most common reliable-ish gap (drop-in subscriptions).

**Ordering/dedup.** Per-receiver, in the ring.

**Back-compat:** the receive path is unchanged; replay is an additive query.

## Option E. Replace multicast with unicast reliability for the reliable path

**Mechanism.** For **reliable** topics, do not use multicast at all: each
subscriber opens a reliable (TCP / gRPC bidi-stream, both of which the repo
already has protobuf+gRPC+strand for) connection to each publisher. This is
the natural "if you really want reliability, use a reliable transport" hack —
and it's also a subtle CLAIM that the library should **not** try to bolt
reliability onto multicast when a reliable unicast stream exists.

**When.** High-throughput ordered, loss-sensitive streams where in-order
delivery and zero-drop matter more than fan-out latency. gRPC Streaming is
named as the future path; TCP-gRPC is the reference implementation.

**Cost.** Loses multicast fan-out efficiency entirely on the reliable path —
each additional receiver is a full TCP connection + copy. Code complexity is
largely absorbed by gRPC/asio plumbing that *already exists*. Low added wire
code; high added server-side connection management. Ordering: exactly-once,
in-order, at-least-once behaviors via TCP.

**Back-compat:** trivially additive; reliable nodes just don't engage the
multicast path for reliable topics. Also the *cleanest* semantics — it is
impossible to be "reliable" if the transport can reorder/flap.

## Summary matrix

| Option | Best when | Loss latency | Throughput tax | Ordering/dedup | Wire change |
|---|---|---|---|---|---|
| A group-NACK | homogeneous, controlled link | multi-ms tail | broadcast repair to all | exactly-once per-recv | none (new NAK packet) |
| B unicast | heterogeneous receivers | low (single RTT) | state linear in receivers | exactly-once per-recv | adds unicast control lane |
| C hybrid (default) | general | bounded (1-fault → unicast) | amortized repair | exactly-once per-recv | adds unicast control lane |
| D replay | late / restarted joiner | join-time replay | ring buffer only | at-least-once | none |
| E no-multicast | high-throughput, ordered | none (TCP) | N full streams | exactly-once in-order | additive (reliable path off-multicast) |

# Decision

**Recommendation: introduce an opt-in per-topic `reliable` QoS, defaulting to
the existing `bestEffort`(unchanged), implemented as Option C — the additive
hybrid — with Option D's topic-history buffer as the retention engine and
Option B's unicast control lane only when a faulting receiver actually disturbs
the group.**

Concretely:

1. Keep `bestEffort` the topic default and the multicast fast path untouched
   (magic-numbered datagrams). Wire format and the existing best-effort
   publisher/subscriber API stay byte-identical.
2. Per-topic QoS flag (the `QOS` TODO in the Topic proto). Only **reliable**
   topics pay for a send-side ring buffer, a per-(publisher, topic) sequence
   counter (reuse `tid`), a NAK control type, and (for uncontested receivers)
   a unicast NAK over the same IO strand.
3. Ordering = **exactly-once, per-order per receiver** via reorder/dedup
   window; no global ordering claim across receivers or across publishers
   (DDS-honest position).
4. Option D is rolled in as the *retention and replay* mechanism so the most
   common failure (missed tail during a restarted subscriber) is cheap; Option
   E (gRPC-stream) is the documented scale-out path if a fleet only wants
   reliable and throughput asks for it — the existing gRPC/asio foundation is
   explicitly a future target, so the wire change belongs at the QoS decision
   time, not bolted onto multicast later.

**Why this and not a full matrix:** the brief asks to choose from the two DDS
terms, not to ship a matrix of twelve modes. `bestEffort` already exists and is
fast; the honest engineering answer is: a single `reliable` tier, realized as
the most cost-effective per-receiver mechanism that does not lie about its
ordering guarantee. A pathological full DDS QoS matrix (plus the
strictly-ordered / keepLast / keepAll beast) is over-scoped for Edriel's
~1500 B MTU and single-node state today.

**Why not Option A alone:** group-wide NAK broadcast repairs a gap to *every*
receiver even when precisely one lost it — explosion amplifier, and the tail
latency is gated by the slowest faulting peer. It is the *cheap* reliable-by-
hack for chapter1, not an honest one.

**Why not Option B alone:** per-recipient ACK on every message discards the
efficiency that is the *whole point* of a multicast fan-out plane; the happy
path (no loss, the overwhelmingly common case) should not pay for per-ACK
bookkeeping. The C hybrid keeps B's repair-on-degradation path, states, and
ordering guarantee while keeping the no-loss silent path at best-effort cost.

# Consequences

Positive:
- **Backward compatible by construction** — best-effort nodes and the wire
  format are untouched; reliable is additive and opt-in.
- **Reliable mode does not tax the common no-loss path** — silent ACK, one
  copy for the typical single-miss repair, ring-buffer replay for joiners.
- **Ordering guarantee is honest and unambiguous**: exactly-once per (publisher,
  topic) per receiver, never a false global total-order claim.
- De-risks the documented "future gRPC streaming" direction by reserving
  reliability for the layer that can actually profit from it (TCP/gRPC), not
  painting it onto multicast.

Negative:
- **Architectural change is real and unavoidable for honest reliability:** a
  unicast control (NAK/repair) channel per peer is required. The multicast-only
  purity of Option E is gone for reliable topics. This is the cost of the
  guarantee and is called out explicitly so the owner is not surprised.
- Sender-side retention history + per-receiver cursor state add heap state that
  did not exist before (bounded, but nonzero).
- NAK-based convergence leaves a bounded window: in pathological
  un-acknowledging receivers the publisher must cap the window; a message older
  than the window cannot be retroactively guaranteed.
- Rejecting >MTU reliable payloads: implementing fragmentation/reassembly is
  deferred; reliable large messages must either be split at the app or wait for
  a future fragmentation/reassembly pass. The wire budget of ~1500 B is
  unchanged.

Follow-up actions (to propose, not yet scheduled):
- Add `QOS` (best_effort | reliable) to `Topic` in the proto.
- Design the control/NAK packet + per-receiver cursor registry in
  `participant_manager.hpp`.
- Determine fragmentation/reassembly policy for reliable payloads > 1500 B
  (recommend: defer; document app-side splitting).
- Vera: define a repro harness for a paired-lossy node (netem / toxiproxy) to
  validate the NAK window and ordered-exactly-once claim before API freeze.