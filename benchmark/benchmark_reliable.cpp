/**
 * @file benchmark_reliable.cpp
 * @brief Reliable-QoS (ADR-0002) gRPC-path benchmarks.
 *
 * The multicast harness (benchmark.cpp) covers only best-effort delivery.
 * These benchmarks measure the RELIABLE gRPC path end-to-end
 * (publish -> bidi StreamParticipants push -> reorder/dedup window ->
 * subscriber callback) hermetically: every publisher/subscriber pair is
 * connected via the same cross-injected-registry pattern
 * Edriel/test/test_reliable.cpp uses (deliverForTest(makeAdvert(...)) +
 * startReliableSubscriptions()), so no multicast group or live network is
 * involved. Payloads ride autoDiscovery::Topic sized inside the 1500-byte
 * MTU budget.
 *
 *   B1  ReliableBenchmark.LatencyPercentiles   publish->callback latency
 *                                              p50/p90/p99/max
 *   B2  ReliableBenchmark.Throughput           sustained paced window,
 *                                              sent vs received gap
 *   B3  ReliableBenchmark.FanOut               one publisher -> N subscribers
 *   B4  ReliableBenchmark.UnpacedFlood         unpaced flood ceiling
 *
 * Output lines carry the same "[bench]" prefix as benchmark.cpp.
 */

#include <gtest/gtest.h>

#include <asio.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Edriel.hpp"
#include "autoDiscovery.pb.h"
#include "autoDiscovery_grpc_service.pb.h"

using edriel::Edriel;
using edriel::SubscriberKey;

namespace edriel {

using Clock = std::chrono::steady_clock;

// ----------------------------------------------------------------------------
// Helpers (mirroring Edriel/test/test_reliable.cpp's anonymous namespace)
// ----------------------------------------------------------------------------

namespace {

std::uint16_t freeTcpPort() {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.open(asio::ip::tcp::v4());
    sock.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::any(), 0));
    return sock.local_endpoint().port();
}

bool waitUntil(const std::function<bool()>& pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

SubscriberKey keyOf(const Edriel::Participant& who) {
    return SubscriberKey{
        static_cast<std::uint32_t>(who.pid), who.tid, who.uid};
}

/// autoDiscovery::Topic payload padded to ~= `bytes` serialized size
/// (topic_name fills the budget; stays under the 1500-byte reliable MTU).
autoDiscovery::Topic makePayload(std::size_t bytes) {
    autoDiscovery::Topic t;
    t.set_topic_name(std::string(bytes - 16, 'x') + "bench-payload");
    return t;
}

/// Advertise from `who` that it publishes/subscribes `topic` with `reliable`
/// QoS, carrying one advertised endpoint so the recipient can dial it.
autoDiscovery::Message makeAdvert(const Edriel::Participant& who,
                                  const std::string& topic, bool isPublisher,
                                  bool reliable, const std::string& addr,
                                  std::uint32_t port) {
    autoDiscovery::Message ad;
    autoDiscovery::TopicAdvertisement* adv = ad.mutable_advertisement();
    auto* id = adv->mutable_identifier();
    id->set_pid(who.pid);
    id->set_tid(who.tid);
    id->set_uid(who.uid);
    auto* ep = id->add_endpoints();
    ep->set_address(addr);
    ep->set_port(port);
    ep->set_transport(autoDiscovery::Endpoint::GRPC_TCP);
    auto* topicProto = adv->mutable_topic();
    topicProto->set_topic_name(topic);
    topicProto->set_message_type("autoDiscovery.Topic");
    topicProto->set_is_publisher(isPublisher);
    topicProto->set_reliable(reliable);
    return ad;
}

/// Latency percentile summary in microseconds.
struct LatencyStats {
    double p50;
    double p90;
    double p99;
    double max;
    std::size_t received;
};

LatencyStats summarize(std::vector<double> samplesUs) {
    if (samplesUs.empty()) {
        return {0.0, 0.0, 0.0, 0.0, 0};
    }
    std::sort(samplesUs.begin(), samplesUs.end());
    auto pct = [&](double p) {
        const auto idx = static_cast<std::size_t>(
            (samplesUs.size() - 1) * p);
        return samplesUs[idx];
    };
    return {pct(0.50), pct(0.90), pct(0.99), samplesUs.back(),
            samplesUs.size()};
}

void printStats(const char* name, const LatencyStats& s) {
    std::printf(
        "[bench] %s: n=%zu  p50=%.1fus  p90=%.1fus  p99=%.1fus  max=%.1fus\n",
        name, s.received, s.p50, s.p90, s.p99, s.max);
    std::fflush(stdout);
}

/**
 * @brief One publisher + one reliable subscriber, wired hermetically the way
 * test_reliable.cpp does it — no io_context run threads at all: everything
 * is test-hook driven and gRPC's own completion-queue threads carry traffic.
 *
 * @param onFrame Subscriber-side callback invoked on each delivered payload.
 */
struct ReliablePair {
    asio::io_context ioPub;
    asio::io_context ioSub;
    edriel::Config cfgPub;
    edriel::Config cfgSub;
    std::unique_ptr<Edriel> pub;
    std::unique_ptr<Edriel> sub;

    ReliablePair(const std::string& topic,
                 std::function<void(const autoDiscovery::Topic&)> onFrame) {
        cfgPub.grpcPort = freeTcpPort();
        cfgSub.grpcPort = freeTcpPort();
        pub = std::make_unique<Edriel>(ioPub, cfgPub);
        sub = std::make_unique<Edriel>(ioSub, cfgSub);

        pub->registerPublisherTopic<autoDiscovery::Topic>(topic, true);
        sub->registerSubscriberTopic<autoDiscovery::Topic>(
            topic, std::move(onFrame), true);

        pub->startGrpcServer();
        sub->startGrpcServer();

        const auto pubSelf = pub->selfIdentityForTest();
        const auto subSelf = sub->selfIdentityForTest();

        // Cross-inject consistent registries, then have the subscriber dial
        // the publisher (subscriber-initiated, ADR-0002).
        sub->deliverForTest(makeAdvert(pubSelf, topic, true, true,
                                       "127.0.0.1", cfgPub.grpcPort));
        pub->deliverForTest(makeAdvert(subSelf, topic, false, true,
                                       "127.0.0.1", cfgSub.grpcPort));
        subSelf_ = subSelf;

        sub->startReliableSubscriptions();
    }

    /// Block until the dial landed and the stream is registered on the pub.
    bool awaitDial(int timeoutMs = 3000) const {
        return waitUntil([&]() {
            return pub->subscriberConnectedForTest(keyOf(subSelf_));
        }, timeoutMs);
    }

    ~ReliablePair() {
        // Order mirrors Edriel::stopAutoDiscovery(): drop subscriber-client
        // connections FIRST, then drain the gRPC servers. Tearing the servers
        // down first can block: a saturated publisher holds a large buffered
        // outbox per stream and grpcServer_->Wait() waits for the streams to
        // wind down from the publisher side while the reader is still
        // consuming. stopReliableSubscriptions() closes the reader side so
        // Wait() completes promptly.
        sub->stopReliableSubscriptions();
        sub->stopGrpcServer();
        pub->stopGrpcServer();
    }

private:
    Edriel::Participant subSelf_{};
};

}  // namespace

// ----------------------------------------------------------------------------
// B1: Reliable publish -> callback latency percentiles
// ----------------------------------------------------------------------------

TEST(ReliableBenchmark, LatencyPercentiles) {
    constexpr int kSamples = 500;
    constexpr std::size_t kPayloadBytes = 128;
    const autoDiscovery::Topic payload = makePayload(kPayloadBytes);

    std::mutex mu;
    Clock::time_point lastSend{};
    std::vector<double> latenciesUs;
    latenciesUs.reserve(static_cast<std::size_t>(kSamples));

    ReliablePair pair("r_lat", [&](const autoDiscovery::Topic&) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mu);
        latenciesUs.push_back(
            std::chrono::duration<double, std::micro>(now - lastSend).count());
    });
    ASSERT_TRUE(pair.awaitDial())
        << "subscriber dial never landed on publisher";

    for (int i = 0; i < kSamples; ++i) {
        const auto t = Clock::now();
        {
            std::lock_guard<std::mutex> lock(mu);
            lastSend = t;
        }
        ASSERT_TRUE(pair.pub->sendMessage("r_lat", payload));
        // Pace like benchmark.cpp's latency test: measure latency, not queueing.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Drain tail before reporting.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    LatencyStats stats;
    {
        std::lock_guard<std::mutex> lock(mu);
        stats = summarize(latenciesUs);
    }
    printStats("reliable publish->callback latency (128B)", stats);
    EXPECT_GE(stats.received, kSamples * 9 / 10);  // tolerate <=10% tail miss
}

// ----------------------------------------------------------------------------
// B2: Sustained paced reliable throughput over a fixed window
// ----------------------------------------------------------------------------

TEST(ReliableBenchmark, ThroughputMsgsPerSecond) {
    constexpr auto kWindowMs = 2000;
    constexpr std::size_t kPayloadBytes = 256;
    const autoDiscovery::Topic msg = makePayload(kPayloadBytes);

    // B4-style arrival span: the subscriber callback stamps the first and last
    // delivery, so the rate below is the TRUE ABSORB rate (delivered / active
    // delivery span) — not the offer rate. Earlier revisions divided the
    // post-drain count by the pre-drain offer window, which (because the
    // unpaced caller finishes offering in a small fraction of the drain and
    // the publisher buffers the overload) printed the OFFER rate mislabeled
    // as a receive rate. See README known-issue and ADR-0002 errata.
    std::atomic<int64_t> firstUs{-1};
    std::atomic<int64_t> lastUs{0};
    std::atomic<int64_t> receivedCount{0};
    ReliablePair pair(
        "r_thr", [&](const autoDiscovery::Topic&) {
            const auto nowUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now().time_since_epoch()).count();
            int64_t expected = -1;
            firstUs.compare_exchange_strong(expected, nowUs,
                                            std::memory_order_relaxed);
            lastUs.store(nowUs, std::memory_order_relaxed);
            receivedCount.fetch_add(1, std::memory_order_relaxed);
        });
    ASSERT_TRUE(pair.awaitDial()) << "subscriber dial never landed";

    const auto start = Clock::now();
    int64_t sent = 0;
    int64_t backpressureStops = 0;
    // ADR-0004: the per-subscriber outbox is now BOUNDED (HWM/LWM gate), so
    // the harness no longer needs a lag backstop to stay hermetic — the
    // transport itself refuses the push with Backpressured (bool false)
    // once the subscriber's outbox reaches its high-water mark. The ~10x
    // offer/absorb gap surfaces as refusals, not publisher-side RAM.
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - start).count() < kWindowMs) {
        if (!pair.pub->sendMessage("r_thr", msg)) {
            ++backpressureStops;
            break;
        }
        ++sent;
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count();

    // Drain the in-flight tail before reporting.
    int64_t last = -1;
    int quietMs = 0;
    for (int i = 0; i < 3000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int64_t nowGot = receivedCount.load(std::memory_order_relaxed);
        if (nowGot == last) {
            quietMs += 2;
            if (quietMs >= 300) {
                break;
            }
        } else {
            last = nowGot;
            quietMs = 0;
        }
    }

    const int64_t got = receivedCount.load();
    // Offer rate: how fast the unpaced caller pushed into the (unbounded)
    // publisher outbox — NOT the delivery rate.
    const double offerRate = sent * 1000.0 / elapsedMs;
    // True absorb rate: delivered / active delivery span (B4 basis).
    const double activeS =
        (got > 0)
            ? static_cast<double>(lastUs.load() - firstUs.load()) / 1e6
            : 0.0;
    const double absorbRate = (activeS > 0.0)
                                  ? static_cast<double>(got) / activeS
                                  : 0.0;
    // The reliable path currently BUFFERS overload: offer rate can exceed the
    // absorb ceiling arbitrarily (README known-issue). Surface the gap so it
    // is never silent (ADR-003 decision #4 discipline on the reliable path).
    const int64_t absorbedLag = sent - got;
    std::printf(
        "[bench] reliable throughput (%zuB): sent=%lld  offer=%.0f msgs/s  "
        "absorbed=%lld  ABSORB=%.0f msgs/s (active span %.3fs)  "
        "buffered_in_outbox=%lld (%.2f%%)\n",
        kPayloadBytes, static_cast<long long>(sent), offerRate,
        static_cast<long long>(got), absorbRate, activeS,
        static_cast<long long>(absorbedLag),
        (sent > 0) ? 100.0 * static_cast<double>(absorbedLag)
                         / static_cast<double>(sent)
                   : 0.0);
    std::printf(
        "[bench] reliable throughput note: stopped=%s (ADR-0004 bounded "
        "outbox: overload now surfaces as Backpressured refusals, not RAM)\n",
        backpressureStops ? "backpressured" : "window-closed");
    std::fflush(stdout);

    EXPECT_GT(sent, 0);
    EXPECT_GT(got, 0);
    // Exactly-once holds: never more than offered.
    EXPECT_LE(got, sent);
    // Regression bar vs the absorb baseline documented in README.md. The
    // baseline is the TRUE absorb rate (delivered / active delivery span).
    // Under ADR-0004 the offer window closes at the outbox HWM within ~10 ms
    // (sent ~820 frames), so the active span is tiny and noisy: observed
    // 54k–140k msgs/s across fresh runs (median ~87k). 50k with 0.75x
    // headroom keeps the bar under the observed floor while still catching a
    // genuine regression; re-base by editing this constant only after a
    // fresh measurement.
    constexpr double kBaselineAbsorbMsgsPerSec = 67000.0;
    constexpr double kMinRecvMsgsPerSec = 0.75 * kBaselineAbsorbMsgsPerSec;
    EXPECT_GE(absorbRate, kMinRecvMsgsPerSec)
        << "reliable absorb rate regressed vs documented baseline";
}

// ----------------------------------------------------------------------------
// B3: One publisher fanning out to N subscribers
// ----------------------------------------------------------------------------

TEST(ReliableBenchmark, FanOutToNSubscribers) {
    constexpr int kSubscribers = 8;
    constexpr auto kWindowMs = 1500;
    constexpr std::size_t kPayloadBytes = 256;
    const autoDiscovery::Topic msg = makePayload(kPayloadBytes);

    asio::io_context ioPub;
    edriel::Config cfgPub;
    cfgPub.grpcPort = freeTcpPort();
    std::unique_ptr<Edriel> pub = std::make_unique<Edriel>(ioPub, cfgPub);
    ASSERT_TRUE(pub->registerPublisherTopic<autoDiscovery::Topic>("r_fan", true));
    pub->startGrpcServer();

    // N subscriber nodes, each an independent Edriel with its own io_context
    // (never run — hook-driven like the pair above) and its own gRPC port.
    struct SubEntry {
        asio::io_context io;
        edriel::Config cfg;
        std::unique_ptr<Edriel> node;
        std::atomic<int64_t> received{0};
    };
    std::vector<std::unique_ptr<SubEntry>> subs;
    subs.reserve(static_cast<std::size_t>(kSubscribers));

    std::atomic<int64_t> totalReceived{0};

    for (int sIdx = 0; sIdx < kSubscribers; ++sIdx) {
        auto entry = std::make_unique<SubEntry>();
        entry->cfg.grpcPort = freeTcpPort();
        entry->node = std::make_unique<Edriel>(entry->io, entry->cfg);
        Edriel* nodePtr = entry->node.get();
        SubEntry* entryPtr = entry.get();
        const std::uint16_t subGrpcPort = entry->cfg.grpcPort;  // keep past move
        nodePtr->registerSubscriberTopic<autoDiscovery::Topic>(
            "r_fan",
            [entryPtr, &totalReceived](const autoDiscovery::Topic&) {
                entryPtr->received.fetch_add(1, std::memory_order_relaxed);
                totalReceived.fetch_add(1, std::memory_order_relaxed);
            },
            true);
        entry->node->startGrpcServer();

        // Cross-inject: pub sees this subscriber; subscriber sees the pub.
        const auto pubSelf = pub->selfIdentityForTest();
        const auto subSelf = nodePtr->selfIdentityForTest();
        subs.push_back(std::move(entry));
        nodePtr->deliverForTest(makeAdvert(pubSelf, "r_fan", true, true,
                                           "127.0.0.1", cfgPub.grpcPort));
        pub->deliverForTest(makeAdvert(subSelf, "r_fan", false, true,
                                       "127.0.0.1", subGrpcPort));
        nodePtr->startReliableSubscriptions();

        const auto subSelfCopy = subSelf;
        ASSERT_TRUE(waitUntil([&]() {
            return pub->subscriberConnectedForTest(keyOf(subSelfCopy));
        }, 4000))
            << "subscriber #" << sIdx << " dial never landed";
    }

    const auto start = Clock::now();
    int64_t sent = 0;
    int64_t backpressureStops = 0;
    // ADR-0004 Q3 discipline (critical for fan-out): the tid commits when
    // ANY subscriber accepts (Q3 rule 3), so a bare send-until-fail loop —
    // or even a retry-until-`Sent` loop — DIVERGES subscribers: the moment
    // one subscriber accepts while another is backpressured, the frame's
    // tid is committed and the refused subscriber's window faces a gap it
    // can never close (same-tid retry only applies when ALL live subs
    // refused and the tid was never committed). The caller-side fix is the
    // ADR-0004 2C `isSendable()` probe — the documented LWM-resume surface:
    // offer a new frame ONLY when every subscriber can accept it (identical
    // gate condition to enqueue's HWM refusal), and when any subscriber is
    // at/above its HWM, STOP offering and bounded-retry the gate (short
    // sleep, wall-clock deadline ~30s, EXPECT it eventually opens) instead
    // of pushing into refusal. The offer loop is single-threaded, so the
    // probe can never go stale between check and push — no partial
    // acceptance ever occurs, every subscriber sees every tid exactly once,
    // and after the final drain-to-quiescence all N per-sub counts converge
    // (asserted EQUAL below).
    constexpr auto kRetryDeadlineMs = 30000;
    const std::string payload = msg.SerializeAsString();
    const std::string messageType(
        std::string(autoDiscovery::Topic::descriptor()->full_name()));
    std::map<std::string, OutboxStatus> deliverability;

    // Snapshot the subscriber keys once (identities are stable for the test).
    std::vector<SubscriberKey> subKeys;
    subKeys.reserve(subs.size());
    for (const auto& entry : subs) {
        subKeys.push_back(keyOf(entry->node->selfIdentityForTest()));
    }
    const auto allSendable = [&]() {
        for (const SubscriberKey& sk : subKeys) {
            if (!pub->isSendable(sk)) {
                return false;
            }
        }
        return true;
    };

    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - start).count() < kWindowMs) {
        if (!allSendable()) {
            // Some subscriber is at/above its outbox HWM: stop offering new
            // frames and wait (bounded) for the drain to cross the LWM.
            ++backpressureStops;
            const auto retryStart = Clock::now();
            bool resumed = false;
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now() - retryStart).count() < kRetryDeadlineMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (allSendable()) {
                    resumed = true;
                    break;
                }
            }
            EXPECT_TRUE(resumed)
                << "outbox never drained below the LWM within the retry "
                   "deadline (stalled subscriber?)";
            if (!resumed) {
                break;
            }
        }
        const ReliableSendResult r = pub->tryPublishReliable(
            "r_fan", messageType, payload, &deliverability);
        if (r == ReliableSendResult::NoSubscribers
            || r == ReliableSendResult::NotServing) {
            break;
        }
        ++sent;
        // Defensive: a partial acceptance (any Backpressured alongside a
        // commit) would permanently diverge subscribers — it must be
        // impossible behind the allSendable gate.
        for (const auto& kv : deliverability) {
            EXPECT_EQ(kv.second, OutboxStatus::Accepted)
                << "partial fan-out acceptance: tid committed while a "
                   "subscriber was refused (Q3.3 gap hazard)";
        }
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count();

    // Drain to quiescence across all subscribers.
    int64_t lastTotal = -1;
    int quietMs = 0;
    for (int i = 0; i < 4000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int64_t nowTotal = totalReceived.load(std::memory_order_relaxed);
        if (nowTotal == lastTotal) {
            quietMs += 2;
            if (quietMs >= 400) {
                break;
            }
        } else {
            lastTotal = nowTotal;
            quietMs = 0;
        }
    }

    int64_t perSubMin = std::numeric_limits<int64_t>::max();
    int64_t perSubMax = 0;
    for (const auto& entry : subs) {
        perSubMin = std::min(perSubMin, entry->received.load());
        perSubMax = std::max(perSubMax, entry->received.load());
    }
    const double fanRate = totalReceived.load() * 1000.0 / elapsedMs;
    std::printf(
        "[bench] reliable fan-out (%d subscribers, %zuB): sent=%lld  "
        "total_delivered=%lld (%.0f msgs/s aggregate, %.0f msgs/s per-sub "
        "ideal)  per-sub=[%lld..%lld]  stopped=%s (ADR-0004 Q3 offer gate: "
        "offer only while ALL subscribers sendable (2C probe), bounded "
        "retry until the gate reopens, per-sub counts converge)\n",
        kSubscribers, kPayloadBytes, static_cast<long long>(sent),
        static_cast<long long>(totalReceived.load()), fanRate,
        fanRate / kSubscribers,
        static_cast<long long>(perSubMin),
        static_cast<long long>(perSubMax),
        backpressureStops ? "backpressured" : "window closed");
    std::fflush(stdout);

    EXPECT_GT(totalReceived.load(), 0LL);      // fan-out actually delivered
    EXPECT_LE(totalReceived.load(), sent * kSubscribers);  // exactly-once bound
    // Per-subscriber exactly-once is a CHECKED property, not a printout:
    // every subscriber must receive an IDENTICAL count (fan-out is uniform),
    // and that count can never exceed what was sent (no dup inflation).
    const int64_t firstSubCount = subs.front()->received.load();
    for (const auto& entry : subs) {
        EXPECT_GT(entry->received.load(), 0LL);   // every subscriber fed
        EXPECT_LE(entry->received.load(), sent);  // <= sent (no dup inflation)
        EXPECT_EQ(entry->received.load(), firstSubCount)
            << "per-subscriber exactly-once violated: fan-out counts differ";
    }
}

// ----------------------------------------------------------------------------
// B4: Unpaced flood ceiling (true unpushed max of the reliable path)
// ----------------------------------------------------------------------------

TEST(ReliableBenchmark, UnpacedFloodCeiling) {
    constexpr auto kFloodDuration = std::chrono::milliseconds(2000);
    constexpr std::size_t kPayloadBytes = 256;
    const autoDiscovery::Topic msg = makePayload(kPayloadBytes);

    // Arrival histogram (5 ms buckets) for the best-1s delivered window.
    constexpr int kBucketMs = 5;
    constexpr int kBucketCount = 1024;  // 5.12 s coverage >> active span
    constexpr int kBucketsPerSec = 1000 / kBucketMs;
    std::array<std::atomic<int64_t>, kBucketCount> arrivalBuckets{};
    std::atomic<int64_t> t0Us{0};
    std::atomic<int64_t> firstUs{-1};
    std::atomic<int64_t> lastUs{0};
    std::atomic<int64_t> receivedCount{0};

    ReliablePair pair("r_fl", [&](const autoDiscovery::Topic&) {
        const auto nowUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch()).count();
        int64_t expected = -1;
        firstUs.compare_exchange_strong(expected, nowUs,
                                        std::memory_order_relaxed);
        lastUs.store(nowUs, std::memory_order_relaxed);
        if (const auto relUs = nowUs - t0Us.load(std::memory_order_relaxed);
            relUs >= 0) {
            arrivalBuckets[static_cast<std::size_t>(
                               relUs / (kBucketMs * 1000)) % kBucketCount]
                .fetch_add(1, std::memory_order_relaxed);
        }
        receivedCount.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(pair.awaitDial()) << "subscriber dial never landed";

    t0Us.store(std::chrono::duration_cast<std::chrono::microseconds>(
                   Clock::now().time_since_epoch()).count());

    // UNPACED flood: no pacing sleep, single producer thread hammering
    // sendMessage until the window closes, a send fails, or — with ADR-0004's
    // bounded outbox — the transport itself backpressures (bool false at the
    // subscriber's HWM). No harness lag cap is needed anymore: the flood can
    // no longer queue unbounded RAM behind the single in-flight write, so
    // the harness stays hermetic without simulating backpressure itself.
    std::atomic<int64_t> sent{0};
    std::atomic<bool> failed{false};
    std::thread flood([&]() {
        const auto winStart = Clock::now();
        while (!failed.load(std::memory_order_relaxed)) {
            if (!pair.pub->sendMessage("r_fl", msg)) {
                failed.store(true, std::memory_order_relaxed);
                break;
            }
            sent.fetch_add(1, std::memory_order_relaxed);
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - winStart) >= kFloodDuration) {
                break;
            }
        }
    });
    flood.join();

    // Drain to quiescence.
    int64_t last = -1;
    int quietMs = 0;
    for (int i = 0; i < 3000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int64_t nowGot = receivedCount.load(std::memory_order_relaxed);
        if (nowGot == last) {
            quietMs += 2;
            if (quietMs >= 300) {
                break;
            }
        } else {
            last = nowGot;
            quietMs = 0;
        }
    }

    // Best 1-second delivered window from the histogram.
    int64_t peakWindow = 0;
    {
        int64_t window = 0;
        for (int i = 0; i < kBucketCount + kBucketsPerSec; ++i) {
            window += arrivalBuckets[i % kBucketCount].load(
                std::memory_order_relaxed);
            if (i >= kBucketsPerSec) {
                window -= arrivalBuckets[(i - kBucketsPerSec) % kBucketCount]
                              .load(std::memory_order_relaxed);
            }
            if (i + 1 >= kBucketsPerSec && window > peakWindow) {
                peakWindow = window;
            }
        }
    }

    const int64_t got = receivedCount.load();
    const int64_t pushed = sent.load();
    const double activeS =
        (got > 0)
            ? static_cast<double>(lastUs.load() - firstUs.load()) / 1e6
            : 0.0;
    const double meanRecvRate = (activeS > 0.0)
                                    ? static_cast<double>(got) / activeS
                                    : 0.0;
    std::printf(
        "[bench] reliable unpaced flood (%zuB): pushed=%lld (stopped=%s)  "
        "delivered=%lld  FLOOD CEILING (best 1s)=%.0f msgs/s  mean=%.0f msgs/s\n",
        kPayloadBytes, static_cast<long long>(pushed),
        failed.load() ? "backpressured" : "window closed",
        static_cast<long long>(got),
        static_cast<double>(peakWindow), meanRecvRate);
    std::fflush(stdout);

    EXPECT_GT(pushed, 0);
    EXPECT_GT(got, 0);
    // Delivered must not exceed pushed (exactly-once dedup holds).
    EXPECT_LE(got, pushed);
}

}  // namespace edriel
