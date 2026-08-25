/**
 * @file Edriel.cpp
 * @brief Main implementation file for Edriel C++20 multi-cast auto-discovery networking library
 * 
 * Implements multicast-based auto-discovery with participant lifecycle management,
 * topic-based message publishing/subscribing, and gRPC streaming integration.
 */

#include "Edriel.hpp"
#include "EdrielGrpcService.hpp"
#include "EdrielReliableClient.hpp"
#include <asio/steady_timer.hpp>
#include <google/protobuf/descriptor.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <fstream>
#include <random>
#include <limits>

#if defined(_WIN32)
#include <process.h>
#include <iphlpapi.h>   // GetAdaptersAddresses (interface discovery)
#include <winsock2.h>
#include <ws2tcpip.h>   // sockaddr_in, ntohl
#else
#include <unistd.h>
#include <ifaddrs.h>    // getifaddrs (interface discovery)
#include <netinet/in.h> // sockaddr_in
#include <arpa/inet.h>  // ntohl, AF_INET on POSIX
#include <sys/time.h>   // struct timeval (SO_RCVTIMEO)
#endif

#include <cstdint>  // std::uint8_t / std::uint32_t

namespace edriel {

// ============================================================================
// Configuration Constants
// ============================================================================

/// Expected magic number for packet integrity verification
constexpr uint32_t MAGIC_NUMBER_VALUE = 0xED75E1ED;

namespace {

/// Give the multicast receive socket a soft receive timeout so the dedicated
/// receiver thread wakes periodically even when idle. This is the shutdown
/// liveness mechanism: closing the socket from another thread does NOT
/// reliably unblock a thread stuck in a blocking asio `receive_from`, so
/// unless we bound the wait, the receiver-thread join at teardown could hang.
/// A short timeout costs nothing here (no data -> EAGAIN -> loop) and makes
/// shutdown deterministic. Platform-guarded (timeval on POSIX, DWORD ms on
/// Windows).
void setSocketReceiveTimeout(asio::ip::udp::socket& sock, unsigned long ms) {
#if defined(_WIN32)
    const DWORD timeoutMs = static_cast<DWORD>(ms);
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    struct timeval tv;
    tv.tv_sec = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/// Receiver wakeup cadence (ms) — the idle loop polls this often; shutdown is
/// observed within ~this interval.
constexpr unsigned long kReceiverPollMs = 300;

}  // namespace

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

// Endpoint candidates this node advertises, and parsing heartbeat endpoints.
// ---------------------------------------------------------------------------

/// Format a host-order IPv4 address as dotted quad (no inet_ntop/ntohl-in-
/// shared-code dependency; kept to library-local, platform-guarded use).
std::string formatIpv4(std::uint32_t hostOrder) {
    return std::to_string((hostOrder >> 24) & 0xFF) + "."
         + std::to_string((hostOrder >> 16) & 0xFF) + "."
         + std::to_string((hostOrder >> 8) & 0xFF) + "."
         + std::to_string(hostOrder & 0xFF);
}

/// Local unicast IPv4 addresses suitable for a unicast gRPC listener — skips
/// the unspecified (0.0.0.0), loopback (127/8), and multicast/reserved
/// (>=224) ranges. Empty on failure; the caller falls back to configured
/// addresses. Platform-guarded: getifaddrs (POSIX) / GetAdaptersAddresses
/// (Windows) so the library stays MSVC-portable.
std::vector<std::string> discoverLocalUnicastIpv4() {
    std::vector<std::string> out;
#if defined(_WIN32)
    ULONG bufsize = 0;
    ::GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &bufsize);
    std::vector<std::uint8_t> buf(bufsize);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (::GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &bufsize) != ERROR_SUCCESS) {
        return out;
    }
    for (auto* a = adapters; a != nullptr; a = a->Next) {
        if (a->OperStatus != I_OPER_UP) {
            continue;
        }
        for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            auto* sa = reinterpret_cast<struct sockaddr_in*>(u->Address.lpSockaddr);
            if (sa == nullptr) {
                continue;
            }
            const std::uint32_t hostOrder = ::ntohl(sa->sin_addr.s_addr);
            const std::uint8_t first = static_cast<std::uint8_t>((hostOrder >> 24) & 0xFF);
            if (first == 127 || first >= 224) {
                continue;  // loopback / multicast
            }
            out.push_back(formatIpv4(hostOrder));
        }
    }
#else
    struct ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) {
        return out;
    }
    for (const struct ifaddrs* p = ifa; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const std::uint32_t hostOrder =
            ::ntohl(reinterpret_cast<const struct sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr);
        const std::uint8_t first = static_cast<std::uint8_t>((hostOrder >> 24) & 0xFF);
        if (first == 0 || first == 127 || first >= 224) {
            continue;  // any / loopback / multicast
        }
        out.push_back(formatIpv4(hostOrder));
    }
    ::freeifaddrs(ifa);
#endif
    return out;
}

/// Assemble the Endpoint candidates this node advertises on its heartbeat
/// (ADR-0002 Channel A): configured `advertise_address` first (deterministic),
/// then discovered interfaces, deduped and capped at
/// `max_advertised_endpoints` (MTU guard so the heartbeat cannot balloon).
std::vector<autoDiscovery::Endpoint> buildSelfEndpoints(const Config& config) {
    std::vector<autoDiscovery::Endpoint> result;
    const auto append = [&result, &config](const std::string& address) {
        if (address.empty() || result.size() >= config.maxAdvertisedEndpoints) {
            return;
        }
        for (const auto& existing : result) {
            if (existing.address() == address && existing.port() == config.grpcPort) {
                return;  // dedupe identical candidate
            }
        }
        autoDiscovery::Endpoint ep;
        ep.set_address(address);
        ep.set_port(config.grpcPort);
        ep.set_transport(autoDiscovery::Endpoint::GRPC_TCP);
        result.push_back(std::move(ep));
    };
    for (const std::string& configured : config.advertiseAddresses) {
        append(configured);
    }
    for (const std::string& discovered : discoverLocalUnicastIpv4()) {
        append(discovered);
    }
    return result;
}

/// Copy an Identifier's repeated endpoints into a standalone vector, used to
/// thread heartbeat-advertised endpoints into the participant registry.
std::vector<autoDiscovery::Endpoint> collectEndpoints(const autoDiscovery::Identifier& id) {
    std::vector<autoDiscovery::Endpoint> out;
    out.reserve(static_cast<std::size_t>(id.endpoints_size()));
    for (int i = 0; i < id.endpoints_size(); ++i) {
        out.push_back(id.endpoints(i));
    }
    return out;
}

/// Turn an advertised Endpoint set into ordered "address:port" dial candidates
/// (multi-homed, tried in order — ADR-0002 §6.3).
std::vector<std::string> endpointCandidates(const std::vector<autoDiscovery::Endpoint>& eps) {
    std::vector<std::string> out;
    out.reserve(eps.size());
    for (const autoDiscovery::Endpoint& ep : eps) {
        out.push_back(ep.address() + ":" + std::to_string(ep.port()));
    }
    return out;
}

/// FNV-1a 64-bit over the endpoint string; the basis for a deterministic
/// (and distinct) synthesized identity for a static seed peer (ADR-0002
/// Channel D), which has no advisory pid/tid/uid of its own.
std::uint64_t fnv1a64(const std::string& s) {
    std::uint64_t h = 14695981039346656037ull;
    for (const char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

/// Deterministic SubscriberKey derived from a static peer "address:port"
/// endpoint, so the reconciliation's (pid,tid,uid) keying stays valid for a
/// config-seeded peer that is never heard on the multicast group. The hash is
/// split into independent pid/high-uid halves to keep distinct endpoints from
/// collapsing onto the same key.
SubscriberKey Edriel::peerKeyForEndpoint(const std::string& endpoint) {
    const std::uint64_t h = fnv1a64(endpoint);
    std::uint32_t pid = static_cast<std::uint32_t>(h >> 1);
    if (pid == 0) {
        pid = 1;  // keep the identity non-zero/null
    }
    std::uint64_t uid = (h << 33) ^ (h >> 31);
    if (uid == 0) {
        uid = 1;
    }
    return SubscriberKey{pid, 0ull, uid};
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
        std::cout << "[Edriel] One or more config.yml values were missing or "
                  << "invalid; using defaults (port=" << config_.port
                  << ", multicast=" << config_.multicastAddress << ")\n";
    }

    autoDiscoverySocket->open(receiverEndpoint.protocol());
    // Configure socket for multicast
    autoDiscoverySocket->set_option(asio::ip::udp::socket::reuse_address(true));
    autoDiscoverySocket->set_option(asio::ip::multicast::join_group(
                                     asio::ip::make_address_v4(config_.multicastAddress)));
    autoDiscoverySocket->set_option(asio::ip::multicast::enable_loopback(true));
    autoDiscoverySocket->bind(receiverEndpoint);
    // ADR-003: raise the kernel receive buffer so the socket can hold a burst
    // between drain arms. 0 (= default config) leaves the OS default untouched.
    if (config_.soRcvbufBytes > 0) {
        autoDiscoverySocket->set_option(
            asio::socket_base::receive_buffer_size(config_.soRcvbufBytes));
    }
    // Bound the receiver thread's blocking receive so shutdown is observable
    // even if the socket close doesn't unblock it (deterministic teardown).
    setSocketReceiveTimeout(*autoDiscoverySocket, kReceiverPollMs);

    // Initialize the ADR-003 sharded pipeline: `worker_threads` shards, each
    // with its own bounded SPSC ring. Worker threads are launched on
    // startAutoDiscovery(), not here.
    workerCount_ = static_cast<std::size_t>(config_.workerThreads);
    if (workerCount_ < kMinWorkerThreads || workerCount_ > kMaxWorkerThreads) {
        workerCount_ = kDefaultWorkerThreads;  // defensive clamp (config validated)
    }
    shards_.reserve(workerCount_);
    for (std::size_t i = 0; i < workerCount_; ++i) {
        auto shard = std::make_unique<Shard>();
        shard->ring = std::make_unique<SpscRing<autoDiscovery::Message>>(
            config_.rxRingSlots > 0 ? config_.rxRingSlots
                                    : kDefaultRxRingSlots);
        shards_.push_back(std::move(shard));
    }

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
    // Advertise our unicast gRPC endpoints on the heartbeat (ADR-0002 Channel
    // A) so reliable streams can open as soon as a peer is discovered.
    for (const auto& ep : buildSelfEndpoints(config_)) {
        *discoveryMessage.mutable_identifier()->add_endpoints() = ep;
    }
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
    // Flag running BEFORE spawning the receiver/worker threads: the receiver
    // loop exits unless isRunning is already true when it first samples it.
    isRunning = true;

    // Start send timer
    autoDiscoverySendTimer->expires_after(config_.discoverySendPeriod);
    
    // Start cleanup timer
    autoDiscoveryCleanUpTimer->expires_after(autoDiscoveryCleanupPeriod());
    
    // Start the ADR-003 sharded receive pipeline (socket-drain + workers) and
    // the periodic send/cleaner timers (these keep running on the io strand).
    startReceivePipeline();
    startAutoDiscoverySender();
    startAutoDiscoveryCleaner();

    // Bring up the reliable-path gRPC server so peers can dial this node's
    // advertised endpoints (ADR-0002). Additive; a port conflict is logged and
    // the multicast plane is unaffected.
    startGrpcServer();
}

// ============================================================================
// ADR-003 sharded receive pipeline: routing + receiver/worker threads
// ============================================================================

std::size_t Edriel::workerCount() const {
    return workerCount_;
}

std::size_t Edriel::shardIndexForTopic(const std::string& compositeKey) const {
    // One topic -> one shard -> one worker (ADR-003 decision #2). N==1 short-
    // circuits to 0 so the single-shard path needs no hashing at all.
    return (workerCount_ <= 1) ? 0 : (fnv1a64(compositeKey) % workerCount_);
}

std::size_t Edriel::shardIndexForUid(std::uint64_t uid) const {
    // Peer registry is sliced by uid alone (ADR-003 decision #1).
    return (workerCount_ <= 1) ? 0
                               : (fnv1a64(std::to_string(uid)) % workerCount_);
}

Edriel::Shard& Edriel::shardRef(std::size_t index) {
    return *shards_.at(index);
}

const Edriel::Shard& Edriel::shardRef(std::size_t index) const {
    return *shards_.at(index);
}

std::map<std::string, Edriel::TopicEntry> Edriel::mergeRegistrySnapshot() const {
    std::map<std::string, TopicEntry> merged;
    // Lock one shard at a time in ascending index order (no nested shard locks).
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mux);
        merged.insert(shard->topics.begin(), shard->topics.end());
    }
    return merged;
}

std::set<Edriel::Participant> Edriel::mergeParticipantSnapshot() const {
    std::set<Participant> merged;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mux);
        for (const auto& kv : shard->participants) {
            merged.insert(kv.second);
        }
    }
    return merged;
}

void Edriel::ageParticipant(std::uint64_t uid) {
    Shard& shard = shardRef(shardIndexForUid(uid));
    std::lock_guard<std::mutex> lock(shard.mux);
    const auto it = shard.participants.find(uid);
    if (it != shard.participants.end()) {
        it->second.lastSeen -= std::chrono::hours(24);
    }
}

void Edriel::startReceivePipeline() {
    if (pipelineStarted_) {
        return;
    }
    pipelineStarted_ = true;
    // Worker threads: one per shard/ring.
    for (std::size_t i = 0; i < workerCount_; ++i) {
        shards_[i]->worker = std::thread(&Edriel::autoDiscoveryWorkerLoop, this, i);
    }
    // Receiver (socket-drain) thread. ADR-003 keeps receiver_threads=1 in v1.
    // Arm the async receive first (queued on receiverIo_), then run it.
    armAutoDiscoveryReceive();
    receiverThread_ = std::thread(&Edriel::autoDiscoveryReceiverLoop, this);
}

void Edriel::stopReceivePipeline() {
    if (!pipelineStarted_) {
        return;
    }
    pipelineStarted_ = false;
    // Wake every worker and let it drain its ring, then leave it.
    for (auto& shard : shards_) {
        shard->ring->close();
    }
    for (auto& shard : shards_) {
        if (shard->worker.joinable()) {
            shard->worker.join();
        }
    }
    // The socket was already closed by stopAutoDiscovery, which cancelled the
    // pending async_receive; receiverIo_.run() then returns and the thread
    // exits, so join() completes deterministically.
    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }
}

void Edriel::autoDiscoveryReceiverLoop() {
    receiverIo_.run();
}

void Edriel::armAutoDiscoveryReceive() {
    autoDiscoverySocket->async_receive(
        asio::buffer(receiverBuffer_.data(), recvBufferSize),
        [this](const asio::error_code& ec, std::size_t n) {
            if (ec) {
                // Aborted (socket closed during shutdown) or a real error:
                // stop re-arming so receiverIo_.run() returns and the thread
                // exits. Don't hot-loop on a wedged socket.
                return;
            }
            handleReceivedDatagram(n);
            if (isRunning.load(std::memory_order_relaxed)) {
                armAutoDiscoveryReceive();  // re-arm immediately (fast drain)
            }
        });
}

void Edriel::handleReceivedDatagram(std::size_t bytesTransferred) {
    if (bytesTransferred < magicNumberSize) {
        return;
    }
    // Validate the magic number (memcpy to avoid unaligned access).
    std::uint32_t networkMagic = 0;
    std::memcpy(&networkMagic, receiverBuffer_.data(), sizeof(networkMagic));
    if (ntohl(networkMagic) != MAGIC_NUMBER_VALUE) {
        return;
    }
    // Parse just enough to route by topic (or by uid for a bare heartbeat).
    // The payload decode + dispatch happen on the owning worker.
    autoDiscovery::Message msg;
    if (!msg.ParseFromArray(
            receiverBuffer_.data() + static_cast<std::ptrdiff_t>(magicNumberSize),
            bytesTransferred - magicNumberSize)) {
        return;
    }
    Shard* target = nullptr;
    if (msg.has_identifier()) {
        const auto& id = msg.identifier();
        target = &shardRef(shardIndexForUid(id.uid()));
    } else if (msg.has_data_message()) {
        const auto& d = msg.data_message();
        target = &shardRef(shardIndexForTopic(
            makeCompositeKey(d.topic_name(), d.message_type())));
    } else if (msg.has_advertisement()) {
        const auto& ad = msg.advertisement();
        target = &shardRef(shardIndexForTopic(
            makeCompositeKey(ad.topic().topic_name(), ad.topic().message_type())));
    }
    if (target == nullptr) {
        return;  // unrecognized oneof; drop
    }
    target->ring->publish(std::make_unique<autoDiscovery::Message>(std::move(msg)));
}

void Edriel::autoDiscoveryWorkerLoop(std::size_t shardIndex) {
    SpscRing<autoDiscovery::Message>* ring = shards_.at(shardIndex)->ring.get();
    while (true) {
        auto msg = ring->pop();
        if (!msg) {
            break;  // ring closed and drained
        }
        handleAutoDiscoveryParse(*msg);
    }
}

void Edriel::postOnStrand(std::function<void()> thunk) {
    asio::post(strand, std::move(thunk));
}

/**
 * @brief Starts periodic discovery message sender
 * 
 * Every config_.discoverySendPeriod seconds (default 2s), sends a discovery
 * heartbeat packet containing our participant information to the multicast group.
 * 
 * The whole send-rearm cycle runs on the strand so the shared socket and the
 * reusable discoveryPacket member are never touched concurrently from another
 * thread (e.g. a sendPacket posted by the public API).
 */
void Edriel::startAutoDiscoverySender() {
    postOnStrand([this] {
        // Refresh advertised endpoints each heartbeat so a config/interface
        // change propagates on the next send. Cheap and strand-confined, so it
        // never races the serialized discoveryMessage.
        auto* id = discoveryMessage.mutable_identifier();
        id->clear_endpoints();
        for (const auto& ep : buildSelfEndpoints(config_)) {
            *id->add_endpoints() = ep;
        }

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
                    autoDiscoverySendTimer->expires_after(config_.discoverySendPeriod);
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
 * Every autoDiscoveryCleanupPeriod() seconds (default 5s, derived as
 * timeout/2 from the configured participant timeout), checks which participants
 * have timed out and removes them from the registry.
 */
void Edriel::startAutoDiscoveryCleaner() {
    postOnStrand([this] {
        removeTimedOutParticipants();  // Initial cleanup
        // Reconcile subscriber-client connections: re-dial on participant /
        // endpoint-set change, drop connections whose publisher left.
        reconcileReliableConnections();

        autoDiscoveryCleanUpTimer->expires_after(autoDiscoveryCleanupPeriod());
        autoDiscoveryCleanUpTimer->async_wait(
            asio::bind_executor(strand,
                [this](const asio::error_code& ec) {
                    if (!ec) {
                        startAutoDiscoveryCleaner();  // Reschedule
                    }
                }));
    });
}

/**
 * @brief Derived participant cleanup cadence (seconds).
 *
 * The cleanup interval is not a separate config key: it is derived from the
 * configured participant alive timeout so that the two stay in step. A
 * participant is dropped when the timeout has elapsed; the cleaner then runs
 * at timeout/2 so no stale participant lingers much past its timeout. Never
 * below 1s (guards the degenerate case of a 1s timeout).
 */
std::chrono::seconds Edriel::autoDiscoveryCleanupPeriod() const {
    const std::chrono::seconds halfTimeout = config_.participantTimeout / 2;
    return halfTimeout >= std::chrono::seconds(1) ? halfTimeout
                                                   : std::chrono::seconds(1);
}

// ============================================================================
// Message Handling
// ============================================================================

/**
 * @brief Dispatches a parsed discovery message by content type (oneof).
 *
 * Runs on each worker for ring-popped frames, and on the calling thread for
 * the direct test hook. Every branch routes to the owning shard. Note the
 * ADR-003 heartbeat micro-opt: only a STANDALONE heartbeat frame (a bare
 * `identifier`) refreshes the peer registry. Data and advertisement frames
 * no longer bump the publisher's `lastSeen`/endpoints on every delivery — a
 * fast O(1) peer refresh (once per peer per 2s heartbeat) replaces the old
 * per-frame O(n) find + per-frame endpoint-rebuild on the multicast hot path.
 * Peers stay alive through their own heartbeat stream; losing the per-data
 * heartbeat does not reduce per-(publisher,topic) delivery order or reliability.
 *
 * @param receivedMessage Parsed protobuf message
 */
void Edriel::handleAutoDiscoveryParse(const autoDiscovery::Message& receivedMessage) {
    // Handle based on message content type (oneof)
    if (receivedMessage.has_identifier()) {
        const auto& id = receivedMessage.identifier();
        handleParticipantHeartbeat(id.pid(), id.tid(), id.uid(),
                                   collectEndpoints(id));
    } else if (receivedMessage.has_data_message()) {
        const auto& data = receivedMessage.data_message();
        handleDataMessageReceive(data);
    } else if (receivedMessage.has_advertisement()) {
        const auto& ad = receivedMessage.advertisement();
        const auto& id = ad.identifier();
        const auto& topic = ad.topic();
        // Advertisements are infrequent (one per topic registration / peer
        // announcement, NOT per data frame), so registering the announcing
        // peer here is cheap and carries its advertised endpoints (needed by
        // the reliable path's reconcile to dial it).
        handleParticipantHeartbeat(id.pid(), id.tid(), id.uid(),
                                   collectEndpoints(id));
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
void Edriel::handleParticipantHeartbeat(unsigned long pid, uint64_t tid, uint64_t uid,
                                        std::vector<autoDiscovery::Endpoint> endpoints) {
    // Multicast loopback is enabled, so the node receives its own discovery
    // packets every send period. Never register ourselves: the uid is a
    // process-unique random token, so skipping it is a reliable self-filter.
    if (uid == selfParticipant.uid) {
        return;
    }

    // O(1) registry (ADR-003 decision #1): the peer lives in shard hash(uid)%N,
    // keyed by `uid` alone. No linear scan, no 3-field compare.
    Shard& shard = shardRef(shardIndexForUid(uid));
    std::lock_guard<std::mutex> lock(shard.mux);

    auto it = shard.participants.find(uid);
    if (it == shard.participants.end()) {
        // New participant, create entry with the configured aliveness timeout.
        Participant newParticipant(pid, tid, uid, config_.participantTimeout);
        newParticipant.lastSeen = std::chrono::steady_clock::now();
        // Adopt the advertised endpoints from this first heartbeat.
        newParticipant.endpoints = std::move(endpoints);
        shard.participants.emplace(uid, std::move(newParticipant));
    } else {
        // Existing participant, update timestamp.
        it->second.updateLastSeen();
        // Refresh advertised endpoints every heartbeat (ADR-0002): a restarted
        // peer re-advertises on its first heartbeat; a moved IP overwrites the
        // list in place. Idempotent and ~µs.
        it->second.endpoints = std::move(endpoints);
    }
}

/**
 * @brief Removes timed-out participants from registry
 * 
 * Iterates through all participants and removes those where the timeout
 * period has elapsed since the last heartbeat.
 */
void Edriel::removeTimedOutParticipants() {
    // Phase 1: locate and evict stale peers from the sharded participant
    // registry (lock one shard at a time, ascending; never nested).
    std::vector<Participant> stale;
    for (std::size_t i = 0; i < workerCount_; ++i) {
        Shard& shard = *shards_[i];
        std::lock_guard<std::mutex> lock(shard.mux);
        for (auto it = shard.participants.begin(); it != shard.participants.end();) {
            if (it->second.shouldBeRemoved()) {
                stale.push_back(it->second);
                it = shard.participants.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Phase 2: purge every stale peer from all topic-entry publisher/subscriber
    // sets (a peer with topics on many shards is referenced from each), and
    // drop topic entries left with no local callbacks and no remote peers.
    const auto isStale = [&stale](const Participant& p) {
        for (const Participant& s : stale) {
            if (s.uid == p.uid) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t i = 0; i < workerCount_; ++i) {
        Shard& shard = *shards_[i];
        std::lock_guard<std::mutex> lock(shard.mux);
        for (auto entry = shard.topics.begin(); entry != shard.topics.end();) {
            for (auto p = entry->second.publishers.begin();
                 p != entry->second.publishers.end();) {
                p = isStale(*p) ? entry->second.publishers.erase(p) : std::next(p);
            }
            for (auto s = entry->second.subscribers.begin();
                 s != entry->second.subscribers.end();) {
                s = isStale(*s) ? entry->second.subscribers.erase(s) : std::next(s);
            }
            if (entry->second.publishers.empty()
                && entry->second.subscribers.empty()
                && entry->second.callbacks.empty()) {
                entry = shard.topics.erase(entry);
            } else {
                ++entry;
            }
        }
    }

    // Phase 3: prune this peer's per-(publisher,topic) reliable receiver
    // windows (ADR-0002 §5 lifecycle: a timed-out participant's endpoints and
    // streams vanish with it). Without this the windows map grows without
    // bound as peers cycle through the registry. reliablePublisherSeq_ is
    // keyed by topic (bounded by topic count), not per-pub, so it needs no
    // peer-keyed pruning. Reliable path off the hot path -> own mutex.
    if (!stale.empty()) {
        std::lock_guard<std::mutex> lock(reliableMutex_);
        for (const Participant& s : stale) {
            const std::string winPrefix = std::to_string(s.uid) + "|";
            auto win = reliableWindows_.begin();
            while (win != reliableWindows_.end()) {
                if (win->first.rfind(winPrefix, 0) == 0) {
                    win = reliableWindows_.erase(win);
                } else {
                    ++win;
                }
            }
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

    // Announcements are routed by the announcing topic's shard (ADR-003
    // decision #2: one topic -> one worker). The peer's liveness/normalized
    // registry entry is maintained by its own heartbeat stream (cheap here).
    Shard& shard = shardRef(shardIndexForTopic(topicInfo.key));
    std::lock_guard<std::mutex> lock(shard.mux);
    TopicEntry& entry = shard.topics[topicInfo.key];

    entry.topicName = topicName;
    entry.messageType = messageType;

    if (isPublisher) {
        entry.publishers.insert(remote);
    } else {
        entry.subscribers.insert(remote);
    }
}

/**
 * @brief Handles a received data message
 *
 * Demuxes by composite key (topic name + message type) and invokes all
 * locally registered typed subscriber callbacks for the topic whose declared
 * message type matches. Runs on the topic's owning worker (or the calling
 * thread for the test hook) and locks only that shard.
 *
 * @param data Parsed DataMessage from the wire
 */
void Edriel::handleDataMessageReceive(const autoDiscovery::DataMessage& data) {
    const std::string key = makeCompositeKey(data.topic_name(), data.message_type());

    // Collect the matching callbacks under the topic's shard lock, then invoke
    // them after releasing it so user code (a callback) may safely re-enter the
    // public API.
    Shard& shard = shardRef(shardIndexForTopic(key));
    std::vector<TopicEntry::Callback> matched;
    {
        std::lock_guard<std::mutex> lock(shard.mux);
        const auto it = shard.topics.find(key);
        if (it == shard.topics.end()) {
            return;  // No local subscription for this topic
        }
        for (const auto& cb : it->second.callbacks) {
            if (cb.invoke && cb.messageType == data.message_type()) {
                matched.push_back(cb);
            }
        }
    }

    // Resolve + decode the payload ONCE per message, then fan out to every
    // matching callback (ADR-003: removes the old decode-per-callback cost).
    const google::protobuf::Message* prototype = nullptr;
    std::unique_ptr<google::protobuf::Message> decoded;
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
        if (!decoded) {
            decoded.reset(prototype->New());
            if (!decoded->ParseFromString(data.payload())) {
                std::cerr << "[Edriel] Failed to decode payload for topic "
                          << data.topic_name() << "\n";
                return;
            }
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
    
    // Close socket (unblocks the dedicated receiver thread), then stop and join
    // the receive pipeline (receiver + workers).
    if (autoDiscoverySocket && autoDiscoverySocket->is_open()) {
        asio::error_code ec;
        autoDiscoverySocket->close(ec);
    }
    stopReceivePipeline();

    // Tear down subscriber-client connections to publishers (reliable path).
    stopReliableSubscriptions();

    // Drain the reliable-path gRPC server.
    stopGrpcServer();

    std::cout << "[Edriel] Auto-discovery stopped\n";
}

// ============================================================================
// Public API: Reliable gRPC path (ADR-0002)
// ============================================================================

void Edriel::startGrpcServer() {
    std::lock_guard<std::mutex> lock(grpcServiceMutex_);
    if (grpcServer_) {
        return;  // already serving
    }

    grpcService_ = std::make_unique<ParticipantStreamServiceImpl>(*this);

    grpc::ServerBuilder builder;
    const std::string address = "0.0.0.0:" + std::to_string(config_.grpcPort);
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(grpcService_.get());

    grpcServer_ = builder.BuildAndStart();
    if (!grpcServer_) {
        std::cerr << "[Edriel] Reliable gRPC server failed to start on "
                  << address << "; multicast continues\n";
        grpcService_.reset();
        return;
    }
    std::cout << "[Edriel] Reliable gRPC server listening on " << address << "\n";
}

void Edriel::stopGrpcServer() {
    std::lock_guard<std::mutex> lock(grpcServiceMutex_);
    if (grpcServer_) {
        grpcServer_->Shutdown();  // reject new RPCs, cancel in-flight streams
        grpcServer_->Wait();      // drain before teardown
        grpcServer_.reset();
        grpcService_.reset();
        std::cout << "[Edriel] Reliable gRPC server stopped\n";
    }
}

bool Edriel::lookupParticipantData(std::uint32_t pid, std::uint64_t tid,
                                   std::uint64_t uid,
                                   autoDiscovery::ParticipantData& out) const {
    // The peer registry is keyed by uid alone, so the peer — if present — lives
    // in shard hash(uid)%N (O(1)). Cold path (gRPC verify/refresh).
    const Shard& shard = shardRef(shardIndexForUid(uid));
    Participant found;
    bool present = false;
    {
        std::lock_guard<std::mutex> lock(shard.mux);
        const auto it = shard.participants.find(uid);
        if (it != shard.participants.end()) {
            found = it->second;
            present = true;
        }
    }
    if (!present) {
        return false;
    }

    out.set_pid(pid);
    out.set_tid(tid);
    out.set_uid(uid);
    out.set_status("online");
    for (const auto& ep : found.endpoints) {
        *out.add_endpoints() = ep;
    }
    // Collect the topics this peer publishes/subscribes to across every shard.
    std::set<std::string> pubTopics;
    std::set<std::string> subTopics;
    for (std::size_t i = 0; i < workerCount_; ++i) {
        const Shard& s = *shards_[i];
        std::lock_guard<std::mutex> lock(s.mux);
        for (const auto& kv : s.topics) {
            if (kv.second.publishers.count(found) != 0) {
                pubTopics.insert(kv.second.topicName);
            }
            if (kv.second.subscribers.count(found) != 0) {
                subTopics.insert(kv.second.topicName);
            }
        }
    }
    for (const std::string& t : pubTopics) {
        out.add_topics_published(t);
    }
    for (const std::string& t : subTopics) {
        out.add_topics_subscribed(t);
    }
    return true;
}

std::vector<autoDiscovery::ParticipantData> Edriel::snapshotParticipantData() const {
    std::vector<autoDiscovery::ParticipantData> presence;
    presence.reserve(workerCount_ * 2);
    for (std::size_t i = 0; i < workerCount_; ++i) {
        const Shard& s = *shards_[i];
        std::lock_guard<std::mutex> lock(s.mux);
        for (const auto& kv : s.participants) {
            const Participant& p = kv.second;
            autoDiscovery::ParticipantData pd;
            pd.set_pid(static_cast<std::uint32_t>(p.pid));
            pd.set_tid(p.tid);
            pd.set_uid(p.uid);
            pd.set_status("online");
            for (const auto& ep : p.endpoints) {
                *pd.add_endpoints() = ep;
            }
            presence.push_back(std::move(pd));
        }
    }
    return presence;
}

bool Edriel::isKnownParticipant(std::uint32_t pid, std::uint64_t tid, std::uint64_t uid) {
    const auto matches = [pid, tid, uid](const Participant& p) {
        return p.pid == pid && p.tid == tid && p.uid == uid;
    };
    // Scan the sharded participant registry (cold path; lock one shard at a time).
    for (std::size_t i = 0; i < workerCount_; ++i) {
        const Shard& s = *shards_[i];
        std::lock_guard<std::mutex> lock(s.mux);
        for (const auto& kv : s.participants) {
            if (matches(kv.second)) {
                return true;
            }
        }
    }
    // A peer that has announced a topic subscription/publish is `known` even
    // before/cross- the heartbeat path populates the peer registry; gate
    // against both so a genuine topic-peer dial is accepted but a random
    // non-participant is rejected (ADR-0002 §6.2 anti-spoof).
    for (std::size_t i = 0; i < workerCount_; ++i) {
        const Shard& s = *shards_[i];
        std::lock_guard<std::mutex> lock(s.mux);
        for (const auto& kv : s.topics) {
            for (const Participant& p : kv.second.publishers) {
                if (matches(p)) {
                    return true;
                }
            }
            for (const Participant& p : kv.second.subscribers) {
                if (matches(p)) {
                    return true;
                }
            }
        }
    }
    // A static config `peers:` seed (ADR-0002 Channel D) is a known peer even
    // though it is multicast-blind: it is never heard on the group and has no
    // registry presence (no heartbeat, no topic announcement from network
    // discovery). Its sole identity is the deterministic (pid,0,uid) key
    // synthesized from its configured endpoint (peerKeyForEndpoint). Accept a
    // dialer matching that key in either direction — the forward seed-subscriber
    // and the reverse seed-only-peer dialing this node.
    for (const std::string& seed : config_.peerEndpoints) {
        const SubscriberKey seedKey = peerKeyForEndpoint(seed);
        if (pid == seedKey.pid && uid == seedKey.uid) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Reliable send path (ADR-0002 M4/M5)
// ============================================================================

bool Edriel::publishReliable(const std::string& topicName,
                             const std::string& messageType,
                             const std::string& payload) {
    std::lock_guard<std::mutex> gLock(grpcServiceMutex_);
    if (!grpcService_) {
        std::cerr << "[Edriel] Reliable send on '" << topicName
                  << "' but no gRPC server is serving\n";
        return false;
    }

    const std::string key = makeCompositeKey(topicName, messageType);

    std::set<Participant> subscribers;
    std::uint64_t nextSeq = 0;
    {
        // Reliable subscribers for the topic live in the topic's shard
        // (ADR-003 decision #2: the sharded registry is shared with reliable).
        Shard& shard = shardRef(shardIndexForTopic(key));
        std::lock_guard<std::mutex> lock(shard.mux);
        const auto it = shard.topics.find(key);
        if (it == shard.topics.end() || it->second.subscribers.empty()) {
            return false;  // no subscribers for this reliable topic
        }
        subscribers = it->second.subscribers;
    }
    {
        // Tentative next tid (reliable path bookkeeping, off the multicast hot
        // path). Commit only if the frame passes the MTU guard below, so a
        // locally-rejected (never placed on the wire) frame cannot consume a
        // sequence number and permanently gap a subscriber's window.
        std::lock_guard<std::mutex> lock(reliableMutex_);
        nextSeq = reliablePublisherSeq_[key] + 1;
    }

    // Stamp a per-(publisher, topic) tid (reusing Identifier.tid) so the
    // receiver's reorder/dedup window can deliver exactly-once in order.
    autoDiscovery::DataMessage data;
    data.mutable_identifier()->set_pid(selfParticipant.pid);
    data.mutable_identifier()->set_tid(nextSeq);
    data.mutable_identifier()->set_uid(selfParticipant.uid);
    data.set_topic_name(topicName);
    data.set_message_type(messageType);
    data.set_payload(payload);

    std::string serialized;
    if (!data.SerializeToString(&serialized)) {
        std::cerr << "[Edriel] Failed to serialize reliable frame\n";
        return false;
    }

    // Same 1500-byte payload MTU budget as the multicast path (ADR-0002,
    // fragmentation deferred): reject an oversized reliable frame at send time
    // rather than delivering a truncated/gapped stream.
    if (serialized.size() > recvBufferSize) {
        std::cerr << "[Edriel] Reliable frame for '" << topicName << "' is "
                  << serialized.size() << " bytes, exceeds "
                  << recvBufferSize << "-byte MTU budget, dropping\n";
        return false;
    }

    // Commit the sequence number now that the frame is guaranteed to be sent
    // (sends are serialized by grpcServiceMutex_, so no two threads race here).
    {
        std::lock_guard<std::mutex> lock(reliableMutex_);
        reliablePublisherSeq_[key] = nextSeq;
    }

    bool pushedAny = false;
    for (const Participant& sub : subscribers) {
        autoDiscovery::ParticipantData frame;
        frame.set_reliable_data(serialized);
        const SubscriberKey sk{static_cast<std::uint32_t>(sub.pid), sub.tid, sub.uid};
        if (grpcService_->pushData(sk, std::move(frame))) {
            pushedAny = true;
        }
    }
    return pushedAny;
}

void Edriel::handleReliableDataFrame(const autoDiscovery::ParticipantData& pd) {
    if (pd.reliable_data().empty()) {
        return;  // presence / control frame
    }

    autoDiscovery::DataMessage data;
    if (!data.ParseFromString(pd.reliable_data())) {
        std::cerr << "[Edriel] Dropping undecodable reliable frame\n";
        return;
    }

    const std::uint64_t pubUid = data.identifier().uid();
    const std::uint64_t tid = data.identifier().tid();
    const std::string winKey =
        std::to_string(pubUid) + "|" + makeCompositeKey(data.topic_name(), data.message_type());

    // Collect everything that becomes deliverable (this frame and any buffered
    // out-of-order frames that now become contiguous) under the lock, then
    // dispatch after releasing it so user callbacks may re-enter the API.
    std::vector<autoDiscovery::DataMessage> toDispatch;
    {
        std::lock_guard<std::mutex> lock(reliableMutex_);
        // A fresh publisher stream was just established; its first frame must
        // baseline every window (the transport's prior in-flight frames are
        // gone, so an unreachable gap would otherwise stall the window). Consume
        // the marker here — even for a window that is only coming into existence
        // on this very frame, so it starts unbaselined instead of at its default
        // nextExpected=1.
        const bool wasFresh = reliableFreshPublishers_.erase(pubUid);
        if (wasFresh) {
            const std::string freshPrefix = std::to_string(pubUid) + "|";
            for (auto& fkv : reliableWindows_) {
                if (fkv.first.rfind(freshPrefix, 0) == 0) {
                    fkv.second.buffer.clear();
                    fkv.second.nextExpected = 0;
                }
            }
        }
        ReliableRxWindow& w = reliableWindows_[winKey];
        if (wasFresh) {
            // Ensure the window itself is unbaselined even when it is only now
            // being created (a fresh, never-yet-data subscriber's first frame):
            // the reset loop above cannot see an entry that does not exist yet.
            w.buffer.clear();
            w.nextExpected = 0;
        }

        const auto deliver = [&](autoDiscovery::DataMessage& m) {
            toDispatch.push_back(std::move(m));
            // This frame (or a buffered one) is tid `w.nextExpected`: the next
            // contiguous tid to deliver is now one past it.
            if (w.nextExpected < std::numeric_limits<std::uint64_t>::max()) {
                ++w.nextExpected;
            }
            // Flush buffered out-of-order frames. A tid gap is treated as a
            // lost frame (no retransmission at this layer): we still deliver
            // every distinct tid exactly-once, in ascending order, advancing
            // `nextExpected` past each flushed tid (absorbing the hole) so a
            // duplicate of an already-flushed frame is dropped.
            while (!w.buffer.empty()) {
                auto it = w.buffer.begin();
                const std::uint64_t bufferedTid = it->first;
                toDispatch.push_back(std::move(it->second));
                w.buffer.erase(it);
                w.nextExpected = std::max(w.nextExpected, bufferedTid);
                if (w.nextExpected < std::numeric_limits<std::uint64_t>::max()) {
                    ++w.nextExpected;
                }
            }
        };

        if (w.nextExpected == 0) {
            // A freshly established (re-established) stream: no frame of this
            // connection can be in flight behind this one, so it baselines the
            // window. This is the resume-after-reconnect catch-up — frames
            // dropped mid-teardown can never be replayed, so start the window
            // at this frame; anything with a smaller tid is unreachable and
            // dropped (bounded at-most-once).
            w.nextExpected = tid;
            deliver(data);
        } else if (tid == w.nextExpected) {
            deliver(data);
        } else if (tid > w.nextExpected) {
            if ((tid - w.nextExpected) < kReliableWindowSize
                && w.buffer.size() < kReliableWindowSize) {
                // Within the bounded window: hold for reordering. A within-bound
                // gap is buffered even when the window is empty, because it is
                // consistent with ordinary in-flight REORDERING (a later tid
                // arriving before earlier ones) and must NOT supersede it —
                // fast-forwarding would silently drop frames of a genuine
                // reorder, weakening ADR-0002's exactly-once/in-order guarantee.
                // Bounded — if full, drop the incoming (a mid-stream gap is
                // unrecoverable anyway).
                w.buffer.emplace(tid, std::move(data));
            } else if ((tid - w.nextExpected) >= kReliableWindowSize
                       && w.buffer.empty()) {
                // A gap past the whole window on a FRESH/EMPTY window is the
                // signature of a late-joining subscriber whose publisher already
                // advanced its seq: the tids between `nextExpected` and `tid`
                // cannot be in flight (they lie beyond the bounded window, and
                // there is no NAK/replay layer), so they will never arrive.
                // Fast-forward nextExpected to the incoming tid and deliver it,
                // dropping the unrecoverable trailing gap. This is the accepted
                // one-time catch-up for late joiners (ADR-0002 has no catch-up
                // spec). Kept ONLY on an empty window — a large gap on a
                // non-empty window is a genuine unrecoverable mid-stream gap and
                // remains dropped below.
                w.nextExpected = tid;
                deliver(data);
            }
            // else (non-empty window, gap beyond the bound, or a within-bound
            // gap while the bound is full) -> drop (stale/unrecoverable); the
            // window is waiting on a mid-stream gap.
        }
        // tid < w.nextExpected -> duplicate or already delivered -> drop.
    }

    for (autoDiscovery::DataMessage& m : toDispatch) {
        handleDataMessageReceive(m);
    }
}

void Edriel::noteReliableStreamEstablished(std::uint64_t publisherUid) {
    // A fresh dial to `publisherUid` tore down whatever the prior stream was
    // buffering (the transport is gone, so any un-delivered frames behind it
    // are lost — no NACK/replay layer exists). Reset every receive window this
    // node keeps for that publisher to an "unbaselined" state so the first
    // frame on the new stream re-baselines it, instead of leaving the window
    // permanently stalled on an unreachable gap.
    std::lock_guard<std::mutex> lock(reliableMutex_);
    // Mark the publisher fresh so a window that only comes into existence on a
    // later frame (late/never-yet-data subscriber) still starts unbaselined.
    reliableFreshPublishers_.insert(publisherUid);
    const std::string prefix = std::to_string(publisherUid) + "|";
    for (auto& kv : reliableWindows_) {
        if (kv.first.rfind(prefix, 0) == 0) {
            kv.second.buffer.clear();
            kv.second.nextExpected = 0;
        }
    }
}

void Edriel::startReliableSubscriptions() {
    reconcileReliableConnections();
}

void Edriel::stopReliableSubscriptions() {
    // Collect the connections under the lock, release it, THEN stop() (which
    // joins the reader thread) outside the mutex. stop() -> thread_.join()
    // must not run while holding reliableConnMutex_: a user frame callback
    // dispatched on one of those reader threads may re-enter
    // startReliableSubscriptions()/unsubscribe, which takes the same mutex —
    // joining a thread blocked on the very mutex we hold is a self-deadlock.
    std::vector<std::unique_ptr<ReliableSubscriberConnection>> conns;
    {
        std::lock_guard<std::mutex> lock(reliableConnMutex_);
        for (auto& kv : reliableConnections_) {
            conns.push_back(std::move(kv.second));
        }
        reliableConnections_.clear();
    }
    // stop() joins the reader thread; run it outside the lock. Note the
    // abandoned unique_ptr's destructor also calls stop() (idempotent), so
    // releasing the map under the lock must not destroy live connections.
    for (auto& c : conns) {
        c->stop();
    }
}

void Edriel::reconcileReliableConnections() {
    // Snapshot the publishers we must dial for locally reliable subscriptions.
    struct Target {
        SubscriberKey key;
        std::vector<std::string> candidates;  // ordered "address:port" endpoints
    };
    std::vector<Target> needed;
    bool subscribedReliable = false;
    std::vector<std::string> subscribedTopics;
    {
        std::lock_guard<std::mutex> lock(reliableMutex_);
        subscribedReliable = !reliableSubscribedTopics_.empty();
        subscribedTopics.assign(reliableSubscribedTopics_.begin(),
                                reliableSubscribedTopics_.end());
    }
    // Build the dial set from the SHARED sharded registry (ADR-003 decision #2):
    // canonical endpoints live in the peer registry (shard hash(uid)%N), and
    // the reliable topic's publishers live in the topic's shard (hash(topic)%N).
    // Each shard is locked independently and briefly — no nested shard locks.
    for (const std::string& topicKey : subscribedTopics) {
        std::vector<Participant> pubs;
        {
            Shard& topShard = shardRef(shardIndexForTopic(topicKey));
            std::lock_guard<std::mutex> lock(topShard.mux);
            const auto it = topShard.topics.find(topicKey);
            if (it == topShard.topics.end()) {
                continue;
            }
            pubs.assign(it->second.publishers.begin(),
                        it->second.publishers.end());
        }
        for (const Participant& pub : pubs) {
            std::vector<autoDiscovery::Endpoint> eps;
            {
                Shard& uidShard = shardRef(shardIndexForUid(pub.uid));
                std::lock_guard<std::mutex> lock(uidShard.mux);
                const auto it = uidShard.participants.find(pub.uid);
                if (it != uidShard.participants.end()) {
                    eps = it->second.endpoints;
                }
            }
            if (eps.empty()) {
                continue;
            }
            needed.push_back(Target{
                {static_cast<std::uint32_t>(pub.pid), pub.tid, pub.uid},
                endpointCandidates(eps)});
        }
    }

    // Static config seed (ADR-0002 Channel D): multicast-blind / cross-subnet
    // peers have no registry presence (never heard on the group) but still must
    // be reached for any reliable subscription. Resolve them directly into the
    // dial set with a deterministic identity so reconcile stays keyed by
    // (pid,tid,uid). Only dial while there is at least one reliable topic.
    if (subscribedReliable) {
        for (const std::string& peer : config_.peerEndpoints) {
            needed.push_back(Target{peerKeyForEndpoint(peer), {peer}});
        }
    }

    // Connections whose reader thread must be joined, stopped AFTER the mutex
    // is released. stop() -> thread_.join() must not run while holding
    // reliableConnMutex_: a user frame callback dispatched on a reader thread
    // may re-enter startReliableSubscriptions()/subscribe (same mutex), and
    // joining a thread blocked on the very mutex we hold is a self-deadlock.
    std::vector<std::unique_ptr<ReliableSubscriberConnection>> retire;
    {
        std::lock_guard<std::mutex> lock(reliableConnMutex_);

        // Drop connections no longer needed (publisher left / topic unsubscribed).
        for (auto it = reliableConnections_.begin(); it != reliableConnections_.end();) {
            bool stillNeeded = false;
            for (const Target& t : needed) {
                if (t.key == it->first) {
                    stillNeeded = true;
                    break;
                }
            }
            if (!stillNeeded) {
                retire.push_back(std::move(it->second));
                it = reliableConnections_.erase(it);
            } else {
                ++it;
            }
        }

        // Dial publishers we are not yet connected to, and re-dial a connection
        // whose *currently-connected endpoint* is no longer in the publisher's
        // advertised set (endpoint change / IP move / peer restart under the same
        // identity — ADR-0002 §5). Connect-in-order (first advertised candidate),
        // and the connection owns the candidate fallback (multi-homed §6.3).
        for (const Target& t : needed) {
            const auto existing = reliableConnections_.find(t.key);
            if (existing != reliableConnections_.end()) {
                const std::string connected = existing->second->currentEndpoint();
                const bool connectedStale = !connected.empty() &&
                    std::find(t.candidates.begin(), t.candidates.end(), connected)
                        == t.candidates.end();
                if (!connectedStale) {
                    continue;  // still connected to a currently advertised endpoint
                }
                // Re-dial: the peer moved to a new endpoint set this node isn't on.
                // Retire the old connection (stopped outside the lock below) and
                // fall through to create a fresh one at the same key.
                retire.push_back(std::move(existing->second));
                reliableConnections_.erase(existing);
            }

            auto conn = std::make_unique<ReliableSubscriberConnection>(
                *this, t.key, t.candidates,
                static_cast<std::uint32_t>(selfParticipant.pid), selfParticipant.tid,
                selfParticipant.uid);
            conn->start();
            reliableConnections_[t.key] = std::move(conn);
        }
    }

    // Join retired reader threads outside the mutex (see comment above). The
    // unique_ptr destructor also calls stop() (idempotent). Deadlock-free by
    // construction: no stop()/join() runs under reliableConnMutex_.
    for (auto& c : retire) {
        c->stop();
    }
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
