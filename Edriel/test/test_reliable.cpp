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
#include <cstdio>
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
    // A GENUINE in-flight reorder arriving at an EMPTY window must be buffered
    // and delivered exactly-once in tid order — NOT fast-forwarded past the
    // hole (which would silently drop frame 1). A within-bound gap — one with
    // (tid - nextExpected) < kReliableWindowSize — is always consistent with
    // ordinary reordering and is held, even when the window is empty (ADR-0001).
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

    // In-order 1 is still en route; 3 and 2 arrive first (within the bound).
    // The window must buffer them and deliver 1,2,3 in order, exactly-once.
    sub.handleReliableDataFrame(makeReliableFrame(3, "three"));
    sub.handleReliableDataFrame(makeReliableFrame(2, "two"));
    sub.handleReliableDataFrame(makeReliableFrame(1, "one"));
    // A late duplicate of an already-delivered frame must be dropped
    // (exactly-once) and must not disturb the ordering.
    sub.handleReliableDataFrame(makeReliableFrame(2, "two"));

    std::vector<std::string> expected{"one", "two", "three"};
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(delivered, expected);
}

TEST(Reliable, LateJoinerReceivesCurrentFrames) {
    // ISSUE #3 regression (kept): a subscriber that joins after the publisher's
    // per-(publisher,topic) tid has already moved PAST the bounded window (a gap
    // >= kReliableWindowSize) cannot see those earlier tids in flight — no NAK/
    // replay layer exists — so a FRESH/EMPTY window fast-forwards nextExpected
    // to the incoming tid and delivers current frames immediately, instead of
    // buffering/starving on a gap that will never fill. The large-gap
    // fast-forward must fire ONLY on an empty window (a >= window gap on a
    // mid-stream non-empty window remains a genuine unrecoverable drop).
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

    // The publisher is already at tid 300, more than a full window (256) past
    // the late joiner's nextExpected=1: (300-1) >= kReliableWindowSize, so the
    // fresh empty window fast-forwards and delivers 300,301,302 in order.
    for (const auto& [tid, v] : std::vector<std::pair<std::uint64_t, std::string>>{
             {300, "three-hundred"}, {301, "three-hundred-one"}, {302, "three-hundred-two"}}) {
        sub.handleReliableDataFrame(makeReliableFrame(tid, v));
    }

    std::lock_guard<std::mutex> lock(mu);
    const std::vector<std::string> expected{
        "three-hundred", "three-hundred-one", "three-hundred-two"};
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

TEST(Reliable, ReverseReliableDialBySeedOnlyPeer) {
    // ISSUE #1: a multicast-blind peer reachable only via the `peers:` config
    // seed dials THIS node in the reliable direction. Its identity is the
    // deterministic (pid,0,uid) key synthesized from its configured endpoint
    // (peerKeyForEndpoint, Channel D). The anti-spoof gate must accept it as a
    // known peer (previously only participants + topicRegistry were consulted,
    // so a seed-only dialer was rejected PERMISSION_DENIED).
    const std::string seedEndpoint = "192.168.99.42:55000";

    asio::io_context io;
    edriel::Config cfg;
    cfg.grpcPort = freeTcpPort();
    cfg.peerEndpoints.push_back(seedEndpoint);
    Edriel node(io, cfg);
    node.startGrpcServer();

    // The identity the seed-only peer dials with = peerKeyForEndpoint(endpoint).
    const edriel::SubscriberKey seedKey = Edriel::peerKeyForEndpoint(seedEndpoint);

    // The seed-only peer is a KNOWN participant per the anti-spoof gate (this
    // is the actual ISSUE #1 fix), even though it was never heard on multicast.
    EXPECT_TRUE(node.isKnownParticipant(seedKey.pid, seedKey.tid, seedKey.uid));

    const auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(cfg.grpcPort), grpc::InsecureChannelCredentials());
    auto stub = autoDiscovery::ParticipantStreamService::NewStub(channel);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto stream = stub->StreamParticipants(&ctx);

    autoDiscovery::ParticipantHeartbeat hb;
    hb.set_pid(seedKey.pid);
    hb.set_tid(seedKey.tid);
    hb.set_uid(seedKey.uid);
    ASSERT_TRUE(stream->Write(hb));
    stream->WritesDone();

    // Drain until the server closes the stream. The reverse reliable dial must
    // NOT be rejected as an unknown dialer (previously PERMISSION_DENIED).
    autoDiscovery::ParticipantData pd;
    while (stream->Read(&pd)) {
    }
    const grpc::Status s = stream->Finish();
    EXPECT_NE(s.error_code(), grpc::StatusCode::PERMISSION_DENIED)
        << s.error_message();

    // A fully-unknown identity is still rejected (anti-spoof intact).
    EXPECT_FALSE(node.isKnownParticipant(9999u, 77u, 424242u));
}

TEST(Reliable, SubscribeFromDataCallbackNoDeadlock) {
    // ISSUE #2 regression: a reliable-data frame is dispatched to the user
    // callback from the connection reader thread, and the callback re-enters
    // the reliable sub-system (startReliableSubscriptions()) while another
    // thread is concurrently stopping/joining the connection. Before the fix,
    // reconcile/stop joined the reader thread while holding reliableConnMutex_,
    // so this self-deadlocked (join waits on a thread blocked on the mutex).
    // After the fix, joins happen outside the mutex and the re-entrancy is safe.
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
        [&](const autoDiscovery::Topic&) {
            // Re-enter the reliable sub-system from the reader thread on every
            // frame (the documented ISSUE #2 hazard). This triggers reconcile/
            // teardown reconciliation; the fix ensures the teardown join runs
            // OUTSIDE reliableConnMutex_, so re-entry from the reader thread is
            // deadlock-free. Under the old lock-held join this self-deadlocked.
            sub.startReliableSubscriptions();
            ++calls;
        },
        true));

    pub.startGrpcServer();

    pub.deliverForTest(makeAdvert(sub.selfIdentityForTest(), "sensor", false, true,
                                  "127.0.0.1", cfgSub.grpcPort));
    sub.deliverForTest(makeAdvert(pub.selfIdentityForTest(), "sensor", true, true,
                                  "127.0.0.1", cfgPub.grpcPort));

    sub.startReliableSubscriptions();
    ASSERT_TRUE(waitUntil([&]() { return pub.subscriberConnectedForTest(selfKey(sub)); }, 3000));

    // Deliver an initial frame through the callback (re-entry happens on it).
    ASSERT_TRUE(pub.sendMessage("sensor", makePayloadValue("first")));
    ASSERT_TRUE(waitUntil([&]() { return calls.load() >= 1; }, 3000))
        << "initial frame never delivered";

    // A teardown while a frame is flowing drives the reader thread through the
    // callback (which re-enters subscribe) AND the teardown join concurrently.
    // The fix runs the join OUTSIDE reliableConnMutex_, so the re-entry is deadlock-
    // free; under the old lock-held join the two threads would self-deadlock.
    sub.stopReliableSubscriptions();
    sub.startReliableSubscriptions();
    ASSERT_TRUE(waitUntil([&]() { return pub.subscriberConnectedForTest(selfKey(sub)); }, 3000))
        << "subscription did not restart after teardown cycle";

    // Reconnected and delivering without deadlock.
    EXPECT_TRUE(pub.sendMessage("sensor", makePayloadValue("second")));
    ASSERT_TRUE(waitUntil([&]() { return calls.load() >= 2; }, 3000))
        << "no delivery after re-entrant teardown/no-deadlock recovery";
}

TEST(Reliable, RapidTeardownRedialNoAbort) {
    // BLOCKER regression: drive N rapid stopReliableSubscriptions()/
    // startReliableSubscriptions() cycles with a reliable frame in flight on
    // each. This is the only thing that deterministically races the server's
    // OnReadDone(false) / OnWriteDone(!ok) terminal pair that the single-Finish
    // guard (finish_) and prompt OnCancel eviction defend. Every cycle puts a
    // frame on the in-flight StartWrite and then tears the connection down
    // immediately, so the write can fail (OnWriteDone(!ok)) at the same moment
    // the read failure (OnReadDone(false)) is delivered on another executor
    // thread. WITHOUT the one-shot finish_ guard, that pair finishes the RPC
    // twice and gRPC aborts the process (callback_common.h `call_ == nullptr`
    // CHECK); with the guard, exactly one Finish is issued and the process
    // survives the churn and keeps delivering afterwards.
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
        "sensor", [&calls](const autoDiscovery::Topic&) { ++calls; }, true));
    pub.startGrpcServer();

    pub.deliverForTest(makeAdvert(sub.selfIdentityForTest(), "sensor", false, true,
                                  "127.0.0.1", cfgSub.grpcPort));
    sub.deliverForTest(makeAdvert(pub.selfIdentityForTest(), "sensor", true, true,
                                  "127.0.0.1", cfgPub.grpcPort));

    sub.startReliableSubscriptions();
    ASSERT_TRUE(waitUntil([&]() { return pub.subscriberConnectedForTest(selfKey(sub)); }, 3000))
        << "initial dial never landed";

    constexpr int kCycles = 25;
    for (int i = 0; i < kCycles; ++i) {
        // A reliable frame lands on the server's outbox / in-flight write...
        pub.sendMessage("sensor", makePayloadValue("churn-" + std::to_string(i)));
        // ...then tear the connection down immediately, racing the terminal
        // OnReadDone(false) / OnWriteDone(!ok) pair. No abort must happen and
        // the re-dial must come back up.
        sub.stopReliableSubscriptions();
        sub.startReliableSubscriptions();
        ASSERT_TRUE(waitUntil([&]() { return pub.subscriberConnectedForTest(selfKey(sub)); }, 2000))
            << "re-dial stalled after churn cycle " << i;
    }

    // Stable delivery across the churn: a frame sent after all cycles still
    // lands exactly once (frames dropped mid-teardown are expected; delivery
    // resuming is not).
    EXPECT_TRUE(pub.sendMessage("sensor", makePayloadValue("final")));
    ASSERT_TRUE(waitUntil([&]() { return calls.load() >= 1; }, 3000))
        << "no delivery after rapid teardown churn";
    EXPECT_TRUE(pub.subscriberConnectedForTest(selfKey(sub)));
}

/// Build a heartbeat Message registering participant (pid, tid, uid) so the
/// timeout cleanup sees it as a live-then-stale peer (populates `participants`).
autoDiscovery::Message makeHeartbeat(std::uint32_t pid, std::uint64_t tid,
                                     std::uint64_t uid) {
    autoDiscovery::Message m;
    auto* id = m.mutable_identifier();
    id->set_pid(pid);
    id->set_tid(tid);
    id->set_uid(uid);
    return m;
}

/// Build a reliable frame from a publisher with the given (uid, pid, tid, topic).
autoDiscovery::ParticipantData makeReliableFrameFrom(std::uint64_t uid,
                                                     std::uint32_t pid,
                                                     std::uint64_t tid,
                                                     const std::string& topic,
                                                     const std::string& value) {
    autoDiscovery::DataMessage dm;
    dm.mutable_identifier()->set_pid(pid);
    dm.mutable_identifier()->set_tid(tid);
    dm.mutable_identifier()->set_uid(uid);
    dm.set_topic_name(topic);
    dm.set_message_type("autoDiscovery.Topic");
    dm.set_payload(makePayloadValue(value).SerializeAsString());
    autoDiscovery::ParticipantData pd;
    pd.set_reliable_data(dm.SerializeAsString());
    return pd;
}

TEST(Reliable, ReliableWindowsPrunedOnPublisherTimeout) {
    // ISSUE #4 regression: per-(publisher,topic) receive windows keyed by
    // `to_string(pubUid)+"|"+compositeKey` must be pruned when the publisher
    // participant times out (removeTimedOutParticipants). Without the prune the
    // window map grows without bound as publishers cycle through the registry.
    // reliablePublisherSeq_ is keyed by topic alone so it needs no per-pub
    // pruning (bounded by topic count; asserted implicitly by the source).
    asio::io_context io;
    Edriel sub(io);

    constexpr std::uint32_t kBasePid = 100;
    constexpr std::uint64_t kBaseUid = 900000u;

    // Register a batch of publishers that advertise/publish "sensor" reliably.
    for (std::size_t i = 0; i < 8; ++i) {
        const std::uint32_t pid = kBasePid + static_cast<std::uint32_t>(i);
        const std::uint64_t uid = kBaseUid + i;
        sub.deliverForTest(makeHeartbeat(pid, 0u, uid));
        // A reliable frame from that publisher creates its receive window.
        sub.handleReliableDataFrame(makeReliableFrameFrom(uid, pid, 1u, "sensor", "s"));
        // A second topic window per publisher too (per-(pub,topic) growth).
        sub.handleReliableDataFrame(makeReliableFrameFrom(uid, pid, 1u, "other", "o"));
    }
    EXPECT_EQ(sub.reliableWindowsSizeForTest(), 16u);

    // Age every publisher and run the timeout cleanup: all windows must go.
    for (std::size_t i = 0; i < 8; ++i) {
        sub.ageParticipantForTest(kBaseUid + i);
    }
    sub.runTimeoutCleanupForTest();
    EXPECT_EQ(sub.reliableWindowsSizeForTest(), 0u);

    // Churn: publishers kept appearing/timing out across time; the map must
    // return to zero on each timeout cleanup (no monotonic growth).
    for (std::size_t i = 0; i < 5; ++i) {
        const std::string topic = (i % 2 == 0) ? "sensor" : "gps";
        const std::uint64_t uid = 300000u + i;
        const std::uint32_t pid = 200u + static_cast<std::uint32_t>(i);
        sub.deliverForTest(makeHeartbeat(pid, 0u, uid));
        sub.handleReliableDataFrame(makeReliableFrameFrom(uid, pid, 1u, topic, "v"));
        EXPECT_GE(sub.reliableWindowsSizeForTest(), 1u);
        sub.ageParticipantForTest(uid);
        sub.runTimeoutCleanupForTest();
        EXPECT_EQ(sub.reliableWindowsSizeForTest(), 0u);
    }
}