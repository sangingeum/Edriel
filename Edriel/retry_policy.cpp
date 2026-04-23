/**
 * @file retry_policy.cpp
 * @brief Retry policy implementation for Edriel C++20 multi-cast auto-discovery
 * 
 * Provides configurable retry logic for network failures.
 * 
 * @version 1.0.0
 */

#include "retry_policy.hpp"
#include <cmath>
#include <algorithm>

namespace edriel {

/**
 * @brief Constructs RetryPolicy with default configuration
 */
RetryPolicy::RetryPolicy()
    : strategy_(RetryStrategy::EXPO_BACKOFF)
    , maxAttempts_(3)
    , baseDelaySeconds_(2)
    , minDelaySeconds_(1)
    , maxDelaySeconds_(60)
    , timeout_(std::chrono::seconds(5))
    , enabled_(true)
{
}

/**
 * @brief Constructs RetryPolicy with custom configuration
 */
RetryPolicy::RetryPolicy(
    RetryStrategy strategy,
    uint32_t maxAttempts,
    uint32_t baseDelaySeconds,
    uint32_t minDelaySeconds,
    uint32_t maxDelaySeconds,
    std::chrono::seconds timeout
)
    : strategy_(strategy)
    , maxAttempts_(maxAttempts)
    , baseDelaySeconds_(baseDelaySeconds)
    , minDelaySeconds_(minDelaySeconds)
    , maxDelaySeconds_(maxDelaySeconds)
    , timeout_(timeout)
    , enabled_(true)
{
}

/**
 * @brief Sets retry strategy
 */
void RetryPolicy::setStrategy(RetryStrategy strategy) {
    strategy_ = strategy;
}

/**
 * @brief Sets maximum retry attempts
 */
void RetryPolicy::setMaxAttempts(uint32_t attempts) {
    maxAttempts_ = attempts;
}

/**
 * @brief Sets base delay interval
 */
void RetryPolicy::setBaseDelaySeconds(uint32_t seconds) {
    baseDelaySeconds_ = seconds;
}

/**
 * @brief Sets minimum delay
 */
void RetryPolicy::setMinDelaySeconds(uint32_t seconds) {
    minDelaySeconds_ = seconds;
}

/**
 * @brief Sets maximum delay cap
 */
void RetryPolicy::setMaxDelaySeconds(uint32_t seconds) {
    maxDelaySeconds_ = seconds;
}

/**
 * @brief Sets total timeout
 */
void RetryPolicy::setTimeout(std::chrono::seconds timeout) {
    timeout_ = timeout;
}

/**
 * @brief Enables/disables retry
 */
void RetryPolicy::setEnabled(bool enabled) {
    enabled_ = enabled;
}

/**
 * @brief Gets current retry strategy
 */
RetryStrategy RetryPolicy::getStrategy() const {
    return strategy_;
}

/**
 * @brief Gets maximum attempts
 */
uint32_t RetryPolicy::getMaxAttempts() const {
    return maxAttempts_;
}

/**
 * @brief Gets base delay
 */
uint32_t RetryPolicy::getBaseDelaySeconds() const {
    return baseDelaySeconds_;
}

/**
 * @brief Gets minimum delay
 */
uint32_t RetryPolicy::getMinDelaySeconds() const {
    return minDelaySeconds_;
}

/**
 * @brief Gets maximum delay
 */
uint32_t RetryPolicy::getMaxDelaySeconds() const {
    return maxDelaySeconds_;
}

/**
 * @brief Gets total timeout
 */
std::chrono::seconds RetryPolicy::getTimeout() const {
    return timeout_;
}

/**
 * @brief Checks if retry is enabled
 */
bool RetryPolicy::isEnabled() const {
    return enabled_;
}

/**
 * @brief Calculates delay before next retry
 * 
 * Implements different delay strategies:
 * - EXPO_BACKOFF: delay = minDelay * 2^attempt
 * - FIXED_INTERVAL: delay = baseDelay
 * - FIBONACCI: delay = F_{attempt+1} * minDelay
 * - LINEAR: delay = (attempt + 1) * baseDelay
 * - IMMEDIATE: delay = 0
 */
std::chrono::milliseconds RetryPolicy::calculateDelay(uint32_t attempt) {
    if (!enabled_) {
        return std::chrono::milliseconds(0);
    }

    switch (strategy_) {
        case RetryStrategy::IMMEDIATE:
            return std::chrono::milliseconds(0);
            
        case RetryStrategy::FIXED_INTERVAL:
            return std::chrono::milliseconds(
                std::min(
                    baseDelaySeconds_ * 1000, 
                    maxDelaySeconds_ * 1000
                )
            );
            
        case RetryStrategy::LINEAR:
            return std::chrono::milliseconds(
                std::min(
                    (attempt + 1) * baseDelaySeconds_ * 1000,
                    maxDelaySeconds_ * 1000
                )
            );
            
        case RetryStrategy::EXPO_BACKOFF: {
            uint32_t delay = minDelaySeconds_ * std::pow(2.0, attempt);
            return std::chrono::milliseconds(
                std::min(static_cast<uint32_t>(delay * 1000), maxDelaySeconds_ * 1000)
            );
        }
            
        case RetryStrategy::FIBONACCI: {
            // Fibonacci sequence: 1, 1, 2, 3, 5, 8, 13, 21, 34, 55...
            std::vector<uint32_t> fibonacci = {1, 1};
            if (attempt > 1) {
                for (uint32_t i = 2; i <= attempt; ++i) {
                    fibonacci.push_back(fibonacci[i-1] + fibonacci[i-2]);
                }
            }
            uint32_t index = std::min(static_cast<uint32_t>(attempt), static_cast<uint32_t>(fibonacci.size() - 1));
            return std::chrono::milliseconds(
                std::min(
                    fibonacci[index] * minDelaySeconds_ * 1000,
                    maxDelaySeconds_ * 1000
                )
            );
        }
            
        default:
            return std::chrono::milliseconds(0);
    }
}

/**
 * @brief Determines if operation should be retried
 */
bool RetryPolicy::shouldRetry(const RetryableResult& result) {
    if (!enabled_) {
        return false;
    }
    
    if (result.attempts >= maxAttempts_) {
        return false;
    }
    
    if (result.errorCode == ErrorCode::SUCCESS) {
        return false;  // Already succeeded
    }
    
    // Check timeout
    return true;
}

/**
 * @brief Gets error message for failed operation
 */
std::string RetryPolicy::getErrorMessage(uint32_t attempts) {
    switch (strategy_) {
        case RetryStrategy::EXPO_BACKOFF:
            return "Exponential backoff strategy exceeded maximum attempts";
            
        case RetryStrategy::FIXED_INTERVAL:
            return "Fixed interval retry strategy exceeded maximum attempts";
            
        case RetryStrategy::FIBONACCI:
            return "Fibonacci backoff strategy exceeded maximum attempts";
            
        case RetryStrategy::LINEAR:
            return "Linear retry strategy exceeded maximum attempts";
            
        case RetryStrategy::IMMEDIATE:
            return "Immediate retry strategy failed after maximum attempts";
            
        default:
            return "Unknown retry strategy error";
    }
}

/**
 * @brief Default retry policy factory function
 */
RetryPolicy makeDefaultRetryPolicy() {
    return RetryPolicy(
        RetryStrategy::EXPO_BACKOFF,
        3,
        2,
        1,
        60,
        std::chrono::seconds(5)
    );
}

} // namespace edriel
