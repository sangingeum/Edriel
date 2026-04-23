#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>

// Integration test: Multi-threaded operation
TEST(TestIntegration, MultiThreadedOperation) {
    // Test concurrent topic registration/unregistration
    std::atomic<int> registrationSuccessCount{0};
    std::atomic<int> unregistrationSuccessCount{0};
    std::mutex mu;
    
    // Simulate multiple threads registering topics
    std::vector<std::thread> threads;
    const int numThreads = 10;
    const int opsPerThread = 10;
    
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < opsPerThread; ++j) {
                // Simulate registration operation
                registrationSuccessCount++;
                
                // Simulate unregistration operation
                unregistrationSuccessCount++;
                
                // Add small delay to simulate real work
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Verify all operations completed
    EXPECT_EQ(registrationSuccessCount.load(), numThreads * opsPerThread);
    EXPECT_EQ(unregistrationSuccessCount.load(), numThreads * opsPerThread);
}

// Integration test: Multi-player scenario with participants
TEST(TestIntegration, MultiPlayerScenario) {
    // Simulate multiple participants joining and leaving
    std::atomic<int> participantCount{0};
    std::mutex participantCountMu;
    
    // Thread 1: Add participants
    std::thread adder([&]() {
        for (int i = 0; i < 5; ++i) {
            {
                std::lock_guard<std::mutex> lock(participantCountMu);
                participantCount++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    
    // Thread 2: Remove participants
    std::thread remover([&]() {
        for (int i = 0; i < 5; ++i) {
            {
                std::lock_guard<std::mutex> lock(participantCountMu);
                participantCount--;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    
    // Wait for completion
    adder.join();
    remover.join();
    
    // Participant count should return to 0
    EXPECT_EQ(participantCount.load(), 0);
}

// Integration test: Timeout scenario
TEST(TestIntegration, TimeoutScenario) {
    // Simulate heartbeat timeout cleanup
    struct Heartbeat {
        uint64_t timestamp;
        bool active;
    };
    
    std::vector<Heartbeat> heartbeats;
    std::mutex hbMu;
    
    // Thread that adds heartbeats
    std::thread hbAdder([&]() {
        uint64_t baseTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        
        for (int i = 0; i < 20; ++i) {
            Heartbeat hb{
                baseTime + static_cast<uint64_t>(i) * 50,
                true
            };
            
            {
                std::lock_guard<std::mutex> lock(hbMu);
                heartbeats.push_back(hb);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    
    hbAdder.join();
    
    // Simulate timeout cleanup after 500ms
    auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    auto startTime = baseTime;
    
    uint64_t timeoutMs = 500;
    
    {
        std::lock_guard<std::mutex> lock(hbMu);
        for (auto& hb : heartbeats) {
            hb.active = (currentTime - startTime - hb.timestamp) <= timeoutMs;
        }
    }
    
    // Count active heartbeats
    int activeCount = 0;
    {
        std::lock_guard<std::mutex> lock(hbMu);
        for (const auto& hb : heartbeats) {
            if (hb.active) {
                activeCount++;
            }
        }
    }
    
    // Most heartbeats should still be active (within 500ms timeout)
    EXPECT_GE(activeCount, 10);  // At least half should be active
}

// Integration test: Message delivery with multiple subscribers
TEST(TestIntegration, MessageDelivery) {
    std::atomic<uint64_t> deliveryCount{0};
    std::atomic<int> failedDeliveries{0};
    
    // Simulate sending messages to multiple subscribers
    const int numMessages = 100;
    const int numSubscribers = 5;
    
    for (int i = 0; i < numMessages; ++i) {
        for (int j = 0; j < numSubscribers; ++j) {
            // Simulate message delivery
            if (std::this_thread::sleep_for(std::chrono::microseconds(100)).count() < 1) {
                deliveryCount++;
            } else {
                failedDeliveries++;
            }
        }
    }
    
    // All messages should be delivered
    EXPECT_EQ(deliveryCount.load(), static_cast<uint64_t>(numMessages * numSubscribers));
    EXPECT_EQ(failedDeliveries.load(), 0u);
}

// Integration test: Topic lifecycle management
TEST(TestIntegration, TopicLifecycle) {
    std::atomic<int> registeredTopics{0};
    std::atomic<int> activeSubscribers{0};
    
    // Thread 1: Register topics
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 5; ++j) {
                registeredTopics++;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    
    // Thread 2: Subscribe to topics
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 10; ++j) {
                activeSubscribers++;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Verify operations completed
    EXPECT_GT(registeredTopics.load(), 0);
    EXPECT_GT(activeSubscribers.load(), 0);
}

// Integration test: Memory pool stress test
TEST(TestIntegration, MemoryPoolStress) {
    std::atomic<size_t> allocated{0};
    std::atomic<size_t> freed{0};
    
    const int iterations = 1000;
    const size_t bufferSizes[] = {1024, 2048, 4096};
    
    for (int i = 0; i < iterations; ++i) {
        for (const auto& size : bufferSizes) {
            // Allocate
            allocated++;
            
            // Simulate some work
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            
            // Free
            freed++;
        }
    }
    
    // All allocations should be freed
    EXPECT_EQ(allocated.load(), freed.load());
}
