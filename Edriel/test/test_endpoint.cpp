#include <gtest/gtest.h>
#include "Edriel.hpp"
#include "autoDiscovery.pb.h"
#include "autoDiscovery_grpc_service.pb.h"

// ADR-0002 Milestone 1: additive proto3 Endpoint / endpoints / reliable tests.
// The best-effort multicast path must stay byte-identical, so these focus on
// the new wire fields round-tripping and on additive (back-compat) semantics.

using autoDiscovery::Endpoint;
using autoDiscovery::Identifier;
using autoDiscovery::Topic;

namespace {

Endpoint makeEndpoint(const std::string& address, uint32_t port) {
    Endpoint ep;
    ep.set_address(address);
    ep.set_port(port);
    ep.set_transport(Endpoint::GRPC_TCP);
    return ep;
}

}  // namespace

TEST(TestEndpoint, RoundTripsSingleEndpoint) {
    Endpoint ep = makeEndpoint("192.168.1.5", 4000);
    EXPECT_EQ(ep.transport(), Endpoint::GRPC_TCP);

    std::string bytes;
    ASSERT_TRUE(ep.SerializeToString(&bytes));
    Endpoint parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_EQ(parsed.address(), "192.168.1.5");
    EXPECT_EQ(parsed.port(), 4000u);
    EXPECT_EQ(parsed.transport(), Endpoint::GRPC_TCP);
}

TEST(TestEndpoint, DefaultTransportIsUnspecified) {
    Endpoint ep;
    EXPECT_EQ(ep.transport(), Endpoint::TRANSPORT_UNSPECIFIED);
}

TEST(TestEndpoint, IdentifierEndpointsFieldRoundTripsInOrder) {
    Identifier id;
    id.set_pid(7);
    id.set_tid(9);
    id.set_uid(11);

    // Multi-homed candidates: order is significant (connect-in-order, first-wins).
    *id.add_endpoints() = makeEndpoint("192.168.1.5", 4000);
    *id.add_endpoints() = makeEndpoint("10.0.0.3", 4001);

    std::string bytes;
    ASSERT_TRUE(id.SerializeToString(&bytes));
    Identifier parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));

    ASSERT_EQ(parsed.endpoints_size(), 2);
    EXPECT_EQ(parsed.endpoints(0).address(), "192.168.1.5");
    EXPECT_EQ(parsed.endpoints(0).port(), 4000u);
    EXPECT_EQ(parsed.endpoints(1).address(), "10.0.0.3");
    EXPECT_EQ(parsed.endpoints(1).port(), 4001u);
    // pid/tid/uid still intact alongside the new field.
    EXPECT_EQ(parsed.pid(), 7u);
    EXPECT_EQ(parsed.tid(), 9u);
    EXPECT_EQ(parsed.uid(), 11u);
}

TEST(TestEndpoint, UnsetEndpointsFieldIsAbsent_AdditiveBackCompat) {
    // proto3 omits default-valued fields on the wire; an Identifier that has
    // no endpoints serializes with no bytes for field 4, so peers that never
    // learned the field parse identically (additive proto3 back-compat).
    Identifier id;
    id.set_pid(7);
    id.set_tid(9);
    id.set_uid(101);

    EXPECT_EQ(id.endpoints_size(), 0);
    std::string bytes;
    id.SerializeToString(&bytes);
    Identifier parsed;
    parsed.ParseFromString(bytes);
    EXPECT_EQ(parsed.endpoints_size(), 0);
}

TEST(TestEndpoint, ParticipantDataEndpointsField8WithLegacyEndpointKept) {
    autoDiscovery::ParticipantData pd;
    pd.set_pid(1);
    pd.set_uid(2);
    pd.set_endpoint("1.2.3.4:4000");  // legacy field 4, kept for back-compat
    *pd.add_endpoints() = makeEndpoint("10.1.1.9", 4000);
    pd.add_topics_published("sensor");

    std::string bytes;
    ASSERT_TRUE(pd.SerializeToString(&bytes));
    autoDiscovery::ParticipantData parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));

    // Both forms survive the wire round-trip.
    EXPECT_EQ(parsed.endpoint(), "1.2.3.4:4000");
    ASSERT_EQ(parsed.endpoints_size(), 1);
    EXPECT_EQ(parsed.endpoints(0).address(), "10.1.1.9");
    EXPECT_EQ(parsed.endpoints(0).port(), 4000u);
    EXPECT_EQ(parsed.topics_published_size(), 1);
}

TEST(TestEndpoint, TopicReliableRoundTrips) {
    Topic t;
    EXPECT_FALSE(t.reliable());  // best-effort is the default (proto3 false)
    t.set_reliable(true);
    std::string bytes;
    ASSERT_TRUE(t.SerializeToString(&bytes));
    Topic parsed;
    ASSERT_TRUE(parsed.ParseFromString(bytes));
    EXPECT_TRUE(parsed.reliable());
}