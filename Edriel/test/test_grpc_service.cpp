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
/// in its registry (delivered as a heartbeat carrying endpoints).
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

    // Dialing subscriber announces itself, then half-closes its output so the
    // server's heartbeat-read loop can terminate (else the two Read calls
    // deadlock until the deadline).
    autoDiscovery::ParticipantHeartbeat hb;
    hb.set_pid(5);
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