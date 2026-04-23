/**
 * @file memory_pool.cpp
 * @brief Memory pool implementation for Edriel C++20 multi-cast auto-discovery
 * 
 * Provides shared buffer pool for message recycling to reduce heap allocations.
 * 
 * @version 1.0.0
 */

#include "memory_pool.hpp"
#include <algorithm>

namespace edriel {

/**
 * @brief Constructs MessageBufferPool with specified configuration
 */
MessageBufferPool::MessageBufferPool(
    std::size_t maxMessageSize,
    std::size_t poolCapacity,
    std::size_t minMessageSize
)
    : maxMessageSize_(maxMessageSize)
    , minMessageSize_(minMessageSize)
    , initialCapacity_(poolCapacity)
{
    // Pre-allocate initial pool capacity
    for (std::size_t i = 0; i < initialCapacity_; ++i) {
        buffers_.push_back(std::make_unique<std::vector<char>>(maxMessageSize_));
        available_.push(std::move(buffers_.back()));
    }
}

/**
 * @brief Acquires an available buffer
 * 
 * Returns a pointer to a reusable buffer. Buffer is tracked in available_ queue.
 * @return Pointer to available buffer, or nullptr if none available
 */
char* MessageBufferPool::acquireBuffer() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Try to get from available queue
    if (!available_.empty()) {
        auto buffer = std::move(available_.front());
        available_.pop();
        
        // Return pointer to buffer data
        return buffer->data();
    }
    
    // No available buffers - pre-allocate more if within max size
    if (buffers_.size() < maxMessageSize_ / minMessageSize_ && 
        buffers_.size() > 0) {
        auto newBuffer = std::unique_ptr<std::vector<char>>(
            new std::vector<char>(minMessageSize_)
        );
        buffers_.push_back(std::move(newBuffer));
        
        // Return pointer to buffer data
        return buffers_.back()->data();
    }
    
    // Pool exhausted
    return nullptr;
}

/**
 * @brief Releases a buffer back to the pool
 */
void MessageBufferPool::releaseBuffer(char* buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Find the buffer index
    auto it = std::find_if(buffers_.begin(), buffers_.end(),
        [buffer](const std::unique_ptr<std::vector<char>>& buf) {
            if (!buf) return false;
            return buf->data() == buffer;
        });
    
    // If found, return to available queue
    if (it != buffers_.end()) {
        auto bufferVector = std::move(*it);
        buffers_.erase(it);
        available_.push(std::move(bufferVector));
    }
}

/**
 * @brief Pre-allocates additional buffers
 */
void MessageBufferPool::preAllocate(std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Don't exceed maximum capacity
    std::size_t availableSize = maxMessageSize_ / minMessageSize_;
    std::size_t currentAllocated = buffers_.size() + available_.size();
    
    std::size_t toAllocate = std::min(count, availableSize - currentAllocated);
    
    for (std::size_t i = 0; i < toAllocate; ++i) {
        buffers_.push_back(std::make_unique<std::vector<char>>(maxMessageSize_));
        available_.push(std::move(buffers_.back()));
    }
}

/**
 * @brief Gets pool statistics
 */
PoolStats MessageBufferPool::getStats() const {
    PoolStats stats{};
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        stats.totalAllocated = maxMessageSize_ * buffers_.size();
        stats.availableBuffers = available_.size();
        stats.usedBuffers = buffers_.size() - available_.size();
        stats.maxAllocated = stats.totalAllocated;
    }
    
    return stats;
}

/**
 * @brief Gets available buffer count
 */
std::size_t MessageBufferPool::getAvailableCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_.size();
}

/**
 * @brief Factory function to create default buffer pool
 */
MessageBufferPool createDefaultMessagePool() {
    return MessageBufferPool(DEFAULT_MAX_MESSAGE_SIZE, DEFAULT_POOL_CAPACITY, DEFAULT_MIN_MESSAGE_SIZE);
}

} // namespace edriel
