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

// TODO: Use magic number
// TODO : Use smart pointers for autoDiscovery socket and timer
class Edriel {
private:

    // --- Configuration ----------------------------------------------------
    static constexpr uint16_t commonPort{ 30002 };
    static constexpr std::string_view multicastAddress{ "239.255.0.1" };
    static constexpr std::size_t  recvBufferSize{ 1500 };
    static constexpr std::chrono::seconds autoDiscoveryPeriod{ 2 };

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

    // --- Random‑number generator --------------------------------------------

    void handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer, const asio::error_code& ec, std::size_t bytesTransferred) {
        if (!ec) {
            autoDiscovery::Identifier receivedMessage;
            if(receivedMessage.ParseFromArray(buffer->data(), static_cast<int>(bytesTransferred))){
                std::cout << "[Recv] PID: " << receivedMessage.pid()
                          << ", TID: " << receivedMessage.tid()
                          << ", UID: " << receivedMessage.uid() << '\n';
            } else {
                std::cerr << "Failed to parse received message.\n";
            }
        } else {
            std::cerr << "Receive error: " << ec.message() << '\n';
        }
        startAutoDiscoveryReceiver(buffer);  // continue looping
    }

    void startAutoDiscoveryReceiver(std::shared_ptr<Buffer> buffer = std::make_shared<Buffer>()) {
        if(!autoDiscoverySocket || !autoDiscoveryTimer) {
            std::cerr << "Auto-discovery components are not initialized.\n";
            return;
        }

        if(autoDiscoverySocket->is_open() == false || !isRunning.load()){
            std::cout << "Receiver stopped.\n";
            return;
        }

        autoDiscoverySocket->async_receive_from(
            asio::buffer(buffer->data(), buffer->size()), senderEndpoint, 
            std::bind(&Edriel::handleAutoDiscoveryReceive, this, buffer, std::placeholders::_1, std::placeholders::_2));
    }

    void startAutoDiscoverySender(std::shared_ptr<Buffer> buffer = std::make_shared<Buffer>()) {
        if(!autoDiscoverySocket || !autoDiscoveryTimer) {
            std::cerr << "Auto-discovery components are not initialized.\n";
            return;
        }
    
        if(autoDiscoverySocket->is_open() == false || !isRunning.load()){
            std::cout << "Sender stopped.\n";
            return;
        }

        autoDiscoveryTimer->expires_after(autoDiscoveryPeriod);
        autoDiscoveryTimer->async_wait([this](const asio::error_code& ec) {
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

    void initializeAutoDiscovery() {
        asio::error_code ec;
        if(autoDiscoverySocket && autoDiscoverySocket->is_open()) {
            autoDiscoverySocket->close(ec);
            if (ec) {
                std::cerr << "Error closing existing socket: " << ec.message() << '\n';
            }
        }
        if(autoDiscoveryTimer){
            autoDiscoveryTimer->cancel();
            if (ec) {
                std::cerr << "Error cancelling existing timer: " << ec.message() << '\n';
            }
        }

        // Create socket and timer ---------------------------------------------
        autoDiscoverySocket = std::make_unique<asio::ip::udp::socket>(io_context);
        autoDiscoveryTimer = std::make_unique<asio::steady_timer>(io_context);

        // Socket options ---------------------------------------------------
        autoDiscoverySocket->open(receiverEndpoint.protocol());
        autoDiscoverySocket->bind(receiverEndpoint);
        autoDiscoverySocket->set_option(asio::socket_base::reuse_address(true));
        autoDiscoverySocket->set_option(asio::ip::multicast::enable_loopback(true));
        // Join multicast group ---------------------------------------------
        try {
            autoDiscoverySocket->set_option(
                asio::ip::multicast::join_group(
                    asio::ip::address::from_string(std::string(multicastAddress))));
        } catch (const std::exception& e) {
            std::cerr << "Join Group Failed: " << e.what() << '\n';
        }
    }

public:
    Edriel(asio::io_context& io_ctx)
        : io_context(io_ctx)
    {
        thread_local static std::random_device                      rd;
        thread_local static std::mt19937_64                         generator(rd());
        thread_local static std::uniform_int_distribution<uint64_t> dist(0, std::numeric_limits<uint64_t>::max());

        // Initialize Discovery Packet --------------------------------
        discoveryMessage.set_pid(static_cast<unsigned long>(::_getpid()));
        discoveryMessage.set_tid(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        discoveryMessage.set_uid(dist(generator));
        discoveryPacket = discoveryMessage.SerializeAsString();
        
        // Initialize Auto-Discovery --------------------------------
        initializeAutoDiscovery();

        std::cout << "Edriel initialized.\n";
    }

    void startAutoDiscovery() {
        std::scoped_lock<std::mutex> lock(runnerMutex);
        if(isRunning.load()) return;
        std::cout << "Starting Auto-Discovery...\n";
        isRunning.store(true);
        startAutoDiscoverySender();
        startAutoDiscoveryReceiver();
    }

    void stopAutoDiscovery() {
        std::scoped_lock<std::mutex> lock(runnerMutex);
        if(!isRunning.load()) return;
        std::cout << "Stopping Auto-Discovery...\n";
        isRunning.store(false);
        initializeAutoDiscovery();
    }

    ~Edriel() { stopAutoDiscovery(); }
};