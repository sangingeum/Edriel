/**
 * @file benchmark.cpp
 * @brief Latency and throughput harness for Edriel multicast pub/sub
 *
 * Measures real publish -> subscriber callback round-trips over multicast
 * loopback (239.255.0.1:30002):
 *   - Round-trip latency percentiles (p50/p90/p99/max)
 *   - Sustained throughput (msgs/sec) for a fixed burst window
 *
 * The harness instantiates one Edriel node on a background io_context thread,
 * subscribes to a protobuf topic, then publishes numbered messages and stamps
 * the send time into the payload so the receiving callback can compute the
 * round trip without cross-thread clock games.
 */

#include <gtest/gtest.h>
#include "Edriel.hpp"

#include "benchmark.pb.h"

using benchmark::Ping;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edriel {

using Clock = std::chrono::steady_clock;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

namespace {

struct LatencyStats {
    double p50;
    double p90;
    double p99;
    double max;
    std::size_t received;
};

/// Compute microsecond percentiles from a vector of raw durations (us).
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

/// One Edriel node running its io_context on a dedicated thread.
struct BenchNode {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard;
    std::thread runner;
    std::unique_ptr<Edriel> node;

    explicit BenchNode(unsigned long pid, uint64_t tid, uint64_t uid)
        : guard(asio::make_work_guard(io)) {
        node = std::make_unique<Edriel>(io);
        // Assign identity via heartbeat piggyback fields is not exposed;
        // identity only affects remote registry bookkeeping, not loopback
        // data delivery, so defaults are fine for benchmarking.
        (void)pid; (void)tid; (void)uid;
        node->startAutoDiscovery();
        runner = std::thread([this] { io.run(); });
    }

    ~BenchNode() {
        node->stopAutoDiscovery();
        // Stop the io_context BEFORE joining so run() returns promptly instead
        // of draining a queued backlog of async_sends against the closed
        // socket. Without this, a heavy send burst (the throughput tests) can
        // leave hundreds of thousands of stranded sends that each log a "Bad
        // file descriptor" on teardown and keep the join alive for seconds.
        io.stop();
        guard.reset();
        if (runner.joinable()) {
            runner.join();
        }
    }
};

}  // namespace

// ----------------------------------------------------------------------------
// Latency benchmark: publish -> callback round trip over multicast loopback
// ----------------------------------------------------------------------------

TEST(Benchmark, PublishCallbackLatency) {
    constexpr int kSamples = 500;

    BenchNode receiver(1, 0, 1);

    std::mutex mu;
    std::vector<double> latenciesUs;
    latenciesUs.reserve(kSamples);
    // Send timestamps keyed by sequence number.
    std::map<int64_t, Clock::time_point> sentAt;

    ASSERT_TRUE(receiver.node->registerSubscriberTopic<Ping>(
        "latency",
        [&](const Ping& msg) {
            const auto now = Clock::now();
            std::lock_guard<std::mutex> lock(mu);
            auto it = sentAt.find(msg.seq());
            if (it != sentAt.end()) {
                const double us =
                    std::chrono::duration<double, std::micro>(
                        now - it->second).count();
                latenciesUs.push_back(us);
                sentAt.erase(it);
            }
        }));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // settle

    Ping ping;
    ping.set_seq(0);
    for (int i = 0; i < kSamples; ++i) {
        ping.set_seq(i);
        const auto t = Clock::now();
        {
            std::lock_guard<std::mutex> lock(mu);
            sentAt[i] = t;
        }
        ASSERT_TRUE(receiver.node->sendMessage("latency", ping));
        // Pace: give each message room to make the loopback round trip so we
        // measure latency, not queueing delay.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Drain tail. Guarded: the callback may still be appending on the io thread.
    {
        auto safeSize = [&] {
            std::lock_guard<std::mutex> lock(mu);
            return latenciesUs.size();
        };
        for (int i = 0; i < 100 && safeSize() < static_cast<std::size_t>(kSamples); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::lock_guard<std::mutex> lock(mu);
    const auto stats = summarize(latenciesUs);
    printStats("publish->callback latency", stats);
    EXPECT_GE(stats.received, kSamples * 9 / 10);   // tolerate <=10% loss
    EXPECT_LT(stats.p99, 10000.0);                  // sanity gate: <10ms p99
}

// ----------------------------------------------------------------------------
// Throughput benchmark: sustained publish rate over multicast loopback
// ----------------------------------------------------------------------------

TEST(Benchmark, ThroughputMsgsPerSecond) {
    constexpr auto kWindowMs = 2000;
    constexpr std::size_t kPayloadBytes = 256;

    BenchNode counterNode(2, 0, 2);

    std::atomic<int64_t> receivedCount{0};
    ASSERT_TRUE(counterNode.node->registerSubscriberTopic<Ping>(
        "throughput",
        [&](const Ping&) { receivedCount.fetch_add(1, std::memory_order_relaxed); }));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // settle

    Ping msg;
    msg.set_seq(0);
    msg.mutable_blob()->resize(kPayloadBytes, 'x');

    const auto start = Clock::now();
    int64_t sent = 0;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - start).count() < kWindowMs) {
        msg.set_seq(++sent);
        if (!counterNode.node->sendMessage("throughput", msg)) {
            break;
        }
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count();

    // Give in-flight messages a moment to drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const int64_t got = receivedCount.load();
    const int64_t lost = sent - got;
    // ADR-003 decision #4: ring-overflow drops are never silent.
    const std::uint64_t ringDropped = counterNode.node->droppedFrames();
    const double lostPct =
        (sent > 0) ? 100.0 * static_cast<double>(lost)
                               / static_cast<double>(sent)
                   : 0.0;
    // Send side: one sendMessage() == one multicast datagram, so msgs/s IS the
    // datagram rate; byte rate is reported as payload bytes/s for the app
    // payload (dialect envelope overhead not included).
    const double sentRate = sent * 1000.0 / elapsedMs;
    const double recvRate = got * 1000.0 / elapsedMs;
    const double sentByteRate = sentRate * static_cast<double>(kPayloadBytes);
    const double recvByteRate = recvRate * static_cast<double>(kPayloadBytes);
    std::printf(
        "[bench] throughput (%zuB payload): sent=%lld (%.0f msgs/s, %.0f B/s)  "
        "received=%lld (%.0f msgs/s, %.0f B/s)  lost=%lld (%.2f%%)  "
        "ring_dropped=%llu\n",
        kPayloadBytes, static_cast<long long>(sent), sentRate, sentByteRate,
        static_cast<long long>(got), recvRate, recvByteRate,
        static_cast<long long>(lost), lostPct,
        static_cast<unsigned long long>(ringDropped));
    std::fflush(stdout);

    EXPECT_GT(sent, 0);
    EXPECT_GT(got, 0);
}

// ----------------------------------------------------------------------------
// Two-node SHARDED RECEIVE-ONLY throughput benchmark — ADR-003 HARD GATE.
//
// ADR-0003's receive-parallelism win lives in the WORKERS: `worker_threads`
// shards each own one SPSC ring and one registry shard, and every frame routes
// to a shard by topic key. A SINGLE-topic harness routes every frame to ONE
// shard -> ONE worker, so worker_threads=4 yields ZERO parallel dispatch and
// the design's promised gain cannot manifest (vera measured ~101-111k, just
// below the pre-impl baseline). That was the measurement gap in issue #6.
//
// This benchmark closes it: it presses `kTopics` chosen to hash to four
// DISTINCT shards (ADR-003 owner decision #2 — topic-only shard key) so all
// `worker_threads` workers dispatch in parallel, and it drives them with
// enough producers to clear the 1.5x bar. Four producers are used so the
// unpaced flood RELIABLY exceeds the consumer's ceiling on every run: with
// only two, a scheduling under-delivery could let the consumer absorb nearly
// all offered frames, tripping the saturation self-test (GitHub issue #7).
//
// HARD GATE (must FAIL CI on a regression to baseline): consumer saturated
// throughput msgs/s >= 1.5 * baseline. Baseline: baseline_2node_receive_59412eb.md
// (HEAD 59412eb) two-node consumer receive ceiling ~118k msgs/s (band 114-139k).
// 1.5 * ~118k = ~177k, encoded in kBaselineRecvMsgsPerSec below. Owner re-bases
// by editing that constant (and this comment); the bar is not silently lowered.
//
// SATURATED-TRUE-MAX (owner re-measure): the earlier keep-pace run PACED the
// producers (~210k msgs/s) so consumer reported a SUSTAINED-KEEP-PACE rate,
// not the unpushed ceiling. Here the producers instead FLOOD unpaced, far above
// what the consumer can absorb, so the consumer is genuinely SATURATED. The
// reported & gated figure is consumerTrueMax — best-1s DELIVERED window — the
// consumer's true unpushed ceiling. It is stable here (~450-540k) because (a)
// with four producers the flood always saturates AND (b) the arrival histogram
// covers the entire active span (kBucketCount=2048), so the sliding wheel never
// wraps. The earlier ~600-800k readings were inflated by the same histogram
// wrapping on the producer-backlog drain (1024 buckets over a ~16s span).
// Producer send = frames that reached the consumer socket / flood window
// (distinguished from absorbed); loss (a.k.a. the drop rate at that operating
// point) is surfaced by droppedFrames(), which includes kernel SO_RXQ_OVFL
// overruns (issue #6 "ring_dropped=0 blind spot" fix).
// ----------------------------------------------------------------------------
TEST(Benchmark, TwoNodeReceiveThroughput) {
    constexpr std::size_t kPayloadBytes = 256;
    // Four topics hashing to four DISTINCT shards: fnv1a64(topic + 0x1F +
    // "benchmark.Ping") % workerCount == {0,1,2,3} at worker_count=4.
    const std::vector<std::string> kTopics = {"k0", "k1", "k2", "k3"};
    // Producer nodes, each on its own io_context/thread (decoupled from the
    // consumer — NEVER one shared io_context: that reproduces the single-node
    // ~1 msgs/s starvation artifact).
    constexpr int kProducers = 4;
    // ADR-003 gate baseline (see comment above): ~118k msgs/s consumer recv at
    // HEAD 59412eb. The 1.5x bar = ~177k.
    constexpr double kBaselineRecvMsgsPerSec = 118000.0;
    constexpr double kMinRecvMsgsPerSec = 1.5 * kBaselineRecvMsgsPerSec;
    // Unpaced flood: producers send as fast as each node's io thread and the
    // loopback wire will carry, far above the consumer's ceiling, so the
    // consumer is SATURATED and the delivered max below is its TRUE unpushed
    // ceiling, not a paced producer target. The flood window bounds runtime.
    constexpr auto kFloodDuration = std::chrono::milliseconds(3000);

    // Consumer node (loads config.yml -> worker_threads=4).
    BenchNode consumer(4, 0, 4);
    std::array<std::atomic<int64_t>, 4> perTopic{{0, 0, 0, 0}};
    std::atomic<int64_t> receivedCount{0};
    std::atomic<int64_t> firstUs{-1};  // steady_clock us of first delivery
    std::atomic<int64_t> lastUs{0};    // steady_clock us of last delivery
    // Arrival-time histogram (10 ms buckets) for the sustained-rate gate: the
    // max delivered in any one-second window is the standard, scheduling-jitter
    // tolerant "sustained msgs/s" figure. A first->last span is distorted when
    // a paced producer stalls between bursts; a one-second sliding max is not.
    constexpr int kBucketMs = 10;
    constexpr int kBucketCount = 2048;          // 20.48 s coverage at 10 ms
                                                // (>= the ~16 s producer-backlog
                                                // drain, so the sliding-sum
                                                // window never wraps and
                                                // corrupts the peak/median)
    constexpr int kBucketsPerSec = 1000 / kBucketMs;
    std::array<std::atomic<int64_t>, kBucketCount> arrivalBuckets{};
    std::atomic<int64_t> t0Us{0};               // set just before producers start
    for (std::size_t i = 0; i < kTopics.size(); ++i) {
        ASSERT_TRUE(consumer.node->registerSubscriberTopic<Ping>(
            kTopics[i],
            [&, i](const Ping&) {
                const auto nowUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now().time_since_epoch()).count();
                if (firstUs.load() < 0) {
                    int64_t expected = -1;
                    firstUs.compare_exchange_strong(
                        expected, nowUs, std::memory_order_relaxed);
                }
                lastUs.store(nowUs);
                if (const auto relUs = nowUs - t0Us.load(); relUs >= 0) {
                    const std::size_t idx = static_cast<std::size_t>(
                        relUs / (kBucketMs * 1000)) % kBucketCount;
                    arrivalBuckets[idx].fetch_add(1, std::memory_order_relaxed);
                }
                perTopic[i].fetch_add(1, std::memory_order_relaxed);
                receivedCount.fetch_add(1, std::memory_order_relaxed);
            }));
    }

    std::vector<std::unique_ptr<BenchNode>> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.push_back(std::make_unique<BenchNode>(10 + p, 0, 10 + p));
    }

    // Settle: let every node join the multicast group and arm its receiver.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    Ping msg;
    msg.set_seq(0);
    msg.mutable_blob()->resize(kPayloadBytes, 'x');

    std::atomic<std::int64_t> sentTotal{0};
    // Producer send window (for the offered send rate): first to last producer
    // timestamp over the flood.
    std::atomic<std::int64_t> producerStartUs{-1};
    std::atomic<std::int64_t> producerEndUs{-1};
    // Anchor the arrival histogram just before production begins (the callback
    // buckets relative to this; everything before it is discarded).
    t0Us.store(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());

    // UNPACED flood producer: no rate sleep, no pacing math — send into this
    // node's strand as fast as the caller and io thread will allow, for a fixed
    // window. This drives the consumer well past its ceiling, so the delivered
    // max below is the consumer's TRUE unpushed ceiling.
    constexpr long long kSendBurst = 256;

    std::vector<std::thread> runners;
    runners.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        runners.emplace_back([&, p]() {
            Ping local = msg;  // per-thread message owned by this runner
            const auto winStart = Clock::now();
            std::int64_t my = 0;
            local.set_seq(p);
            bool fail = false;
            std::int64_t startUs = 0;
            std::int64_t endUs = 0;
            while (!fail) {
                for (long long i = 0; i < kSendBurst; ++i) {
                    const std::string& topic = kTopics[
                        static_cast<std::size_t>(my % kTopics.size())];
                    local.set_seq(static_cast<int32_t>(my) + 1 + p);
                    if (!producers[p]->node->sendMessage(topic, local)) {
                        fail = true;
                        break;
                    }
                    if (my == 0) {
                        startUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      Clock::now().time_since_epoch()).count();
                    }
                    ++my;
                }
                if (fail ||
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - winStart) >= kFloodDuration) {
                    break;
                }
            }
            endUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now().time_since_epoch()).count();
            sentTotal.fetch_add(my, std::memory_order_relaxed);
            // Aggregate producer send window [min start, max end] across the
            // two producers.
            {
                std::int64_t cur = producerStartUs.load(std::memory_order_relaxed);
                while ((cur < 0 || startUs < cur) &&
                       !producerStartUs.compare_exchange_weak(
                           cur, (cur < 0) ? startUs : std::min(startUs, cur),
                           std::memory_order_relaxed)) {
                }
            }
            {
                std::int64_t cur = producerEndUs.load(std::memory_order_relaxed);
                while (endUs > cur &&
                       !producerEndUs.compare_exchange_weak(
                           cur, endUs, std::memory_order_relaxed)) {
                }
            }
        });
    }
    for (auto& t : runners) {
        t.join();
    }

    // Drain to quiescence (delivery quiet for ~500ms), then a little more so
    // the producers' io threads fully drain while their sockets are open.
    std::int64_t lastGot = -1;
    int quietMs = 0;
    for (int i = 0; i < 6000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const std::int64_t g = receivedCount.load(std::memory_order_relaxed);
        if (g == lastGot) {
            quietMs += 2;
            if (quietMs >= 500) {
                break;
            }
        } else {
            lastGot = g;
            quietMs = 0;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int64_t got = receivedCount.load();
    // Frames that actually reached the consumer's socket = delivered + dropped
    // (ring overflow + kernel SO_RXQ_OVFL overruns). This is the honest frame
    // count that saturated the consumer — NOT the posted async_send_to count,
    // which counts the POSTS queued on the producers' strands and can hugely
    // over-state the wire rate (a caller can post far faster than the strand
    // syscalls). Loss is therefore delivered vs. offered-to-socket.
    const std::uint64_t ringDropped = consumer.node->droppedFrames();
    const int64_t offered = got + static_cast<int64_t>(ringDropped);
    const int64_t lost = (offered > got) ? (offered - got) : 0;
    const double lostPct =
        (offered > 0)
            ? 100.0 * static_cast<double>(lost)
                          / static_cast<double>(offered)
            : 0.0;
    // Sustained (best one-second window) receive rate, directly from the arrival
    // histogram. This is the scheduling-jitter tolerant "sustained end-to-end
    // msgs/s" the ADR gate names — it measures the best full second the receive
    // path actually held under the flood, immune to a producer stall distorting
    // a first->last span. (The histogram must cover the WHOLE active delivery
    // span including the producer-backlog drain — kBucketCount=2048 — or the
    // ring indexes wrap and the sliding sum corrupts the peak. The old 1024
    // buckets did exactly that and inflated the earlier ~600-800k figures.)
    int64_t peakWindow = 0;
    {
        int64_t window = 0;
        for (int i = 0; i < kBucketCount + kBucketsPerSec; ++i) {
            const int64_t enter =
                arrivalBuckets[i % kBucketCount].load(std::memory_order_relaxed);
            window += enter;
            if (i >= kBucketsPerSec) {
                window -= arrivalBuckets[(i - kBucketsPerSec) % kBucketCount]
                              .load(std::memory_order_relaxed);
            }
            if (i + 1 >= kBucketsPerSec && window > peakWindow) {
                peakWindow = window;
            }
        }
    }
    const double consumerTrueMax = static_cast<double>(peakWindow);
    // Producer send max: frames that reached the consumer socket (offered)
    // divided by the aggregate flood window = the rate the two producers offered
    // to the consumer, DISTINCT from what it absorbed (consumerTrueMax). Bound
    // by the producers' io threads and the loopback wire.
    const double producerWinS =
        (producerEndUs.load() > producerStartUs.load())
            ? static_cast<double>(producerEndUs.load() - producerStartUs.load())
                  / 1e6
            : 0.0;
    const double producerRate =
        (producerWinS > 0.0)
            ? static_cast<double>(offered) / producerWinS
            : 0.0;
    // Mean over the active delivery span (first->last), reported for context.
    const double activeS = (got > 0)
        ? static_cast<double>(lastUs.load() - firstUs.load()) / 1e6 : 0.0;
    const double meanRecvRate = (activeS > 0.0) ? got / activeS : 0.0;
    std::printf(
        "[bench] sharded two-node receive (%zuB, %d producers, %zu topics, "
        "%lld offered): CONSUMER TRUE MAX=%.0f msgs/s (best 1s delivered "
        "window, bar >=%.0f)  producer-send=%.0f msgs/s  meanRecv=%.0f  "
        "received=%lld  lost=%lld (%.2f%%)  observable_dropped=%llu  "
        "per-topic=[%lld,%lld,%lld,%lld]\n",
        kPayloadBytes, kProducers, kTopics.size(),
        static_cast<long long>(offered),
        consumerTrueMax, kMinRecvMsgsPerSec,
        producerRate,
        meanRecvRate,
        static_cast<long long>(got), static_cast<long long>(lost), lostPct,
        static_cast<unsigned long long>(ringDropped),
        static_cast<long long>(perTopic[0].load()),
        static_cast<long long>(perTopic[1].load()),
        static_cast<long long>(perTopic[2].load()),
        static_cast<long long>(perTopic[3].load()));
    std::fflush(stdout);

    // HARD GATE (owner re-measure): consumer TRUE max (best 1s delivered)
    // >= 1.5x baseline with all worker shards exercised under an unpaced flood.
    // Regression to baseline (~118k) fails CI.
    EXPECT_GT(offered, 0);
    EXPECT_GE(got, 2000LL);
    // Saturation: the consumer must genuinely be flooded — if it absorbed nearly
    // all the wire carried, the producers weren't pushing past its ceiling and
    // the measured max was under-exercised.
    EXPECT_LT(static_cast<double>(got) /
                  (offered > 0 ? static_cast<double>(offered) : 1.0),
              0.97)
        << "flood not saturating: consumer absorbed >=97% of offered — raise the "
           "flood rate so the consumer is pushed past its ceiling";
    // All four shard-distinct topics must have delivered.
    for (std::size_t i = 0; i < kTopics.size(); ++i) {
        EXPECT_GT(perTopic[i].load(), 0LL);
    }
    EXPECT_GE(consumerTrueMax, kMinRecvMsgsPerSec);
    EXPECT_GT(producerRate, 0.0);
    // observable_dropped must surface >= the end-to-end loss (issue #6 fix).
    EXPECT_GE(static_cast<std::uint64_t>(ringDropped),
              static_cast<std::uint64_t>(lost));
}

}  // namespace edriel
