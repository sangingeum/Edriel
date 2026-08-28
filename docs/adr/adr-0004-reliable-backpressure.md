---
title: "ADR-0004: Reliable-Path Backpressure (Bounded Outbox, Tri-State Send)"
status: "Accepted (implemented; butler decision round 2026-08-27, gates passed 2026-08-28)"
date: "2026-08-27"
authors: "solomon (architect) — decision proposal for owner"
---

# Context

Owner question: **"Is there a way to dynamically adjust the sender's send rate?
The backpressure feature seems essential. How are you going to implement it?"**

The phrase "dynamically adjust the send rate" has two readings, and this record
resolves both: (1) the sender must not push faster than the slowest reliable
subscriber can absorb (reactive backpressure — the sender *stops* when the pipe
is full), and (2) an optional sender-side pacing ceiling (a configured rate
*limit*) so an application can bound its own offering regardless of subscriber
health. (1) is the essential part; (2) is a cheap complement the same mechanism
can carry.

Grounding (verified against HEAD `d2dfda5`, `master == origin/master`, CI green
— not re-derived, not assumed):

- **Send path today.** `Edriel::publishReliable` (`Edriel/Edriel.cpp:1503`)
  takes `grpcServiceMutex_`, resolves subscribers from the topic shard, stamps
  `tid = reliablePublisherSeq_[key] + 1`, then loops subscribers calling
  `grpcService_->pushData(sk, frame)` → `SubscriberReactor::enqueue`
  (`EdrielGrpcService.cpp:194`) → `outbox_.push_back`. The outbox is an
  **unbounded** `std::deque<ParticipantData>` (`EdrielGrpcService.hpp:125`,
  `std::mutex m_` + `bool writing_` single-in-flight `StartWrite`, `OnWriteDone`
  pops the next). `pushData` returns `false` only when the subscriber is not
  connected; a connected-but-slow subscriber absorbs everything into RAM.
  **The sender thread never observes subscriber lag.**
- **Measured gap** (`benchmark/benchmark_reliable.cpp`, README Reliable-QoS
  baseline): unpaced offer ~1M msgs/s vs single-subscriber absorb ~86–90k
  msgs/s (stream write-out ceiling ~340k/s measured earlier in B4). The ~10×
  gap currently lands in publisher-side memory, not in backpressure.
- **Existing harness affordances:** B2 (offer/absorb split, per-sub
  exactly-once assert), B4 (unpaced flood ceiling), and a 50k-frame lag
  backstop (`kMaxLagFrames`) that stops the sender when the subscriber lags —
  i.e. the benchmarks already *simulate* backpressure in the harness because
  the transport does not have it.
- **Constraints that must hold:** per-(publisher, topic) `tid` ordering per
  subscriber; receiver exactly-once window `kReliableWindowSize = 256`
  (`Edriel/Edriel.hpp:59`); sends serialized by `grpcServiceMutex_`;
  anti-spoof/registry untouched; best-effort multicast path byte-identical and
  untouched; ADR-0002 ERRATA already documents the unbounded outbox as a
  retired claim and names this as the planned follow-up.

The design tension this record resolves:

> **"Reliable" must mean *lossless with a bound*, not *lossless by buffering
> without limit*.** A transport that cannot tell the sender to slow down is a
> buffer, not a reliable channel — the failure it defers today is OOM at
> exactly the moment the reliable path matters (a slow or stalled subscriber).

The questions are split into: mechanism (§Q1), caller surface (§Q2), tid
lifecycle under refusal (§Q3), partial fan-out (§Q4), fairness (§Q5),
configuration (§Q6), tests/benchmarks (§Q7), recommendation (§Decision),
minimal first slice (§Slice), consequences (§Consequences).

---

# Options

## Q1 — Mechanism

### Option 1A — Bounded per-reactor outbox with HWM/LWM

**Mechanism.** Cap `outbox_` at a configured bound (default comfortably above
the 256-frame receiver window, e.g. 1024). `enqueue` returns a status:
`Accepted`, or `Backpressured` when the outbox is at its **high-water mark**
(HWM, configurable, e.g. 75% of the bound). On `OnWriteDone` drain, when the
outbox falls to or below a **low-water mark** (LWM, e.g. 25%), the reactor
clears its backpressured flag — that is the "resume" signal. This is the
classic producer/consumer HWM/LWM gate and is entirely local to
`SubscriberReactor`; no wire change, no new protocol message, no change to
ordering (the deque is still FIFO under `m_`).

**Cost.** Small, contained diff: `enqueue` signature, two atomics/flags, one
constant pair. The cost is honest and unavoidable — *someone* must observe
fullness, and the outbox is where fullness lives. Refusal is instantaneous
(no blocking in the gRPC executor thread), which matters: `enqueue` is called
under `grpcServiceMutex_` via `pushData`, so a blocking variant would serialize
*all* reliable topics behind one slow subscriber.

### Option 1B — gRPC-native flow control (`WriteOptions` / `WriteBufferHint`)

**Mechanism.** Use gRPC's built-in write buffering: `WriteBufferHint` /
`SetWriteBufferSize` and let the core HTTP/2 flow-control window signal
fullness via `WriteReturnsNotSent`-style feedback, i.e. check
`g.grpc_call_error` / pending-write counts.

**Cost.** Rejected on three grounds. (a) asio-grpc's callback-server reactor
API does not expose a clean "write refused, try later" primitive for the
server-push pattern we use — the single-in-flight `StartWrite` + `OnWriteDone`
pattern is already the correct use of the API, and core buffering sits *below*
it, so our deque would still absorb the overload first. (b) The granularity is
the HTTP/2 connection/window, not the logical subscriber, and tuning bytes vs
frames conflates two layers. (c) It moves the pressure point into opaque gRPC
core state, which is harder to test deterministically than our own deque.
gRPC-native flow control is a good *transport* bound (it already throttles the
wire at ~340k/s); it is not an application-visible backpressure surface.

### Option 1C — Receive-side credit window

**Mechanism.** Subscriber grants `tid` credits (à la RPC credit flow); the
sender blocks or limits when credits run out. It is the most *precise* mechanism
— the pressure signal comes from the actual consumer.

**Cost.** Requires a new wire direction (subscriber→publisher control frames on
the bidi stream), credit accounting on both ends, resync semantics on
reconnect, and — critically — it *couples* the sender to per-subscriber
round-trips: a slow subscriber's credit starvation now throttles the publisher
loop unless fan-out is made per-subscriber asynchronous (which it is not today;
`publishReliable` loops inline under `grpcServiceMutex_`). This is real
protocol work (new message type, lifecycle, reconnect rules) for a benefit
that Option 1A already delivers at the only point where overload can be
detected cheaply — the outbox. **Deferred as a future refinement**, not the
default. Note also that credits regulate *unacknowledged* data; Edriel's
exactly-once window is already a de-facto credit scheme on the receive side
(`kReliableWindowSize = 256`) — 1C would effectively mirror it to the sender.

### Option 1D — Rate limiter at `sendMessage`

**Mechanism.** Token-bucket pacing inside `publishReliable`: the sender self-limits
to N msgs/s regardless of subscriber state.

**Cost.** Cheap, but solves the wrong problem alone: it bounds the *rate*, not
the *backlog*. A subscriber at 10k/s absorbing from a 50k/s paced sender still
accumulates unboundedly, just more slowly. It also caps healthy subscribers'
throughput to the configured rate even when the pipe could carry more. It is,
however, the direct implementation of the owner's literal "dynamically adjust
the send rate" — so it is kept as an **optional sender-side ceiling**
(config knob, default off) layered on top of 1A, not as the backpressure
mechanism itself.

## Q2 — Caller surface

### Option 2A — Keep `bool`, add a tri-state return on a new method

`publishReliable` becomes (or gains an overload returning)
`std::expected<SentOk, SendError>`-shaped tri-state: `Sent`,
`Backpressured` (retryable, tid not consumed — see Q3), `NoSubscribers` /
`NotServing` (today's `false` cases).

**Cost.** Breaks nothing if additive: keep `bool publishReliable(...)` as a
thin wrapper (`return tryPublishReliable(...) == Sent`), migrate internals to
the tri-state core. `std::expected` is a C++23 facility and the house
standard is C++20 — a small `enum class ReliableSendResult { Sent,
Backpressured, NoSubscribers, NotServing }` is the honest, dependency-free
shape.

### Option 2B — Blocking send (sender waits for LWM)

The sender's thread parks until the outbox drains below LWM. This is the
"true backpressure pushes back on the producer" ideal — the caller's send rate
is *automatically* adjusted to the slowest subscriber without any caller
changes.

**Cost.** Rejected as default: `publishReliable` holds `grpcServiceMutex_` for
its whole body; a blocking variant would serialize every reliable topic behind
one slow subscriber and can deadlock against the same thread's other duties.
Blocking semantics belong to the *caller's* thread, chosen by the caller — the
library should report `Backpressured` and let the app retry/park/rate-limit.
If the owner wants blocking semantics, the app-side wrapper (retry loop with
backoff, or a condition-variable "sendable" wait offered later) is the right
place, not the transport.

### Option 2C — Optional callback / `sendable()` probe

Completing the surface: `onSendable(topic, subscriberKey)` callback fired at
LWM-cross, and/or a `bool isSendable(subscriberKey)` probe. Either is a
cheap addition; the callback avoids app-side polling, the probe avoids
callback re-entrancy risk (a callback that publishes again from the gRPC
executor thread is a footgun). **Recommend the probe first, callback later** —
the probe is re-entrancy-free and sufficient for a retry loop.

## Q3 — tid lifecycle under backpressure (the critical one)

**Current code** (`Edriel.cpp:1562`): `reliablePublisherSeq_[key] = nextSeq`
is committed **before** the subscriber loop, with an explicit comment that the
frame is "guaranteed to be sent" — true today only because `pushData` always
buffers. The moment 1A makes `pushData` able to refuse, that invariant breaks,
and a refused frame that consumed its tid creates a **permanent window gap** on
every subscriber's exactly-once window (receiver waits for a tid that never
arrives; `(tid - nextExpected) < kReliableWindowSize` buffer logic then
stalls or the window rolls and the frame is "delivered" as a gap).

**Rules the implementation must follow:**

1. **A backpressured frame must NOT consume its tid.** Commit
   `reliablePublisherSeq_[key] = nextSeq` only after the frame has been
   *accepted* (enqueued to at least one live subscriber, per Q4). On
   `Backpressured`, `reliablePublisherSeq_[key]` is left at `nextSeq - 1`, so
   a retry stamps the **same tid**.
2. **Retry = same tid, full re-stamp.** The caller re-invokes
   `publishReliable` with the same payload; the transport re-stamps
   `nextSeq` (= the uncommitted tid) onto a freshly serialized frame. No
   caller-side tid bookkeeping, no half-stamped stale frame ever on the wire.
   This is exactly the semantics the MTU-guard path already uses (serialize /
   MTU checks happen before commit today — the commit point simply moves to
   after the enqueue, joining the existing "never consume a tid for a frame
   that never left" rule documented in the `nextSeq` comment).
3. **No partial-commit hazard across subscribers.** Because tid is per
   (publisher, topic) — not per (publisher, topic, subscriber) — a frame
   accepted by subscriber A and refused by subscriber B (Q4 partial fan-out)
   *has* committed its tid. B's window therefore never sees the tid at all:
   correct, because B was backpressured *before* acceptance, i.e. before
   commit. The rule that makes this safe: **commit iff at least one
   subscriber accepted; a subscriber that refuses was never offered the tid,
   and the retry re-stamps the same tid — subscribers that already accepted
   see the same tid twice, which their dedup window absorbs as the duplicate
   it is.** This is precisely the at-least-once→exactly-once conversion the
   window already performs for retransmits.

This is the strongest argument *for* try-send (2A) over blocking (2B): a
refusal is a clean, pre-commit, retryable state; a blocking send inside the
current lock discipline is where tid lifecycle and deadlock hazards multiply.

> **ERRATA (2026-08, post-implementation review): same-tid retry covers ONLY
> the all-refused case — not partial fan-out.** Rule 3's narrative above
> describes a partial-fan-out same-tid re-offer that the implementation
> deliberately does not perform and cannot perform: once ANY live subscriber
> accepts, `tryPublishReliable` returns `Sent` (the tid is committed) and
> there is no API surface to re-offer a committed tid to a subscriber that
> refused. Empirically confirmed by the fairness-test root-cause
> investigation (`t_143a98cf`: a backpressured subscriber's reactor can keep
> draining into socket buffers, so it can still be the accepting side while
> another subscriber refuses). Consequences, binding on callers: **(a)**
> same-tid retry applies only when ALL live subscribers refused
> (`Backpressured`, tid uncommitted); **(b)** under partial acceptance, the
> refused subscriber's window gap is permanent at the transport level —
> "Sent" means "committed (≥1 accepted)", never "all subscribers will
> deliver"; **(c)** the only safe fan-out calling pattern is the
> `isSendable()` all-gate — offer a frame only while every live subscriber
> reports sendable (as implemented in `benchmark_reliable.cpp` B3 and
> documented in the README behavior-change note); **(d)** a future
> `reOfferCommittedTid(subscriberKey)` surface is noted for v2 — deferred;
> the gate pattern is simpler and lossless by construction.

## Q4 — Partial fan-out

### Option 4A — Best-effort per subscriber, report the result (recommended)

Send to every subscriber independently (as today's loop already does —
`pushData` per sub is independent); each returns Accepted/Backpressured/Dead.
Overall result: `Sent` if ≥1 accepted, `Backpressured` if all live subscribers
refused (tid uncommitted), `NoSubscribers` if none. Surface per-subscriber
detail via an optional out-param or callback (`deliverability` map:
subscriberKey → result) for callers that need it.

**Cost.** One slow subscriber no longer gates the others (this is also the
Q5 fairness answer). The caller must understand "Sent" ≠ "delivered to all" —
mitigated by the deliverability report. Matches the semantics of the existing
`pushedAny` bool.

### Option 4B — All-or-nothing

Block/reject the whole publish if any subscriber is backpressured.

**Cost.** Rejected: turns the slowest subscriber into a global rate limiter
(fairness violation, Q5), requires either blocking (2B's problems) or
retry-the-whole-fan-out machinery (re-stamping hazards against the
already-accepted subscribers). There is no per-subscriber delivery
guarantee in the transport today that all-or-nothing would strengthen —
reliability per subscriber is the receiver window's job, not the fan-out
loop's.

## Q5 — Fairness

Already structurally satisfied and must be preserved: `pushData` /
`enqueue` are independent per subscriber; a slow subscriber only fills its own
outbox. The rule for the implementation: **the backpressured state, the HWM/LWM
flags, and any future LWM callback must all be strictly per-reactor
state — never a shared "someone is slow" flag on the service or an
upgraded `grpcServiceMutex_` hold.** The outbox bound is per subscriber; N
subscribers cost at most N × bound frames of RAM, worst case, which is the
memory bound the whole ADR buys.

## Q6 — Configuration (house convention: config.yml + validation + fallback)

```yaml
# Reliable-path backpressure (ADR-0004).
# Per-subscriber outbox bound (frames). At HWM the publisher's push is
# refused with Backpressured until the outbox drains to LWM.
#   - must be a positive integer; >= 512 recommended (receiver window is 256)
#   - non-numeric, 0, or empty -> falls back to 1024
reliable_outbox_max_frames: 1024

# High-water mark as a fraction of the bound (0 < hwm < 1): pushes are refused
# at or above this fill level. -> falls back to 0.75
reliable_outbox_hwm: 0.75

# Low-water mark as a fraction of the bound (0 < lwm < hwm): pushes resume
# when the outbox drains to or below this level. -> falls back to 0.25
reliable_outbox_lwm: 0.25

# Optional sender-side pacing ceiling (frames/s, 0 = unlimited). A cheap
# complement to backpressure: bounds the publisher's own offering rate.
#   - non-numeric or negative -> falls back to 0 (unlimited)
reliable_send_rate_limit: 0
```

Validation mirrors the existing `participant_timeout_seconds` /
`max_advertised_endpoints` pattern: numeric check, range check
(`lwm < hwm < 1`), documented fallback default. No new config mechanism.

## Q7 — Tests and benchmarks

- **New unit test — "subscriber stalls, sender observes":** connect a
  subscriber, stop it from reading (or fill its outbox directly), publish
  `bound + margin` frames; assert (a) sender receives `Backpressured` at HWM,
  (b) `reliablePublisherSeq_[key]` unchanged for the refused frame, (c)
  publisher-side RSS/frame count bounded by per-reactor bound × subscribers,
  (d) after the subscriber drains to LWM, a retry with the *same* tid is
  accepted and delivered exactly once.
- **Exactly-once regression under blocked/retry:** extend the existing
  per-sub exactly-once assertion in B2 to a scenario with interleaved
  backpressured refusals and same-tid retries; assert no gaps, no duplicates
  delivered, ordering per (publisher, topic) preserved.
- **B2/B4 re-basing:** B4's "unpaced flood ceiling" becomes the *backpressure
  onset* measurement (offer rate at which `Backpressured` first fires vs
  absorb rate — the ~10× gap must now surface as refusals, not RSS). B2's lag
  backstop (`kMaxLagFrames`, the harness's *simulated* backpressure) is
  deleted or reduced to an assertion that the transport — not the harness —
  stops the sender. README Reliable-QoS baseline and the ADR-0002 ERRATA
  "until a bounded outbox lands" clause are updated when this lands.
- **Fairness test:** one stalled + one healthy subscriber; healthy subscriber's
  delivery rate must be unaffected by the stalled one.

---

# Decision

**Recommended default: Option 1A (bounded per-reactor outbox, HWM/LWM) with
try-send semantics (2A tri-state, same-tid retry per Q3), per-subscriber
independent failure reporting (4A), per-reactor fairness preserved (Q5),
config knobs per Q6, and the optional `reliable_send_rate_limit` pacing
ceiling (1D) as a cheap complement to the owner's "dynamically adjust the
send rate" reading. Credit window (1C) is explicitly deferred as a future
refinement; gRPC-native flow control (1B) is rejected.**

Stress-tested against the hard questions:

- **Q3 (tid lifecycle)** — the try-send + commit-after-acceptance rule is
  *strictly safer* than today's commit-before-push: today's correctness
  already depends on "push never fails for a live subscriber"; 1A makes that
  assumption explicit and repairable. Same-tid retry reuses the receiver's
  existing dedup window, so no new wire semantics.
- **Q4 (partial fan-out)** — per-sub independence is today's structure; 1A
  changes only *where* a refusal is observed, not the fan-out loop. NoSubscribers
  and Backpressured are distinguishable, so "Sent" retains its honest meaning
  (≥1 live subscriber accepted).
- **Owner's rate-adjuster phrasing** — reactive backpressure (1A) *is* the
  dynamic send-rate adjustment: the sender's effective rate automatically
  equals the slowest subscriber's absorb rate, with zero caller cooperation.
  The optional `reliable_send_rate_limit` additionally gives a *proactive*
  ceiling for deployments that want to bound offering regardless of subscriber
  health. Credit windows (1C) would add wire protocol and reconnect complexity
  for precision the outbox HWM already provides at the point of overload;
  revisit only if cross-subnet deployments show the per-reactor bound's
  worst-case memory (N × bound frames) is unacceptable.
- **Why not blocking (2B) as default** — `publishReliable` runs under
  `grpcServiceMutex_`; blocking there serializes all reliable topics behind
  one slow subscriber and risks self-deadlock. The app can build blocking
  semantics on the tri-state + `isSendable()` probe without the transport
  taking that hazard on.

**API shape (additive, back-compatible):**

```cpp
enum class ReliableSendResult { Sent, Backpressured, NoSubscribers, NotServing };

// New core; same tid semantics, same MTU guard, same ordering guarantees.
ReliableSendResult tryPublishReliable(const std::string& topic,
                                      const std::string& messageType,
                                      const std::string& payload);

// Existing signature kept as a wrapper — source-compatible for callers:
//   true  == Sent (== today's pushedAny)
//   false == Backpressured | NoSubscribers | NotServing
// (a caller that ignored the bool cannot be made worse by this change;
//  today that same caller silently overruns RAM instead.)
bool publishReliable(const std::string& topic,
                     const std::string& messageType,
                     const std::string& payload);
```

An `isSendable(subscriberKey)` probe (2C) is included in the slice as the
LWM-resume surface; an `onSendable` callback is deferred (re-entrancy risk:
a callback publishing from the gRPC executor thread).

---

# Minimal first slice

Each step is independently shippable; the best-effort multicast path stays
byte-identical and untouched throughout.

1. **Outbox bound + enqueue status (mechanism, no caller change yet).** Add
   `reliable_outbox_max_frames` (config + validation + fallback per Q6);
   `enqueue` returns `Accepted`/`Backpressured` at HWM; LWM crossing clears
   the flag. `pushData` propagates the status; `publishReliable` initially
   maps `Backpressured` → `false` (behavior: sender stops growing RAM, same
   bool surface). Bounded memory is already delivered here.
2. **Tri-state core + tid commit-after-acceptance (Q3).** Introduce
   `tryPublishReliable` + `ReliableSendResult`; move the
   `reliablePublisherSeq_[key]` commit to after ≥1 acceptance; same-tid retry
   semantics; keep the `bool publishReliable` wrapper. Exactly-once
   blocked/retry regression test lands here.
3. **Deliverability + probe (Q4/Q5 surface).** Per-subscriber result map
   (optional out-param), `isSendable()` probe, fairness test
   (stalled subscriber does not gate healthy ones).
4. **Rate ceiling (optional complement).** `reliable_send_rate_limit` token
   bucket in `publishReliable` (sender-side, default off), B2/B4 re-basing,
   README/ADR-0002-ERRATA updates.

Follow-up actions (to propose, not yet scheduled):
- Vera: exactly-once regression + memory-bound soak test before the API
  freeze of `ReliableSendResult`.
- Mikael: B2/B4 re-base and the lag-backstop removal in the same PR as step 4.
- Future option, deferred: receive-side credit window (1C) if per-reactor
  worst-case memory (N × bound) is ever measured as unacceptable.

---

# Consequences

Positive:
- **The ~10× offer/absorb gap stops being RAM.** Publisher-side memory for
  reliable fan-out becomes bounded and configurable: worst case
  `subscribers × reliable_outbox_max_frames` frames, instead of unbounded.
- **The sender finally observes subscriber health** — the first honest
  "reliable" contract: lossless up to an explicit, reported bound, with
  `Backpressured` distinguishing "retry me" from "nobody is listening".
- **tid lifecycle gets *more* correct, not less**: commit-after-acceptance
  closes the latent gap hazard that today's commit-before-push would expose
  the moment any refusal path exists (and which the MTU guard already had to
  work around on the local-reject path).
- **Zero wire/protocol change** — everything is publisher-side state; receivers
  are unaffected; best-effort multicast untouched; ordering per (publisher,
  topic) per subscriber preserved by the unchanged FIFO deque.
- **Dynamic send-rate adjustment, both senses**: reactive (backpressure makes
  the sender's effective rate track the slowest subscriber automatically) and
  proactive (optional rate ceiling), matching the owner's phrasing.

Negative:
- **Callers now can be refused.** A caller that fires-and-forgets at 1M msgs/s
  gets `false`/`Backpressured` where today it gets silent infinite buffering.
  That is the point, but it is a behavior change: applications must retry or
  pace. The `bool` wrapper keeps source compatibility while making the
  refusal visible to anyone who checks.
- **Throughput for a well-matched sender is unchanged, but a stubborn
  unpaced sender's *accepted* rate is capped at the slowest subscriber's
  absorb rate (~90k/s single-sub)** — previously the excess went to RAM;
  now it is refused. Deployments that (incorrectly) relied on buffering as
  throughput will see drops-as-refusals; they must raise the outbox bound or
  pace. The bound trades invisible OOM risk for explicit refusals.
- **HWM/LWM adds two knobs** to reason about; misconfigured (`lwm ≥ hwm`)
  must be validated to a fallback, adding config surface per house convention.
- **Same-tid retry means duplicate frames are possible on the wire** (already
  possible in at-least-once retransmit) — absorbed by the receiver dedup
  window, but it is a semantic applications should know about.
- **Credit-window precision (1C) is deferred**: until/if it lands, backpressure
  granularity is the outbox fill level, not receiver-acknowledged outstanding
  data — for this system (LAN, window 256, measured ceilings) the difference
  is not load-bearing.
