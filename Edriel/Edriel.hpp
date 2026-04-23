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
#include <unordered_map>
#include <set>

// ============================================================================
// C++20 Concepts for Type Constraints
// ============================================================================

/**
 * @brief Concept for protobuf message types used in topic registration/sending
 */
template<typename T>
concept Topic = std::is_base_of_v<google::protobuf::Message, T>;

/**
 * @brief Concept for participant heartbeat messages
 */
template<typename T>
concept ParticipantHeartbeat = std::is_same_v<T, autoDiscovery::ParticipantHeartbeat>;

/**
 * @brief Concept for timestamps to prevent replay attacks
 */
template<typename Clock>
concept HasTimestamp = requires(Clock c) {
    { c.now() } -> std::same_as<std::chrono::steady_clock::time_point>;
};

// ============================================================================
// Topic Info Structure
// ============================================================================

namespace edriel {

/**
 * @brief Magic number constant for packet integrity verification
 */
constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;

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

    /**
     * @brief Gets current timestamp as uint64_t for replay attack checking
     * @return Current timestamp in seconds since epoch
     */
    static uint64_t getCurrentTimestamp() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    // ========================================================================
    // Sockets and Timers
    // ========================================================================
    std::unique_ptr<asio::ip::udp::socket> autoDiscoverySocket{};  ///< UDP socket for multicast
    std::unique_ptr<asio::steady_timer> autoDiscoverySendTimer{};  ///< Timer for periodic discovery messages
    std::unique_ptr<asio::steady_timer> autoDiscoveryCleanUpTimer{};  ///< Timer for participant cleanup
    asio::ip::udp::endpoint multicastEndpoint{ asio::ip::make_address_v4(std::string(multicastAddress)), commonPort };  ///< Multicast group endpoint
    asio::ip::udp::endpoint receiverEndpoint{asio::ip::address_v4::any(), commonPort};  ///< Local endpoint used for sending discovery packets 
    
    // ========================================================================
    // Discovery Message Buffer
    // ========================================================================
    autoDiscovery::Message discoveryMessage;       ///< Protobuf message template
    std::string discoveryPacket;                   ///< Serialized packet with magic number prepended
    
    // ========================================================================
    // Control Flags
    // ========================================================================
    std::atomic_bool isRunning{ false };           ///< Main loop running flag
    std::mutex runnerMutex;                        ///< Mutex for isRunning flag
    
    // ========================================================================
    // Participant Registry
    // ========================================================================
    std::set<Participant> participants;             ///< All discovered participants
    Participant selfParticipant{0, 0, 0};           ///< Placeholder for self
    
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
     * @brief Stops socket and timers for graceful shutdown
     */
    void stopAutoDiscoverySocketAndTimer();
    
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
    // ========================================================================
    /**
     * @brief Registers a topic for publishing
     * @tparam Topic Protobuf message type
     * @param topicName Topic name to register
     * @return true if registration succeeded
     */
    template<typename T> requires Topic<T>
    bool registerPublisherTopic(const std::string& topicName);
    
    /**
     * @brief Unregisters a topic for publishing
     * @tparam Topic Protobuf message type
     * @param topicName Topic name to unregister
     * @return true if unregistration succeeded
     */
    template<typename T> requires Topic<T>
    bool unregisterPublisherTopic(const std::string& topicName);
    
    /**
     * @brief Registers a topic for subscribing
     * @tparam Topic Protobuf message type
     * @param topicName Topic name to register
     * @return true if registration succeeded
     */
    template<typename T> requires Topic<T>
    bool registerSubscriberTopic(const std::string& topicName);
    
    /**
     * @brief Unregisters a topic for subscribing
     * @tparam Topic Protobuf message type
     * @param topicName Topic name to unregister
     * @return true if unregistration succeeded
     */
    template<typename T> requires Topic<T>
    bool unregisterSubscriberTopic(const std::string& topicName);
    
    // ========================================================================
    // Public API: Message Sending
    // ========================================================================
    /**
     * @brief Sends a message to a topic via multicast broadcast
     * @tparam Topic Protobuf message type
     * @param topicName Topic to send to
     * @param message Message instance to serialize and broadcast
     * @return true if sending succeeded
     */
    template<typename T> requires Topic<T>
    bool sendMessage(const std::string& topicName, const T& message);
    
    // ========================================================================
    // Future: gRPC Streaming Support
    // ========================================================================
    /**
     * @brief Stream participant heartbeat data to gRPC client
     * 
     * Note: Implementation deferred to separate gRPC service module.
     */
    // void streamParticipants(grpc::ServerContext* context,
    //                         const google::protobuf::RepeatedPtrField<autoDiscovery::ParticipantHeartbeat>& initialHeartbeats,
    //                         grpc::ServerWriter<autoDiscovery::ParticipantData>* response_writer);
    
};

} // namespace edriel

