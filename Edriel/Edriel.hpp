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

    // --- Random‑number generator --------------------------------------------
    bool hasValidMagicNumber(std::shared_ptr<Buffer> buffer, std::size_t length) const {
        if (length < magicNumberSize) return false;
        auto rawBytes = *reinterpret_cast<const std::array<char, magicNumberSize>*>(buffer->data());
        uint32_t receivedMagicNumber = ntohl(std::bit_cast<uint32_t>(rawBytes));
        return receivedMagicNumber == magicNumber;
    }

    void handleAutoDiscoveryReceive(std::shared_ptr<Buffer> buffer, const asio::error_code& ec, std::size_t bytesTransferred) {
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

    void startAutoDiscoverySender() {
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

    void stopAutoDiscoverySocketAndTimer(){
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
    }

    void initializeAutoDiscovery() {
        // Clean up existing socket and timer if any ------------------------
        stopAutoDiscoverySocketAndTimer();
        // Create socket and timer ---------------------------------------------
        autoDiscoverySocket = std::make_unique<asio::ip::udp::socket>(io_context);
        autoDiscoveryTimer = std::make_unique<asio::steady_timer>(io_context);
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

public:
    Edriel(asio::io_context& io_ctx)
        : io_context(io_ctx)
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
        // Attach magic number at the beginning
        uint32_t networkMagicNumber = htonl(magicNumber);
        discoveryPacket.insert(0, reinterpret_cast<const char*>(&networkMagicNumber), sizeof(networkMagicNumber));
    
        std::cout << "Edriel initialized.\n";
    }

    void startAutoDiscovery() {
        std::scoped_lock<std::mutex> lock(runnerMutex);
        if(isRunning.load()) return;
        std::cout << "Starting Auto-Discovery...\n";
        isRunning.store(true);
        // Initialize Auto-Discovery --------------------------------
        initializeAutoDiscovery();
        // Start Sender and Receiver -------------------------------
        startAutoDiscoverySender();
        startAutoDiscoveryReceiver();
    }

    void stopAutoDiscovery() {
        std::scoped_lock<std::mutex> lock(runnerMutex);
        if(!isRunning.load()) return;
        std::cout << "Stopping Auto-Discovery...\n";
        isRunning.store(false);
        stopAutoDiscoverySocketAndTimer();
    }

    ~Edriel() { stopAutoDiscovery(); }
};