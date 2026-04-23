/**
 * @file topic_manager.cpp
 * @brief Topic management implementation for Edriel C++20 multi-cast auto-discovery
 * 
 * Provides efficient topic-based message routing with hash indexing and LRU caching.
 * 
 * @version 1.0.0
 */

#include "topic_manager.hpp"
#include <chrono>
#include <mutex>
#include <algorithm>

namespace edriel {

// ============================================================================
// TopicManager Implementation
// ============================================================================

/**
 * @brief Constructs TopicManager with specified cache size
 * @param cacheSize LRU cache capacity (default 1024 entries)
 */
TopicManager::TopicManager(std::size_t cacheSize)
    : cacheSize_(cacheSize)
{
}

// ============================================================================
// Public API: Topic Registration
// ============================================================================

/**
 * @brief Registers a topic for publishing
 * 
 * Adds a topic to the registry as a publisher. Uses hash-based indexing
 * for O(1) lookup and caching.
 * 
 * @param topicName Topic name to register
 * @param messageType Message type (e.g., "update", "heartbeat")
 * @return true if registration succeeded, false if topic already exists
 */
bool TopicManager::registerPublisherTopic(const std::string& topicName, const std::string& messageType) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    std::string topicKey = getTopicKey(topicName, messageType);
    
    // Check if topic already registered
    if (topicRegistry_.find(topicKey) != topicRegistry_.end()) {
        // Topic exists but might be subscriber - convert to publisher
        topicRegistry_[topicKey].type = TopicSubscription::Type::PUBLISHER;
    } else {
        // New topic
        topicRegistry_[topicKey] = TopicSubscription(TopicSubscription::Type::PUBLISHER);
        
        // Cache the topic entry
        TopicCacheEntry entry(topicKey, getCurrentTimestamp(), MAGIC_NUMBER, 0);
        cacheEntry(topicKey, entry);
    }
    
    return true;
}

/**
 * @brief Registers a topic for subscribing
 * 
 * Adds a topic to the registry as a subscriber.
 * 
 * @param topicName Topic name to register
 * @param messageType Message type
 * @return true if registration succeeded, false if topic doesn't exist
 */
bool TopicManager::registerSubscriberTopic(const std::string& topicName, const std::string& messageType) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    std::string topicKey = getTopicKey(topicName, messageType);
    
    // Check if topic exists
    if (topicRegistry_.find(topicKey) == topicRegistry_.end()) {
        // Topic doesn't exist, return false
        return false;
    }
    
    // Mark as subscriber
    topicRegistry_[topicKey].type = TopicSubscription::Type::SUBSCRIBER;
    
    // Cache entry
    TopicCacheEntry entry(topicKey, getCurrentTimestamp(), MAGIC_NUMBER, 0);
    cacheEntry(topicKey, entry);
    
    return true;
}

/**
 * @brief Unregisters a topic for publishing
 * 
 * Removes a topic from the publisher registry.
 * 
 * @param topicName Topic name to unregister
 * @param messageType Message type
 * @return true if unregistration succeeded, false if topic not found
 */
bool TopicManager::unregisterPublisherTopic(const std::string& topicName, const std::string& messageType) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    std::string topicKey = getTopicKey(topicName, messageType);
    
    // Check if topic exists as publisher
    auto it = topicRegistry_.find(topicKey);
    if (it != topicRegistry_.end() && it->second.type == TopicSubscription::Type::PUBLISHER) {
        // Remove from registry
        topicRegistry_.erase(it);
        
        // Remove from cache if it was cached
        if (cacheAccessOrder_.find(topicKey) != cacheAccessOrder_.end()) {
            // Find and remove from cache
            auto cacheIt = topicCache_.find(topicKey);
            if (cacheIt != topicCache_.end()) {
                topicCache_.erase(cacheIt);
            }
            cacheAccessOrder_.erase(topicKey);
        }
        
        return true;
    }
    
    return false;
}

/**
 * @brief Unregisters a topic for subscribing
 * 
 * Removes a topic from the subscriber registry.
 * 
 * @param topicName Topic name to unregister
 * @param messageType Message type
 * @return true if unregistration succeeded, false if topic not found
 */
bool TopicManager::unregisterSubscriberTopic(const std::string& topicName, const std::string& messageType) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    std::string topicKey = getTopicKey(topicName, messageType);
    
    // Check if topic exists as subscriber
    auto it = topicRegistry_.find(topicKey);
    if (it != topicRegistry_.end() && it->second.type == TopicSubscription::Type::SUBSCRIBER) {
        // Remove from registry
        topicRegistry_.erase(it);
        
        // Remove from cache
        if (cacheAccessOrder_.find(topicKey) != cacheAccessOrder_.end()) {
            auto cacheIt = topicCache_.find(topicKey);
            if (cacheIt != topicCache_.end()) {
                topicCache_.erase(cacheIt);
            }
            cacheAccessOrder_.erase(topicKey);
        }
        
        return true;
    }
    
    return false;
}

// ============================================================================
// Public API: Message Routing
// ============================================================================

/**
 * @brief Looks up subscribers for a topic
 * 
 * Retrieves the set of participant IDs that have subscribed to the topic.
 * 
 * @param topicName Topic name
 * @param messageType Message type
 * @param subscribers Output parameter for subscriber set
 * @return true if topic exists and has subscribers
 */
bool TopicManager::getSubscribers(const std::string& topicName, const std::string& messageType,
                                  std::unordered_set<unsigned long>& subscribers) const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    std::string topicKey = getTopicKey(topicName, messageType);
    
    auto it = topicRegistry_.find(topicKey);
    if (it == topicRegistry_.end() || it->second.type != TopicSubscription::Type::SUBSCRIBER) {
        return false;
    }
    
    // TODO: Populate subscribers from cached data
    // For now, return empty set - actual implementation would track subscriber IDs per topic
    return false;
}

/**
 * @brief Looks up publishers for a topic
 * 
 * Retrieves the set of participant IDs that are publishing to the topic.
 * 
 * @param topicName Topic name
 * @param messageType Message type
 * @param publishers Output parameter for publisher set
 * @return true if topic exists and has publishers
 */
bool TopicManager::getPublishers(const std::string& topicName, const std::string& messageType,
                                 std::unordered_set<unsigned long>& publishers) const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    std::string topicKey = getTopicKey(topicName, messageType);
    
    auto it = topicRegistry_.find(topicKey);
    if (it == topicRegistry_.end() || it->second.type != TopicSubscription::Type::PUBLISHER) {
        return false;
    }
    
    // TODO: Populate publishers from cached data
    return false;
}

// ============================================================================
// Public API: Cache Management
// ============================================================================

/**
 * @brief Checks if a topic is cached
 * @param topicKey Topic composite key
 * @return true if cached and valid
 */
bool TopicManager::isCached(const std::string& topicKey) const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return topicCache_.find(topicKey) != topicCache_.end();
}

/**
 * @brief Gets cached entry (if exists and valid)
 * @param topicKey Topic composite key
 * @param entry Output parameter for cached entry
 * @return true if cached and valid
 */
bool TopicManager::getCachedEntry(const std::string& topicKey, TopicCacheEntry& entry) const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    auto it = topicCache_.find(topicKey);
    if (it != topicCache_.end()) {
        entry = it->second;
        return true;
    }
    
    return false;
}

/**
 * @brief Puts entry into cache (LRU eviction if full)
 * @param key Topic composite key
 * @param entry Entry to cache
 */
void TopicManager::cacheEntry(const std::string& key, const TopicCacheEntry& entry) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    // If cache is at capacity, evict LRU entry
    if (topicCache_.size() >= cacheSize_) {
        evictLRU();
    }
    
    // Insert new entry
    topicCache_[key] = entry;
    cacheAccessOrder_.insert(key);
}

/**
 * @brief Gets cache size
 * @return Current number of cached entries
 */
std::size_t TopicManager::getCacheSize() const {
    return topicCache_.size();
}

/**
 * @brief Clears the LRU cache
 */
void TopicManager::clearCache() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    topicCache_.clear();
    cacheAccessOrder_.clear();
}

// ============================================================================
// Helper Methods
// ============================================================================

/**
 * @brief Evicts least recently used entry from cache
 */
void TopicManager::evictLRU() {
    // Find the least recently used entry (first in access order)
    if (!cacheAccessOrder_.empty()) {
        std::string lruKey = *cacheAccessOrder_.begin();
        auto it = topicCache_.find(lruKey);
        if (it != topicCache_.end()) {
            topicCache_.erase(it);
        }
        cacheAccessOrder_.erase(lruKey);
    }
}

/**
 * @brief Updates access order for LRU tracking
 * @param key Topic key
 */
void TopicManager::updateAccessOrder(const std::string& key) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    
    // Remove from current position and re-insert at end
    cacheAccessOrder_.erase(key);
    cacheAccessOrder_.insert(key);
}

/**
 * @brief Gets composite key from topic name and message type
 * @param topicName Topic name
 * @param messageType Message type
 * @return Composite key (topicName + messageType)
 */
std::string TopicManager::getTopicKey(const std::string& topicName, const std::string& messageType) const {
    return topicName + "/" + messageType;
}

// Helper function (declared in header but needs implementation)
inline uint64_t getCurrentTimestamp() {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    return static_cast<uint64_t>(duration.count());
}

} // namespace edriel
