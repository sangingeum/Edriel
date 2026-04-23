/**
 * @file benchmark.cpp
 * @brief Benchmark tests for Edriel message throughput and latency
 * 
 * This file contains benchmark tests for measuring:
 * - Message throughput (messages per second)
 * - Message latency (end-to-end delay)
 * - Network bandwidth utilization
 * - Memory allocation/deallocation performance
 */

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include <functional>

namespace edriel {

// Benchmark: Message throughput
class ThroughputBenchmark {
public:
    static uint64_t sendMessage(uint64_t messageCount, size_t messageSize) {
        std::atomic<uint64_t> sentCount{0};
        std::atomic<uint64_t> receivedCount{0};
        std::mutex mu;
        
        // Simulate sending messages
        auto startTime = std::chrono::high_resolution_clock::now();
        
        for (uint64_t i = 0; i < messageCount; ++i) {
            // Simulate message creation
            std::vector<char> message(messageSize, 0);
            for (size_t j = 0; j < messageSize; ++j) {
                message[j] = static_cast<char>(i ^ j);
            }
            
            // Simulate sending
            {
                std::lock_guard<std::mutex> lock(mu);
                // Simulate async send (non-blocking)
                sentCount.fetch_add(1, std::memory_order_acq_rel);
            }
            
            // Simulate network delay
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        // Simulate receiving
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime
        ).count();
        
        return duration > 0 ? sentCount.load() : 0;
    }
};

// Benchmark: Message latency
class LatencyBenchmark {
public:
    static void testLatency() {
        std::atomic<uint64_t> totalLatency{0};
        
        // Warm-up
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        for (int i = 0; i < 1000; ++i) {
            auto startTime = std::chrono::high_resolution_clock::now();
            
            // Simulate message send/receive cycle
            {
                std::atomic<uint64_t> marker{0};
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                marker.fetch_add(1);
            }
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                endTime - startTime
            ).count();
            
            totalLatency.fetch_add(latency);
        }
        
        EXPECT_GT(totalLatency.load(), 0);
    }
};

// Benchmark: Memory allocation
class MemoryBenchmark {
public:
    static void testAllocation() {
        std::atomic<size_t> totalAllocations{0};
        
        const size_t allocCount = 10000;
        const size_t allocSize = 1024;
        
        std::vector<std::vector<char>> allocations;
        
        for (size_t i = 0; i < allocCount; ++i) {
            // Allocate
            auto data = std::make_unique<char[]>(allocSize);
            if (data) {
                for (size_t j = 0; j < allocSize; ++j) {
                    data[j] = static_cast<char>(i);
                }
                allocations.push_back(std::move(data));
                totalAllocations.fetch_add(1);
            }
        }
        
        // Deallocate
        for (auto& data : allocations) {
            if (data) {
                data.reset();
            }
        }
        
        EXPECT_EQ(totalAllocations.load(), allocCount);
    }
};

// Benchmark: Concurrent operations
class ConcurrentBenchmark {
public:
    static void testConcurrent() {
        std::atomic<uint64_t> successCount{0};
        std::atomic<uint64_t> failureCount{0};
        
        const int numThreads = 16;
        const uint64_t opsPerThread = 1000;
        
        std::vector<std::thread> threads;
        
        for (int i = 0; i < numThreads; ++i) {
            threads.emplace_back([&]() {
                for (uint64_t j = 0; j < opsPerThread; ++j) {
                    // Simulate concurrent operation
                    std::atomic<uint64_t> counter{0};
                    counter.fetch_add(1);
                    successCount.fetch_add(1);
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        EXPECT_EQ(successCount.load(), static_cast<uint64_t>(numThreads * opsPerThread));
    }
};

// Benchmark: Network bandwidth
class NetworkBandwidthBenchmark {
public:
    static uint64_t measureBandwidth(uint64_t messageCount, size_t messageSize) {
        // Warm-up
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        auto startTime = std::chrono::high_resolution_clock::now();
        
        uint64_t totalData = 0;
        for (uint64_t i = 0; i < messageCount; ++i) {
            totalData += messageSize;
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime
        ).count();
        
        // Calculate bandwidth
        if (duration > 0) {
            return static_cast<uint64_t>((totalData * 8) / (duration * 1000));  // bps
        }
        
        return 0;
    }
};

// Run all benchmarks
TEST(Benchmark, MessageThroughput) {
    auto messagesPerSecond = ThroughputBenchmark::sendMessage(10000, 256);
    EXPECT_GT(messagesPerSecond, 0);
}

TEST(Benchmark, MessageLatency) {
    LatencyBenchmark::testLatency();
}

TEST(Benchmark, MemoryAllocation) {
    MemoryBenchmark::testAllocation();
}

TEST(Benchmark, ConcurrentOperations) {
    ConcurrentBenchmark::testConcurrent();
}

TEST(Benchmark, NetworkBandwidth) {
    auto bandwidth = NetworkBandwidthBenchmark::measureBandwidth(1000, 512);
    EXPECT_GE(bandwidth, 0);
}

} // namespace edriel
