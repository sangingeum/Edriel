/**
 * @file Edriel.cpp
 * @brief Main implementation file for Edriel C++20 multi-cast auto-discovery networking library
 * 
 * Implements multicast-based auto-discovery with participant lifecycle management,
 * topic-based message publishing/subscribing, and gRPC streaming integration.
 */

#include "Edriel.hpp"
#include <asio/steady_timer.hpp>
#include <google/protobuf/descriptor.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <random>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace edriel {

// ============================================================================
// Configuration Constants
// ============================================================================

/// Expected magic number for packet integrity verification
constexpr uint32_t MAGIC_NUMBER_VALUE = 0xED75E1ED;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Validates magic number in received packet
 * 
 * The magic number is the first 4 bytes of every discovery packet.
 * Validates it against our expected magic number 0xED75E1ED.
 * 
 * @param buffer Shared pointer to receive buffer
 * @param length Packet length
 * @return true if magic number is valid
 */
bool Edriel::hasValidMagicNumber(std::shared_ptr<Buffer> buffer, 
                                  std::size_t length) const {
    if (length < magicNumberSize) {
        std::cout << "[Edriel] Packet too small for magic number validation\n";
        return false;
    }
    
    // Copy to a properly aligned uint32_t rather than reinterpreting the
    // (possibly unaligned) receive buffer, avoiding UB / strict-aliasing issues.
    std::uint32_t networkMagic = 0;
    std::memcpy(&networkMagic, buffer->data(), sizeof(networkMagic));
    const std::uint32_t receivedMagic = ntohl(networkMagic);

    if (receivedMagic != MAGIC_NUMBER_VALUE) {
        return false;
    }
    
    return true;
}

/**
 * @brief Prepends magic number to packet for broadcast
 * 
 * All outgoing discovery packets have the magic number prepended
 * to ensure receiving nodes can validate packet integrity.
 * 
 * @param packet Reference to packet buffer
 */
void Edriel::prependMagicNumberToPacket(std::string& packet) const {
    // Create new buffer with magic number + original packet
    std::string newPacket(magicNumberSize + packet.length(), '\0');
    
    // Write magic number (first 4 bytes) in network byte order. Copied via
    // memcpy so no aliasing/alignment assumptions are made on the packet.
    const std::uint32_t networkMagic = htonl(MAGIC_NUMBER_VALUE);
    std::memcpy(newPacket.data(), &networkMagic, sizeof(networkMagic));
    
    // Copy original packet after magic number
    std::memcpy(newPacket.data() + magicNumberSize, packet.data(), packet.length());
    
    packet = std::move(newPacket);
}

// ============================================================================
// Constructor & Destructor
// ============================================================================

/**
 * @brief Constructor using the config.yml defaults
 *
 * Delegates to the config-taking constructor after loading config.yml (or the
 * built-in defaults when the file is missing or its values are invalid).
 *
 * @param io_ctx ASIO I/O context reference
 */
Edriel::Edriel(asio::io_context& io_ctx)
    : Edriel(io_ctx, loadConfig())
{}

/**
 * @brief Constructor for Edriel class
 *
 * Initializes ASIO socket, timers, and other components for auto-discovery,
 * bound to the configured multicast group address and port.
 *
 * @param io_ctx ASIO I/O context reference
 * @param config Parsed runtime config (port + multicast group)
 */
Edriel::Edriel(asio::io_context& io_ctx, const Config& config)
    : io_context(io_ctx)
    , strand(asio::make_strand(io_ctx))
    , autoDiscoverySocket(std::make_unique<asio::ip::udp::socket>(io_ctx))
    , autoDiscoverySendTimer(std::make_unique<asio::steady_timer>(io_ctx))
    , autoDiscoveryCleanUpTimer(std::make_unique<asio::steady_timer>(io_ctx))
    , multicastEndpoint(asio::ip::make_address_v4(config.multicastAddress), config.port)
    , receiverEndpoint(asio::ip::address_v4::any(), config.port)
    , config_(config)
{
    if (config_.fellBackToDefaults) {
        std::cout << "[Edriel] One or more invalid config.yml values; "
                  << "using defaults (port=" << config_.port
                  << ", multicast=" << config_.multicastAddress << ")\n";
    }

    autoDiscoverySocket->open(receiverEndpoint.protocol());
    // Configure socket for multicast
    autoDiscoverySocket->set_option(asio::ip::udp::socket::reuse_address(true));
    autoDiscoverySocket->set_option(asio::ip::multicast::join_group(
                                     asio::ip::make_address_v4(config_.multicastAddress)));
    autoDiscoverySocket->set_option(asio::ip::multicast::enable_loopback(true));
    autoDiscoverySocket->bind(receiverEndpoint);

    // Initialize self participant: real pid + a process-unique uid so two
    // nodes never collapse into the same registry identity.
    static std::random_device rd;
    const auto selfPid = static_cast<unsigned long>(::getpid());
    selfParticipant.pid = selfPid;
    selfParticipant.tid = 0;
    selfParticipant.uid = (static_cast<uint64_t>(rd()) << 32)
                        | static_cast<uint64_t>(rd());
    selfParticipant.lastSeen = std::chrono::steady_clock::now();

    // Initialize discovery message template
    discoveryMessage.mutable_identifier()->set_pid(selfParticipant.pid);
    discoveryMessage.mutable_identifier()->set_tid(selfParticipant.tid);
    discoveryMessage.mutable_identifier()->set_uid(selfParticipant.uid);
}

/**
 * @brief Destructor for Edriel class
 * 
 * Cleans up resources and stops all timers.
 */
Edriel::~Edriel() {
    stopAutoDiscovery();
}

// ============================================================================
// Initialization & Cleanup
// ============================================================================

/**
 * @brief Initializes auto-discovery components
 * 
 * Sets up socket options, starts send/receive timers, and configures the endpoint.
 */
void Edriel::initializeAutoDiscovery() {
    // Start send timer
    autoDiscoverySendTimer->expires_after(autoDiscoverySendPeriod);
    
    // Start cleanup timer
    autoDiscoveryCleanUpTimer->expires_after(autoDiscoveryCleanUpPeriod);
    
    // Start receivers
    startAutoDiscoveryReceiver();
    startAutoDiscoverySender();
    startAutoDiscoveryCleaner();
    
    // Set running flag
    isRunning = true;
}

/**
 * @brief Starts the auto-discovery receiver loop
 * 
 * Listens for incoming multicast packets and handles discovery messages.
 * 
 * @param buffer Optional buffer (uses default if not provided)
 */
void Edriel::startAutoDiscoveryReceiver(std::shared_ptr<Buffer> buffer) {
    if (!buffer) {
        buffer = std::make_shared<Buffer>();
    }

    // Registry mutations are serialized on this object's strand so concurrent
    // completions (multi-threaded io_context) cannot race the std::set/map.
    autoDiscoverySocket->async_receive(
        asio::buffer(buffer->data(), recvBufferSize),
        asio::bind_executor(strand,
            [this, buffer](const asio::error_code& ec, std::size_t bytesTransferred) {
                if (!ec && bytesTransferred > 0) {
                    if (hasValidMagicNumber(buffer, bytesTransferred)) {
                        handleAutoDiscoveryReceive(buffer, ec, bytesTransferred);
                    }
                }

                // Keep listening. On abort (socket closed during shutdown) or
                // a closed socket the re-arm would spin the io_context forever
                // (bad-fd completes immediately), so stop instead.
                if (ec == asio::error::operation_aborted || !isRunning
                    || !autoDiscoverySocket->is_open()) {
                    return;
                }
                startAutoDiscoveryReceiver(buffer);
            }));
}

void Edriel::postOnStrand(std::function<void()> thunk) {
    asio::post(strand, std::move(thunk));
}

/**
 * @brief Starts periodic discovery message sender
 * 
 * Every autoDiscoverySendPeriod seconds (default 2s), sends a discovery
 * heartbeat packet containing our participant information to the multicast group.
 * 
 * The whole send-rearm cycle runs on the strand so the shared socket and the
 * reusable discoveryPacket member are never touched concurrently from another
 * thread (e.g. a sendPacket posted by the public API).
 */
void Edriel::startAutoDiscoverySender() {
    postOnStrand([this] {
        // Serialize discovery message
        discoveryMessage.SerializeToString(&discoveryPacket);
        prependMagicNumberToPacket(discoveryPacket);

        // Set up async send operation
        autoDiscoverySocket->async_send_to(
            asio::buffer(discoveryPacket),
            multicastEndpoint,
            asio::bind_executor(strand,
                [this](const asio::error_code& ec,
                       std::size_t /*bytesTransferred*/) {
                    if (ec) {
                        std::cerr << "[Edriel] Discovery send failed: "
                                  << ec.message() << "\n";
                    }

                    // Schedule next send
                    autoDiscoverySendTimer->expires_after(autoDiscoverySendPeriod);
                    autoDiscoverySendTimer->async_wait(
                        asio::bind_executor(strand,
                            [this](const asio::error_code& ec) {
                                if (!ec) {
                                    startAutoDiscoverySender();
                                }
                            }));
                }));
    });
}

/**
 * @brief Starts periodic participant cleanup timer
 * 
 * Every autoDiscoveryCleanUpPeriod seconds (default 5s), checks which participants
 * have timed out and removes them from the registry.
 */
void Edriel::startAutoDiscoveryCleaner() {
    postOnStrand([this] {
        removeTimedOutParticipants();  // Initial cleanup

        autoDiscoveryCleanUpTimer->expires_after(autoDiscoveryCleanUpPeriod);
        autoDiscoveryCleanUpTimer->async_wait(
            asio::bind_executor(strand,
                [this](const asio::error_code& ec) {
                    if (!ec) {
                        startAutoDiscoveryCleaner();  // Reschedule
                    }
                }));
    });
}

// ============================================================================
// Message Handling
// ============================================================================

/**
 * @brief Handles received auto-discovery packets
 * 
 * Parses the discovery message, extracts participant information,
 * and registers or updates participants in the registry.
 * 
 * @param buffer Shared pointer to receive buffer
 * @param ec ASIO error code
 * @param bytesTransferred Number of bytes received
 */
void Edriel::handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer, 
                                        const asio::error_code& ec, 
                                        std::size_t bytesTransferred) {
    if (ec || bytesTransferred == 0) {
        return;
    }
    
    // Parse discovery message
    autoDiscovery::Message receivedMessage;
    if (!receivedMessage.ParseFromArray(buffer->data() + magicNumberSize, 
                                        bytesTransferred - magicNumberSize)) {
        // Failed to parse protobuf message
        return;
    }
    handleAutoDiscoveryParse(receivedMessage);
}

/**
 * @brief Dispatches a parsed discovery message by content type (oneof)
 *
 * @param receivedMessage Parsed protobuf message
 */
void Edriel::handleAutoDiscoveryParse(const autoDiscovery::Message& receivedMessage) {
    // Handle based on message content type (oneof)
    if (receivedMessage.has_identifier()) {

        const auto& id = receivedMessage.identifier();
        handleParticipantHeartbeat(id.pid(), id.tid(), id.uid());
    } else if (receivedMessage.has_data_message()) {
        const auto& data = receivedMessage.data_message();
        const auto& id = data.identifier();
        handleParticipantHeartbeat(id.pid(), id.tid(), id.uid());
        handleDataMessageReceive(data);
    } else if (receivedMessage.has_advertisement()) {
        const auto& ad = receivedMessage.advertisement();
        const auto& id = ad.identifier();
        const auto& topic = ad.topic();
        handleParticipantHeartbeat(id.pid(), id.tid(), id.uid());
        handleTopicAnnouncement(
            id.pid(), id.tid(), id.uid(),
            topic.topic_name(),
            topic.message_type(),
            topic.is_publisher());
    }
}

/**
 * @brief Handles incoming participant heartbeat
 * 
 * Updates or creates participant entry in registry based on heartbeat data.
 * 
 * @param pid Participant ID
 * @param tid Transaction ID
 * @param uid Unique identifier
 */
void Edriel::handleParticipantHeartbeat(unsigned long pid, uint64_t tid, uint64_t uid) {
    // Multicast loopback is enabled, so the node receives its own discovery
    // packets every send period. Never register ourselves: the uid is a
    // process-unique random token, so skipping it is a reliable self-filter.
    if (uid == selfParticipant.uid) {
        return;
    }

    std::lock_guard<std::mutex> lock(stateMutex);

    // Check if participant already exists
    auto it = std::find_if(
        participants.begin(),
        participants.end(),
        [pid, tid, uid](const Participant& p) {
            return p.pid == pid && p.tid == tid && p.uid == uid;
        });
    
    if (it == participants.end()) {
        // New participant, create entry
        Participant newParticipant(pid, tid, uid);
        
        // Set initial timestamp
        newParticipant.lastSeen = std::chrono::steady_clock::now();
        
        participants.insert(newParticipant);
    } else {
        // Existing participant, update timestamp
        it->updateLastSeen();
    }
}

/**
 * @brief Removes timed-out participants from registry
 * 
 * Iterates through all participants and removes those where the timeout
 * period has elapsed since the last heartbeat.
 */
void Edriel::removeTimedOutParticipants() {
    std::lock_guard<std::mutex> lock(stateMutex);

    // Find participants to remove
    auto it = participants.begin();
    while (it != participants.end()) {
        if (it->shouldBeRemoved()) {
            const Participant stale = *it;

            // Stale topic purge: drop this participant from every registry
            // entry so a peer that times out no longer appears as a publisher
            // or subscriber of any topic.
            for (auto& kv : topicRegistry) {
                TopicEntry& entry = kv.second;
                entry.publishers.erase(stale);
                entry.subscribers.erase(stale);
            }

            // Remove and advance
            it = participants.erase(it);
        } else {
            ++it;
        }
    }

    // Drop registry entries left with no local callbacks and no remote peers.
    for (auto entry = topicRegistry.begin(); entry != topicRegistry.end(); ) {
        if (entry->second.publishers.empty()
            && entry->second.subscribers.empty()
            && entry->second.callbacks.empty()) {
            entry = topicRegistry.erase(entry);
        } else {
            ++entry;
        }
    }
}

/**
 * @brief Handles topic announcements in discovery packets
 * 
 * Registers or updates topic information for the announcing participant.
 * 
 * @param pid Sender participant ID
 * @param tid Sender transaction ID
 * @param uid Sender unique ID
 * @param topicName Topic name
 * @param messageType Message type
 * @param isPublisher Whether announcing as publisher
 */
void Edriel::handleTopicAnnouncement(unsigned long pid,
                                     uint64_t tid,
                                     uint64_t uid,
                                     const std::string& topicName,
                                     const std::string& messageType,
                                     bool isPublisher) {

    // Loopback delivers our own topic advertisements back to us; never record
    // ourselves as a peer publisher/subscriber in the registry.
    if (uid == selfParticipant.uid) {
        return;
    }

    TopicInfo topicInfo(topicName, messageType);

    Participant remote(pid, tid, uid);

    std::lock_guard<std::mutex> lock(stateMutex);
    TopicEntry& entry = topicRegistry[topicInfo.key];

    entry.topicName = topicName;
    entry.messageType = messageType;

    if (isPublisher) {
        entry.publishers.insert(remote);
    } else {
        entry.subscribers.insert(remote);
    }

    // Drop registry entries with no remaining local interest and no remote peers.
    if (entry.publishers.empty() && entry.subscribers.empty()
        && entry.callbacks.empty()) {
        topicRegistry.erase(topicInfo.key);
    }
}

/**
 * @brief Handles a received data message
 *
 * Demuxes by composite key (topic name + message type) and invokes all
 * locally registered typed subscriber callbacks for the topic whose declared
 * message type matches. Callbacks run on this object's strand.
 *
 * @param data Parsed DataMessage from the wire
 */
void Edriel::handleDataMessageReceive(const autoDiscovery::DataMessage& data) {
    const std::string key = makeCompositeKey(data.topic_name(), data.message_type());

    // Collect the matching callbacks under the lock, then invoke them after
    // releasing it so user code (a callback) may safely re-enter the public API.
    std::vector<TopicEntry::Callback> matched;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto it = topicRegistry.find(key);
        if (it == topicRegistry.end()) {
            return;  // No local subscription for this topic
        }
        for (const auto& cb : it->second.callbacks) {
            if (cb.invoke && cb.messageType == data.message_type()) {
                matched.push_back(cb);
            }
        }
    }

    const google::protobuf::Message* prototype = nullptr;
    for (auto& cb : matched) {
        if (prototype == nullptr) {
            // Resolve the payload prototype once per message, not once per
            // callback — the generated-pool lookup is a hot-path cost.
            const google::protobuf::Descriptor* descriptor =
                google::protobuf::DescriptorPool::generated_pool()->
                    FindMessageTypeByName(data.message_type());
            if (descriptor == nullptr) {
                std::cerr << "[Edriel] Unknown payload type: "
                          << data.message_type() << "\n";
                return;
            }
            prototype =
                google::protobuf::MessageFactory::generated_factory()->GetPrototype(descriptor);
        }
        std::unique_ptr<google::protobuf::Message> decoded(prototype->New());
        if (!decoded->ParseFromString(data.payload())) {
            std::cerr << "[Edriel] Failed to decode payload for topic "
                      << data.topic_name() << "\n";
            continue;
        }
        cb.invoke(*decoded);
    }
}

/**
 * @brief Prepends magic number and multicasts a serialized protobuf message
 *
 * MTU note: the receive path reads at most recvBufferSize (1500) bytes per
 * datagram — one classic Ethernet frame without VLAN tagging. UDP datagrams
 * larger than the socket buffer are truncated silently, so any outgoing
 * packet exceeding the budget is rejected here rather than sent.
 *
 * @param message Protobuf message to send
 * @return true if the send was dispatched successfully
 */
bool Edriel::sendPacket(const google::protobuf::Message& message) {
    std::string packet;
    if (!message.SerializeToString(&packet)) {
        std::cerr << "[Edriel] Failed to serialize outgoing message\n";
        return false;
    }
    prependMagicNumberToPacket(packet);

    if (packet.size() > recvBufferSize) {
        std::cerr << "[Edriel] Outgoing packet of " << packet.size()
                  << " bytes exceeds " << recvBufferSize
                  << "-byte MTU budget, dropping\n";
        return false;
    }

    // The socket is shared with the strand-confined discovery sender/receiver,
    // so the actual async_send_to is initiated on the strand. Serialization and
    // the MTU check above happen synchronously on the calling thread (they only
    // touch the local message), so a non-strand thread can safely publish. The
    // buffer is copied and kept alive by the strand task so no member data is
    // aliased across the send.
    auto sharedPacket = std::make_shared<std::string>(std::move(packet));
    postOnStrand([this, sharedPacket] {
        autoDiscoverySocket->async_send_to(
            asio::buffer(sharedPacket->data(), sharedPacket->size()),
            multicastEndpoint,
            asio::bind_executor(strand,
                [this, sharedPacket](const asio::error_code& ec,
                                     std::size_t /*bytesTransferred*/) {
                    if (ec) {
                        std::cerr << "[Edriel] Failed to send packet: "
                                  << ec.message() << "\n";
                    }
                }));
    });
    return true;
}

/**
 * @brief Serializes and multicasts a DataMessage envelope
 *
 * @param topicName Topic name to publish under
 * @param messageType Protobuf full name of payload type
 * @param payload Serialized user message bytes
 * @return true if sending succeeded
 */
bool Edriel::publishData(const std::string& topicName, const std::string& messageType,
                         const std::string& payload) {
    autoDiscovery::Message envelope;
    autoDiscovery::DataMessage* data = envelope.mutable_data_message();
    data->mutable_identifier()->set_pid(selfParticipant.pid);
    data->mutable_identifier()->set_tid(selfParticipant.tid);
    data->mutable_identifier()->set_uid(selfParticipant.uid);
    data->set_topic_name(topicName);
    data->set_message_type(messageType);
    data->set_payload(payload);
    return sendPacket(envelope);
}

// ============================================================================
// Public API: Auto-Discovery Control
// ============================================================================

/**
 * @brief Starts auto-discovery receiver/sender loop
 * 
 * Initializes all timers and starts the discovery process.
 */
void Edriel::startAutoDiscovery() {
    if (!isRunning) {
        initializeAutoDiscovery();
    }
}

/**
 * @brief Stops auto-discovery and cleans up resources
 * 
 * Stops all timers, closes socket, and sets running flag to false.
 */
void Edriel::stopAutoDiscovery() {
    isRunning = false;
    
    // Cancel timers
    if (autoDiscoverySendTimer) {
        autoDiscoverySendTimer->cancel();
    }
    
    if (autoDiscoveryCleanUpTimer) {
        autoDiscoveryCleanUpTimer->cancel();
    }
    
    // Close socket
    if (autoDiscoverySocket && autoDiscoverySocket->is_open()) {
        asio::error_code ec;
        autoDiscoverySocket->close(ec);
    }
    
    std::cout << "[Edriel] Auto-discovery stopped\n";
}

// ============================================================================
// Public API: Topic Registration (C++20 templates)
// ============================================================================
// The topic-registration and send APIs are templates parameterized on the
// user's protobuf message type; their definitions appear at the end of this
// header so they are visible at every instantiation site — including types
// generated from user .proto files via edriel_add_proto_messages(). No
// explicit instantiations are needed.

}  // namespace edriel
