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
        guard.reset();
        if (runner.joinable()) {
            runner.join();
        }
        io.stop();
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
    const double sentRate = sent * 1000.0 / elapsedMs;
    const double recvRate = got * 1000.0 / elapsedMs;
    std::printf(
        "[bench] throughput (%zuB payload): sent=%lld (%.0f msgs/s)  "
        "received=%lld (%.0f msgs/s)\n",
        kPayloadBytes, static_cast<long long>(sent), sentRate,
        static_cast<long long>(got), recvRate);
    std::fflush(stdout);

    EXPECT_GT(sent, 0);
    EXPECT_GT(got, 0);
}

}  // namespace edriel
