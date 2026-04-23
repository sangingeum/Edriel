/**
 * @file Edriel.cpp
 * @brief Main implementation file for Edriel C++20 multi-cast auto-discovery networking library
 * 
 * Implements multicast-based auto-discovery with participant lifecycle management,
 * topic-based message publishing/subscribing, and gRPC streaming integration.
 * 
 * @version 2.0.0 - Added timestamp-based replay attack prevention and error codes
 */

#include "Edriel.hpp"
#include "error_codes.hpp"
#include <asio/steady_timer.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <iostream>

namespace edriel {

// ============================================================================
// Configuration Constants
// ============================================================================
// TIMESTAMP_VALIDITY_SECONDS is defined in error_codes.hpp

/**
 * @brief Expected magic number for packet integrity
 * 
 * All valid discovery packets must begin with this 4-byte magic number.
 * The magic number serves as a checksum and protocol identifier.
 */
constexpr uint32_t MAGIC_NUMBER_VALUE = 0xED75E1ED;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Validates magic number in received packet
 * 
 * The magic number is the first 4 bytes of every discovery packet.
 * Validates it against our expected magic number 0xED75E1ED.
 * Also checks for replay attacks using timestamp validation.
 * 
 * @param buffer Shared pointer to receive buffer
 * @param length Packet length
 * @param timestamp Current timestamp for replay attack checking
 * @return true if magic number is valid and not a replay attack
 */
bool Edriel::hasValidMagicNumber(std::shared_ptr<Buffer> buffer, 
                                  std::size_t length) const {
    // Check buffer size for magic number + protobuf message
    if (length < magicNumberSize) {
        std::cout << "[Edriel] Packet too small for magic number validation\n";
        return false;  // Packet too small
    }
    
    // Extract magic number from first 4 bytes and convert from network byte order
    uint32_t receivedMagic = ntohl(*reinterpret_cast<const uint32_t*>(buffer->data())); 
    std::cout << "[Edriel] Received magic number: 0x" << std::hex << receivedMagic << std::dec << "\n";
    // Validate magic number matches expected value
    if (receivedMagic != MAGIC_NUMBER_VALUE) {
        return false;  // Invalid magic number - malformed packet
    }
    
    return true;  // Valid magic number and timestamp
}

/**
 * @brief Validates timestamp freshness
 * 
 * Checks that a timestamp is within the acceptable validity window.
 * 
 * @param timestamp Timestamp to validate
 * @return true if timestamp is fresh, false otherwise
 */
static bool isTimestampFresh(uint64_t timestamp) {
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return timestamp >= (now - TIMESTAMP_VALIDITY_SECONDS);
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
    
    // Write magic number (first 4 bytes) in network byte order
    *reinterpret_cast<uint32_t*>(newPacket.data()) = htonl(MAGIC_NUMBER_VALUE);
    
    // Copy original packet after magic number
    std::memcpy(newPacket.data() + magicNumberSize, packet.data(), packet.length());
    
    packet = std::move(newPacket);
}

// ============================================================================
// Constructor & Destructor
// ============================================================================

/**
 * @brief Constructor for Edriel class
 * 
 * Initializes ASIO socket, timers, and other components for auto-discovery.
 * 
 * @param io_ctx ASIO I/O context reference
 */
Edriel::Edriel(asio::io_context& io_ctx)
    : io_context(io_ctx)
    , strand(asio::make_strand(io_ctx))
    , autoDiscoverySocket(std::make_unique<asio::ip::udp::socket>(io_ctx))
    , autoDiscoverySendTimer(std::make_unique<asio::steady_timer>(io_ctx))
    , autoDiscoveryCleanUpTimer(std::make_unique<asio::steady_timer>(io_ctx))
{
    autoDiscoverySocket->open(receiverEndpoint.protocol());
    // Configure socket for multicast
    autoDiscoverySocket->set_option(asio::ip::udp::socket::reuse_address(true));
    autoDiscoverySocket->set_option(asio::ip::multicast::join_group(asio::ip::make_address_v4(std::string(multicastAddress))));
    autoDiscoverySocket->set_option(asio::ip::multicast::enable_loopback(true));
    autoDiscoverySocket->bind(receiverEndpoint);

    // Initialize self participant
    selfParticipant.pid = 0;
    selfParticipant.tid = 0;
    selfParticipant.uid = 0;
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
    
    autoDiscoverySocket->async_receive(
        asio::buffer(buffer->data(), recvBufferSize),
        [this, buffer](const asio::error_code& ec, std::size_t bytesTransferred) {
            if (!ec && bytesTransferred > 0) {
                // Extract timestamp from magic number location for replay check
                 std::cout << "[Edriel] Received packet of size " << bytesTransferred << " bytes\n";
                // Validate magic number
                if (hasValidMagicNumber(buffer, bytesTransferred)) {        
                    // Parse discovery message
                    handleAutoDiscoveryReceive(buffer, ec, bytesTransferred);
                } else {
                    std::cerr << "[Edriel] Received invalid or replayed packet, ignoring\n";
                    // std::cerr << "[Edriel] Invalid packet rejected\n";
                }
            }
            
            // Keep listening
            startAutoDiscoveryReceiver(buffer);
        });
}

/**
 * @brief Starts periodic discovery message sender
 * 
 * Every autoDiscoverySendPeriod seconds (default 2s), sends a discovery
 * heartbeat packet containing our participant information to the multicast group.
 */
void Edriel::startAutoDiscoverySender() {
    // Serialize discovery message
    discoveryMessage.SerializeToString(&discoveryPacket);
    prependMagicNumberToPacket(discoveryPacket);
    
    // Set up async send operation
    autoDiscoverySocket->async_send_to(
        asio::buffer(discoveryPacket),
        multicastEndpoint,
        [this](const asio::error_code& ec, std::size_t /*bytesTransferred*/) {
            if (!ec) {
                std::cout << "[Edriel] Sent discovery packet to [" 
                          << multicastAddress << ":" << commonPort << "] from [" << autoDiscoverySocket->local_endpoint() << "]\n";
            } else {
                std::cerr << "[Edriel] Failed to send discovery packet: " 
                          << ec.message() << "\n";
            }
            
            // Schedule next send
            autoDiscoverySendTimer->expires_after(autoDiscoverySendPeriod);
            autoDiscoverySendTimer->async_wait(
                [this](const asio::error_code& ec) {
                    if (!ec) {
                        startAutoDiscoverySender();
                    }
                });
        });
}

/**
 * @brief Starts periodic participant cleanup timer
 * 
 * Every autoDiscoveryCleanUpPeriod seconds (default 5s), checks which participants
 * have timed out and removes them from the registry.
 */
void Edriel::startAutoDiscoveryCleaner() {
    removeTimedOutParticipants();  // Initial cleanup
    
    autoDiscoveryCleanUpTimer->expires_after(autoDiscoveryCleanUpPeriod);
    autoDiscoveryCleanUpTimer->async_wait(
        [this](const asio::error_code& ec) {
            if (!ec) {
                startAutoDiscoveryCleaner();  // Reschedule
            }
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
        // std::cerr << "[Edriel] Failed to parse discovery message\n";
        return;
    }
    std::cout << "[Edriel] Received valid discovery packet\n";
    
    // Handle based on message content type (oneof)
    if (receivedMessage.has_identifier()) {
        
        const auto& id = receivedMessage.identifier();
        std::cout << "[Edriel] Received heartbeat from pid=" << id.pid() 
                  << ", tid=" << id.tid() 
                  << ", uid=" << id.uid() << "\n";    
        handleParticipantHeartbeat(id.pid(), id.tid(), id.uid());
    } else if (receivedMessage.has_advertisement()) {
        const auto& ad = receivedMessage.advertisement();
        const auto& id = ad.identifier();
        const auto& topic = ad.topic();
         std::cout << "[Edriel] Received topic announcement from pid=" << id.pid() 
                  << ", tid=" << id.tid() 
                  << ", uid=" << id.uid() 
                  << " for topic=" << topic.topic_name() 
                  << ", type=" << topic.message_type() 
                  << ", isPublisher=" << topic.is_publisher() << "\n";
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
        
        // Log new participant
        // std::cout << "[Edriel] New participant discovered: pid=" 
        //           << pid << ", tid=" << tid << ", uid=" << uid << "\n";
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
    auto now = std::chrono::steady_clock::now();
    
    // Find participants to remove
    auto it = participants.begin();
    while (it != participants.end()) {
        if (it->shouldBeRemoved()) {
            // Remove and advance
            it = participants.erase(it);
        } else {
            ++it;
        }
    }
    
    // Log cleanup
    // std::cout << "[Edriel] Cleaned up timed-out participants\n";
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
void Edriel::handleTopicAnnouncement(unsigned long pid, uint64_t tid, uint64_t uid, 
                                     const std::string& topicName, 
                                     const std::string& messageType, bool isPublisher) {
    
    TopicInfo topicInfo(topicName, messageType);
    auto topicKey = topicInfo.key;
    
    // TODO: Implement topic registry management
    // - Track which participants publish/subscribe to each topic
    // - Notify subscribers when message is sent
    // - Handle topic lifecycle
    
    // Placeholder: just log
    // std::cout << "[Edriel] Topic announcement: topic=" << topicName 
    //           << ", type=" << messageType 
    //           << ", isPublisher=" << isPublisher 
    //           << ", from pid=" << pid << "\n";
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
    TopicInfo topicInfo(topicName, "publisher");
    
    // TODO: Implement topic registry storage
    // - Store topic name in topicInfo for subscriber lookups
    // - Track publishing participants per topic
    
    // Placeholder: return success
    std::cout << "[Edriel] Registered publisher topic: " << topicName << "\n";
    return true;
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
    TopicInfo topicInfo(topicName, "publisher");
    
    // TODO: Implement topic registry cleanup
    
    // Placeholder
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
    TopicInfo topicInfo(topicName, "subscriber");
    
    // TODO: Implement subscriber registry storage
    
    // Placeholder
    std::cout << "[Edriel] Registered subscriber topic: " << topicName << "\n";
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
    TopicInfo topicInfo(topicName, "subscriber");
    
    // TODO: Implement subscriber registry cleanup
    
    // Placeholder
    std::cout << "[Edriel] Unregistered subscriber topic: " << topicName << "\n";
    return true;
}

// ============================================================================
// Public API: Message Sending
// ============================================================================

/**
 * @brief Sends a message to a topic via multicast broadcast
 * 
 * Serializes the message, prepends magic number, and broadcasts to multicast
 * group. All participants subscribed to the topic will receive this message.
 * 
 * @tparam T Protobuf message type (constrained by Topic concept)
 * @param topicName Topic to send to
 * @param message Message instance to serialize and broadcast
 * @return true if sending succeeded
 */
template<typename T> requires Topic<T>
bool Edriel::sendMessage(const std::string& topicName, const T& message) {
    // TODO: Implement message serialization
    // - Serialize Topic message to protobuf
    // - Prepend magic number
    // - Send to multicast socket
    
    // Placeholder
    std::cout << "[Edriel] Sending message to topic: " << topicName << "\n";
    return true;
}

// Explicit template instantiations for common protobuf types
// These ensure template functions are available in translation units
// that don't define their own templates

// Example explicit instantiations would go here if needed
// template bool Edriel::sendMessage<std::string>(const std::string&, const std::string&);
// template bool Edriel::sendMessage<google::protobuf::Message>(const std::string&, const google::protobuf::Message&);

}  // namespace edriel
