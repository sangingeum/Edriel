#pragma once

#include <iostream>
#include <asio.hpp>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include "hello.pb.h"
#include <string>
#include <string_view>
#include <functional>

class Edriel {
private:
    static constexpr uint16_t listenerPort{ 30001 };
    static constexpr std::string_view listenerAddress{ "0.0.0.0" };
    static constexpr std::string_view multicastAddress{ "239.255.0.1" };

    asio::io_context& io_context;
    std::shared_ptr<asio::ip::udp::socket> autoDiscoverySocket;
	std::shared_ptr<asio::ip::udp::endpoint> autoDiscoveryEndpoint;
	std::shared_ptr<asio::steady_timer> autoDiscoveryTimer;
    
    static void timerHandler(std::shared_ptr<asio::steady_timer> timer, const asio::error_code& ec) {
        if (!ec) {
            std::cout << "Auto-discovery timer expired." << std::endl;
            timer->expires_after(std::chrono::seconds(5));
            timer->async_wait(std::bind(timerHandler, timer, std::placeholders::_1));
        }
    }

public:
    Edriel(asio::io_context& io_context_)
		: io_context(io_context_), 
        autoDiscoverySocket{ std::make_shared<asio::ip::udp::socket>(io_context_) },
        autoDiscoveryEndpoint{std::make_shared<asio::ip::udp::endpoint>(asio::ip::address_v4::from_string(std::string(listenerAddress)), listenerPort)},
		autoDiscoveryTimer{ std::make_shared<asio::steady_timer>(io_context_) }
    {
        autoDiscoverySocket->open(autoDiscoveryEndpoint->protocol());
        autoDiscoverySocket->set_option(asio::socket_base::reuse_address(true));
		autoDiscoverySocket->bind(*autoDiscoveryEndpoint);
		autoDiscoveryTimer->expires_after(std::chrono::seconds(5));
        autoDiscoveryTimer->async_wait(std::bind(timerHandler, autoDiscoveryTimer, std::placeholders::_1));
        std::cout << "Edriel constructor" << std::endl;
    }
    ~Edriel(){}
};
