#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "Edriel.hpp"
#include "autoDiscovery.pb.h"

using edriel::Edriel;

namespace {

// Simple protobuf payload used across the pub/sub tests.
autoDiscovery::Topic makePayload(int id) {
    autoDiscovery::Topic topic;
    topic.set_topic_name("t" + std::to_string(id));
    return topic;
}

}  // namespace

TEST(TestPubSub, RegisterPublisherCreatesRegistryEntry) {
    asio::io_context io;
    Edriel edriel(io);

    EXPECT_TRUE(edriel.registerPublisherTopic<autoDiscovery::Topic>("sensor"));

    const auto& registry = edriel.registryForTest();
    EXPECT_EQ(registry.size(), 1u);
    ASSERT_TRUE(registry.count(edriel::makeCompositeKey("sensor", "autoDiscovery.Topic")) == 1);
    EXPECT_EQ(registry.at(edriel::makeCompositeKey("sensor", "autoDiscovery.Topic")).topicName, "sensor");
}

TEST(TestPubSub, UnregisterUnknownTopicFails) {
    asio::io_context io;
    Edriel edriel(io);

    EXPECT_FALSE(edriel.unregisterPublisherTopic<autoDiscovery::Topic>("nope"));
}

TEST(TestPubSub, UnregisterPublisherRemovesEntry) {
    asio::io_context io;
    Edriel edriel(io);

    EXPECT_TRUE(edriel.registerPublisherTopic<autoDiscovery::Topic>("sensor"));
    EXPECT_TRUE(edriel.unregisterPublisherTopic<autoDiscovery::Topic>("sensor"));
    EXPECT_TRUE(edriel.registryForTest().empty());
}

TEST(TestPubSub, RegisterSubscriberStoresCallback) {
    asio::io_context io;
    Edriel edriel(io);

    std::atomic<int> calls{0};
    EXPECT_TRUE(edriel.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor", [&calls](const autoDiscovery::Topic&) { ++calls; }));

    const auto entry = edriel.registryForTest().at(
        edriel::makeCompositeKey("sensor", "autoDiscovery.Topic"));
    EXPECT_EQ(entry.callbacks.size(), 1u);
}

TEST(TestPubSub, CallbackDemuxInvokesMatchingType) {
    asio::io_context io;
    Edriel edriel(io);

    std::atomic<int> calls{0};
    ASSERT_TRUE(edriel.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor", [&calls](const autoDiscovery::Topic& t) {
            if (!t.topic_name().empty()) { ++calls; }
        }));

    // Deliver a matching data message directly through the receive path.
    autoDiscovery::Message msg;
    auto* data = msg.mutable_data_message();
    data->mutable_identifier()->set_pid(7);
    data->set_topic_name("sensor");
    data->set_message_type("autoDiscovery.Topic");
    auto payload = makePayload(42).SerializeAsString();
    data->set_payload(payload);
    edriel.deliverForTest(msg);

    EXPECT_EQ(calls.load(), 1);
}

TEST(TestPubSub, CallbackIgnoresMismatchedType) {
    asio::io_context io;
    Edriel edriel(io);

    std::atomic<int> calls{0};
    ASSERT_TRUE(edriel.registerSubscriberTopic<autoDiscovery::Topic>(
        "sensor", [&calls](const autoDiscovery::Topic&) { ++calls; }));

    autoDiscovery::Message msg;
    auto* data = msg.mutable_data_message();
    data->mutable_identifier()->set_pid(7);
    data->set_topic_name("sensor");
    data->set_message_type("some.OtherType");  // wrong type -> no dispatch
    data->set_payload(makePayload(1).SerializeAsString());
    edriel.deliverForTest(msg);

    EXPECT_EQ(calls.load(), 0);
}

TEST(TestPubSub, SendMessageRejectsOversizedPayload) {
    asio::io_context io;
    Edriel edriel(io);

    // Build a payload whose serialized envelope exceeds the 1500-byte budget.
    autoDiscovery::Topic big;
    big.set_topic_name(std::string(4000, 'x'));
    EXPECT_FALSE(edriel.sendMessage("bigtopic", big));
}
