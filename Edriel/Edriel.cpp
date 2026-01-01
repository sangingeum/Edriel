#include "Edriel.hpp"

bool Edriel::hasValidMagicNumber(std::shared_ptr<Buffer> buffer, std::size_t length) const {
    if (length < magicNumberSize) return false;
    auto rawBytes = *reinterpret_cast<const std::array<char, magicNumberSize>*>(buffer->data());
    uint32_t receivedMagicNumber = ntohl(std::bit_cast<uint32_t>(rawBytes));
    return receivedMagicNumber == magicNumber;
}

void Edriel::handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer, const asio::error_code& ec, std::size_t bytesTransferred) {
    if (!ec) {
        // Handle magic number
        if(!hasValidMagicNumber(buffer, bytesTransferred)){
            std::cerr << "Invalid magic number received. Discarding packet.\n";
            startAutoDiscoveryReceiver(buffer);  // continue looping
            return;
        }
    
        autoDiscovery::Identifier receivedMessage;
        if(receivedMessage.ParseFromArray(buffer->data() + magicNumberSize, static_cast<int>(bytesTransferred - magicNumberSize))) {
            std::cout << "[Recv] PID: " << receivedMessage.pid()
                        << ", TID: " << receivedMessage.tid()
                        << ", UID: " << receivedMessage.uid() << '\n';
            handleParticipantHeartbeat(receivedMessage.pid(), receivedMessage.tid(), receivedMessage.uid());
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
            autoDiscoverySocket->async_send_to(
                asio::buffer(discoveryPacket, discoveryPacket.size()), multicastEndpoint,
                [](const asio::error_code& ec, std::size_t /*n*/) {
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
    discoveryMessage.set_pid(static_cast<unsigned long>(::_getpid()));
    discoveryMessage.set_tid(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    discoveryMessage.set_uid(dist(generator));
    discoveryPacket = discoveryMessage.SerializeAsString();
    selfParticipant = Participant(discoveryMessage.pid(), discoveryMessage.tid(), discoveryMessage.uid());
    // Attach magic number at the beginning
    uint32_t networkMagicNumber = htonl(magicNumber);
    discoveryPacket.insert(0, reinterpret_cast<const char*>(&networkMagicNumber), sizeof(networkMagicNumber));

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


