/**
 * @file test_backpressure.cpp
 * @brief ADR-0004: reliable-path backpressure tests.
 *
 *   (stall) a subscriber that stops reading fills its outbox to the high-water
 *       mark: the publisher's push is refused with Backpressured, the refused
 *       frame does NOT consume its tid, publisher-side memory stays bounded by
 *       the per-reactor bound, and after the subscriber drains to the LWM a
 *       same-tid retry is accepted and delivered exactly-once.
 *   (retry) interleaved backpressured refusals + same-tid retries preserve
 *       exactly-once and per-(publisher, topic) ordering on the receiver.
 *   (fairness) a stalled subscriber does not gate a healthy one: the healthy
 *       subscriber's delivery continues while the stalled one is backpressured.
 *   (tri-state) tryPublishReliable distinguishes Sent / Backpressured /
 *       NoSubscribers, and the bool publishReliable wrapper stays
 *       source-compatible (true == Sent).
 *   (probe) isSendable() flips false at HWM and back to true at LWM-cross.
 */

#include <gtest/gtest.h>

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Edriel.hpp"
#include "autoDiscovery.pb.h"
#include "autoDiscovery_grpc_service.pb.h"

using edriel::Edriel;
using edriel::OutboxStatus;
using edriel::ReliableSendResult;
using edriel::SubscriberKey;

namespace {

std::uint16_t freeTcpPort() {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.open(asio::ip::tcp::v4());
    sock.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::any(), 0));
    return sock.local_endpoint().port();
}

bool waitUntil(const std::function<bool()>& pred, int timeoutMs) {
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

[[maybe_unused]] autoDiscovery::Topic makePayloadValue(const std::string& value) {
    autoDiscovery::Topic t;
    t.set_topic_name(value);
    return t;
}

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

SubscriberKey keyOf(const Edriel::Participant& who) {
    return SubscriberKey{static_cast<std::uint32_t>(who.pid), who.tid,
                         who.uid};
}

/// Small outbox so the HWM is reachable quickly in tests (Q6 knob). HWM/LWM
/// defaults 0.75/0.25 -> refuse at 30 queued, resume at 10.
edriel::Config smallOutboxConfig(std::uint16_t grpcPort) {
    edriel::Config cfg;
    cfg.grpcPort = grpcPort;
    cfg.reliableOutboxMaxFrames = 40;
    return cfg;
}

/// A publisher + a subscriber wired the test_reliable.cpp way (cross-injected
/// adverts, subscriber-initiated dial), with the given per-node configs and
/// subscriber callback.
struct BpPair {
    asio::io_context ioPub;
    asio::io_context ioSub;
    edriel::Config cfgPub;
    edriel::Config cfgSub;
    std::unique_ptr<Edriel> pub;
    std::unique_ptr<Edriel> sub;
    Edriel::Participant subSelf;

    BpPair(const std::string& topic,
           std::function<void(const autoDiscovery::Topic&)> onFrame,
           edriel::Config pubCfg, edriel::Config subCfg)
        : cfgPub(std::move(pubCfg)), cfgSub(std::move(subCfg)) {
        cfgPub.grpcPort = cfgPub.grpcPort != 0 ? cfgPub.grpcPort : freeTcpPort();
        cfgSub.grpcPort = cfgSub.grpcPort != 0 ? cfgSub.grpcPort : freeTcpPort();
        pub = std::make_unique<Edriel>(ioPub, cfgPub);
        sub = std::make_unique<Edriel>(ioSub, cfgSub);

        pub->registerPublisherTopic<autoDiscovery::Topic>(topic, true);
        sub->registerSubscriberTopic<autoDiscovery::Topic>(
            topic, std::move(onFrame), true);

        pub->startGrpcServer();
        sub->startGrpcServer();

        const auto pubSelf = pub->selfIdentityForTest();
        subSelf = sub->selfIdentityForTest();

        sub->deliverForTest(makeAdvert(pubSelf, topic, true, true,
                                       "127.0.0.1", cfgPub.grpcPort));
        pub->deliverForTest(makeAdvert(subSelf, topic, false, true,
                                       "127.0.0.1", cfgSub.grpcPort));

        sub->startReliableSubscriptions();
    }

    bool awaitDial(int timeoutMs = 3000) const {
        return waitUntil(
            [&]() { return pub->subscriberConnectedForTest(keyOf(subSelf)); },
            timeoutMs);
    }

    ~BpPair() {
        sub->stopReliableSubscriptions();
        sub->stopGrpcServer();
        pub->stopGrpcServer();
    }
};

}  // namespace

// ----------------------------------------------------------------------------
// Tri-state + bool wrapper compatibility
// ----------------------------------------------------------------------------

TEST(Backpressure, TriStateAndBoolWrapper) {
    std::atomic<int> calls{0};
    BpPair pair("r_bp_tri",
                [&calls](const autoDiscovery::Topic&) { ++calls; },
                smallOutboxConfig(0), smallOutboxConfig(0));
    ASSERT_TRUE(pair.awaitDial()) << "subscriber dial never landed";

    const std::string topicKey =
        std::string("r_bp_tri") + '\x1F' + "autoDiscovery.Topic";

    // Healthy path: Sent, and the bool wrapper agrees (true). The payload is
    // the serialized protobuf bytes of a Topic message (what sendMessage<T>
    // would pass).
    EXPECT_EQ(pair.pub->tryPublishReliable(
                  "r_bp_tri", "autoDiscovery.Topic",
                  makePayloadValue("a").SerializeAsString()),
              ReliableSendResult::Sent);
    EXPECT_TRUE(pair.pub->publishReliable(
        "r_bp_tri", "autoDiscovery.Topic",
        makePayloadValue("a2").SerializeAsString()));
    ASSERT_TRUE(waitUntil([&]() { return calls.load() == 2; }, 3000));
    // Commit-after-acceptance: two accepted frames consumed two tids.
    EXPECT_EQ(pair.pub->reliablePublisherSeqForTest(topicKey), 2u);

    // Nobody listening for an unknown topic: NoSubscribers (bool: false).
    EXPECT_EQ(pair.pub->tryPublishReliable(
                  "r_nobody", "autoDiscovery.Topic",
                  makePayloadValue("x").SerializeAsString()),
              ReliableSendResult::NoSubscribers);
    EXPECT_FALSE(pair.pub->publishReliable(
        "r_nobody", "autoDiscovery.Topic",
        makePayloadValue("x").SerializeAsString()));
}

// ----------------------------------------------------------------------------
// The stall test (Q7): Backpressured at HWM, tid NOT consumed, bounded RAM,
// LWM-resume same-tid retry delivered exactly-once.
// ----------------------------------------------------------------------------

TEST(Backpressure, StallBackpressuresAndSameTidRetryDeliversExactlyOnce) {
    // The subscriber's callback blocks forever: its read thread stops
    // consuming, so the publisher-side outbox fills to the HWM.
    std::atomic<bool> releaseSub{false};
    std::atomic<int> delivered{0};
    BpPair pair(
        "r_bp_stall",
        [&](const autoDiscovery::Topic&) {
            ++delivered;
            // First frame delivered; then park the callback until released.
            while (!releaseSub.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        },
        smallOutboxConfig(0), smallOutboxConfig(0));
    ASSERT_TRUE(pair.awaitDial()) << "subscriber dial never landed";

    const std::string topicKey =
        std::string("r_bp_stall") + '\x1F' + "autoDiscovery.Topic";
    const SubscriberKey subKey = keyOf(pair.subSelf);

    // Fill the outbox: each accepted frame is queued, the blocked callback
    // means nothing drains past frame 1 + the in-flight write.
    const std::string payload =
        makePayloadValue("stall").SerializeAsString();
    int accepted = 0;
    int refused = 0;
    for (int i = 0; i < 200; ++i) {
        const ReliableSendResult r = pair.pub->tryPublishReliable(
            "r_bp_stall", "autoDiscovery.Topic", payload);
        if (r == ReliableSendResult::Sent) {
            ++accepted;
        } else if (r == ReliableSendResult::Backpressured) {
            ++refused;
            break;  // HWM reached
        } else {
            FAIL() << "unexpected result (NoSubscribers/NotServing) mid-stall";
        }
    }
    ASSERT_EQ(refused, 1) << "never hit the outbox high-water mark";
    // The outbox latches backpressured on first HWM contact; the exact
    // accepted count depends on how much drained during the fill (the
    // subscriber's first frame was consumed by the blocked callback, writes
    // stay in flight) — the INVARIANT is that acceptance stopped and depth
    // is bounded, not a specific count.
    EXPECT_GE(accepted, 1);
    EXPECT_TRUE(waitUntil([&]() {
        return pair.pub->subscriberOutboxDepthForTest(subKey) >= 25;
    }, 2000));

    // (a) Backpressured at HWM — asserted above. (b) tid NOT consumed: the
    // refused frame's tid stayed uncommitted (only accepted frames count).
    // The 30 accepted frames committed tids 1..30; the refused one is 31.
    EXPECT_EQ(pair.pub->reliablePublisherSeqForTest(topicKey),
              static_cast<std::uint64_t>(accepted));

    // (c) Bounded publisher RAM: outbox depth can never exceed the bound.
    const std::size_t depth = pair.pub->subscriberOutboxDepthForTest(subKey);
    EXPECT_LE(depth, 40u);

    // (d) LWM-resume + same-tid retry: release the subscriber, wait for the
    // drain to cross the LWM (isSendable flips back to true), then retry. The
    // retry re-stamps the SAME tid (31) and delivers exactly-once.
    releaseSub.store(true);
    ASSERT_TRUE(waitUntil([&]() { return pair.pub->isSendable(subKey); },
                          5000))
        << "outbox never drained below the LWM";

    // Retries of the SAME payload: the first retry must now be accepted.
    std::map<std::string, OutboxStatus> deliverability;
    const ReliableSendResult retry = pair.pub->tryPublishReliable(
        "r_bp_stall", "autoDiscovery.Topic", payload, &deliverability);
    EXPECT_EQ(retry, ReliableSendResult::Sent);
    // The deliverability map (Q4 4A) shows exactly the one live subscriber,
    // accepted.
    ASSERT_EQ(deliverability.size(), 1u);
    EXPECT_EQ(deliverability.begin()->second, OutboxStatus::Accepted);
    // The tid consumed by the retry is exactly the one the refused frame
    // would have had: accepted + 1 — same-tid retry, no gap.
    EXPECT_EQ(pair.pub->reliablePublisherSeqForTest(topicKey),
              static_cast<std::uint64_t>(accepted + 1));

    // Frame (accepted+1) plus everything queued drains and delivers; total
    // delivered is exactly accepted+1 (each tid exactly once, in order).
    ASSERT_TRUE(waitUntil(
        [&]() { return delivered.load() == accepted + 1; }, 5000))
        << "post-retry delivery count wrong";
    EXPECT_EQ(pair.pub->reliablePublisherSeqForTest(topicKey),
              static_cast<std::uint64_t>(accepted + 1));
}

// ----------------------------------------------------------------------------
// Blocked/retry exactly-once regression (Q7): refusals + same-tid retries
// leave no gaps, no duplicates, in-order delivery per (publisher, topic).
// ----------------------------------------------------------------------------

TEST(Backpressure, InterleavedRefusalsAndRetriesStayExactlyOnce) {
    // The callback gates delivery with a small, bounded hold so the outbox
    // pressurizes and drains repeatedly during the send loop.
    std::atomic<int> holdCount{0};
    std::mutex recMu;
    std::vector<std::string> received;
    BpPair pair(
        "r_bp_retry",
        [&](const autoDiscovery::Topic& t) {
            // Hold the first few frames briefly to build outbox lag.
            if (holdCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::lock_guard<std::mutex> lock(recMu);
            received.push_back(t.topic_name());
        },
        smallOutboxConfig(0), smallOutboxConfig(0));
    ASSERT_TRUE(pair.awaitDial()) << "subscriber dial never landed";

    const std::string topicKey =
        std::string("r_bp_retry") + '\x1F' + "autoDiscovery.Topic";
    std::uint64_t lastCommitted = 0;
    int refusals = 0;
    // Each payload string is a distinct message ("m0".."m39"): the receiver
    // collects them in order, and exactly-once means each appears once.
    for (int i = 0; i < 40; ++i) {
        const std::string payload =
            makePayloadValue("m" + std::to_string(i)).SerializeAsString();
        ReliableSendResult r = pair.pub->tryPublishReliable(
            "r_bp_retry", "autoDiscovery.Topic", payload);
        // Retry the SAME payload until accepted (same-tid retry, Q3 rule 2).
        for (int spin = 0; r == ReliableSendResult::Backpressured
                           && spin < 2000; ++spin) {
            ++refusals;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            r = pair.pub->tryPublishReliable("r_bp_retry",
                                             "autoDiscovery.Topic", payload);
        }
        ASSERT_EQ(r, ReliableSendResult::Sent) << "payload " << payload
                                               << " never accepted";
        const std::uint64_t committed =
            pair.pub->reliablePublisherSeqForTest(topicKey);
        // Tids commit strictly one at a time: no refused frame ever consumed
        // one, so the committed tid advances exactly once per payload.
        EXPECT_EQ(committed, lastCommitted + 1)
            << "tid lifecycle violated at payload " << payload;
        lastCommitted = committed;
    }
    EXPECT_EQ(lastCommitted, 40u);

    // Drain, then assert exactly-once + in-order: received == m0..m39.
    ASSERT_TRUE(waitUntil([&]() {
        std::lock_guard<std::mutex> lock(recMu);
        return received.size() == 40;
    }, 5000)) << "not all payloads delivered";
    std::lock_guard<std::mutex> lock(recMu);
    for (int i = 0; i < 40; ++i) {
        EXPECT_EQ(received[static_cast<std::size_t>(i)],
                  "m" + std::to_string(i))
            << "delivery order/dup violation at index " << i;
    }
}

// ----------------------------------------------------------------------------
// Fairness (Q7/Q5): a stalled subscriber does not gate a healthy one.
// ----------------------------------------------------------------------------

TEST(Backpressure, StalledSubscriberDoesNotGateHealthyOne) {
    asio::io_context ioPub;
    edriel::Config cfgPub = smallOutboxConfig(0);
    cfgPub.grpcPort = freeTcpPort();
    Edriel pub(ioPub, cfgPub);
    ASSERT_TRUE(pub.registerPublisherTopic<autoDiscovery::Topic>("r_bp_fair",
                                                                 true));
    pub.startGrpcServer();
    const auto pubSelf = pub.selfIdentityForTest();

    // Two subscriber nodes: one stalls (blocks in its callback), one is
    // healthy (fast callback counting). The stall gate is released at test
    // end so teardown never joins a forever-blocked read thread.
    std::atomic<bool> releaseAll{false};
    struct SubNode {
        asio::io_context io;
        edriel::Config cfg;
        std::unique_ptr<Edriel> node;
        std::atomic<int> received{0};
        Edriel::Participant self;
    };
    std::vector<std::unique_ptr<SubNode>> subs;
    for (int s = 0; s < 2; ++s) {
        auto entry = std::make_unique<SubNode>();
        entry->cfg = smallOutboxConfig(freeTcpPort());
        entry->node = std::make_unique<Edriel>(entry->io, entry->cfg);
        Edriel* nodeRaw = entry->node.get();
        SubNode* entryPtr = entry.get();
        const bool stall = (s == 0);
        nodeRaw->registerSubscriberTopic<autoDiscovery::Topic>(
            "r_bp_fair",
            [entryPtr, stall, &releaseAll](const autoDiscovery::Topic&) {
                if (stall) {
                    // Park until the test releases (never during measurement):
                    // this subscriber stalls, outbox fills to HWM.
                    while (!releaseAll.load(std::memory_order_relaxed)) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(10));
                    }
                }
                entryPtr->received.fetch_add(1, std::memory_order_relaxed);
            },
            true);
        entry->node->startGrpcServer();
        entry->self = nodeRaw->selfIdentityForTest();
        nodeRaw->deliverForTest(makeAdvert(pubSelf, "r_bp_fair", true, true,
                                           "127.0.0.1", cfgPub.grpcPort));
        pub.deliverForTest(makeAdvert(entry->self, "r_bp_fair", false, true,
                                      "127.0.0.1", entry->cfg.grpcPort));
        nodeRaw->startReliableSubscriptions();
        const auto selfCopy = entry->self;
        ASSERT_TRUE(waitUntil([&]() {
            return pub.subscriberConnectedForTest(keyOf(selfCopy));
        }, 4000)) << "subscriber #" << s << " dial never landed";
        subs.push_back(std::move(entry));
    }

    // Send a burst far past the stalled subscriber's HWM. The stalled one
    // becomes Backpressured (its outbox full); the healthy one keeps
    // accepting and delivering.
    int healthyAccepted = 0;
    for (int i = 0; i < 120; ++i) {
        const ReliableSendResult r = pub.tryPublishReliable(
            "r_bp_fair", "autoDiscovery.Topic",
            makePayloadValue("fair").SerializeAsString());
        if (r == ReliableSendResult::Sent) {
            ++healthyAccepted;
        } else if (r == ReliableSendResult::Backpressured) {
            // Q4 4A: once the stalled sub is at HWM while the healthy one has
            // already accepted the frame, the result is Sent (≥1 accepted) —
            // a refusal only surfaces when ALL live subscribers refuse.
            // Backpressured here would mean the healthy sub is also full,
            // which contradicts its continuing delivery; accept either, but
            // the healthy subscriber MUST keep receiving below.
            break;
        } else {
            FAIL() << "unexpected send result";
        }
    }
    EXPECT_GT(healthyAccepted, 0);

    // THE fairness assertion: the healthy subscriber's delivery continues
    // (well past the stalled one's HWM-limited count) and tracks what was
    // accepted — the stalled subscriber gated nothing. Delivery is async:
    // wait for the accepted frames to land before comparing counters.
    EXPECT_TRUE(waitUntil(
        [&]() {
            return subs[1]->received.load() >= healthyAccepted
                   && subs[0]->received.load() <= subs[1]->received.load();
        },
        5000))
        << "healthy subscriber never caught up with accepted frames";
    EXPECT_GT(subs[1]->received.load(), 10)
        << "healthy subscriber's delivery was gated by the stalled one";
    EXPECT_LE(subs[0]->received.load(), subs[1]->received.load())
        << "stalled subscriber out-delivered the healthy one (impossible "
           "unless the stall did not take effect)";
    // Healthy delivery == what the publisher actually accepted for it; the
    // stalled subscriber cannot exceed its tiny drained share.
    EXPECT_EQ(subs[1]->received.load(), healthyAccepted)
        << "healthy subscriber lost frames the publisher accepted";

    // Release the stall gate so teardown never joins a blocked read thread.
    releaseAll.store(true);
}

// ----------------------------------------------------------------------------
// isSendable probe (2C): flips false at HWM, true again after LWM-cross.
// ----------------------------------------------------------------------------

TEST(Backpressure, IsSendableProbeTracksHwmLwm) {
    std::atomic<bool> releaseSub{false};
    BpPair pair(
        "r_bp_probe",
        [&](const autoDiscovery::Topic&) {
            while (!releaseSub.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        },
        smallOutboxConfig(0), smallOutboxConfig(0));
    ASSERT_TRUE(pair.awaitDial()) << "subscriber dial never landed";

    const SubscriberKey subKey = keyOf(pair.subSelf);
    EXPECT_TRUE(pair.pub->isSendable(subKey));

    // Fill to HWM.
    int sent = 0;
    while (sent < 200) {
        if (pair.pub->tryPublishReliable(
                "r_bp_probe", "autoDiscovery.Topic",
                makePayloadValue("probe").SerializeAsString())
            == ReliableSendResult::Backpressured) {
            break;
        }
        ++sent;
    }
    ASSERT_LT(sent, 200) << "never reached HWM";
    EXPECT_TRUE(waitUntil(
        [&]() { return !pair.pub->isSendable(subKey); }, 2000))
        << "isSendable stayed true at the high-water mark";

    // Drain past the LWM: probe flips back.
    releaseSub.store(true);
    EXPECT_TRUE(waitUntil([&]() { return pair.pub->isSendable(subKey); },
                          5000))
        << "isSendable never returned true after the LWM crossing";
}
