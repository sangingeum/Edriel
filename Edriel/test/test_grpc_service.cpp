/**
 * @file test_grpc_service.cpp
 * @brief ADR-0002 M3: in-process client test for the reliability gRPC server.
 *
 * Starts an Edriel node's ParticipantStreamService on a free loopback port,
 * then drives it from a synchronous client stub: GetParticipantInfo (unary,
 * Channel C verifier) for a known and an unknown participant, and
 * StreamParticipants (bidi) presence delivery.
 */

#include <gtest/gtest.h>

#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <string>

#include "Edriel.hpp"
#include "autoDiscovery_grpc_service.grpc.pb.h"

using edriel::Edriel;

namespace {

/// Grab a currently-free ephemeral TCP port (bind :0, read it, close).
std::uint16_t freeTcpPort() {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.open(asio::ip::tcp::v4());
    sock.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::any(), 0));
    return sock.local_endpoint().port();
}

/// Build an Edriel bound to a free grpc port and a remote participant already
/// in its registry (delivered as a heartbeat carrying endpoints). A second
/// participant (`probe`) is also injected so a StreamParticipants dialer can
/// present a *known* identity that passes the server's anti-spoof gate.
std::unique_ptr<Edriel> makeNode(asio::io_context& io, std::uint16_t grpcPort) {
    edriel::Config cfg;
    cfg.grpcPort = grpcPort;

    auto node = std::make_unique<Edriel>(io, cfg);

    // A remote peer ("router") heartbeating two unicast endpoints.
    autoDiscovery::Message hb;
    auto* id = hb.mutable_identifier();
    id->set_pid(77);
    id->set_tid(1);
    id->set_uid(987654321u);
    auto* ep1 = id->add_endpoints();
    ep1->set_address("192.168.1.20");
    ep1->set_port(std::min<std::uint32_t>(grpcPort, 65535u));
    ep1->set_transport(autoDiscovery::Endpoint::GRPC_TCP);
    auto* ep2 = id->add_endpoints();
    ep2->set_address("10.0.0.20");
    ep2->set_port(std::min<std::uint32_t>(grpcPort, 65535u));
    ep2->set_transport(autoDiscovery::Endpoint::GRPC_TCP);
    node->deliverForTest(hb);

    // A second peer ("probe") that acts as a legitimate dialing subscriber.
    autoDiscovery::Message probe;
    auto* pid = probe.mutable_identifier();
    pid->set_pid(5);
    pid->set_tid(3);
    pid->set_uid(888813u);
    auto* pep = pid->add_endpoints();
    pep->set_address("127.0.0.1");
    pep->set_port(std::min<std::uint32_t>(grpcPort, 65535u));
    pep->set_transport(autoDiscovery::Endpoint::GRPC_TCP);
    node->deliverForTest(probe);

    return node;
}

}  // namespace

TEST(TestGrpcService, StartsAndStops) {
    asio::io_context io;
    const std::uint16_t port = freeTcpPort();
    auto node = makeNode(io, port);

    node->startGrpcServer();

    const auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = autoDiscovery::ParticipantStreamService::NewStub(channel);

    autoDiscovery::ParticipantHeartbeat req;
    req.set_pid(77);
    req.set_tid(1);
    req.set_uid(987654321u);
    autoDiscovery::ParticipantData resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    EXPECT_TRUE(stub->GetParticipantInfo(&ctx, req, &resp).ok());

    // Idempotent start: a second call is a no-op (already serving).
    node->startGrpcServer();
    node->stopGrpcServer();
}

TEST(TestGrpcService, GetParticipantInfoKnownIdentity) {
    asio::io_context io;
    const std::uint16_t port = freeTcpPort();
    auto node = makeNode(io, port);
    node->startGrpcServer();

    const auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = autoDiscovery::ParticipantStreamService::NewStub(channel);

    autoDiscovery::ParticipantHeartbeat req;
    req.set_pid(77);
    req.set_tid(1);
    req.set_uid(987654321u);
    autoDiscovery::ParticipantData resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    const grpc::Status s = stub->GetParticipantInfo(&ctx, req, &resp);
    EXPECT_TRUE(s.ok()) << s.error_message();
    EXPECT_EQ(resp.pid(), 77u);
    EXPECT_EQ(resp.uid(), 987654321u);
    EXPECT_EQ(resp.status(), "online");
    ASSERT_EQ(resp.endpoints_size(), 2);
    EXPECT_EQ(resp.endpoints(0).address(), "192.168.1.20");
    EXPECT_EQ(resp.endpoints(1).address(), "10.0.0.20");
}

TEST(TestGrpcService, GetParticipantInfoUnknownIdentity) {
    asio::io_context io;
    const std::uint16_t port = freeTcpPort();
    auto node = makeNode(io, port);
    node->startGrpcServer();

    const auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = autoDiscovery::ParticipantStreamService::NewStub(channel);

    autoDiscovery::ParticipantHeartbeat req;
    req.set_pid(9999);
    req.set_tid(0);
    req.set_uid(424242u);
    autoDiscovery::ParticipantData resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    const grpc::Status s = stub->GetParticipantInfo(&ctx, req, &resp);
    EXPECT_EQ(s.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST(TestGrpcService, StreamParticipantsPushesPresence) {
    asio::io_context io;
    const std::uint16_t port = freeTcpPort();
    auto node = makeNode(io, port);
    node->startGrpcServer();

    const auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = autoDiscovery::ParticipantStreamService::NewStub(channel);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto stream = stub->StreamParticipants(&ctx);

    // Dialing subscriber announces itself with a *known* identity (the injected
    // `probe`), then half-closes its output so the server's heartbeat-read loop
    // can terminate (else the two Read calls deadlock until the deadline).
    autoDiscovery::ParticipantHeartbeat hb;
    hb.set_pid(5);
    hb.set_tid(3);
    hb.set_uid(888813u);
    ASSERT_TRUE(stream->Write(hb));
    stream->WritesDone();

    // Collect presence until the server closes the stream.
    std::vector<autoDiscovery::ParticipantData> presence;
    autoDiscovery::ParticipantData pd;
    while (stream->Read(&pd)) {
        presence.push_back(pd);
    }
    EXPECT_TRUE(stream->Finish().ok());

    // The injected remote participant must be present.
    ASSERT_FALSE(presence.empty());
    bool foundRouter = false;
    for (const auto& p : presence) {
        if (p.uid() == 987654321u) {
            foundRouter = true;
            EXPECT_EQ(p.pid(), 77u);
            EXPECT_EQ(p.status(), "online");
            EXPECT_EQ(p.endpoints_size(), 2);
        }
    }
    EXPECT_TRUE(foundRouter);
}

TEST(TestGrpcService, AntiSpoofRejectsUnknownDialer) {
    asio::io_context io;
    const std::uint16_t port = freeTcpPort();
    auto node = makeNode(io, port);
    node->startGrpcServer();

    const auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = autoDiscovery::ParticipantStreamService::NewStub(channel);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto stream = stub->StreamParticipants(&ctx);

    // Dial with an identity that is NOT a known participant (never heartbeated,
    // never announced a topic). The server must reject it: no registration, no
    // presence pushed, and the stream is finished with a denied status.
    autoDiscovery::ParticipantHeartbeat hb;
    hb.set_pid(9999);
    hb.set_tid(0);
    hb.set_uid(424242u);
    ASSERT_TRUE(stream->Write(hb));
    stream->WritesDone();

    // The server closes the stream without writing any presence frame.
    autoDiscovery::ParticipantData pd;
    EXPECT_FALSE(stream->Read(&pd));

    const grpc::Status s = stream->Finish();
    EXPECT_EQ(s.error_code(), grpc::StatusCode::PERMISSION_DENIED)
        << s.error_message();

    // The spoofed identity is never registered on the server.
    const edriel::SubscriberKey spoofed{9999u, 0u, 424242u};
    EXPECT_FALSE(node->subscriberConnectedForTest(spoofed));
}