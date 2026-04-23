#include <gtest/gtest.h>
#include <chrono>

// Timer-based cleaner tests
TEST(TestCleaner, TimeoutLogic) {
    constexpr auto timeoutPeriod = std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    auto validUntil = now - timeoutPeriod;
    auto expired = now - (timeoutPeriod + std::chrono::seconds(1));
    
    EXPECT_TRUE(expired < now);
    EXPECT_FALSE(validUntil > now);  // Should not be removed
    EXPECT_FALSE(expired > validUntil);  // expired is further in the past
}
