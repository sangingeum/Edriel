/**
 * @file memory_pool.hpp
 * @brief Memory pool implementation for Edriel C++20 multi-cast auto-discovery
 * 
 * Provides shared buffer pool for message recycling to reduce heap allocations.
 * 
 * @version 1.0.0
 */

#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <memory>
#include <cstdint>

namespace edriel {

/**
 * @brief Maximum message size for buffer pool
 * 
 * Configurable limit for message buffer sizes.
 */
constexpr std::size_t DEFAULT_MAX_MESSAGE_SIZE = 16384;  ///< 16KB default

/**
 * @brief Minimum message size for buffer pool
 */
constexpr std::size_t DEFAULT_MIN_MESSAGE_SIZE = 512;     ///< 512 bytes minimum

/**
 * @brief Initial buffer pool capacity
 */
constexpr std::size_t DEFAULT_POOL_CAPACITY = 1024;       ///< 1024 buffers initially

/**
 * @brief Pool statistics
 */
struct PoolStats {
    std::size_t totalAllocated{0};      ///< Total bytes allocated
    std::size_t availableBuffers{0};    ///< Currently available buffers
    std::size_t usedBuffers{0};         ///< Currently used buffers
    std::size_t recycledCount{0};       ///< Number of times buffers recycled
    std::size_t maxAllocated{0};        ///< Peak allocation
    std::size_t totalRecycled{0};       ///< Total recyclings over lifetime
};

/**
 * @brief Simple memory pool for message buffers
 * 
 * Pre-allocates a pool of fixed-size buffers to avoid repeated
 * heap allocations during high-frequency message processing.
 * 
 * Thread-safe with mutex protection.
 */
class MessageBufferPool {
public:
    /**
     * @brief Constructs MessageBufferPool with specified configuration
     * @param maxMessageSize Maximum message size to support
     * @param poolCapacity Initial number of buffers to pre-allocate
     * @param minMessageSize Minimum message size to support
     */
    MessageBufferPool(
        std::size_t maxMessageSize = DEFAULT_MAX_MESSAGE_SIZE,
        std::size_t poolCapacity = DEFAULT_POOL_CAPACITY,
        std::size_t minMessageSize = DEFAULT_MIN_MESSAGE_SIZE
    );

    /**
     * @brief Destroys MessageBufferPool and releases all buffers
     */
    ~MessageBufferPool() = default;

    // Disable copy
    MessageBufferPool(const MessageBufferPool&) = delete;
    MessageBufferPool& operator=(const MessageBufferPool&) = delete;

    // Enable move
    MessageBufferPool(MessageBufferPool&&) noexcept = default;
    MessageBufferPool& operator=(MessageBufferPool&&) noexcept = default;

    /**
     * @brief Acquires an available buffer
     * @return Pointer to available buffer, or nullptr if none available
     */
    char* acquireBuffer();

    /**
     * @brief Releases a buffer back to the pool
     * @param buffer Pointer to buffer to release
     */
    void releaseBuffer(char* buffer);

    /**
     * @brief Pre-allocates additional buffers
     * @param count Number of buffers to pre-allocate
     */
    void preAllocate(std::size_t count);

    /**
     * @brief Gets pool statistics
     * @return Current pool statistics
     */
    PoolStats getStats() const;

    /**
     * @brief Gets maximum message size
     * @return Maximum message size
     */
    std::size_t getMaxMessageSize() const { return maxMessageSize_; }

    /**
     * @brief Gets minimum message size
     * @return Minimum message size
     */
    std::size_t getMinMessageSize() const { return minMessageSize_; }

    /**
     * @brief Gets current buffer capacity
     * @return Number of buffers available
     */
    std::size_t getCapacity() const { return buffers_.size(); }

    /**
     * @brief Gets available (unused) buffer count
     * @return Count of unused buffers
     */
    std::size_t getAvailableCount() const;

private:
    std::size_t maxMessageSize_;
    std::size_t minMessageSize_;
    std::size_t initialCapacity_;
    std::vector<std::unique_ptr<std::vector<char>>> buffers_;  ///< Pool of buffers
    std::queue<std::unique_ptr<std::vector<char>>> available_;  ///< Available buffers queue
    mutable std::mutex mutex_;
};

/**
 * @brief Factory function to create default buffer pool
 */
MessageBufferPool createDefaultMessagePool();

} // namespace edriel
