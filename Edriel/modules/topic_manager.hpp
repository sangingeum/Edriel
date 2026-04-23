/**
 * @file topic_manager.hpp
 * @brief Topic management module for Edriel C++20 multi-cast auto-discovery
 * 
 * Provides efficient topic-based message routing with hash indexing and LRU caching.
 * Manages publisher/subscriber registration and topic lifecycle.
 * 
 * @version 1.0.0
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include "../concepts.hpp"
#include "../Edriel.hpp"

namespace edriel {

/**
 * @brief Topic subscription information
 * 
 * Tracks whether a topic is published by or subscribed by a participant.
 */
struct TopicSubscription {
    enum class Type { PUBLISHER, SUBSCRIBER };
    Type type;  ///< Subscription type (publisher or subscriber)
    
    TopicSubscription(Type t) : type(t) {}
};

/**
 * @brief Topic exchange cache entry
 * 
 * LRU cache for topic metadata with time-based expiration.
 */
struct TopicCacheEntry {
    std::string topicKey;        ///< Topic + message type composite key
    uint64_t accessTime;         ///< Last access timestamp
    uint32_t magicNumber;        ///< Cached magic number
    std::size_t messageSize;     ///< Typical message size for this topic
    
    TopicCacheEntry(const std::string& key, uint64_t time, uint32_t magic, std::size_t size)
        : topicKey(key), accessTime(time), magicNumber(magic), messageSize(size) {}
    
    /**
     * @brief Checks if entry is still valid (not expired)
     * @param now Current timestamp
     * @return true if entry is valid
     */
    bool isValid(uint64_t now) const {
        constexpr uint64_t CACHE_EXPIRATION_SECONDS = 300;  // 5 minutes
        return (now - accessTime) < CACHE_EXPIRATION_SECONDS;
    }
};

/**
 * @class TopicManager
 * @brief Manages topic registration, routing, and LRU caching
 * 
 * Provides efficient topic exchange mechanisms using:
 * - Hash-based topic indexing for O(1) lookup
 * - LRU caching for topic metadata
 * - Publisher/subscriber tracking
 */
class TopicManager {
public:
    /**
     * @brief Constructs TopicManager
     * @param cacheSize LRU cache size
     */
    explicit TopicManager(std::size_t cacheSize = 1024);
    
    /**
     * @brief Destructor
     */
    ~TopicManager() = default;
    
    // ========================================================================
    // Public API: Topic Registration
    // ========================================================================
    
    /**
     * @brief Registers a topic for publishing
     * @param topicName Topic name to register
     * @param messageType Message type (e.g., "update", "heartbeat")
     * @return true if registration succeeded
     */
    bool registerPublisherTopic(const std::string& topicName, const std::string& messageType);
    
    /**
     * @brief Registers a topic for subscribing
     * @param topicName Topic name to register
     * @param messageType Message type
     * @return true if registration succeeded
     */
    bool registerSubscriberTopic(const std::string& topicName, const std::string& messageType);
    
    /**
     * @brief Unregisters a topic for publishing
     * @param topicName Topic name to unregister
     * @return true if unregistration succeeded
     */
    bool unregisterPublisherTopic(const std::string& topicName, const std::string& messageType);
    
    /**
     * @brief Unregisters a topic for subscribing
     * @param topicName Topic name to unregister
     * @return true if unregistration succeeded
     */
    bool unregisterSubscriberTopic(const std::string& topicName, const std::string& messageType);
    
    // ========================================================================
    // Public API: Message Routing
    // ========================================================================
    
    /**
     * @brief Looks up subscribers for a topic
     * @param topicName Topic name
     * @param messageType Message type
     * @param subscribers Output parameter for subscriber set
     * @return true if topic exists and has subscribers
     */
    bool getSubscribers(const std::string& topicName, const std::string& messageType,
                        std::unordered_set<unsigned long>& subscribers) const;
    
    /**
     * @brief Looks up publishers for a topic
     * @param topicName Topic name
     * @param messageType Message type
     * @param publishers Output parameter for publisher set
     * @return true if topic exists and has publishers
     */
    bool getPublishers(const std::string& topicName, const std::string& messageType,
                       std::unordered_set<unsigned long>& publishers) const;
    
    // ========================================================================
    // Public API: Cache Management
    // ========================================================================
    
    /**
     * @brief Checks if a topic is cached
     * @param topicKey Topic composite key
     * @return true if cached
     */
    bool isCached(const std::string& topicKey) const;
    
    /**
     * @brief Gets cached entry (if exists and valid)
     * @param topicKey Topic composite key
     * @param entry Output parameter for cached entry
     * @return true if cached and valid
     */
    bool getCachedEntry(const std::string& topicKey, TopicCacheEntry& entry) const;
    
    /**
     * @brief Puts entry into cache (LRU eviction if full)
     * @param key Topic composite key
     * @param entry Entry to cache
     */
    void cacheEntry(const std::string& key, const TopicCacheEntry& entry);
    
    /**
     * @brief Gets cache size
     * @return Current number of cached entries
     */
    std::size_t getCacheSize() const;
    
    /**
     * @brief Clears the LRU cache
     */
    void clearCache();
    
private:
    // ========================================================================
    // Configuration
    // ========================================================================
    std::size_t cacheSize_;     ///< LRU cache capacity
    
    // ========================================================================
    // Topic Registry (Hash-based indexing)
    // ========================================================================
    std::unordered_map<std::string, TopicSubscription> topicRegistry_;  ///< Topic name -> subscription type
    
    // ========================================================================
    // LRU Cache
    // ========================================================================
    std::unordered_map<std::string, TopicCacheEntry> topicCache_;       ///< Topic key -> cache entry
    std::unordered_set<std::string> cacheAccessOrder_;                   ///< Access order for LRU eviction
    std::mutex cacheMutex_;                                              ///< Protect cache operations
    
    // ========================================================================
    // Helper Methods
    // ========================================================================
    
    /**
     * @brief Evicts least recently used entry from cache
     */
    void evictLRU();
    
    /**
     * @brief Updates access order for LRU tracking
     * @param key Topic key
     */
    void updateAccessOrder(const std::string& key);
    
    /**
     * @brief Gets composite key from topic name and message type
     * @param topicName Topic name
     * @param messageType Message type
     * @return Composite key
     */
    std::string getTopicKey(const std::string& topicName, const std::string& messageType) const;
    
};

} // namespace edriel
