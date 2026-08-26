# Reliable-QoS Loss-Injection QA Harness

Validates the Edriel reliable transport (ADR-0001/0002) under **simulated
packet loss** — the validation gate called out in GitHub issue #5 ahead of the
reliable-QoS API freeze.

The reliable path (subscriber-initiated gRPC bidi streams via
`ParticipantStreamService`, per-`(publisher,topic)` `tid`, bounded reorder/dedup
window for exactly-once per receiver, multi-homed connect-in-order with
candidate fallback, re-dial on endpoint change, anti-spoof gate, Channel D
static seed) is tested in-process by `test_reliable`. Those tests prove the
*logic* under perfect delivery. This harness additionally exercises them with
the kernel actually **dropping packets**, and captures the best-effort multicast
loopback baseline to confirm it is not regressed.

## What it does

1. Applies Linux `tc` **netem** loss on a target interface (`lo` by default,
   which both the reliable gRPC path and the multicast pub/sub use on this
   host).
2. Runs the ordered-exactly-once reliable suite (`Reliable.*`) several times
   under the injected loss and counts passes/crashes.
3. Runs the loopback multicast `benchmark` to snapshot the best-effort
   baseline (latency p50/p90/p99, throughput).
4. **Always removes the netem qdisc** (trap on exit), leaving the host clean.

A crash or any failed run fails the harness gate.

## Requirements

- `tc` (iproute2) and **passwordless sudo** (`sudo -n true` must work) for the
  netem path. On a host without these the harness can still run a **no-loss
  control** with `--no-netem`.
- A configured build tree (default `cmbuild_final`) containing
  `Edriel/test/test_reliable` and `benchmark/benchmark`.

## Usage

```sh
# full loss run @5% on loopback
scripts/reliable_loss_harness.sh --loss 5 --runs 6

# different interface / loss / build dir
scripts/reliable_loss_harness.sh --loss 2 --iface ens18 --build-dir cmbuild_m45

# no-loss control (does not touch any interface)
scripts/reliable_loss_harness.sh --no-netem --runs 8
```

## What it asserts

- **Ordered exactly-once under loss** — `Reliable.ReorderDedupExactlyOnce`,
  `Reliable.LateJoinerReceivesCurrentFrames`, and the whole `Reliable.*` suite
  must pass with injected loss: no frame dropped-then-duplicated, none
  delivered out of order, no crash.
- **No regression of the best-effort multicast baseline** — compare the
  `[bench]` lines against a `--no-netem` control run; latency p99 should stay
  well under 10 ms and the receive path should not regress.

## Verified results (dev VM, 2026-08-25)

| Run | Result |
|-----|--------|
| no-loss control, 8 runs | `Reliable.*` **8/8**, 0 crash; `GATE: PASS` |
| netem 5% on `lo`, 4 runs | `Reliable.*` **4/4**, 0 crash; `GATE: PASS` |
| single netem 5% run | all 11 `Reliable.*` tests pass in ~3.4 s |

Loopback multicast baseline (256 B payload): `sent≈1.5 M msgs/s`,
latency `p50≈20–32 µs, p90≈61–66 µs, p99≈72–79 µs`. The unpaced throughput
test bursts far faster than the single-threaded loopback receiver drains, so
`received` undercounts (documented characteristic of the existing benchmark);
the latency numbers are the meaningful baseline.

## Scope & honest limits

This harness runs both endpoints in-process over loopback on one host, so it
covers **loss injection on the reliable stream** but not true cross-host
topologies, multi-candidate IP moves under a live interface flap, or a
proxy-based NAK simulation. Those (netem on a distinct unicast link, or
toxiproxy between two nodes) are the natural next step listed in issue #5; the
script is structured so a `--iface` + two VMs/namespaces can drive the
cross-subnet case. The `re-dial-on-endpoint-change` and `multi-homed advance`
scenarios are exhaustively covered by in-process tests
(`Reliable.ReDialOnEndpointChange`, `Reliable.MultiHomedAdvanceToNextCandidate`)
without needing loss injection; this harness validates them together with real
dropped packets end-to-end.
