---
title: "ADR-0002: Sharing gRPC Connection Information for the Reliable Transport"
status: "Proposed"
date: "2026-08-24"
authors: "solomon (architect) — decision proposal for owner"
---

# Context

ADR-0001 laid out the reliable-delivery option space. The owner has now
**chosen Option E — unicast-reliable transport**: for reliable topics, do not
use multicast at all; carry reliable payloads over the existing
gRPC/asio-grpc bidi-stream foundation. This record is the design that Option E
begs for and ADR-0001 deliberately deferred:

> **How does a participant learn the reachable unicast gRPC endpoints
> (address:port) of the peers it must serve, so it can open a bidi stream per
> subscribed peer?**

Grounding (verified against HEAD `e531757`, not assumed):

- `Proto/autoDiscovery.proto`: multicast envelope `Message{ oneof content {
  Identifier, TopicAdvertisement, DataMessage } }`. `Identifier{pid, tid, uid}`
  is what the periodic (2 s) heartbeat actually carries. `Topic` has a
  `// TODO: QOS policies`.
- `Proto/autoDiscovery_grpc_service.proto`: `ParticipantStreamService` with
  bidi `StreamParticipants(stream ParticipantHeartbeat) returns (stream
  ParticipantData)` and unary `GetParticipantInfo`. `ParticipantData` already
  declares `string endpoint = 4;` and topic lists, plus a `// TODO: Add
  capability flags`. No server implementation exists; `streamParticipants()` is
  commented out in `Edriel.hpp`.
- Discovery: 2 s heartbeat, 10 s alive timeout, 5 s cleanup; participant
  registry is a `std::set<Participant>` keyed `(pid, tid, uid)` guarded by
  `stateMutex`. The `Participant` struct has **no** endpoint field today.
- gRPC/asio-grpc/strand plumbing is linked. **No server is implemented.**

The design tension this record resolves:

> **Discovery (multicast, group-scoped, 1-to-N) and connection-setup (unicast,
> peer-to-peer, TCP) are two different planes. They must be bridged by
> *connection information* — and the cheapest place to carry that information
> is the same heartbeat that already tells every peer a participant is alive.**

The question is not *whether* to advertise endpoints, but *where* and *in what
shape*, and *who* consumes them (a resolver that maps topics to reachable
peers). The answers below are split into: channel choice (§1), payload shape
(§2), lifecycle/state (§3), topic routing (§4), connection management (§5),
security/robustness (§6), and a minimal first slice (§7).

---

# Options — the channel for conveying endpoint info

All four are viable; they differ in when endpoint info reaches a peer and how
much state the discovery plane carries.

## Channel A — Bootstrap over the multicast discovery plane (piggyback)

**Mechanism.** Add an endpoint list to `Identifier` (the heartbeat message).
Every 2 s heartbeat already carries a participant's `(pid, tid, uid)`; it now
also carries that participant's reachable unicast gRPC endpoints. By the time a
peer is populated into the registry, its endpoints are already there. **Zero
new channel, zero new protocol message, zero new timing.**

**Cost.** Heartbeat bloat is trivial: a candidate endpoint serializes to
~20–30 bytes ("192.168.1.5:4000"); even 4 candidates is ~120 B against the
~1460 B per-datagram budget, and the heartbeat is a *separate* small datagram
from the 1500 B data plane, so it does not tax payloads. Peers that never need
gRPC simply ignore the field (proto3 unknown-field semantics).

## Channel B — Advertise-on-interest (lazy)

**Mechanism.** A subscriber, on registering a *reliable* topic, sends a
multicast interest request (a new `oneof` case, e.g. `ReliableSubscribe`); each
interested publisher replies unicast (or multicast) with its endpoint. Keeps
endpoints off the always-on heartbeat when most topics are best-effort.

**Cost.** A *second* protocol message + a request/reply correlation + a
wait-for-reply window before the first reliable message can flow. The savings
only matter if reliable topics are rare AND heartbeat size is a real budget —
neither is true at Edriel's scale. Adds latency on the join path (must wait for
a reply) that piggybacking eliminates.

## Channel C — gRPC-bootstrap via `GetParticipantInfo`

**Mechanism.** Once *some* endpoint is known, use the gRPC service itself to
learn other members' endpoints.

**Cost.** Chicken-and-egg: you need an endpoint to reach the server before the
server can tell you endpoints. It cannot bootstrap; it can only *refine* an
already-reachable peer (refresh the endpoint list, confirm capability flags)
after a stream is up. Valuable as a verifier, useless as a primary source.

## Channel D — Static/config seed

**Mechanism.** Seed endpoints from `config.yml` (`grpc_port` + optional
`peers:` list / `advertise_address`).

**Cost.** Manual and stale-prone, but the **only** option that works for nodes
that cannot receive multicast at all (cross-subnet, multicast TTL=1, firewalled
groups). Static peers never expire on their own.

## Summary

| Channel | Bootstrap? | New wire msg | Latency to first stream | Cross-subnet |
|---|---|---|---|---|
| A piggyback heartbeat | yes | none | none (endpoint already present) | no (needs D) |
| B advertise-on-interest | yes | 1 new oneof | join-time wait | no (needs D) |
| C GetParticipantInfo | **no** | none | n/a (post-connect) | only after D/A |
| D static config seed | yes | none | none | **yes** |

---

# Decision

**Recommended default: hybrid of A (primary) + D (fallback), with C as the
post-connect verifier. Defer B as an optimization.**

1. **Advertise endpoints on the always-on multicast heartbeat (A).** Zero new
   channel, zero new timing, and every peer's endpoint is present in the
   registry the moment it is discovered. The heartbeat-bloat objection does not
   bite at Edriel's scale.
2. **Add a static config seed (D) for multicast-blind / cross-subnet nodes.**
   A node that cannot hear the group gets its peers' endpoints from config, and
   advertises its own via `advertise_address`. This is the only path that
   crosses subnets.
3. **Use `GetParticipantInfo` (C) to refresh/verify** a live endpoint and read
   capability flags once a stream is established — never to bootstrap.
4. **Defer advertise-on-interest (B).** It is a real optimization only when the
   heartbeat is size-constrained or reliable topics dominate; neither holds.
   Revisit only if heartbeat size becomes a measured problem.

## What endpoint info to carry (the shape)

**Add a new `Endpoint` message and a `repeated Endpoint` list to `Identifier`**
(the multicast heartbeat) — and mirror the same list on `ParticipantData`
(gRPC side). `Endpoint` is richer than a bare string so it can express
multi-homed candidates and transport:

```proto
message Endpoint {
  string address = 1;   // IPv4/IPv6 literal or hostname
  uint32  port    = 2;  // gRPC TCP listener port
  enum Transport {
    TRANSPORT_UNSPECIFIED = 0;
    GRPC_TCP = 1;
  }
  Transport transport = 3;  // reserved for future (e.g. TLS, QUIC)
}

// In Identifier — rides every 2s heartbeat.
// field 4 is free (1..3 = pid, tid, uid).
repeated Endpoint endpoints = 4;

// In ParticipantData — same shape, gRPC-side self-description.
// Keep legacy `string endpoint = 4;` for back-compat (deprecated), add:
repeated Endpoint endpoints = 8;
```

**Why `repeated`, not a single `endpoint`:** the multicast-reachable interface
is frequently *not* the unicast-reachable interface — a node on a multihomed
host (Wi-Fi + wired, separate subnets, or behind a NAT) may be reachable by
peers via a different address than the one the multicast packet egresses. The
established solution — the SDP RFC 4566 / RTPS-locator-list trick — is to
advertise **every candidate address**; the receiver tries them in order and
picks the first that connects. A single string cannot express this. `port` and
`transport` are split out so future transports (TLS/QUIC) are additive without
breaking parsing.

**Backward compatibility.** Fully additive, proto3: old nodes skip the unknown
`endpoints` field and keep working byte-identically on the magic-numbered
multicast datagrams. The existing `ParticipantData.endpoint` (field 4) stays
for wire compat; new peers read the richer `repeated Endpoint endpoints`.

**MTU guard.** The 1500 B budget applies per datagram. The heartbeat is its own
small datagram, but cap advertised candidates (config `max_advertised_endpoints`,
default ~4) so the heartbeat cannot balloon. Reject the *data* plane at send
time as today; the heartbeat is separate and small.

## Lifecycle & state

- **Where it lives:** add `std::vector<Endpoint> endpoints;` directly to the
  `Participant` struct. The `Participant` is already the per-peer state holder,
  keyed `(pid,tid,uid)` and guarded by `stateMutex`. A separate parallel table
  is redundant — it would just duplicate what the struct already owns, at the
  cost of a second lock. **Single source of truth: on the Participant.**
- **When refreshed:** every heartbeat. `handleParticipantHeartbeat` already
  runs under `stateMutex`; on each heartbeat, overwrite `endpoints` from the
  parsed `Identifier.endpoints` (idempotent, cheap, ~µs).
- **When removed:** the existing 5 s cleanup / 10 s timeout removes the
  `Participant` → its endpoints vanish automatically. No separate timer, no
  orphan cleanup.
- **Stale endpoints / peer restart / IP change:** endpoints ride the heartbeat,
  so a restarted peer re-advertises on its first heartbeat; a changed IP
  overwrites the vector on the next heartbeat. The connection manager (below)
  compares the incoming endpoint set against what it is currently connected to
  and re-dials on mismatch. A *fully restarted* peer is a new `(pid,tid,uid)` →
  a new `Participant`, so stale state cannot leak into it.

## Topic routing

Two-step lookup under `stateMutex` (registry is small; the lock is held for
microseconds — resolve at publish time, do not cache):

1. **Topic → subscriber participants.** Already exists:
   `topicRegistry[compositeKey].subscribers` is a `std::set<Participant>`.
2. **Participant → stream.** The connection manager keeps
   `participant → active gRPC stream` keyed `(pid,tid,uid)`. For each
   subscriber participant, read its `.endpoints` (already on the struct) and
   pick the first candidate that connects.

**Key decoupling:** the *persistent bidi stream is per-peer, not per-topic*. A
single stream to a subscriber carries every reliable topic that subscriber
subscribes to. Topic churn (subscribe/unsubscribe) then only changes which
messages are multiplexed onto an existing stream, never the connection graph.
The publisher's reliable send becomes: resolve subscribers for topic T, for
each, write the stamped `DataMessage` to that peer's stream.

> **Directionality — where the resolver lives (a genuine architectural
> recommendation, not a detail).** The brief frames this as "the publisher
> resolves subscriber endpoints and dials them." I recommend the **inverse:
> the subscriber dials the publisher's server** and opens `StreamParticipants`,
> with the publisher serving (server-side push of `ParticipantData`). Why:
> (1) it matches the existing `StreamParticipants` service shape (a client
> dials in and receives a server stream); (2) backpressure/ACK flows naturally
> *to the data source* via gRPC flow control and the client's
> `ParticipantHeartbeat` request stream — exactly the "ack/backpressure flows
> back" the brief wants, without the publisher holding N outbound client
> streams and N reconnection machines; (3) the hot data source keeps the fewest
> moving parts — it just serves. Symmetry is preserved: **every node runs one
> server (serving its own published topics) and is a client (dialing the
> publishers of topics it subscribes to).** If the owner prefers the literal
> push model, the mechanism below is unchanged — the connection manager and
> topic→peer resolver simply live on the publisher instead of the subscriber.
> The endpoint-sharing design is direction-agnostic; only *who dials* differs.

## Connection management

- **Every node runs ONE small gRPC server** on a single TCP port
  (`grpc_port`, default e.g. 4000), advertised in the heartbeat as the `port`
  of its `Endpoint` candidates. **Per-connection servers are rejected** — one
  listener per node, one port, advertised once.
- **Reuse the discovery port?** Legally possible (Linux allows a TCP listener
  and the UDP multicast socket to share numeric port 30002), but **not
  recommended** — a dedicated `grpc_port` keeps the two planes unambiguous and
  avoids surprising bind behavior. Leave port reuse as a config option only if
  a deployment genuinely needs to conserve ports.
- **Symmetry:** each node both *serves* (its published reliable topics) and
  *connects out* (as a subscriber dialing publishers). The connection manager
  runs on the dialing side and maintains `participant → bidi stream`, opened on
  subscribe, repaired on timeout/stream-break (re-dial), torn down when the
  peer's 10 s heartbeat timeout fires and its `Participant` is removed.

## Security & robustness (LAN-appropriate)

- **No auth — by design for a trusted LAN.** Note honestly: endpoint info is
  world-readable on multicast (any LAN host sees it); acceptable on a trusted
  LAN, a documented boundary otherwise.
- **Anti-spoof, minimal:** a dialing peer must be a *known* participant
  (present in the registry with a matching `(pid,tid,uid)`); unknown dialers are
  rejected. Optionally require the dialing socket's source address to match a
  candidate endpoint the peer advertised — a cheap LAN defense against a node
  impersonating another's UID.
- **Multi-homed:** repeated candidates, connect-in-order, first-wins — the
  SDP/RTPS approach.
- **Cross-subnet:** multicast TTL=1 does not cross subnets; such peers are
  covered by the config seed (D) and `advertise_address`.
- **Replay-attack TODO:** for the *reliable* path, dedup on a per-(publisher,
  topic) `tid` stamp (reusing `Identifier.tid`) gives at-least-once → exactly-
  once per (pub, topic) per receiver, which is the practical replay defense at
  this layer. A wall-clock timestamp on `ParticipantHeartbeat`/`Endpoint` for
  freshness is deferred; the 10 s heartbeat liveness already bounds staleness.

---

# Minimal first slice (micro-wire breakdown)

Each step is independently shippable and keeps the existing best-effort
multicast path **green** (byte-identical, untouched).

1. **Proto change + codegen only — no behavior change.** Add `Endpoint`; add
   `repeated Endpoint endpoints` to `Identifier` (field 4) and `ParticipantData`
   (field 8); add `bool reliable = 4;` to `Topic` (the QOS TODO). Regenerate.
   Existing tests still pass; no runtime code touched.
2. **Populate endpoints on heartbeat.** In
   `handleParticipantHeartbeat`/`handleAutoDiscoveryParse`, parse
   `Identifier.endpoints` into `Participant.endpoints` under `stateMutex`;
   advertise own candidates in the self-heartbeat. Best-effort unaffected.
3. **Implement the gRPC server** (`ParticipantStreamService` on `grpc_port`):
   `StreamParticipants` server-side streaming — a client can dial and receive
   `ParticipantData` (discovery presence first; reliable payload wiring is
   step 4). Verify with a unit client / grpcurl.
4. **Reliable send path.** Resolver (topic→subscriber participants→their
   streams) + connection manager opens a bidi stream per subscribed peer (per
   the chosen direction); stamp a per-(publisher, topic) `tid` for receiver
   dedup; write `DataMessage` frames. Publisher pushes / subscriber pulls per
   the direction decision.
5. **QoS flag wiring.** `Topic.reliable` opts a topic into the gRPC path;
   `bestEffort` topics stay on multicast exactly as today. Only after 1–4 land.

Follow-up actions (to propose, not yet scheduled):
- Decide the *dial direction* (subscriber-initiated recommended; publisher-
  initiated is a drop-in flip) before step 4.
- Add `grpc_port`, `advertise_address`, `peers:`, `max_advertised_endpoints`
  to `Config`/`config.yml`.
- Vera: a netem/toxiproxy paired-loss node to validate ordered-exactly-once
  and the re-dial-on-IP-change behavior before API freeze.

---

# Consequences

Positive:
- **Zero new channel / protocol** for the common case — endpoints ride the
  existing 2 s heartbeat, so reliable streams can be opened as soon as a peer
  is discovered. Lowest join latency.
- **Backward compatible by construction**: additive proto3 fields; best-effort
  multicast path is byte-identical and untouched.
- **Multi-homed handled honestly** via repeated candidate endpoints (SDP/RTPS
  precedent), not a single fragile address.
- **Cross-subnet covered** by the static seed, which piggybacking alone cannot.
- **One server per node**, one advertised port — the connection graph stays
  small and symmetric; backpressure/ACK flow to the data source.

**ERRATA (2026-08):** measured evidence falsifies the claim that
"backpressure flows naturally *to the data source*" at the API boundary.
Benchmarks (`benchmark/benchmark_reliable.cpp`, see the README Reliable-QoS
baseline) show the publisher-side `SubscriberReactor::outbox_` is **unbounded**:
an unpaced caller offers ~1M msgs/s while the stream write-out / subscriber
absorb ceiling is only ~90k msgs/s — the excess is buffered, not
backpressured, so memory grows with the offered backlog. The connection
direction decision above is unaffected; only the backpressure claim is
retired. Until a bounded outbox + real backpressure lands (planned
follow-up), treat "reliable" as "lossless up to unbounded publisher-side
buffering" — see the README known-issue in the Reliable-QoS benchmark
baseline.

Negative:
- **Endpoint info is LAN-visible on multicast** — acceptable on a trusted LAN,
  a stated boundary if the LAN is not trusted.
- **Heartbeat is slightly larger** (a few candidate endpoints); bounded by a
  config cap, negligible against the per-datagram budget.
- **Reliable payloads still capped at the 1500 B budget** unless
  fragmentation/reassembly is later added (deferred — app-side split for large
  reliable messages, as in ADR-0001).
- The static seed is **manual and can go stale** — it is the fallback, not the
  primary; the heartbeat remains the source of truth where multicast is heard.
