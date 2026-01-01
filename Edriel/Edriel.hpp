#pragma once

#include <iostream>
#include <asio.hpp>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include "autoDiscovery.pb.h"
#include <string>
#include <string_view>
#include <functional>
#include <array>
#include <cstddef>
#include <mutex>
#include <bit>
#include <set>

// TODO: Participant management (list of known participants)
//      - Add participant struct with PID, TID, UID, last seen timestamp
//      - Drop participants not seen for a certain timeout period

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
    // --- Magic number, 4 bytes --------------------------------------------
    static constexpr uint32_t   magicNumber{ 0xED75E1ED };
    static constexpr std::size_t  magicNumberSize{ sizeof(magicNumber) };
    using Buffer = std::array<char, recvBufferSize>;
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

    autoDiscovery::Identifier discoveryMessage;
    std::string discoveryPacket;

    // --- Control flags ------------------------------------------------------
    std::atomic_bool                    isRunning{ false };
    std::mutex                          runnerMutex;

    // -- Participant Info ---------------------------------------------------
    std::set<Participant> participants;

    // --- Internal Methods ---------------------------------------------------
    bool hasValidMagicNumber(std::shared_ptr<Buffer> buffer, std::size_t length) const;
    void handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer, const asio::error_code& ec, std::size_t bytesTransferred);
    void startAutoDiscoveryReceiver(std::shared_ptr<Buffer> buffer = std::make_shared<Buffer>());
    void startAutoDiscoverySender();
    void startAutoDiscoveryCleaner();
    void stopAutoDiscoverySocketAndTimer();
    void initializeAutoDiscovery();

    // --- Participant Management -----------------------------------------------
    void handleParticipantHeartbeat(unsigned long pid, uint64_t tid, uint64_t uid);
    void removeTimedOutParticipants();

public:
    Edriel(asio::io_context& io_ctx);
    void startAutoDiscovery();
    void stopAutoDiscovery();
    ~Edriel();
};


