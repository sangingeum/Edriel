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

class Edriel {
private:
    // [수정 1] 포트 통일: 보내는 포트와 받는 포트가 같아야 수신 가능
    static constexpr uint16_t commonPort{ 30002 };
    
    static constexpr std::string_view listenerAddress{ "0.0.0.0" };
    static constexpr std::string_view multicastAddress{ "239.255.0.1" };

    asio::io_context& io_context;
    
    // 수신용 소켓
    std::shared_ptr<asio::ip::udp::socket> autoDiscoverySocket;
    std::shared_ptr<asio::ip::udp::endpoint> autoDiscoveryEndpoint; // 바인딩용 (Any)
    
    // 송신용 소켓
    std::shared_ptr<asio::ip::udp::socket> multicastSocket;
    std::shared_ptr<asio::ip::udp::endpoint> multicastTargetEndpoint; // 전송 타겟

    std::shared_ptr<asio::steady_timer> autoDiscoveryTimer;

    std::array<char, 1024> recvBuffer;
    asio::ip::udp::endpoint senderEndpoint; // 패킷을 보낸 사람의 주소를 담을 곳
    
    std::shared_ptr<std::string> message = std::make_shared<std::string>("Discovery Packet");

    void startReceive() {
        autoDiscoverySocket->async_receive_from(
            asio::buffer(recvBuffer), senderEndpoint,
            std::bind(&Edriel::handleReceive, this, std::placeholders::_1, std::placeholders::_2));
    }

    void handleReceive(const asio::error_code& error, std::size_t bytes_transferred) {
        if (!error) {
            std::string msg(recvBuffer.data(), bytes_transferred);
            std::cout << "[Recv] From " << senderEndpoint << " : " << msg << std::endl;
            
            startReceive();
        } else {
            std::cerr << "Receive error: " << error.message() << std::endl;
        }
    }

    void timerHandler(const asio::error_code& ec) {
        if (!ec) {
            // std::cout << "Auto-discovery timer expired." << std::endl;
            
            
            // [참고] 멀티캐스트 전송
            multicastSocket->async_send_to(
                asio::buffer(*message), *multicastTargetEndpoint,
                [](const asio::error_code& ec, std::size_t bytes_transferred) {
                    if (ec) {
                        std::cerr << "Send failed: " << ec.message() << std::endl;
                    } else {
                        // std::cout << "Sent " << bytes_transferred << " bytes" << std::endl;
                    }
                });

            autoDiscoveryTimer->expires_after(std::chrono::seconds(2)); 
            autoDiscoveryTimer->async_wait(std::bind(&Edriel::timerHandler, this, std::placeholders::_1));
        }
    }

public:
    Edriel(asio::io_context& io_context_)
        : io_context(io_context_),
        // 1. 수신 소켓 초기화
        autoDiscoverySocket{ std::make_shared<asio::ip::udp::socket>(io_context_) },
        autoDiscoveryEndpoint{ std::make_shared<asio::ip::udp::endpoint>(asio::ip::address_v4::any(), commonPort) }, // 0.0.0.0:30002
        
        // 2. 송신 소켓 초기화
        multicastSocket{ std::make_shared<asio::ip::udp::socket>(io_context_) },
        multicastTargetEndpoint{ std::make_shared<asio::ip::udp::endpoint>(asio::ip::address_v4::from_string(std::string(multicastAddress)), commonPort) }, // 239.255.0.1:30002
        
        autoDiscoveryTimer{ std::make_shared<asio::steady_timer>(io_context_) }
    {
        // -------------------------------------------------------
        // [수신 소켓 설정]
        // -------------------------------------------------------
        autoDiscoverySocket->open(autoDiscoveryEndpoint->protocol());
        autoDiscoverySocket->set_option(asio::socket_base::reuse_address(true));
        
        // [수정 2] 반드시 0.0.0.0(Any) 포트 30002에 바인딩해야 함
        autoDiscoverySocket->bind(*autoDiscoveryEndpoint);

        // [수정 3] 멀티캐스트 그룹 가입 (가장 중요)
        // 이 코드가 있어야 239.255.0.1로 온 패킷을 이 소켓이 낚아챕니다.
        try {
            autoDiscoverySocket->set_option(
                asio::ip::multicast::join_group(asio::ip::address::from_string(std::string(multicastAddress)))
            );
        } catch (std::exception& e) {
            std::cerr << "Join Group Failed: " << e.what() << std::endl;
        }

        // -------------------------------------------------------
        // [송신 소켓 설정]
        // -------------------------------------------------------
        multicastSocket->open(multicastTargetEndpoint->protocol());
        
        // [수정 4] 내가 보낸 걸 내가 받기 위해 Loopback 옵션 활성화
        ///multicastSocket->set_option(asio::ip::multicast::enable_loopback(true));
        
        // 송신 소켓은 굳이 Bind 할 필요 없거나, 하더라도 Any 포트로 함.
        // (기존 코드처럼 멀티캐스트 IP에 바인딩하면 송신용으로는 부적절할 수 있음)

        // 타이머 시작
        autoDiscoveryTimer->expires_after(std::chrono::seconds(2));
        autoDiscoveryTimer->async_wait(std::bind(&Edriel::timerHandler, this, std::placeholders::_1));

        // 수신 시작
        startReceive();
        
        std::cout << "Edriel initialized. Listening on port " << commonPort << " group " << multicastAddress << std::endl;
    }
    
    ~Edriel(){}
};