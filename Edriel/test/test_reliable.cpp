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
#include <utility>
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

/// Advertise a publisher with identity (pid,tid,uid) announcing `topic` as
/// reliable, carrying the given ordered endpoint candidates (multi-homed). Used
/// to drive the subscriber's reconciliation without a live multicast group.
autoDiscovery::Message makePubAdvert(
    std::uint32_t pid, std::uint64_t tid, std::uint64_t uid,
    const std::string& topic, bool reliable,
    const std::vector<std::pair<std::string, std::uint32_t>>& endpoints) {
    autoDiscovery::Message ad;
    auto* adv = ad.mutable_advertisement();
    auto* id = adv->mutable_identifier();
    id->set_pid(pid);
    id->set_tid(tid);
    id->set_uid(uid);
    for (const auto& [addr, port] : endpoints) {
        auto* ep = id->add_endpoints();
        ep->set_address(addr);
        ep->set_port(port);
        ep->set_transport(autoDiscovery::Endpoint::GRPC_TCP);
    }
    auto* tp = adv->mutable_topic();
    tp->set_topic_name(topic);
    tp->set_message_type("autoDiscovery.Topic");
    tp->set_is_publisher(true);
    tp->set_reliable(reliable);
    return ad;
}

/// The identity this node's publisher server registers a dialing subscriber
/// under (the subscriber's OWN self identity).
edriel::SubscriberKey selfKey(const Edriel& node) {
    const auto& self = node.selfIdentityForTest();
    return edriel::SubscriberKey{static_cast<std::uint32_t>(self.pid),
                                 self.tid, self.uid};
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

TEST(Reliable, MultiHomedAdvanceToNextCandidate) {
    // A publisher advertised at two endpoint candidates: the FIRST is dead
    // (connection refused) and the SECOND is a live server. The subscriber's
    // connect-in-order must advance past the dead candidate and land on the live
    // one (ADR-0002 §6.3, first-wins-on-reachability).
    asio::io_context ioPub, ioSub;

    const std::uint16_t deadPort = freeTcpPort();  // grabbed, then closed -> refused
    edriel::Config cfgPub;
    cfgPub.grpcPort = freeTcpPort();
    edriel::Config cfgSub;
    cfgSub.grpcPort = freeTcpPort();

    Edriel pub(ioPub, cfgPub);
    Edriel sub(ioSub, cfgSub);

    ASSERT_TRUE(pub.registerPublisherTopic<autoDiscovery::Topic>("sensor", true));
    std::atomic<int> calls{0};
    ASSERT_TRUE(sub.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor",
        [&calls](const autoDiscovery::Topic&) { ++calls; }, true));
    pub.startGrpcServer();

    // pub knows the dialing subscriber (anti-spoof gate accepts it).
    pub.deliverForTest(makeAdvert(sub.selfIdentityForTest(), "sensor", false, true,
                                  "127.0.0.1", cfgSub.grpcPort));
    // sub sees a publisher X advertised at [dead, live].
    sub.deliverForTest(makePubAdvert(77, 7, 20201u, "sensor", true,
                                     {{"127.0.0.1", deadPort},
                                      {"127.0.0.1", cfgPub.grpcPort}}));

    sub.startReliableSubscriptions();

    // The connection resolves to the SECOND (live) candidate.
    ASSERT_TRUE(waitUntil([&]() { return pub.subscriberConnectedForTest(selfKey(sub)); }, 4000))
        << "multi-homed connect did not advance to the live candidate";
}

TEST(Reliable, ReDialOnEndpointChange) {
    // The same publisher identity is discovered at endpoint P1, connected, then
    // re-advertises at a new endpoint P2 (IP change / peer move under the same
    // (pid,tid,uid)). Reconciliation must tear down the stale P1 connection and
    // re-dial P2 (ADR-0002 §5 stale-endpoint re-dial).
    asio::io_context ioPub1, ioPub2, ioSub;

    edriel::Config cfgPub1;
    cfgPub1.grpcPort = freeTcpPort();
    edriel::Config cfgPub2;
    cfgPub2.grpcPort = freeTcpPort();
    edriel::Config cfgSub;
    cfgSub.grpcPort = freeTcpPort();

    Edriel pub1(ioPub1, cfgPub1);
    Edriel pub2(ioPub2, cfgPub2);
    Edriel sub(ioSub, cfgSub);

    ASSERT_TRUE(pub1.registerPublisherTopic<autoDiscovery::Topic>("sensor", true));
    ASSERT_TRUE(pub2.registerPublisherTopic<autoDiscovery::Topic>("sensor", true));
    std::atomic<int> calls{0};
    ASSERT_TRUE(sub.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor",
        [&calls](const autoDiscovery::Topic&) { ++calls; }, true));
    pub1.startGrpcServer();
    pub2.startGrpcServer();

    // Both servers know the dialing subscriber (anti-spoof).
    pub1.deliverForTest(makeAdvert(sub.selfIdentityForTest(), "sensor", false, true,
                                   "127.0.0.1", cfgSub.grpcPort));
    pub2.deliverForTest(makeAdvert(sub.selfIdentityForTest(), "sensor", false, true,
                                   "127.0.0.1", cfgSub.grpcPort));

    constexpr std::uint32_t kPid = 77;
    constexpr std::uint64_t kTid = 7;
    constexpr std::uint64_t kUid = 30303u;  // the publisher identity

    // Phase 1: publisher X lives at P1.
    sub.deliverForTest(makePubAdvert(kPid, kTid, kUid, "sensor", true,
                                     {{"127.0.0.1", cfgPub1.grpcPort}}));
    sub.startReliableSubscriptions();
    ASSERT_TRUE(waitUntil([&]() { return pub1.subscriberConnectedForTest(selfKey(sub)); }, 4000))
        << "initial connection to P1 never established";

    // Phase 2: X moves to P2. Reconcile again with the new advertised endpoint.
    sub.deliverForTest(makePubAdvert(kPid, kTid, kUid, "sensor", true,
                                     {{"127.0.0.1", cfgPub2.grpcPort}}));
    sub.startReliableSubscriptions();

    ASSERT_TRUE(waitUntil([&]() {
        return !pub1.subscriberConnectedForTest(selfKey(sub))
            && pub2.subscriberConnectedForTest(selfKey(sub));
    }, 5000))
        << "re-dial did not move off the stale P1 onto the new P2 endpoint";
}

TEST(Reliable, StaticPeerSeedChannelD) {
    // Channel D (ADR-0002): a multicast-blind subscriber learns no publisher via
    // discovery but dials a configured static `peers:` endpoint directly. With a
    // live server there, the dial lands and reliable frames flow.
    asio::io_context ioPub, ioSub;

    edriel::Config cfgPub;
    cfgPub.grpcPort = freeTcpPort();
    const std::string peerEndpoint = "127.0.0.1:" + std::to_string(cfgPub.grpcPort);

    edriel::Config cfgSub;
    cfgSub.grpcPort = freeTcpPort();
    cfgSub.peerEndpoints.push_back(peerEndpoint);

    Edriel pub(ioPub, cfgPub);
    Edriel sub(ioSub, cfgSub);

    ASSERT_TRUE(pub.registerPublisherTopic<autoDiscovery::Topic>("sensor", true));
    std::atomic<int> calls{0};
    std::string received;
    std::mutex mu;
    ASSERT_TRUE(sub.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor",
        [&calls, &received, &mu](const autoDiscovery::Topic& t) {
            ++calls;
            std::lock_guard<std::mutex> lock(mu);
            received = t.topic_name();
        },
        true));
    pub.startGrpcServer();

    // The peer server must know the dialing subscriber (anti-spoof).
    pub.deliverForTest(makeAdvert(sub.selfIdentityForTest(), "sensor", false, true,
                                  "127.0.0.1", cfgSub.grpcPort));

    // No multicast advertisement reaches `sub`; only the static peer seed.
    sub.startReliableSubscriptions();

    ASSERT_TRUE(waitUntil([&]() { return pub.subscriberConnectedForTest(selfKey(sub)); }, 4000))
        << "static Channel D peer was never dialed";

    EXPECT_TRUE(pub.sendMessage("sensor", makePayloadValue("seed-42")));
    ASSERT_TRUE(waitUntil([&]() { return calls.load() == 1; }, 3000))
        << "no reliable frame over the static-peer stream";
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(received, "seed-42");
}

TEST(Reliable, MtuRejectOnOversizedFrame) {
    // The reliable path shares the multicast data-plane MTU budget (1500 B):
    // an oversized reliable frame is rejected at send time (returns false) and
    // a within-budget frame still flows.
    asio::io_context ioPub, ioSub;

    edriel::Config cfgPub;
    cfgPub.grpcPort = freeTcpPort();
    edriel::Config cfgSub;
    cfgSub.grpcPort = freeTcpPort();

    Edriel pub(ioPub, cfgPub);
    Edriel sub(ioSub, cfgSub);

    ASSERT_TRUE(pub.registerPublisherTopic<autoDiscovery::Topic>("sensor", true));
    std::atomic<int> calls{0};
    ASSERT_TRUE(sub.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor",
        [&calls](const autoDiscovery::Topic&) { ++calls; }, true));
    pub.startGrpcServer();
    sub.startGrpcServer();

    pub.deliverForTest(makeAdvert(sub.selfIdentityForTest(), "sensor", false, true,
                                  "127.0.0.1", cfgSub.grpcPort));
    sub.deliverForTest(makeAdvert(pub.selfIdentityForTest(), "sensor", true, true,
                                  "127.0.0.1", cfgPub.grpcPort));

    sub.startReliableSubscriptions();
    ASSERT_TRUE(waitUntil([&]() { return pub.subscriberConnectedForTest(selfKey(sub)); }, 3000));

    // A reliable payload that serializes past the 1500-byte budget -> rejected.
    autoDiscovery::Topic big;
    big.set_topic_name(std::string(3000, 'x'));
    EXPECT_FALSE(pub.sendMessage("sensor", big));

    // A normal-size reliable message still flows exactly once.
    EXPECT_TRUE(pub.sendMessage("sensor", makePayloadValue("42")));
    ASSERT_TRUE(waitUntil([&]() { return calls.load() == 1; }, 3000));
}