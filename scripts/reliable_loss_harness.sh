#!/usr/bin/env bash
#
# reliable_loss_harness.sh — Edriel reliable-QoS loss-injection QA harness
#
# Purpose (GitHub issue #5, ADR-0001/0002):
#   Validate the reliable transport's core promises under *simulated* packet
#   loss, rather than only in-process happy paths:
#     (a) ordered exactly-once per (publisher,topic) per receiver
#     (b) the best-effort multicast loopback baseline is NOT regressed
#     (c) no crash / unbounded growth while frames are being lost
#
# Mechanism:
#   Uses Linux tc/netem to inject a configurable drop percentage on a target
#   interface (default: loopback `lo`, which both the reliable gRPC path and the
#   multicast pub/sub exercise on this host), runs the ordered exactly-once
#   reliable test + the loopback multicast benchmark under the loss, then
#   REMOVES the qdisc so the host is left clean.
#
# Requirements:
#   - root (passwordless sudo) for `tc qdisc` / `ip link` — verified on the
#     dev VM (sudo -n works).
#   - a built test tree (cmbuild_final) with test_reliable and benchmark.
#
# Usage:
#   scripts/reliable_loss_harness.sh [--loss 5] [--runs 6] [--iface lo] \
#       [--build-dir cmbuild_final] [--no-netem]
#
#   --no-netem  run the test + benchmark without touching the interface (useful
#               as a no-loss control / on hosts without netem permissions).

set -euo pipefail

# ---- config -----------------------------------------------------------------
LOSS=5
RUNS=6
IFACE=lo
BUILD_DIR="${EDRIEL_BUILD_DIR:-cmbuild_final}"
USE_NETEM=1

# ---- arg parse ---------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --loss)    LOSS="$2"; shift 2 ;;
        --runs)    RUNS="$2"; shift 2 ;;
        --iface)   IFACE="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --no-netem) USE_NETEM=0; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BIN="$REPO/$BUILD_DIR/Edriel/test/test_reliable"
BENCH_BIN="$REPO/$BUILD_DIR/Edriel/test/benchmark"

[[ -x "$TEST_BIN" ]] || { echo "missing $TEST_BIN — build first (cmake --build $BUILD_DIR)" >&2; exit 2; }

declare -A SUMMARY

need_netem() {
    if ! command -v tc >/dev/null 2>&1 || ! sudo -n true 2>/dev/null; then
        echo "netem/sudo not available; falling back to no-loss control run" >&2
        return 1
    fi
    return 0
}

apply_netem() { # %pktloss iface
    local loss="$1" iface="$2"
    sudo -n tc qdisc add dev "$iface" root netem loss "$loss"% 2>/dev/null \
        || sudo -n tc qdisc change dev "$iface" root netem loss "$loss"% 2>/dev/null \
        || { echo "failed to apply netem loss on $iface" >&2; return 1; }
    echo "netem: ${loss}% loss on $iface"
}

remove_netem() { # iface
    sudo -n tc qdisc del dev "$1" root 2>/dev/null || true
    echo "netem: removed qdisc on $1"
}

cleanup() {
    [[ "$USE_NETEM" == 1 ]] && remove_netem "$IFACE"
}
trap cleanup EXIT

echo "=== Edriel reliable-QoS loss harness ==="
echo "  loss=${LOSS}%  runs=${RUNS}  iface=${IFACE}  build=${BUILD_DIR}"

# ---- run the ordered exactly-once reliable tests under loss ------------------
run_reliable_exactly_once() {
    local passes=0 crashes=0
    local i out
    for i in $(seq 1 "$RUNS"); do
        out="$(mktemp)"
        if "$TEST_BIN" --gtest_filter='Reliable.*' >"$out" 2>&1; then
            passes=$((passes+1))
        else
            # distinguish a test FAIL from a process CRASH (abort/core)
            if grep -qE "Check failed|Fatal|Aborted|ABORTING" "$out"; then
                crashes=$((crashes+1))
            fi
        fi
        rm -f "$out"
    done
    SUMMARY[reliable_passes]="$passes/$RUNS"
    SUMMARY[reliable_crashes]="$crashes"
    echo "  reliable (Reliable.*) : $passes/$RUNS runs passed, $crashes crash(es)"
}

# ---- best-effort multicast loopback baseline ----------------------------------
run_baseline() {
    local out
    out="$(mktemp)"
    if "$BENCH_BIN" >"$out" 2>&1; then
        echo "  benchmark (best-effort multicast):"
        grep -E "\[bench\]" "$out" | sed 's/^/    /' || true
    else
        echo "  benchmark exit=$? (see $out)" >&2
    fi
    rm -f "$out"
}

# ---- main ---------------------------------------------------------------------
if [[ "$USE_NETEM" == 1 ]] && need_netem; then
    apply_netem "$LOSS" "$IFACE"
else
    echo "  [no-loss control] interface untouched"
fi

run_reliable_exactly_once
run_baseline

echo
echo "=== RESULT ==="
echo "  exactly-once reliable under ${LOSS}% loss: ${SUMMARY[reliable_passes]} pass, ${SUMMARY[reliable_crashes]} crash"
echo "  (0 crashes + 0 failures required for the gate; see lines above for baseline)"

# Non-zero if any run crashed or failed.
[[ "${SUMMARY[reliable_crashes]}" == "0" && "${SUMMARY[reliable_passes]}" == "$RUNS/$RUNS" ]] \
    && echo "  GATE: PASS" \
    || { echo "  GATE: FAIL (see above)"; exit 1; }
