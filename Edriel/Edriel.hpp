#pragma once

#include <iostream>
#include <asio.hpp>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include "hello.pb.h"
#include "autoDiscovery.pb.h"
#include <string>
#include <string_view>
#include <functional>
#include <array>
#include <cstddef>
#include <mutex>

class Edriel {
private:

    // --- Configuration ----------------------------------------------------
    static constexpr uint16_t commonPort{ 30002 };
    static constexpr std::string_view multicastAddress{ "239.255.0.1" };
    static constexpr std::size_t  recvBufferSize{ 1500 };
    static constexpr std::chrono::seconds autoDiscoveryPeriod{ 2 };
    // magic number, 4 bytes
    static constexpr uint32_t   magicNumber{ 0xED75E1ED };
    static constexpr std::size_t  magicNumberSize{ sizeof(magicNumber) };

    using Buffer = std::array<char, recvBufferSize>;

    asio::io_context& io_context;

    // --- Socket / timer (plain objects, not shared_ptrs) -------------------
    std::unique_ptr<asio::ip::udp::socket> autoDiscoverySocket{};
    std::unique_ptr<asio::steady_timer> autoDiscoveryTimer{};
    asio::ip::udp::endpoint           receiverEndpoint{ asio::ip::address_v4::any(), commonPort };
    asio::ip::udp::endpoint           senderEndpoint{};
    asio::ip::udp::endpoint           multicastEndpoint{ asio::ip::address_v4::from_string(std::string(multicastAddress)), commonPort };

    autoDiscovery::Identifier discoveryMessage;
    std::string discoveryPacket;

    // --- Control flags ------------------------------------------------------
    std::atomic_bool                    isRunning{ false };
    std::mutex                          runnerMutex;

    bool hasValidMagicNumber(std::shared_ptr<Buffer> buffer, std::size_t length) const;
    void handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer, const asio::error_code& ec, std::size_t bytesTransferred);
    void startAutoDiscoveryReceiver(std::shared_ptr<Buffer> buffer = std::make_shared<Buffer>());
    void startAutoDiscoverySender();
    void stopAutoDiscoverySocketAndTimer();
    void initializeAutoDiscovery();
public:
    Edriel(asio::io_context& io_ctx);
    void startAutoDiscovery();
    void stopAutoDiscovery();
    ~Edriel();
};


