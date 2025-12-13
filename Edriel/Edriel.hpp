#pragma once

#include <iostream>
#include <asio.hpp>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include "hello.pb.h"
#include <string>
#include <string_view>
#include <functional>
#include <array>
#include <cstddef>
#include <mutex>

// TODO: Use dynamic buffer
// TODO: Unique auto-discovery signature

class Edriel {
private:
    // --- Configuration ----------------------------------------------------
    static constexpr uint16_t commonPort{ 30002 };
    static constexpr std::string_view multicastAddress{ "239.255.0.1" };
    static constexpr std::size_t  recvBufferSize{ 1024 };
    static constexpr std::string_view packet{ "Discovery Packet" };
    static constexpr std::chrono::seconds autoDiscoveryPeriod{ 2 };

    asio::io_context& io_context;

    // --- Socket / timer (plain objects, not shared_ptrs) -------------------
    asio::ip::udp::socket             autoDiscoverySocket;
    asio::steady_timer                autoDiscoveryTimer;
    asio::ip::udp::endpoint           receiverEndpoint{ asio::ip::address_v4::any(), commonPort };
    asio::ip::udp::endpoint           senderEndpoint{ };
    asio::ip::udp::endpoint           multicastEndpoint{ asio::ip::address_v4::from_string(std::string(multicastAddress)), commonPort };

    std::array<char, recvBufferSize> recvBuffer;

    // --- Control flags ------------------------------------------------------
    std::atomic_bool                    isRunning{ false };
    std::mutex                          runnerMutex;

    void startAutoDiscoveryReceiver() {
        autoDiscoverySocket.async_receive_from(
            asio::buffer(recvBuffer), senderEndpoint,
            [this](const asio::error_code& ec, std::size_t n) {
                if (!ec) {
                    std::cout << "[Recv] From " << senderEndpoint << " : "
                              << std::string(recvBuffer.data(), n) << '\n';
                } else {
                    std::cerr << "Receive error: " << ec.message() << '\n';
                }
                startAutoDiscoveryReceiver();  // continue looping
            });
    }

    void startAutoDiscoverySender() {
        autoDiscoveryTimer.expires_after(autoDiscoveryPeriod);
        autoDiscoveryTimer.async_wait([this](const asio::error_code& ec) {
            if (!ec) {
                autoDiscoverySocket.async_send_to(
                    asio::buffer(packet), multicastEndpoint,
                    [](const asio::error_code& ec, std::size_t /*n*/) {
                        if (ec) std::cerr << "Send failed: " << ec.message() << '\n';
                    });
            } else {
                std::cerr << "Timer error: " << ec.message() << '\n';
            }
            startAutoDiscoverySender();  // keep sending
        });
    }

public:
    Edriel(asio::io_context& io_ctx)
        : io_context(io_ctx),
          autoDiscoverySocket(io_ctx),
          autoDiscoveryTimer(io_ctx)
    {
        // Socket options ---------------------------------------------------
        autoDiscoverySocket.open(receiverEndpoint.protocol());
        autoDiscoverySocket.set_option(asio::socket_base::reuse_address(true));
        autoDiscoverySocket.set_option(asio::ip::multicast::enable_loopback(true));
        autoDiscoverySocket.bind(receiverEndpoint);

        // Join multicast group ---------------------------------------------
        try {
            autoDiscoverySocket.set_option(
                asio::ip::multicast::join_group(
                    asio::ip::address::from_string(std::string(multicastAddress))));
        } catch (const std::exception& e) {
            std::cerr << "Join Group Failed: " << e.what() << '\n';
        }

        std::cout << "Edriel initialized.\n";
    }

    void startAutoDiscovery() {
        if(isRunning.load()) return;
        std::scoped_lock<std::mutex> lock(runnerMutex);
        startAutoDiscoverySender();
        startAutoDiscoveryReceiver();
    }

    void stopAutoDiscovery() {
        if(!isRunning.load()) return;
        std::scoped_lock<std::mutex> lock(runnerMutex);
        asio::error_code ec;
        autoDiscoveryTimer.cancel(ec);
        autoDiscoverySocket.close(ec);
    }

    ~Edriel() { stopAutoDiscovery(); }
};