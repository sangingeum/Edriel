/**
 * @file Edriel.hpp
 * @brief Main header for Edriel C++20 multi-cast auto-discovery networking library
 * 
 * Provides multicast-based auto-discovery of network participants with topic-based
 * message publishing/subscribing capabilities, gRPC streaming support, and
 * participant lifecycle management.
 */

#pragma once

#include <asio.hpp>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include "autoDiscovery.pb.h"
#include "autoDiscovery_grpc_service.pb.h"
#include <string_view>
#include <memory>
#include <map>
#include <set>
#include <functional>
#include <atomic>

// ============================================================================
// Topic Info Structure
// ============================================================================

namespace edriel {

/**
 * @brief Magic number constant for packet integrity verification
 */
constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;

/**
 * @brief Concept for protobuf message types used in topic registration/sending
 */
template<typename T>
concept Topic = std::is_base_of_v<google::protobuf::Message, T>;

/**
 * @struct TopicInfo
 * @brief Represents a topic with message type for registration management
 */
class Edriel {
public:
    /**
     * @struct TopicInfo
     * @brief Combines topic name and message type into a unique key
     */
    struct TopicInfo {
        std::string topicName;      ///< Base topic name (e.g., "temperature")
        std::string messageType;    ///< Message type (e.g., "update", "heartbeat")
        std::string key;            ///< Composite key: topicName + messageType
        
        /**
         * @brief Constructs TopicInfo with derived key
         * @param topicName_ Base topic name
         * @param messageType_ Message type
         */
        TopicInfo(const std::string& topicName_, const std::string& messageType_)
            : topicName(topicName_), messageType(messageType_), 
              key(topicName_ + messageType_) {}
        
        /**
         * @brief Equality operator for topic lookups
         * @param other Right-hand TopicInfo
         * @return true if composite keys match
         */
        bool operator==(const TopicInfo& other) const {
            return key == other.key;
        }
        
        /**
         * @brief Equality operator for string lookups
         * @param findKey String key to match against
         * @return true if composite keys match
         */
        bool operator==(const std::string& findKey) const {
            return key == findKey;
        }
        
        /**
         * @brief Less-than operator for map/set ordering
         * @param other Right-hand TopicInfo
         * @return true if this key is lexicographically smaller
         */
        bool operator<(const TopicInfo& other) const {
            return key < other.key;
        }
    };

    /**
     * @struct Participant
     * @brief Represents a discovered participant (peer node)
     */
    struct Participant {
        unsigned long pid;          ///< Participant ID (from multicast packet)
        uint64_t tid;               ///< Transaction ID (sequence number)
        uint64_t uid;               ///< Unique identifier (from uid field)
        mutable std::chrono::steady_clock::time_point lastSeen;  ///< Last heartbeat time
        std::set<TopicInfo> publishedTopics;   ///< Topics this participant publishes
        std::set<TopicInfo> subscribedTopics;  ///< Topics this participant subscribes to
        
        static constexpr std::chrono::seconds timeoutPeriod{ 10 };  ///< Heartbeat timeout
        
        /**
         * @brief Constructs Participant with timestamps
         * @param p Participant ID
         * @param t Transaction ID
         * @param u Unique ID
         */
        Participant(unsigned long p, uint64_t t, uint64_t u)
            : pid(p), tid(t), uid(u), 
              lastSeen(std::chrono::steady_clock::now()) {}
        
        /**
         * @brief Default constructor
         */
        Participant()
            : pid(0), tid(0), uid(0),
              lastSeen(std::chrono::steady_clock::now()) {}
        
        /**
         * @brief Checks if participant should be removed due to timeout
         * @return true if timeout has elapsed since last heartbeat
         */
        bool shouldBeRemoved() const {
            return (std::chrono::steady_clock::now() - lastSeen) > timeoutPeriod;
        }
        
        /**
         * @brief Updates the last seen timestamp
         */
        void updateLastSeen() const {
            lastSeen = std::chrono::steady_clock::now();
        }
        
        /**
         * @brief Equality operator for set containment
         * @param other Right-hand Participant
         * @return true if all fields match
         */
        bool operator==(const Participant& other) const {
            return pid == other.pid && tid == other.tid && uid == other.uid;
        }
        
        /**
         * @brief Less-than operator for set ordering
         * @param other Right-hand Participant
         * @return true if tie comparison indicates this is smaller
         */
        bool operator<(const Participant& other) const {
            return std::tie(pid, tid, uid) < std::tie(other.pid, other.tid, other.uid);
        }
    };

private:
    // ========================================================================
    // Configuration Constants
    // ========================================================================
    static constexpr uint16_t commonPort{ 30002 };       ///< Multicast port
    static constexpr std::string_view multicastAddress{ "239.255.0.1" };  ///< Multicast group address
    static constexpr std::size_t recvBufferSize{ 1500 };  ///< UDP receive buffer size
    static constexpr std::chrono::seconds autoDiscoverySendPeriod{ 2 };     ///< Send heartbeat interval
    static constexpr std::chrono::seconds autoDiscoveryCleanUpPeriod{ 5 };  ///< Cleanup interval
    
    using Buffer = std::array<char, recvBufferSize>;  ///< Buffer type for message serialization
    
    // ========================================================================
    // Magic Number Configuration
    // ========================================================================
    static constexpr uint32_t magicNumber{ MAGIC_NUMBER };  ///< 4-byte magic number for integrity
    static constexpr std::size_t magicNumberSize{ sizeof(magicNumber) };  ///< Magic number size
    
    // ========================================================================
    // ASIO Context
    // ========================================================================
    asio::io_context& io_context;        ///< ASIO I/O context
    asio::strand<asio::io_context::executor_type> strand;  ///< Strand for thread-safe async operations
    // ========================================================================
    // Sockets and Timers
    // ========================================================================
    std::unique_ptr<asio::ip::udp::socket> autoDiscoverySocket{};  ///< UDP socket for multicast
    std::unique_ptr<asio::steady_timer> autoDiscoverySendTimer{};  ///< Timer for periodic discovery messages
    std::unique_ptr<asio::steady_timer> autoDiscoveryCleanUpTimer{};  ///< Timer for participant cleanup
    asio::ip::udp::endpoint multicastEndpoint{ asio::ip::make_address_v4(std::string(multicastAddress)), commonPort };  ///< Multicast group endpoint
    asio::ip::udp::endpoint receiverEndpoint{asio::ip::address_v4::any(), commonPort};  ///< Local bind endpoint for receiving discovery packets
    
    // ========================================================================
    // Discovery Message Buffer
    // ========================================================================
    autoDiscovery::Message discoveryMessage;       ///< Protobuf message template
    std::string discoveryPacket;                   ///< Serialized packet with magic number prepended
    
    // ========================================================================
    // Control Flags
    // ========================================================================
    std::atomic_bool isRunning{ false };           ///< Main loop running flag
    
    // ========================================================================
    // Participant Registry
    // ========================================================================
    std::set<Participant> participants;             ///< All discovered participants
    Participant selfParticipant{0, 0, 0};           ///< Placeholder for self
    
    // ========================================================================
    // Topic Registry
    // ========================================================================
    /**
     * @struct TopicEntry
     * @brief Per-topic registry entry: local subscriber callbacks plus
     *        remote participants seen publishing/subscribing to the topic.
     */
    struct TopicEntry {
        std::string topicName;      ///< Base topic name
        std::string messageType;    ///< Protobuf message type name
        std::set<Participant> publishers;    ///< Remote participants publishing this topic
        std::set<Participant> subscribers;   ///< Remote participants subscribing to this topic
        /// Local typed callbacks invoked on matching received data messages.
        /// Stored type-erased; each carries its own expected message type name.
        struct Callback {
            std::string messageType;
            std::function<void(const google::protobuf::Message&)> invoke;
        };
        std::vector<Callback> callbacks;
    };

    /// Registry keyed by composite key (topicName + messageType)
    std::map<std::string, TopicEntry> topicRegistry;

    // ---- Test hooks (unit-test access to internals) ------------------------
  public:
    const std::map<std::string, TopicEntry>& registryForTest() const {
        return topicRegistry;
    }
    void deliverForTest(const autoDiscovery::Message& msg) {
        handleAutoDiscoveryParse(msg);
    }

  private:
    // ========================================================================
    // Internal Methods
    // ========================================================================
    
    /**
     * @brief Handles received auto-discovery packets
     * @param buffer Shared pointer to receive buffer
     * @param ec ASIO error code
     * @param bytesTransferred Number of bytes received
     */
    void handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer,
                                     const asio::error_code& ec,
                                     std::size_t bytesTransferred);

    /**
     * @brief Dispatches a parsed discovery message by content type (oneof)
     * @param receivedMessage Parsed protobuf message
     */
    void handleAutoDiscoveryParse(const autoDiscovery::Message& receivedMessage);
    
    /**
     * @brief Starts the auto-discovery receiver loop
     * @param buffer Optional buffer (uses default if not provided)
     */
    void startAutoDiscoveryReceiver(std::shared_ptr<Buffer> buffer = std::make_shared<Buffer>());
    
    /**
     * @brief Starts periodic discovery message sender
     */
    void startAutoDiscoverySender();
    
    /**
     * @brief Starts periodic participant cleanup timer
     */
    void startAutoDiscoveryCleaner();
    
    
    /**
     * @brief Initializes auto-discovery components
     */
    void initializeAutoDiscovery();
    
    // ========================================================================
    // Participant Management
    // ========================================================================
    /**
     * @brief Handles incoming participant heartbeat
     * @param pid Participant ID
     * @param tid Transaction ID
     * @param uid Unique identifier
     */
    void handleParticipantHeartbeat(unsigned long pid, uint64_t tid, uint64_t uid);
    
    /**
     * @brief Removes participants that have timed out
     */
    void removeTimedOutParticipants();
    
    // ========================================================================
    // Topic Management
    // ========================================================================
    /**
     * @brief Handles topic announcements in discovery packets
     * @param pid Sender participant ID
     * @param tid Sender transaction ID
     * @param uid Sender unique ID
     * @param topicName Topic name
     * @param messageType Message type
     * @param isPublisher Whether announcing as publisher
     */
    void handleTopicAnnouncement(unsigned long pid, uint64_t tid, uint64_t uid,
                                 const std::string& topicName,
                                 const std::string& messageType, bool isPublisher);

    /**
     * @brief Handles a received data message (demux by topic, invoke callbacks)
     * @param data Parsed DataMessage from the wire
     */
    void handleDataMessageReceive(const autoDiscovery::DataMessage& data);

    /**
     * @brief Prepends magic number and multicasts a serialized protobuf message
     * @param message Protobuf message to send
     * @return true if the send was dispatched successfully
     */
    bool sendPacket(const google::protobuf::Message& message);

    /**
     * @brief Serializes and multicasts a DataMessage envelope
     * @param topicName Topic name to publish under
     * @param messageType Protobuf full name of payload type
     * @param payload Serialized user message bytes
     * @return true if sending succeeded
     */
    bool publishData(const std::string& topicName, const std::string& messageType,
                     const std::string& payload);

    
    // ========================================================================
    // Helper Methods
    // ========================================================================
    /**
     * @brief Validates magic number in received packet
     * @param buffer Shared pointer to receive buffer
     * @param length Packet length
     * @return true if magic number is valid
     */
    bool hasValidMagicNumber(std::shared_ptr<Buffer> buffer, std::size_t length) const;
    
    /**
     * @brief Prepends magic number to packet for broadcast
     * @param packet Reference to packet buffer
     */
    void prependMagicNumberToPacket(std::string& packet) const;

public:
    /**
     * @brief Constructor
     * @param io_ctx ASIO I/O context reference
     */
    Edriel(asio::io_context& io_ctx);
    
    /**
     * @brief Destructor
     */
    ~Edriel();
    
    // ========================================================================
    // Public API: Auto-Discovery Control
    // ========================================================================
    /**
     * @brief Starts auto-discovery receiver/sender loop
     */
    void startAutoDiscovery();
    
    /**
     * @brief Stops auto-discovery and cleans up resources
     */
    void stopAutoDiscovery();

    // ========================================================================
    // Public API: Topic Registration (C++20 templates)
    // Declarations only; template definitions are at the end of this header.
    // ========================================================================
    template<typename T> requires Topic<T>
    bool registerPublisherTopic(const std::string& topicName);

    template<typename T> requires Topic<T>
    bool unregisterPublisherTopic(const std::string& topicName);

    template<typename T> requires Topic<T>
    bool registerSubscriberTopic(const std::string& topicName);

    template<typename T> requires Topic<T>
    bool registerSubscriberTopic(const std::string& topicName,
                                 std::function<void(const T&)> callback);

    /**
     * @brief Subscribes with a typed callback to an already-registered topic
     * @param topicName Topic name previously registered for subscription
     * @param callback Invoked with each received message of type T
     * @return true if the callback was attached
     */
    template<typename T> requires Topic<T>
    bool subscribe(const std::string& topicName,
                   std::function<void(const T&)> callback);

    template<typename T> requires Topic<T>
    bool unregisterSubscriberTopic(const std::string& topicName);

    // ========================================================================
    // Public API: Message Sending (C++20 templates)
    // ========================================================================
    template<typename T> requires Topic<T>
    bool sendMessage(const std::string& topicName, const T& message);

    // ========================================================================
    /**
     * @brief Stream participant heartbeat data to gRPC client
     * 
     * Note: Implementation deferred to separate gRPC service module.
     */
    // void streamParticipants(grpc::ServerContext* context,
    //                         const google::protobuf::RepeatedPtrField<autoDiscovery::ParticipantHeartbeat>& initialHeartbeats,
    //                         grpc::ServerWriter<autoDiscovery::ParticipantData>* response_writer);
};  // class Edriel


// ============================================================================
// Public API: Topic Registration / Message Sending (template definitions)
// Defined as out-of-class member templates so they are visible at every
// instantiation site — including types generated from user .proto files via
// edriel_add_proto_messages(). No explicit instantiations are needed.
// ============================================================================

/**
 * @brief Registers a topic for publishing
 *
 * Adds a topic to the internal registry for this participant to publish to.
 *
 * @tparam T Protobuf message type (constrained by Topic concept)
 * @param topicName Topic name to register
 * @return true if registration succeeded
 */
template<typename T> requires Topic<T>
bool Edriel::registerPublisherTopic(const std::string& topicName) {
    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));

    const std::string key = topicInfo.key;
    TopicEntry& entry = topicRegistry[key];
    entry.topicName = topicName;
    entry.messageType = topicInfo.messageType;

    // Announce our publishing interest via the existing discovery path.
    autoDiscovery::TopicAdvertisement ad;
    ad.mutable_identifier()->set_pid(selfParticipant.pid);
    ad.mutable_identifier()->set_tid(selfParticipant.tid);
    ad.mutable_identifier()->set_uid(selfParticipant.uid);
    ad.mutable_topic()->set_topic_name(topicName);
    ad.mutable_topic()->set_message_type(topicInfo.messageType);
    ad.mutable_topic()->set_is_publisher(true);

    std::cout << "[Edriel] Registered publisher topic: " << topicName << "\n";
    return sendPacket(ad);
}

/**
 * @brief Unregisters a topic for publishing
 *
 * Removes a topic from the internal publishing registry.
 *
 * @tparam T Protobuf message type (constrained by Topic concept)
 * @param topicName Topic name to unregister
 * @return true if unregistration succeeded
 */
template<typename T> requires Topic<T>
bool Edriel::unregisterPublisherTopic(const std::string& topicName) {
    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));
    const std::string key = topicInfo.key;

    auto it = topicRegistry.find(key);
    if (it == topicRegistry.end()) {
        std::cout << "[Edriel] Unregister for unknown topic: " << topicName << "\n";
        return false;
    }

    // No local publisher-side state beyond the entry itself; drop the entry
    // if no local callbacks remain, otherwise keep it for the subscriber side.
    if (it->second.callbacks.empty()) {
        topicRegistry.erase(it);
    }

    std::cout << "[Edriel] Unregistered publisher topic: " << topicName << "\n";
    return true;
}

/**
 * @brief Registers a topic for subscribing
 *
 * Adds a topic to the internal registry for this participant to subscribe to.
 *
 * @tparam T Protobuf message type (constrained by Topic concept)
 * @param topicName Topic name to register
 * @return true if registration succeeded
 */
template<typename T> requires Topic<T>
bool Edriel::registerSubscriberTopic(const std::string& topicName) {
    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));
    const std::string key = topicInfo.key;

    TopicEntry& entry = topicRegistry[key];
    entry.topicName = topicName;
    entry.messageType = topicInfo.messageType;

    autoDiscovery::TopicAdvertisement ad;
    ad.mutable_identifier()->set_pid(selfParticipant.pid);
    ad.mutable_identifier()->set_tid(selfParticipant.tid);
    ad.mutable_identifier()->set_uid(selfParticipant.uid);
    ad.mutable_topic()->set_topic_name(topicName);
    ad.mutable_topic()->set_message_type(topicInfo.messageType);
    ad.mutable_topic()->set_is_publisher(false);

    std::cout << "[Edriel] Registered subscriber topic: " << topicName << "\n";
    return sendPacket(ad);
}

/**
 * @brief Registers a topic for subscribing with a typed callback
 *
 * Stores the type-erased callback under the composite key and announces the
 * subscription via a TopicAdvertisement discovery packet. Callbacks are
 * invoked on this object's strand when matching data messages arrive.
 *
 * @tparam T Protobuf message type (constrained by Topic concept)
 * @param topicName Topic name to register
 * @param callback Invoked with each received message of type T
 * @return true if registration succeeded
 */
template<typename T> requires Topic<T>
bool Edriel::registerSubscriberTopic(const std::string& topicName,
                                     std::function<void(const T&)> callback) {
    if (!callback) {
        return false;
    }

    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));
    const std::string key = topicInfo.key;

    TopicEntry& entry = topicRegistry[key];
    entry.topicName = topicName;
    entry.messageType = topicInfo.messageType;

    TopicEntry::Callback erased;
    erased.messageType = topicInfo.messageType;
    erased.invoke = [fn = std::move(callback)](const google::protobuf::Message& msg) {
        fn(dynamic_cast<const T&>(msg));
    };
    entry.callbacks.push_back(std::move(erased));

    std::cout << "[Edriel] Registered subscriber topic with callback: "
              << topicName << "\n";
    return registerSubscriberTopic<T>(topicName);
}

/**
 * @brief Subscribes to an already-registered topic with a typed callback
 *
 * Attaches the type-erased callback to the existing registry entry for
 * topicName (with T's message type) without re-announcing the subscription
 * on the wire. Use after registerSubscriberTopic(topicName).
 *
 * @tparam T Protobuf message type (constrained by Topic concept)
 * @param topicName Topic name to attach the callback to
 * @param callback Invoked with each received message of type T
 * @return true if the callback was attached successfully
 */
template<typename T> requires Topic<T>
bool Edriel::subscribe(const std::string& topicName,
                       std::function<void(const T&)> callback) {
    if (!callback) {
        return false;
    }

    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));

    auto it = topicRegistry.find(topicInfo.key);
    if (it == topicRegistry.end()) {
        std::cout << "[Edriel] Subscribe for unregistered topic: "
                  << topicName << "\n";
        return false;
    }

    TopicEntry::Callback erased;
    erased.messageType = topicInfo.messageType;
    erased.invoke = [fn = std::move(callback)](const google::protobuf::Message& msg) {
        fn(dynamic_cast<const T&>(msg));
    };
    it->second.callbacks.push_back(std::move(erased));
    return true;
}

/**
 * @brief Unregisters a topic for subscribing
 *
 * Removes a topic from the internal subscriber registry.
 *
 * @tparam T Protobuf message type (constrained by Topic concept)
 * @param topicName Topic name to unregister
 * @return true if unregistration succeeded
 */
template<typename T> requires Topic<T>
bool Edriel::unregisterSubscriberTopic(const std::string& topicName) {
    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));
    const std::string key = topicInfo.key;

    auto it = topicRegistry.find(key);
    if (it == topicRegistry.end()) {
        std::cout << "[Edriel] Unregister for unknown topic: " << topicName << "\n";
        return false;
    }

    // Remove all local callbacks for this topic; keep the entry only if it
    // still tracks remote peers (registry bookkeeping for discovery).
    it->second.callbacks.clear();
    if (it->second.publishers.empty() && it->second.subscribers.empty()) {
        topicRegistry.erase(it);
    }

    std::cout << "[Edriel] Unregistered subscriber topic: " << topicName << "\n";
    return true;
}

// ============================================================================
// Public API: Message Sending
// ============================================================================

/**
 * @brief Sends a message to a topic via multicast broadcast
 *
 * Serializes the message into a DataMessage envelope, prepends the magic
 * number, and multicasts it. All participants subscribed to the topic will
 * receive this message.
 *
 * @tparam T Protobuf message type (constrained by Topic concept)
 * @param topicName Topic to send to
 * @param message Message instance to serialize and broadcast
 * @return true if sending succeeded
 */
template<typename T> requires Topic<T>
bool Edriel::sendMessage(const std::string& topicName, const T& message) {
    std::string payload;
    if (!message.SerializeToString(&payload)) {
        std::cerr << "[Edriel] Failed to serialize message for topic: "
                  << topicName << "\n";
        return false;
    }
    return publishData(topicName, std::string(T::descriptor()->full_name()), payload);
}

} // namespace edriel

