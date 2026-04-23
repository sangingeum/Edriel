#include <gtest/gtest.h>
#include <chrono>

// Timer-based cleaner tests
TEST(TestCleaner, TimeoutLogic) {
    constexpr auto timeoutPeriod = std::chrono::seconds(10);
    constexpr auto now = std::chrono::steady_clock::now();
    constexpr auto validUntil = now - timeoutPeriod;
    constexpr auto expired = now - (timeoutPeriod + std::chrono::seconds(1));
    
    EXPECT_TRUE(expired < now);
    EXPECT_FALSE(validUntil > now);  // Should not be removed
    EXPECT_TRUE(expired > validUntil);
}
