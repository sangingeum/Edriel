#pragma once

#include <asio.hpp>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include "autoDiscovery.pb.h"
#include <string_view>
#include <memory>

// TODO: grpc streaming for participant data exchange
// TODO: topic exchange
// TODO: configurable parameters
// TODO: TTL for multicast packets
// TODO: unit tests
// TODO: use concept in cpp20
// TODO: domain separation

class Edriel {
public:
    struct Participant {
            public:
            unsigned long pid;
            uint64_t tid;
            uint64_t uid;
            mutable std::chrono::steady_clock::time_point lastSeen;
            static constexpr std::chrono::seconds timeoutPeriod{ 10 };
            
            Participant(unsigned long p, uint64_t t, uint64_t u)
                : pid(p), tid(t), uid(u), lastSeen(std::chrono::steady_clock::now()) {}
            bool shouldBeRemoved() const {
                return (std::chrono::steady_clock::now() - lastSeen) > timeoutPeriod;
            }
            void updateLastSeen() const {
                lastSeen = std::chrono::steady_clock::now();
            }
            bool operator==(const Participant& other) const {
                return pid == other.pid && tid == other.tid && uid == other.uid;
            }
            bool operator<(const Participant& other) const {
                return std::tie(pid, tid, uid) < std::tie(other.pid, other.tid, other.uid);
            }
        };
private:

    // --- Configuration ----------------------------------------------------
    static constexpr uint16_t commonPort{ 30002 };
    static constexpr std::string_view multicastAddress{ "239.255.0.1" };
    static constexpr std::size_t  recvBufferSize{ 1500 };
    static constexpr std::chrono::seconds autoDiscoverySendPeriod{ 2 };
    static constexpr std::chrono::seconds autoDiscoveryCleanUpPeriod{ 5 };
    using Buffer = std::array<char, recvBufferSize>;
    // --- Magic number, 4 bytes --------------------------------------------
    static constexpr uint32_t   magicNumber{ 0xED75E1ED };
    static constexpr std::size_t  magicNumberSize{ sizeof(magicNumber) };
    // --- ASIO Context ------------------------------------------------------
    asio::io_context& io_context;
    asio::strand<asio::io_context::executor_type> strand;  // for protecting participants
    // --- Socket / timer (plain objects, not shared_ptrs) -------------------
    std::unique_ptr<asio::ip::udp::socket> autoDiscoverySocket{};
    std::unique_ptr<asio::steady_timer> autoDiscoverySendTimer{};
    std::unique_ptr<asio::steady_timer> autoDiscoveryCleanUpTimer{};
    asio::ip::udp::endpoint           receiverEndpoint{ asio::ip::address_v4::any(), commonPort };
    asio::ip::udp::endpoint           senderEndpoint{};
    asio::ip::udp::endpoint           multicastEndpoint{ asio::ip::address_v4::from_string(std::string(multicastAddress)), commonPort };

    autoDiscovery::Message discoveryMessage;
    std::string discoveryPacket; // Serialized discoveryMessage with a magic number prepended

    // --- Control flags ------------------------------------------------------
    std::atomic_bool                    isRunning{ false };
    std::mutex                          runnerMutex;

    // -- Participant Info ---------------------------------------------------
    std::set<Participant> participants;
    Participant selfParticipant{0,0,0}; // placeholder

    // --- Internal Methods ---------------------------------------------------
    void handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer, const asio::error_code& ec, std::size_t bytesTransferred);
    void startAutoDiscoveryReceiver(std::shared_ptr<Buffer> buffer = std::make_shared<Buffer>());
    void startAutoDiscoverySender();
    void startAutoDiscoveryCleaner();
    void stopAutoDiscoverySocketAndTimer();
    void initializeAutoDiscovery();

    // --- Participant Management -----------------------------------------------
    void handleParticipantHeartbeat(unsigned long pid, uint64_t tid, uint64_t uid);
    void removeTimedOutParticipants();

    // --- Helper Methods -----------------------------------------------------
    bool hasValidMagicNumber(std::shared_ptr<Buffer> buffer, std::size_t length) const;
    void prependMagicNumberToPacket(std::string& packet) const;

public:
    Edriel(asio::io_context& io_ctx);
    // Auto-discovery control
    void startAutoDiscovery();
    void stopAutoDiscovery();

    ~Edriel();
};


