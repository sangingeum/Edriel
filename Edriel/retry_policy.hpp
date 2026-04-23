/**
 * @file retry_policy.hpp
 * @brief Retry policy implementation for Edriel C++20 multi-cast auto-discovery
 * 
 * Provides configurable retry logic for network failures to improve
 * reliability under transient conditions.
 * 
 * @version 1.0.0
 */

#pragma once

#include <chrono>
#include <string>
#include <optional>
#include "error_codes.hpp"

namespace edriel {

/**
 * @brief Retry strategy for network operations
 * 
 * Supports multiple retry strategies:
 * - Exponential backoff
 * - Fixed interval
 * - Fibonacci backoff
 * - Linear increment
 */
enum class RetryStrategy {
    EXPO_BACKOFF,      ///< Exponential backoff (1s, 2s, 4s, 8s...)
    FIXED_INTERVAL,    ///< Fixed delay between retries
    FIBONACCI,         ///< Fibonacci backoff
    LINEAR,            ///< Linear increment (1s, 2s, 3s, 4s...)
    IMMEDIATE          ///< No delay (retry immediately)
};

/**
 * @brief Retry policy configuration (data-only)
 */
struct RetryPolicyConfig {
    RetryStrategy strategy{RetryStrategy::EXPO_BACKOFF};  ///< Retry strategy
    uint32_t maxAttempts{3};                               ///< Maximum retry attempts
    uint32_t baseDelaySeconds{2};                          ///< Base delay in seconds (used for linear/fixed)
    uint32_t minDelaySeconds{1};                           ///< Minimum delay (exponential/fibonacci)
    uint32_t maxDelaySeconds{60};                          ///< Maximum delay cap
    std::chrono::seconds timeout{5};                       ///< Total timeout for retries
    bool enabled{true};                                    ///< Whether retry is enabled
};

/**
 * @brief Retryable operation result
 */
struct RetryableResult {
    bool success{false};         ///< Operation succeeded
    ErrorCode errorCode{ErrorCode::SUCCESS};  ///< Error code if failed
    uint32_t attempts{0};        ///< Number of attempts made
    std::string errorMessage;    ///< Error message
};

/**
 * @brief Retry policy for network operations
 */
class RetryPolicy {
public:
    /**
     * @brief Constructs RetryPolicy with default configuration
     */
    RetryPolicy();

    /**
     * @brief Constructs RetryPolicy with custom configuration
     * @param strategy Retry strategy to use
     * @param maxAttempts Maximum retry attempts
     * @param baseDelaySeconds Base delay in seconds
     * @param minDelaySeconds Minimum delay
     * @param maxDelaySeconds Maximum delay cap
     * @param timeout Total timeout for retries
     */
    RetryPolicy(
        RetryStrategy strategy = RetryStrategy::EXPO_BACKOFF,
        uint32_t maxAttempts = 3,
        uint32_t baseDelaySeconds = 2,
        uint32_t minDelaySeconds = 1,
        uint32_t maxDelaySeconds = 60,
        std::chrono::seconds timeout = std::chrono::seconds(5)
    );

    /**
     * @brief Sets retry strategy
     * @param strategy Retry strategy to use
     */
    void setStrategy(RetryStrategy strategy);

    /**
     * @brief Sets maximum retry attempts
     * @param attempts Maximum attempts
     */
    void setMaxAttempts(uint32_t attempts);

    /**
     * @brief Sets base delay interval
     * @param seconds Base delay in seconds
     */
    void setBaseDelaySeconds(uint32_t seconds);

    /**
     * @brief Sets minimum delay
     * @param seconds Minimum delay
     */
    void setMinDelaySeconds(uint32_t seconds);

    /**
     * @brief Sets maximum delay cap
     * @param seconds Maximum delay
     */
    void setMaxDelaySeconds(uint32_t seconds);

    /**
     * @brief Sets total timeout
     * @param timeout Timeout duration
     */
    void setTimeout(std::chrono::seconds timeout);

    /**
     * @brief Enables/disables retry
     * @param enabled Whether retry is enabled
     */
    void setEnabled(bool enabled);

    /**
     * @brief Gets current retry strategy
     * @return Current strategy
     */
    RetryStrategy getStrategy() const;

    /**
     * @brief Gets maximum attempts
     * @return Maximum attempts
     */
    uint32_t getMaxAttempts() const;

    /**
     * @brief Gets base delay
     * @return Base delay in seconds
     */
    uint32_t getBaseDelaySeconds() const;

    /**
     * @brief Gets minimum delay
     * @return Minimum delay in seconds
     */
    uint32_t getMinDelaySeconds() const;

    /**
     * @brief Gets maximum delay
     * @return Maximum delay in seconds
     */
    uint32_t getMaxDelaySeconds() const;

    /**
     * @brief Gets total timeout
     * @return Timeout duration
     */
    std::chrono::seconds getTimeout() const;

    /**
     * @brief Checks if retry is enabled
     * @return True if retry is enabled
     */
    bool isEnabled() const;

    /**
     * @brief Calculates delay before next retry
     * @param attempt Current attempt number (0-indexed)
     * @return Delay duration in milliseconds
     */
    std::chrono::milliseconds calculateDelay(uint32_t attempt);

    /**
     * @brief Determines if operation should be retried
     * @param result Previous operation result
     * @return True if should retry, false if should give up
     */
    bool shouldRetry(const RetryableResult& result);

    /**
     * @brief Gets error message for failed operation
     * @return Error message
     */
    std::string getErrorMessage(uint32_t attempts);

private:
    RetryStrategy strategy_;
    uint32_t maxAttempts_;
    uint32_t baseDelaySeconds_;
    uint32_t minDelaySeconds_;
    uint32_t maxDelaySeconds_;
    std::chrono::seconds timeout_;
    bool enabled_;
};

/**
 * @brief Default retry policy factory function
 */
RetryPolicy makeDefaultRetryPolicy();

} // namespace edriel
