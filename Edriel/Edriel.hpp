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
#include <string>
#include <memory>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>   // registry synchronization
#include <tuple>   // std::tie in Participant::operator<
#include "EdrielConfig.hpp"
#include "EdrielGrpcService.hpp"

// ============================================================================
// Topic Info Structure
// ============================================================================

namespace edriel {

// Separator placed between topic name and message type in a composite
// registry key so that "ab"+"c" and "a"+"bc" can never collide. A control
// char (unit separator) is used because it cannot appear in topic names or
// protobuf type names.
constexpr char kTopicKeySeparator = static_cast<char>(0x1F);

/// Build the composite registry key for a topic name + message type pair.
inline std::string makeCompositeKey(const std::string& topicName,
                                    const std::string& messageType) {
    return topicName + kTopicKeySeparator + messageType;
}

/**
 * @brief Magic number constant for packet integrity verification
 */
constexpr uint32_t MAGIC_NUMBER = 0xED75E1ED;

/// Bounded reorder/dedup window for the reliable receiver (exactly-once per
/// (publisher, topic)). Out-of-order reliable frames buffer within this many
/// tids; older-than-window duplicates are dropped.
inline constexpr std::size_t kReliableWindowSize = 256;

// Forward declaration: the gRPC ParticipantStreamService server for the
// reliable path (ADR-0002), implemented in EdrielGrpcService.{hpp,cpp}.
class ParticipantStreamServiceImpl;

// Forward declaration: one dial from this (subscriber) node to a publisher,
// implemented in EdrielReliableClient.{hpp,cpp}.
class ReliableSubscriberConnection;

// ============================================================================
// Concept for protobuf message types used in topic registration/sending
// ============================================================================
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
              key(makeCompositeKey(topicName_, messageType_)) {}
        
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
        /// Advertised unicast gRPC endpoints (ADR-0002 Channel A), refreshed
        /// on every heartbeat. Not part of the ordering key, hence mutable
        /// (same pattern as `lastSeen`). Candidates tried in order, first-wins.
        mutable std::vector<autoDiscovery::Endpoint> endpoints;
        std::chrono::seconds timeout = std::chrono::seconds(10);  ///< Aliveness timeout (from Config)

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
         * @brief Constructs Participant with an explicit aliveness timeout
         * @param p Participant ID
         * @param t Transaction ID
         * @param u Unique ID
         * @param alivenessTimeout Seconds before the participant is dropped
         */
        Participant(unsigned long p, uint64_t t, uint64_t u,
                    std::chrono::seconds alivenessTimeout)
            : pid(p), tid(t), uid(u), 
              lastSeen(std::chrono::steady_clock::now()),
              timeout(alivenessTimeout) {}

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
            return (std::chrono::steady_clock::now() - lastSeen) > timeout;
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
    static constexpr std::size_t recvBufferSize{ 1500 };  ///< UDP receive buffer size
    
    // The discovery send period and cleanup interval are not hardcoded here:
    // the send period is read from Config.discoverySendPeriod (config.yml
    // `discovery_period_seconds`) and the cleanup interval is derived from the
    // configured participant timeout (see autoDiscoveryCleanupPeriod()).
    
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
    asio::ip::udp::endpoint multicastEndpoint;       ///< Multicast group endpoint
    asio::ip::udp::endpoint receiverEndpoint;       ///< Local bind endpoint for receiving discovery packets
    Config config_;                                  ///< Parsed runtime config (port + multicast group)

    // Reliable-path gRPC server (ADR-0002): one small server per node, serving
    // ParticipantStreamService on config_.grpcPort.
    std::unique_ptr<grpc::Server> grpcServer_;
    std::unique_ptr<ParticipantStreamServiceImpl> grpcService_;
    /// Guards `grpcServer_`/`grpcService_` lifecycle against a concurrent
    /// reliable send: startGrpcServer/stopGrpcServer and publishReliable share
    /// this barrier so a send cannot dereference a destroyed service.
    mutable std::mutex grpcServiceMutex_;

    // ------------------------------------------------------------------------
    // Reliable send/receive state (ADR-0002 M4/M5)
    // ------------------------------------------------------------------------
    /// Bounded exactly-once (and in-order) receiver window per (publisher
    /// uid, topic). `nextExpected` is the next contiguous per-(pub,topic) tid;
    /// out-of-order frames buffer up to `kReliableWindowSize`.
    struct ReliableRxWindow {
        std::uint64_t nextExpected = 1;
        std::map<std::uint64_t, autoDiscovery::DataMessage> buffer;
    };
    /// Windows are mutable; updates come from subscriber-client threads.
    mutable std::map<std::string, ReliableRxWindow> reliableWindows_;
    /// Per-(publisher,topic) monotonic sequence stamps (reuses Identifier.tid).
    std::map<std::string, std::uint64_t> reliablePublisherSeq_;
    /// Composite keys of topics this node subscribes to with reliable QoS;
    /// these drive which publishers this node dials.
    std::set<std::string> reliableSubscribedTopics_;

    /// Subscriber-client connections (this node dialing publishers).
    mutable std::mutex reliableConnMutex_;
    std::map<SubscriberKey, std::unique_ptr<ReliableSubscriberConnection>>
        reliableConnections_;
    
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
        /// True when this node registered the topic as reliable (QoS): send
        /// traffic over the gRPC path; best-effort otherwise (multicast).
        bool reliable = false;
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

    /// Guards the strand-shared mutables (`topicRegistry`, `participants`)
    /// against concurrent access from the public API (any calling thread) and
    /// the strand-serialized receive/cleaner loop. The public API must keep
    /// synchronous mutation semantics, so the registry/participant documents
    /// are protected here; the ASIO socket operations are separately confined
    /// to the strand (see sendPacket).
    mutable std::mutex stateMutex;

    // ---- Test hooks (unit-test access to internals) ------------------------
  public:
    const std::map<std::string, TopicEntry>& registryForTest() const {
        return topicRegistry;
    }
    const std::set<Participant>& participantsForTest() const {
        return participants;
    }
    /// This node's self-identity (test hook for wiring consistent registry
    /// snapshots without multicast).
    const Participant& selfIdentityForTest() const {
        return selfParticipant;
    }
    /// Whether a subscriber stream for `key` is currently registered on this
    /// node's server (test hook to wait for the reliable dial to land).
    bool subscriberConnectedForTest(const SubscriberKey& key) const {
        return grpcService_ && grpcService_->hasSubscriber(key);
    }
    void deliverForTest(const autoDiscovery::Message& msg) {
        handleAutoDiscoveryParse(msg);
    }
    /// Number of live per-(publisher,topic) reliable receiver windows (test
    /// hook for ISSUE #4's bounded-growth assertion).
    std::size_t reliableWindowsSizeForTest() const {
        std::lock_guard<std::mutex> lock(stateMutex);
        return reliableWindows_.size();
    }
    /// Age a known participant's last-seen so the next timeout cleanup removes
    /// it (and its reliable receive windows). Test hook for the ISSUE #4 prune.
    void ageParticipantForTest(std::uint64_t uid) {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (auto& p : participants) {
            if (p.uid == uid) {
                p.lastSeen -= std::chrono::hours(24);
            }
        }
    }
    /// Run the participant-timeout cleanup (removes stale participants and
    /// prunes their reliable windows). Test hook for the ISSUE #4 prune.
    void runTimeoutCleanupForTest() {
        removeTimedOutParticipants();
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
     * @brief Derived cleanup cadence (seconds), used to reschedule the
     *        participant cleaner. Runs at timeout/2 (e.g. 10s timeout -> 5s
     *        cleanup) so a stale participant is dropped shortly after its
     *        timeout elapses, never below 1s.
     * @return cleanup interval derived from the configured participant timeout
     */
    std::chrono::seconds autoDiscoveryCleanupPeriod() const;
    
    
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
     * @param endpoints Advertised unicast gRPC endpoints from the heartbeat
     *        (ADR-0002 Channel A); overwrites the participant's list on each
     *        heartbeat so stale/moved peers re-advertise and refresh in place.
     */
    void handleParticipantHeartbeat(unsigned long pid, uint64_t tid, uint64_t uid,
                                    std::vector<autoDiscovery::Endpoint> endpoints);
    
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

    /**
     * @brief Pushes a reliable DataMessage over the subscriber streams of a
     * topic. Stamps a per-(publisher,topic) tid (reusing Identifier.tid) so the
     * receiver can dedup/reorder to exactly-once. Serves from the gRPC server's
     * subscriber table (publisher side of subscriber-initiated dialing).
     * @return true if pushed to at least one connected subscriber
     */
    bool publishReliable(const std::string& topicName, const std::string& messageType,
                         const std::string& payload);

    /**
     * @brief Reconciles the subscriber-client connection set against the
     *        current publishers of locally reliable topics: dial new ones with
     *        their ordered candidate endpoints (connect-in-order), tear down
     *        those whose publisher left, and re-dial a connection whose
     *        currently-connected endpoint is no longer advertised (endpoint
     *        change). Also resolves the static `peers:` seed (Channel D).
     */
    void reconcileReliableConnections();

    /**
     * @brief Schedules a function on this object's strand
     * @param thunk Invoked by the io_context's strand executor
     */
    void postOnStrand(std::function<void()> thunk);

    
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
     * @brief Constructor with explicit runtime configuration
     * @param io_ctx ASIO I/O context reference
     * @param config Parsed config (port + multicast group)
     */
    Edriel(asio::io_context& io_ctx, const Config& config);

    /**
     * @brief Constructor using default config values
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
    // Public API: Reliable gRPC path (ADR-0002)
    // ========================================================================
    /**
     * @brief Starts the reliable-path gRPC server on config_.grpcPort.
     *
     * Serves ParticipantStreamService (GetParticipantInfo + StreamParticipants)
     * so subscribers can dial this node's advertised endpoints. Additive to the
     * multicast plane; a port conflict logs a warning and leaves multicast
     * untouched. Also started implicitly by startAutoDiscovery().
     */
    void startGrpcServer();

    /**
     * @brief Stops and drains the reliable-path gRPC server.
     */
    void stopGrpcServer();

    /**
     * @brief Builds the ParticipantData for a known participant identity.
     *
     * Internal accessor used by the gRPC service (Channel C verifier). Returns
     * false when the identity is not in the registry.
     */
    bool lookupParticipantData(std::uint32_t pid, std::uint64_t tid, std::uint64_t uid,
                               autoDiscovery::ParticipantData& out) const;

    /**
     * @brief Whether a (pid,tid,uid) is a known peer in this node's registry —
     *        a discovered participant, a topic publisher/subscriber, or the
     *        synthesized identity of a static `peers:` seed (Channel D).
     *
     * Internal accessor used by the gRPC service's anti-spoof gate (ADR-0002
     * §6.2): a dialer that is not a known participant is rejected before its
     * stream is registered or fed frames.
     */
    bool isKnownParticipant(std::uint32_t pid, std::uint64_t tid, std::uint64_t uid);

    /**
     * @brief Deterministic (pid,0,uid) key synthesized from a static `peers:`
     * endpoint (ADR-0002 Channel D), giving a multicast-blind seed peer a
     * stable identity for the subscriber-initiated dials and the anti-spoof gate.
     */
    static SubscriberKey peerKeyForEndpoint(const std::string& endpoint);

    /**
     * @brief Snapshot of every registered peer as ParticipantData (presence).
     *
     * Internal accessor used by the gRPC service to push discovery presence on
     * a freshly-dialed StreamParticipants stream. Excludes the node itself.
     */
    std::vector<autoDiscovery::ParticipantData> snapshotParticipantData() const;

    /**
     * @brief Drives this node's subscriber-client connections to the
     *        publishers of every locally registered reliable topic.
     *
     * Dialing is subscriber-initiated (ADR-0002): for each reliable topic this
     * node subscribes to, ensure a StreamParticipants connection to each
     * current publisher, dialing its ordered candidate endpoints
     * (connect-in-order, multi-homed) and re-dialing on break or on an
     * advertised-endpoint change. Also resolves the static `peers:` seed
     * (Channel D). Called automatically on each discovery cleanup; safe to
     * call again.
     */
    void startReliableSubscriptions();

    /**
     * @brief Tears down all subscriber-client connections to publishers.
     *
     * Stops and joins each dialing thread. Also called by stopAutoDiscovery().
     */
    void stopReliableSubscriptions();

    /**
     * @brief Handles a ParticipantData frame read from a publisher's stream.
     *
     * Extracts a reliable DataMessage payload (reliable_data), applies the
     * bounded exactly-once reorder/dedup window per (publisher uid, topic),
     * then dispatches to the local callbacks. Presence frames (empty
     * reliable_data) are ignored. Internal, called by ReliableSubscriberConnection.
     */
    void handleReliableDataFrame(const autoDiscovery::ParticipantData& pd);

    // ========================================================================
    // Public API: Topic Registration (C++20 templates)
    // Declarations only; template definitions are at the end of this header.
    // ========================================================================
    template<typename T> requires Topic<T>
    bool registerPublisherTopic(const std::string& topicName, bool reliable = false);

    template<typename T> requires Topic<T>
    bool unregisterPublisherTopic(const std::string& topicName);

    template<typename T> requires Topic<T>
    bool registerSubscriberTopic(const std::string& topicName, bool reliable = false);

    template<typename T> requires Topic<T>
    bool registerSubscriberTopic(const std::string& topicName,
                                 std::function<void(const T&)> callback,
                                 bool reliable = false);

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
bool Edriel::registerPublisherTopic(const std::string& topicName, bool reliable) {
    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));

    {
        // Registry mutation is strand-shared state: guard it so a concurrent
        // caller thread cannot race the receive/cleaner loop.
        std::lock_guard<std::mutex> lock(stateMutex);
        TopicEntry& entry = topicRegistry[topicInfo.key];
        entry.topicName = topicName;
        entry.messageType = topicInfo.messageType;
        entry.reliable = reliable;
    }

    // Announce our publishing interest via the existing discovery path.
    autoDiscovery::TopicAdvertisement ad;
    ad.mutable_identifier()->set_pid(selfParticipant.pid);
    ad.mutable_identifier()->set_tid(selfParticipant.tid);
    ad.mutable_identifier()->set_uid(selfParticipant.uid);
    ad.mutable_topic()->set_topic_name(topicName);
    ad.mutable_topic()->set_message_type(topicInfo.messageType);
    ad.mutable_topic()->set_is_publisher(true);
    ad.mutable_topic()->set_reliable(reliable);

    std::cout << "[Edriel] Registered publisher topic: " << topicName
              << (reliable ? " (reliable)" : "") << "\n";
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

    std::lock_guard<std::mutex> lock(stateMutex);
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
bool Edriel::registerSubscriberTopic(const std::string& topicName, bool reliable) {
    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        TopicEntry& entry = topicRegistry[topicInfo.key];
        entry.topicName = topicName;
        entry.messageType = topicInfo.messageType;
        entry.reliable = reliable;
        if (reliable) {
            reliableSubscribedTopics_.insert(topicInfo.key);
        }
    }

    autoDiscovery::TopicAdvertisement ad;
    ad.mutable_identifier()->set_pid(selfParticipant.pid);
    ad.mutable_identifier()->set_tid(selfParticipant.tid);
    ad.mutable_identifier()->set_uid(selfParticipant.uid);
    ad.mutable_topic()->set_topic_name(topicName);
    ad.mutable_topic()->set_message_type(topicInfo.messageType);
    ad.mutable_topic()->set_is_publisher(false);
    ad.mutable_topic()->set_reliable(reliable);

    std::cout << "[Edriel] Registered subscriber topic: " << topicName
              << (reliable ? " (reliable)" : "") << "\n";
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
                                     std::function<void(const T&)> callback,
                                     bool reliable) {
    if (!callback) {
        return false;
    }

    TopicInfo topicInfo(topicName, std::string(T::descriptor()->full_name()));

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        TopicEntry& entry = topicRegistry[topicInfo.key];
        entry.topicName = topicName;
        entry.messageType = topicInfo.messageType;
        entry.reliable = reliable;
        if (reliable) {
            reliableSubscribedTopics_.insert(topicInfo.key);
        }

        TopicEntry::Callback erased;
        erased.messageType = topicInfo.messageType;
        erased.invoke = [fn = std::move(callback)](const google::protobuf::Message& msg) {
            fn(dynamic_cast<const T&>(msg));
        };
        entry.callbacks.push_back(std::move(erased));
    }

    std::cout << "[Edriel] Registered subscriber topic with callback: "
              << topicName << (reliable ? " (reliable)" : "") << "\n";
    return registerSubscriberTopic<T>(topicName, reliable);
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

    std::lock_guard<std::mutex> lock(stateMutex);
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

    std::lock_guard<std::mutex> lock(stateMutex);
    const std::string key = topicInfo.key;

    auto it = topicRegistry.find(key);
    if (it == topicRegistry.end()) {
        std::cout << "[Edriel] Unregister for unknown topic: " << topicName << "\n";
        return false;
    }

    // Remove all local callbacks for this topic; keep the entry only if it
    // still tracks remote peers (registry bookkeeping for discovery).
    it->second.callbacks.clear();
    reliableSubscribedTopics_.erase(key);
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

    const std::string messageType = std::string(T::descriptor()->full_name());

    // QoS routing: reliable topics go over the gRPC path (unicast, exactly-
    // once), best-effort topics stay on multicast exactly as before.
    bool reliable = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        const auto it = topicRegistry.find(makeCompositeKey(topicName, messageType));
        if (it != topicRegistry.end() && it->second.reliable) {
            reliable = true;
        }
    }

    if (reliable) {
        return publishReliable(topicName, messageType, payload);
    }
    return publishData(topicName, messageType, payload);
}

} // namespace edriel

