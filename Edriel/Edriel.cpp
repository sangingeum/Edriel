#include "Edriel.hpp"

#include <string>
#include <iostream>
#include <functional>
#include <array>
#include <cstddef>
#include <mutex>
#include <bit>
#include <set>


void Edriel::handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer, const asio::error_code& ec, std::size_t bytesTransferred) {
    if (!ec) {
        // Handle magic number
        if(!hasValidMagicNumber(buffer, bytesTransferred)){
            std::cerr << "Invalid magic number received. Discarding packet.\n";
            startAutoDiscoveryReceiver(buffer);  // continue looping
            return;
        }
        autoDiscovery::Message receivedMessage;
        if(receivedMessage.ParseFromArray(std::next(buffer->data(), magicNumberSize), static_cast<int>(bytesTransferred - magicNumberSize))) {
            switch (receivedMessage.content_case())
            {
            case autoDiscovery::Message::kIdentifier:{
                // Handle identifier
                autoDiscovery::Identifier parsedMessage = receivedMessage.identifier();
                std::cout << "[Recv] PID: " << parsedMessage.pid()
                          << ", TID: " << parsedMessage.tid()
                          << ", UID: " << parsedMessage.uid() << '\n';
                handleParticipantHeartbeat(parsedMessage.pid(), parsedMessage.tid(), parsedMessage.uid());
                break;
            }
            case autoDiscovery::Message::kTopic:{
                // Handle topic
                autoDiscovery::Topic parsedMessage = receivedMessage.topic();
                std::cout << "[Recv] Topic Name: " << parsedMessage.topic_name()
                          << ", Message Type: " << parsedMessage.message_type() << '\n';
                break;
            }
            default:
                break;
            }
        } else {
            std::cerr << "Failed to parse received message.\n";
        }
    } else {
        std::cerr << "Receive error: " << ec.message() << '\n';
    }
    startAutoDiscoveryReceiver(buffer);  // continue looping
}

void Edriel::startAutoDiscoveryReceiver(std::shared_ptr<Buffer> buffer) {
    if(!autoDiscoverySocket || !autoDiscoverySendTimer) {
        std::cerr << "Auto-discovery components are not initialized.\n";
        return;
    }

    if(autoDiscoverySocket->is_open() == false || !isRunning.load()){
        std::cout << "Receiver stopped.\n";
        return;
    }

    autoDiscoverySocket->async_receive_from(
        asio::buffer(buffer->data(), buffer->size()), senderEndpoint, 
        asio::bind_executor(strand, std::bind(&Edriel::handleAutoDiscoveryReceive, this, buffer, std::placeholders::_1, std::placeholders::_2)));
}

void Edriel::startAutoDiscoverySender() {
    // Send Auto-Discovery Packet ------------------------
    if(!autoDiscoverySocket || !autoDiscoverySendTimer) {
        std::cerr << "Auto-discovery components are not initialized.\n";
        return;
    }

    if(autoDiscoverySocket->is_open() == false || !isRunning.load()){
        std::cout << "Sender stopped.\n";
        return;
    }

    autoDiscoverySendTimer->expires_after(autoDiscoverySendPeriod);
    autoDiscoverySendTimer->async_wait([this](const asio::error_code& ec) {
        if (!ec) {
            // Send discovery packet to multicast group
            autoDiscoverySocket->async_send_to(
                asio::buffer(discoveryPacket, discoveryPacket.size()), multicastEndpoint,
                [](const asio::error_code& ec, std::size_t /*n*/) {
                    if (ec) std::cerr << "Send failed: " << ec.message() << '\n';
                });
            // Send topic packet (TODO)
            autoDiscovery::Message topicMessage;
            auto* topic = topicMessage.mutable_topic(); // ensure the oneof is set to topic
            topic->set_topic_name("example/topic-name"); // example topic name
            topic->set_message_type("example.MessageType"); // example message type
            std::shared_ptr<std::string> topicPacket = std::make_shared<std::string>(topicMessage.SerializeAsString());
            // Attach magic number at the beginning
            prependMagicNumberToPacket(*topicPacket);
            // Send topic packet to multicast group
            autoDiscoverySocket->async_send_to(
                asio::buffer(*topicPacket, topicPacket->size()), multicastEndpoint,
                [topicPacket](const asio::error_code& ec, std::size_t /*n*/) {
                    if (ec) std::cerr << "Send failed: " << ec.message() << '\n';
                });
            startAutoDiscoverySender();  // keep sending
        } else {
            std::cerr << "Timer error: " << ec.message() << '\n';
        }
    });
}

void Edriel::startAutoDiscoveryCleaner() {
    // Clean up timed-out participants ------------------------
    if(!autoDiscoveryCleanUpTimer) {
        std::cerr << "Auto-discovery cleaner timer is not initialized.\n";
        return;
    }

    autoDiscoveryCleanUpTimer->expires_after(autoDiscoveryCleanUpPeriod);
    autoDiscoveryCleanUpTimer->async_wait(asio::bind_executor(strand, [this](const asio::error_code& ec) {
        if (!ec) {
            removeTimedOutParticipants();
            startAutoDiscoveryCleaner();  // keep cleaning
        } else {
            std::cerr << "Cleaner timer error: " << ec.message() << '\n';
        }
    }));
}

void Edriel::stopAutoDiscoverySocketAndTimer(){
    asio::error_code ec;
    if(autoDiscoverySocket && autoDiscoverySocket->is_open()) {
        autoDiscoverySocket->close(ec);
        if (ec) {
            std::cerr << "Error closing existing socket: " << ec.message() << '\n';
        }
    }
    if(autoDiscoverySendTimer){
        autoDiscoverySendTimer->cancel();
        if (ec) {
            std::cerr << "Error cancelling existing timer: " << ec.message() << '\n';
        }
    }
    if(autoDiscoveryCleanUpTimer){
        autoDiscoveryCleanUpTimer->cancel();
        if (ec) {
            std::cerr << "Error cancelling existing timer: " << ec.message() << '\n';
        }
    }   
}

void Edriel::initializeAutoDiscovery() {
    // Clean up existing socket and timer if any ------------------------
    stopAutoDiscoverySocketAndTimer();
    // Create socket and timer ---------------------------------------------
    autoDiscoverySocket = std::make_unique<asio::ip::udp::socket>(io_context);
    autoDiscoverySendTimer = std::make_unique<asio::steady_timer>(io_context);
    autoDiscoveryCleanUpTimer = std::make_unique<asio::steady_timer>(io_context);   
    // Socket options ---------------------------------------------------
    autoDiscoverySocket->open(receiverEndpoint.protocol());
    autoDiscoverySocket->set_option(asio::socket_base::reuse_address(true));
    autoDiscoverySocket->set_option(asio::ip::multicast::enable_loopback(true));
    // Socket bind ------------------------------------------------------
    autoDiscoverySocket->bind(receiverEndpoint);
    
    // Join multicast group ---------------------------------------------
    try {
        autoDiscoverySocket->set_option(
            asio::ip::multicast::join_group(
                asio::ip::address::from_string(std::string(multicastAddress))));
    } catch (const std::exception& e) {
        std::cerr << "Join Group Failed: " << e.what() << '\n';
    }
}

void Edriel::handleParticipantHeartbeat(unsigned long pid, uint64_t tid, uint64_t uid){
    Participant incomingParticipant(pid, tid, uid);
    if(incomingParticipant == selfParticipant){
        // Ignore self messages
        return;
    }

    auto it = participants.find(incomingParticipant);
    if(it != participants.end()){
        // Update last seen timestamp
        it->updateLastSeen();
    } else {
        // New participant
        participants.insert(incomingParticipant);
        std::cout << "New participant added: PID: " << pid
                  << ", TID: " << tid
                  << ", UID: " << uid << '\n';
    }
}

void Edriel::removeTimedOutParticipants(){
    for(auto it = participants.begin(); it != participants.end(); ) {
        if(it->shouldBeRemoved()) {
            std::cout << "Removing timed-out participant PID: " << it->pid
                      << ", TID: " << it->tid
                      << ", UID: " << it->uid << '\n';
            it = participants.erase(it);
        } else {
            ++it;
        }
    }
}


Edriel::Edriel(asio::io_context& io_ctx)
        : io_context{io_ctx},
          strand{asio::make_strand(io_ctx)}
{
    thread_local static std::random_device                      rd;
    thread_local static std::mt19937_64                         generator(rd());
    thread_local static std::uniform_int_distribution<uint64_t> dist(0, std::numeric_limits<uint64_t>::max());
    std::cout << "Initializing Edriel...\n";
    // Initialize Discovery Packet --------------------------------
    auto* identifier = discoveryMessage.mutable_identifier(); // ensure the oneof is set to identifier
    identifier->set_pid(static_cast<unsigned long>(::_getpid()));
    identifier->set_tid(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    identifier->set_uid(dist(generator));
    discoveryPacket = discoveryMessage.SerializeAsString();
    selfParticipant = Participant(identifier->pid(), identifier->tid(), identifier->uid());
    // Attach magic number at the beginning
    prependMagicNumberToPacket(discoveryPacket);
    std::cout << "Edriel initialized.\n";
}

void Edriel::startAutoDiscovery() {
    std::scoped_lock<std::mutex> lock(runnerMutex);
    if(isRunning.load()) return;
    std::cout << "Starting Auto-Discovery...\n";
    isRunning.store(true);
    // Initialize Auto-Discovery --------------------------------
    initializeAutoDiscovery();
    // Start Sender and Receiver -------------------------------
    startAutoDiscoverySender();
    startAutoDiscoveryReceiver();
    startAutoDiscoveryCleaner();
}

void Edriel::stopAutoDiscovery() {
    std::scoped_lock<std::mutex> lock(runnerMutex);
    if(!isRunning.load()) return;
    std::cout << "Stopping Auto-Discovery...\n";
    isRunning.store(false);
    stopAutoDiscoverySocketAndTimer();
}

Edriel::~Edriel() { stopAutoDiscovery(); }

bool Edriel::hasValidMagicNumber(std::shared_ptr<Buffer> buffer, std::size_t length) const {
    if (length < magicNumberSize) return false;
    auto rawBytes = *reinterpret_cast<const std::array<char, magicNumberSize>*>(buffer->data());
    uint32_t receivedMagicNumber = ntohl(std::bit_cast<uint32_t>(rawBytes));
    return receivedMagicNumber == magicNumber;
}

void Edriel::prependMagicNumberToPacket(std::string& packet) const {
    uint32_t networkMagicNumber = htonl(magicNumber);
    packet.insert(0, reinterpret_cast<const char*>(&networkMagicNumber), sizeof(networkMagicNumber));
}