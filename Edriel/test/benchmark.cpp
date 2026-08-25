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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
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
        "received=%lld (%.0f msgs/s, %.0f B/s)  lost=%lld (%.2f%%)\n",
        kPayloadBytes, static_cast<long long>(sent), sentRate, sentByteRate,
        static_cast<long long>(got), recvRate, recvByteRate,
        static_cast<long long>(lost), lostPct);
    std::fflush(stdout);

    EXPECT_GT(sent, 0);
    EXPECT_GT(got, 0);
}

// ----------------------------------------------------------------------------
// Two-node RECEIVE-ONLY throughput benchmark: dedicated producer + dedicated
// consumer on DECOUPLED io_contexts (each its own thread).
//
// This is the ADR-003 "before" for the real receive ceiling. The single-node
// ThroughputMsgsPerSecond collapses producer+consumer onto ONE io_context
// thread, so the send side saturates the strand and starves the receive drain
// (measured ~1 msgs/s, 100% silent kernel drop). Here the producer publishes
// from its own io_context/thread and the consumer drains from its own, so the
// consumer's receive path never contends with the send side. The number this
// produces is the trustworthy receive ceiling ADR-003's >=1.5x gate builds
// against.
// ----------------------------------------------------------------------------

TEST(Benchmark, TwoNodeReceiveThroughput) {
    constexpr std::size_t kPayloadBytes = 256;
    // Distinct topic so a stray single-node subscriber would not overlap.
    const std::string kTopic = "throughput_2node";
    // Publish a FIXED, bounded set of datagrams as fast as the producer's io
    // thread will drain. A fixed count (rather than an unbounded time-window
    // flood) keeps the producer's stranded async_send queue bounded, so every
    // published datagram is delivered-or-dropped within the drain window and
    // teardown stays fast. The count is chosen to outrun the consumer's
    // single-threaded receive on loopback so the consumer saturates and reveals
    // its true receive ceiling.
    constexpr std::int64_t kPublishCount = 500000;

    // Two independent Edriel nodes, each owning its own io_context/thread.
    // NEVER share one io_context between them (that reproduces the starvation
    // artifact the ADR-003 baseline measured).
    BenchNode producer(3, 0, 3);
    BenchNode consumer(4, 0, 4);

    std::atomic<int64_t> receivedCount{0};
    ASSERT_TRUE(consumer.node->registerSubscriberTopic<Ping>(
        kTopic,
        [&](const Ping&) { receivedCount.fetch_add(1, std::memory_order_relaxed); }));

    // Settle: allow both nodes to join the multicast group / arm their
    // receivers before the producer starts.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    Ping msg;
    msg.set_seq(0);
    msg.mutable_blob()->resize(kPayloadBytes, 'x');

    const auto start = Clock::now();
    std::int64_t sent = 0;
    for (; sent < kPublishCount; ++sent) {
        msg.set_seq(sent + 1);
        if (!producer.node->sendMessage(kTopic, msg)) {
            break;  // dispatch refused (MTU/serialize); stop cleanly
        }
    }

    // Wait for the producer's strand to drain every send and the consumer to
    // deliver everything it is going to. Poll until the consumer counter goes
    // quiet (no new deliveries for ~500ms) or a hard ceiling is hit.
    std::int64_t lastGot = -1;
    int quietMs = 0;
    for (int i = 0; i < 4000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const std::int64_t g = receivedCount.load(std::memory_order_relaxed);
        if (g == lastGot) {
            quietMs += 2;
            if (quietMs >= 500) {
                break;  // consumer drain is done
            }
        } else {
            lastGot = g;
            quietMs = 0;
        }
    }
    // After the consumer has gone quiet, wait briefly more so the producer's
    // io thread fully drains its queue while its socket is still open (keeps
    // the BenchNode teardown from blocking on a mid-queue shutdown).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto end = Clock::now();

    const int64_t got = receivedCount.load();
    const int64_t lost = sent - got;
    const double lostPct =
        (sent > 0) ? 100.0 * static_cast<double>(lost)
                               / static_cast<double>(sent)
                   : 0.0;
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    const double elapsedS =
        std::chrono::duration<double>(end - start).count();
    // Each sendMessage() carries one multicast datagram. The receive ceiling is
    // delivered-datagrams / the true drain span (start -> quiesce).
    const double sentRate = sent * 1000.0 / elapsedMs;
    const double recvRate = (elapsedS > 0.0) ? got / elapsedS : 0.0;
    const double sentByteRate = sentRate * static_cast<double>(kPayloadBytes);
    const double recvByteRate = recvRate * static_cast<double>(kPayloadBytes);
    std::printf(
        "[bench] two-node receive-only (%zuB payload, %lld published): "
        "producer %.0f msgs/s (%.0f B/s)  consumer received=%lld (%.0f "
        "msgs/s, %.0f B/s)  lost=%lld (%.2f%%)\n",
        kPayloadBytes, static_cast<long long>(sent),
        sentRate, sentByteRate,
        static_cast<long long>(got), recvRate, recvByteRate,
        static_cast<long long>(lost), lostPct);
    std::fflush(stdout);

    // Real assertion, not a smoke check: the decoupled consumer must receive a
    // meaningful fraction of what the producer sent — the single-node artifact
    // collapses to ~1 msgs/s and 0.00% delivery. Keep the floor well above that
    // artifact but comfortably checkable on an idle loopback host.
    EXPECT_GT(sent, 0);
    EXPECT_GE(got, 2000LL);
    EXPECT_GT(recvRate, 1000.0);          // >> 1 msgs/s single-node artifact
    EXPECT_LT(lostPct, 99.99);            // not the 100%-silent-drop case
}

}  // namespace edriel
