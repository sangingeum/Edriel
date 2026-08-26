---
title: "ADR-0003 (DRAFT): Sharded SPSC receive pipeline for higher message-reception throughput"
status: "Draft"
date: "2026-08-25"
authors: "solomon (architect) — refined proposal for owner review, pending go-ahead"
baseline: "59412eb (fix(relible*): close reconnect teardown gap)"
supersedes: "candidate: N receiver threads + shared lock-free ring (owner's original)"
---

# Context

The multicast receive path exposes a kernel-overrun capability that occasionally
silently drops datagrams, and the receive+dispatch loop is entirely serialized
on one strand. Grounded against HEAD `59412eb` (verified, not assumed):

- **Single re-arm**: one `async_receive` is re-armed only AFTER parse+dispatch
  (`Edriel.cpp:365-390`). The kernel socket buffer overruns mid-parse => silent
  datagram loss; `SO_RCVBUF` is never raised (only `reuse_address`,
  `join_group`, `enable_loopback` are set), so the default receive-buffer cap
  applies.
- **O(n) heartbeat on every frame — including data frames.** Every data
  datagram and every advertisement first runs `handleParticipantHeartbeat`
  (`Edriel.cpp:530-546`), which does a `std::find_if` over
  `std::set<Participant>` (3-field compare) and then reassigns
  `it->endpoints = std::move(endpoints)` (a vector rebuild) even when the peer
  is unchanged (`Edriel.cpp:559-596`). So each data message pays a linear
  registry lookup it rarely needs.
- **Decode is duplicated per callback.** `FindMessageTypeByName` is already
  hoisted (one symbol lookup per message), but `prototype->New()` +
  `ParseFromString(payload)` runs once *per local callback* for the same
  payload (`Edriel.cpp:749-755`). Two subscribers to the same topic → the
  payload is decoded twice.
- **One strand, one thread, one global mutex.** All multicast Rx, the
  discovery send timer, the cleaner, and `reconcileReliableConnections` run on
  one `strand`; the benchmark runs the io_context on one thread. All registry
  and topic mutations are guarded by a single `stateMutex`
  (`Edriel.hpp:331`). Callbacks are collected under the lock but invoked after
  release (the existing reentrancy contract, `Edriel.cpp:719-757`).
- **Reliable path is separate and already threaded.** ADR-0002 delivers
  reliable frames on per-`(publisher)` blocking bidi-stream `Read` threads
  (`EdrielReliableClient.cpp:40-129`), relying on per-`(publisher, topic)` tid
  ordering bounded by `kReliableWindowSize=256` (only reorder) windows
  (`Edriel.hpp:53-56`, `242-273`). It does NOT run on the strand.

The owner's candidate — N receiver threads pushing into an N-worker
lock-free ring — is a **necessary but not sufficient** first hit: it fixes the
kernel overrun symptom, but the dominant serial costs (global `stateMutex`,
O(n) heartbeat, per-callback decode) are *unchanged*. A refined topology is
proposed below.

---

# Decision

Adopt a **sharded receive pipeline**: separate the *transport* work (copy a
raw frame off the socket) from the *semantic* work (parse/dispatch), run the
semantics **per-`(publisher, topic)`-shard**, and replace the single
`stateMutex` with **per-shard locks**. Concretely:

1. **One multicast receiver thread** drains the socket into a bounded SPSC
   ring per worker, then immediately re-arms the socket — the kernel buffer
   stops being the bottleneck. On loopback there is one kernel RX queue, so
   multiple receiver threads do not add parallel receive; the parallelism
   that matters is downstream (workers).
2. **N worker threads**, each owning **exactly one** SPSC ring. Each worker
   owns a **shard** of the topic + participant registry, guarded by that
   shard's own mutex — no global lock, so the true msg/s lever is realized.
3. **Key pinning:** every frame is routed to
   `worker = hash(publisher_uid, topic) % N`. This preserves per-`(publisher,
   topic)` tid ordering inside a single worker; no cross-worker reorder.

The reliable gRPC path is **left on its per-connection threads** (out of scope
for the worker topology) but it **shares the same sharded registry**: the
per-`(publisher, topic)` reorder window makes it single-writer per shard, so
exactly-once/in-order is preserved without re-architecting the delivery
threads. This is the one cheap place where both QoS classes benefit.

**Why this and not the candidate:** the candidate's lock-free ring is only
cheap under SPSC; the second it is a shared MPMC/MPSC buffer the "lock-free"
claim collapses. Sharding *fixes* the real serialization point (the single
stateMutex) which N rings do not. N "receivers" over-sells: the msg/s win is
parallel worker processing, which a single global lock forbids.

---

# §1 — N semantics (precise)

Two independent knobs; they MUST NOT be conflated (the owner's "one N" muddles
them):

| config key | meaning | default | range | notes |
|---|---|---|---|---|
| `receiver_threads` | socket-draining threads | `1` | `[1, 4]` | >1 rarely helps: loopback has one kernel RX queue; resize only for multi-queue NICs. Default 1. |
| `worker_threads` | SPSC ring + registry-shard count (=N) | `4` | `[1, 16]` | this is the true parallel lever. Cap at 16 to avoid shard-lock churn / thread oversubscription on typical hosts. |
| `rx_ring_slots` | slots per worker's SPSC ring | `4096` | power of two | userland drop buffer; see §4. |
| `so_rcvbuf_bytes` | `SO_RCVBUF` on the UDP socket | OS default | `[0, 1<<30]` | 0 = leave OS default; tuned once a baseline exists (e.g. 1 MiB). |

`N` = `worker_threads` ONLY. It is a number of workers/sharads/rings, not a
number of receiver pairs. Config follows the existing strict pattern
(`EdrielConfig.{hpp,cpp}`, `config.yml`): each key validates independently and
falls back to default on malformed input; never throws.

---

# §2 — Ordering / lock-free correctness story

- **SPSC per ring.** Receiver→ring→worker is exactly one producer and one
  consumer. A bounded SPSC ring needs no CAS in the locked loop: the single
  producer owns the write index, its counter tagged release; the single
  consumer owns the read index with acquire. Codethe classic Vyukov-style SPSC
  positioning.
- **Ordering = shard pinning.** The receiver enqueues frames in kernel RX
  order; hash-pinning routes same-`(publisher, topic)` frames to the same
  worker, so per-shard order equals per-`(publisher, topic)` wire order.
  Best-effort makes *no* cross-publisher ordering promise anyway; this
  preserves the *stronger* ordering the reliable path needs.
- **Drops are drained to a counter, never silent.** If a ring is full, the
  receiver counts a drop and drops (best-effort QoS; the socket already drops
  kernel-overrun frames today, but *observably*). The drop counter is surfaced
  (metric), then thrown away — matching “no *silent* loss”.

---

# §3 — Scope: best-effort multicast, AND the registry share for reliable

- **Workers / SPSC N topology → best-effort multicast ONLY.** This is the place
  the orderless QoS and the strand latency justify the change.
- **Reliable → no worker topology.** The gRPC delivery already has per-pub
  threads and a stricter exactly-once/window contract (`kReliableWindowSize`).
  Routing it through a secondary SPSC shard would gamble with replay/order and
  buys little (it is not currently on the bottlenecked strand). The reliable
  path changes only via the shared *registry*: it routes into the sharded
  registry by the same `(publisher, topic)` hash so the reorder window is
  single-writer and each window's expected per-`(publisher, topic)` order is
  preserved.
- One semantic honored in both: no functional reduction in per-`(publisher,
  topic)` delivery order.

---

# §4 — What the design deliberately does NOT do (owner reviews)

1. **No MPMC/MPSC `lock-free` claim.** Only strict SPSC rings are intended to be
   lock-free; any shared buffer falls back to a yielding mutex and is out of
   this design's fast path.
2. **Does not parallelize socket receive with N receivers** (loopback has one
   kernel queue).
3. **Does not change reliable delivery threads** (only their registry access).
4. **No NACK / replay / retransmit** on either path (out of scope, existing
   exactly-once windows for reliable).
5. **No hardening of heartbeat yet in this pass:** the O(n) `find_if` +
   endpoint reassign is the single hottest per-frame cost and worth removing
   too, but sharding already partitions it. Lowering it (cache,
   heartbeat-on-message skip) is a follow-up micro-opt, not a gate.
   
   (Intentionally NOT: switching `std::set<Participant>` to O(1) map here —
   it is easy but changes (pid,tid,uid) key semantics, therefore behavior; it
   must be its own decision, flagged in Open Questions.)
6. **No protobuf-arena** (deferred; decode-once/dispatch-many already removes
   the same repeated cost, arena is a smaller further win).

---

## §5 — Measurement-first gate (non-negotiable)

1. **Baseline commit** the current numbers BEFORE any change. HEAD `59412eb`.
   Extend `benchmark/benchmark.cpp` `ThroughputMsgsPerSecond` to also report the
   sent-vs-received gap and a drop counter; commit a baseline file (msgs/s,
   p99, drop%) as an artifact so a regression is detectable. The benchmark
   currently only smoke-asserts `got > 0`; a perf gate needs teeth.
2. **Flame graph** of the receive path BEFORE and AFTER (`perf record` /
   Callgrind on `handleAutoDiscoveryParse`/`handleParticipantHeartbeat`/
   `handleDataMessageReceive`) so the decision is data-driven, not assumed.
3. **Success metric:**
   - At the baseline build's dictated high-rate burst (fixed payload, loopback,
     no pacing), **drop = 0%** of sent datagrams (vs the silent >0 baseline),
     AND
   - sustained end-to-end msgs/s ≥ **1.5× baseline** with `worker_threads=4`
     (shard parallelism proven), AND
   - no regression on the existing latency gate (`p99 < 10ms`,
     `received ≥ 90%`), and TSan-clean for the new shard/ring threading.
   The 1.5× is a target the design is expected to beat; it is re-set by
   whatever the flame-graph discovery shows if the re-grid is primed. The
   metric stays honest: either the shards have no lock-free claimed win
   (baseline proves it) or the gate fails.

---

## §6 — API / ABI / thread-affinity flag

- **No ABI break.** The public API surface (`sendMessage`, topic registration,
  callbacks, config loader) is unchanged; shard count comes from config, not a
  constructor/ABI. No members added to `Edriel`'s public ABI.
- **Behavioral / thread-affinity BREAK to document:** today every local callback
  runs on the single strand (one thread, deterministic sequence). With N>1
  workers callbacks for *different* shards can run concurrently on different
  threads. Any user callback storing mutable state outside the reentrancy
  pattern breaks. Mitigations: (a) keep the existing collect-then-invoke-after-
  unlock reentrancy contract per shard (unchanged) and (b) **a `worker_threads=1`
  mode is the exact back-compat single-threaded behavior** — document that
  `worker_threads=1` (`= receiver_threads` implicit) restores today's ordering
  guarantee for callers that need it, at the cost of the throughput win.
- Call the reentrancy thread-safety contract in the public doc explicitly once
  this lands.

---

## §7 — Consequences

Positive
- No global `stateMutex` contention on the hot path (per-shard locks).
- No silent kernel drops (observable drop counter), and the socket stays drained
  (re-arm after copy, not after parse).
- Ordering preserved for both QoS; parallel shards give real msg/s when N>1.
- Cheap to roll back: `worker_threads=1` reproduces today's model.

Negative
- Threading complexity + a prototype ring; wiring the receiver → rings → workers
  is real new code that the collective state (multicast Rx, discovery timers,
  cleaner) currently serializes on the strand.
- Registry is now shard-local; a topic spanning multiple shards (different
  publishers) means its local subscriber callback set is the union over shards,
  so **listener registration must fan out to each worker's shard**. If a topic
  has N distinct publishers, its subscriber callbacks would be referenced from
  N shards. **Recommendation: de-dupe registration so a topic lives on ONE
  shard, and pin frames by `topic` only (not `(publisher, topic)`)** — a
  simpler, single-copy subscriber set; cross-publisher ordering is irrelevant
  in best-effort QoS, and reliable stays `(publisher, topic)`-keyed. Default
  choice; flagged again in Open Questions.
- Drop latency is pulled forward on best-effort (accepted; it is smaller than
  the status-quo silent kernel drop).

---

## §8 — Follow-up / Open Questions (for the owner)

1. **Registry key semantics (forceful):** should heartbeat lookup become O(1)?
   `(pid,tid,uid)` triple is the set key; changing to `uid`-keyed requires
   confirming a restarted peer keeps `uid`-stable, else two generations of one
   node would collide. I do NOT include it as a gate — only worth doing with
   its own check.
2. **Shard key for callbacks:** pin by `topic` only (recommended — keeps one
   subscriber copy per worker, simpler dispatch) vs `(publisher, topic)`
   (better per-publisher shard separation, but the subscriber set shatters).
   Confirms §7.
3. **Callback thread-affinity contract:** the run-time `worker_threads=1`
   back-compat mode is the safe default for a single-thread-only subscriber.
   Is an explicit per-topic threading guarantee enough, or is the single
   back-compat mode the only blessing for all callers?
4. **Ring drop policy:** silent counter-only (recommended v1) vs block (apply
   backpressure) option exposed? Recommend counter-only v1.
5. **`receiver_threads>1`**: keep 1; only revisit if a real multi-queue NIC
   port appears and loopback stops being the target topology.

---

# §9 — Owner decisions (2026-08-25) — RESOLVED, pending baseline gate

The owner answered the five open questions; recorded verbatim-resolved so the
implementation brief and the go-ahead gate are unambiguous.

1. **Registry key → O(1).** Rename the concern to a decision: replace
   `std::set<Participant>` with **`std::unordered_map<uint64_t, Participant>`
   keyed on `uid` alone** — NOT a packed `(pid,tid,uid)` composite key.
   Rationale: `uid` is already a random 64-bit process-unique token and is
   already the self-filter identity (`Edriel.cpp:564`); keying on it gives true
   O(1) with no hash-collision risk and no 3-field pack ambiguity. Identity
   semantics unchanged (`uid` uniquely tags a peer; pid alone cannot due to
   cross-machine reuse). Timeout cleanup and per-shard slicing follow trivially.
   Marked: does NOT change ownership semantics; behavior-neutral to identity.
2. **Shard key → `topic` only** (owner chose shared key = Solomon's §7
   recommendation). Each topic lives on one shard; one copy of the subscriber
   set; simpler dispatch. Cross-publisher ordering is irrelevant in
   best-effort QoS.
3. **Callback thread-affinity.** Per-topic sequential execution is guaranteed
   FOR FREE by topic-only pinning (one topic → one shard → one worker → serial
   ring). Cross-topic concurrency is the only coupling the user must manage;
   the owner accepts leaving that to user synchronization primitives —
   *provided* (a) the per-topic ordering guarantee is explicit and documented,
   (b) `worker_threads=1` is documented as exact back-compat single-threaded
   behavior, (c) no in-v1 per-topic executor/strand framework is built. This is
   NOT considered irresponsible; it is a documented contract.
4. **Ring overflow → LMO-style overwrite (drop oldest), not block.** A full
   ring advances and overwrites the oldest slot, protecting the consumer's
   read index; surfaced as an observable drop-oldest counter (never silent).
   Explicitly NOT strict FIFO when ≥ raid: older frames are evicted, retained
   ones stay in order. Suitable for best-effort QoS.
5. **`receiver_threads`** stays **1**; a `>1` value is out of scope until a
   real multi-queue NIC port justifies it.

Status: **DRAFT → ACLOWN-per-owner-decisions.** Implementation remains gated on
the §5 measurement-first baseline (commit current numbers + flame graph, then
build against `worker_threads=4`; gate = drop 0%, ≥1.5× msgs/s, p99/90% no
regression, TSan-clean). No implementation code has been committed.

---

*This is a decision record. The owner has resolved the open questions; the
go-ahead gate (baseline + measured improvement) still precedes any code.*