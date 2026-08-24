/**
 * @file test_reliable.cpp
 * @brief ADR-0002 M4/M5: reliable send path tests.
 *
 *   (c) reliable-over-gRPC round-trip in one process: a subscriber dials a
 *       publisher's server (subscriber-initiated), the publisher pushes a
 *       reliable payload frame over the bidi stream, and the subscriber
 *       receives it exactly-once through its callback.
 *   (reorder/dedup) the bounded exactly-once window is exercised with
 *       out-of-order and duplicate per-(pub,topic) tids.
 *   (routing/M5) sendMessage dispatches reliable topics to the gRPC path and
 *       best-effort topics to multicast, as today.
 */

#include <gtest/gtest.h>

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "Edriel.hpp"
#include "autoDiscovery.pb.h"
#include "autoDiscovery_grpc_service.pb.h"

using edriel::Edriel;
using edriel::SubscriberKey;

namespace {

std::uint16_t freeTcpPort() {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.open(asio::ip::tcp::v4());
    sock.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::any(), 0));
    return sock.local_endpoint().port();
}

bool waitUntil(std::function<bool()> pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

autoDiscovery::Topic makePayloadValue(const std::string& value) {
    autoDiscovery::Topic t;
    t.set_topic_name(value);
    return t;
}

/// Advertise from `who` that it publishes/subscribes `topic` with `reliable`
/// QoS, carrying one advertised endpoint so the recipient can dial it.
autoDiscovery::Message makeAdvert(const Edriel::Participant& who,
                                  const std::string& topic, bool isPublisher,
                                  bool reliable, const std::string& addr,
                                  std::uint32_t port) {
    autoDiscovery::Message ad;
    autoDiscovery::TopicAdvertisement* adv = ad.mutable_advertisement();
    auto* id = adv->mutable_identifier();
    id->set_pid(who.pid);
    id->set_tid(who.tid);
    id->set_uid(who.uid);
    auto* ep = id->add_endpoints();
    ep->set_address(addr);
    ep->set_port(port);
    ep->set_transport(autoDiscovery::Endpoint::GRPC_TCP);
    auto* topicProto = adv->mutable_topic();
    topicProto->set_topic_name(topic);
    topicProto->set_message_type("autoDiscovery.Topic");
    topicProto->set_is_publisher(isPublisher);
    topicProto->set_reliable(reliable);
    return ad;
}

/// Build a ParticipantData reliable frame carrying a DataMessage for
/// (pub uid 999, topic "sensor") with the given per-(pub,topic) tid.
autoDiscovery::ParticipantData makeReliableFrame(std::uint64_t tid,
                                                 const std::string& value) {
    autoDiscovery::DataMessage dm;
    dm.mutable_identifier()->set_pid(77);
    dm.mutable_identifier()->set_tid(tid);
    dm.mutable_identifier()->set_uid(999);
    dm.set_topic_name("sensor");
    dm.set_message_type("autoDiscovery.Topic");
    dm.set_payload(makePayloadValue(value).SerializeAsString());
    autoDiscovery::ParticipantData pd;
    pd.set_reliable_data(dm.SerializeAsString());
    return pd;
}

}  // namespace

TEST(Reliable, RoundTripPublisherToSubscriber) {
    asio::io_context ioPub, ioSub;

    edriel::Config cfgPub;
    cfgPub.grpcPort = freeTcpPort();
    edriel::Config cfgSub;
    cfgSub.grpcPort = freeTcpPort();

    Edriel pub(ioPub, cfgPub);
    Edriel sub(ioSub, cfgSub);

    ASSERT_TRUE(pub.registerPublisherTopic<autoDiscovery::Topic>("sensor", true));
    std::atomic<int> calls{0};
    std::string received;
    // The subscriber node registers the reliable topic with a callback.
    ASSERT_TRUE(sub.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor",
        [&calls, &received](const autoDiscovery::Topic& t) {
            ++calls;
            received = t.topic_name();
        },
        true));

    pub.startGrpcServer();
    // (The subscriber node also serves, mirroring "every node serves".)
    sub.startGrpcServer();

    // Cross-inject a consistent registry: `sub` subscribes to "sensor" (seen
    // by pub) and `pub` publishes "sensor" at 127.0.0.1:pubPort (seen by sub).
    const Edriel::Participant pubSelf = pub.selfIdentityForTest();
    const Edriel::Participant subSelf = sub.selfIdentityForTest();

    // sub sees pub as a publisher of "sensor".
    sub.deliverForTest(makeAdvert(pubSelf, "sensor", true, true,
                                  "127.0.0.1", cfgPub.grpcPort));
    // pub sees sub as a subscriber of "sensor".
    pub.deliverForTest(makeAdvert(subSelf, "sensor", false, true,
                                  "127.0.0.1", cfgSub.grpcPort));

    // Subscriber-initiated: sub dials pub.
    sub.startReliableSubscriptions();

    const SubscriberKey subKey{
        static_cast<std::uint32_t>(subSelf.pid), subSelf.tid, subSelf.uid};

    // Wait for the dial to land (stream registered on pub's server).
    ASSERT_TRUE(waitUntil([&]() { return pub.subscriberConnectedForTest(subKey); }, 3000))
        << "subscriber stream never registered on publisher";

    // Publisher sends a reliable message; it must be pushed (not multicast).
    EXPECT_TRUE(pub.sendMessage("sensor", makePayloadValue("42")));

    // Subscriber's callback fires exactly once with the delivered payload.
    ASSERT_TRUE(waitUntil([&]() { return calls.load() == 1; }, 3000))
        << "subscriber callback never fired";
    EXPECT_EQ(received, "42");

    // No duplicates on retransmission-free TCP; still exactly once after a
    // second publisher send.
    EXPECT_TRUE(pub.sendMessage("sensor", makePayloadValue("43")));
    ASSERT_TRUE(waitUntil([&]() { return calls.load() == 2; }, 3000));
    EXPECT_EQ(received, "43");
}

TEST(Reliable, ReorderDedupExactlyOnce) {
    asio::io_context io;
    Edriel sub(io);

    std::vector<std::string> delivered;
    std::mutex mu;
    ASSERT_TRUE(sub.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor",
        [&delivered, &mu](const autoDiscovery::Topic& t) {
            std::lock_guard<std::mutex> lock(mu);
            delivered.push_back(t.topic_name());
        },
        true));

    // Deliver out of order and duplicated; expect exactly-once, in order.
    sub.handleReliableDataFrame(makeReliableFrame(3, "three"));   // buffered
    sub.handleReliableDataFrame(makeReliableFrame(1, "one"));     // delivers 1,3
    sub.handleReliableDataFrame(makeReliableFrame(3, "three"));   // duplicate -> drop
    sub.handleReliableDataFrame(makeReliableFrame(5, "five"));    // buffered (gap 4)
    sub.handleReliableDataFrame(makeReliableFrame(4, "four"));    // delivers 4,5

    std::vector<std::string> expected{"one", "three", "four", "five"};
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(delivered, expected);
}

TEST(Reliable, SendMessageRoutesByQoS) {
    asio::io_context io;
    Edriel pub(io);

    // A reliable topic with a server up but no subscriber: the reliable send
    // path is taken (returns false, nothing to push -- no multicast frame).
    ASSERT_TRUE(pub.registerPublisherTopic<autoDiscovery::Topic>("reliable_topic", true));
    pub.startGrpcServer();
    EXPECT_FALSE(pub.sendMessage("reliable_topic", makePayloadValue("x")));

    // A best-effort topic: routed to multicast exactly as before (returns true;
    // frames are dispatched to the strand socket, not the gRPC path).
    ASSERT_TRUE(pub.registerPublisherTopic<autoDiscovery::Topic>("besteffort_topic", false));
    EXPECT_TRUE(pub.sendMessage("besteffort_topic", makePayloadValue("y")));
}