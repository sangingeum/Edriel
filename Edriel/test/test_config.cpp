/**
 * @file test_config.cpp
 * @brief Unit tests for config.yml parsing and per-key validation fallback.
 *
 * Exercises parsePort()/parseMulticastAddress() directly plus loadConfig()
 * against real (temporary) files covering valid, invalid, missing, and
 * partially-missing configs.
 */

#include <gtest/gtest.h>

#include <chrono>      // std::chrono::seconds
#include <cstdio>      // std::remove
#include <filesystem>  // std::filesystem
#include <fstream>     // std::ofstream
#include <string>

#include "EdrielConfig.hpp"

namespace {

/// Write `content` to the shared temp path and return that path.
std::filesystem::path writeTempConfig(const std::string& content) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "edriel_test_config.yml";
    std::ofstream out(path);
    out << content;
    out.close();
    return path;
}

constexpr uint16_t kDefaultPort = 30002;
const std::string kDefaultMc = "239.255.0.1";
const std::chrono::seconds kDefaultDiscovery = edriel::kDefaultDiscoverySendPeriod;
const std::chrono::seconds kDefaultTimeout = edriel::kDefaultParticipantTimeout;

}  // namespace

// --- parsePort -------------------------------------------------------------

TEST(ParsePort, AcceptsValidRange) {
    EXPECT_EQ(edriel::parsePort("1"), 1);
    EXPECT_EQ(edriel::parsePort("30002"), 30002);
    EXPECT_EQ(edriel::parsePort("65535"), 65535);
}

TEST(ParsePort, RejectsZero) {
    EXPECT_EQ(edriel::parsePort("0"), kDefaultPort);
}

TEST(ParsePort, RejectsOutOfRange) {
    EXPECT_EQ(edriel::parsePort("65536"), kDefaultPort);
    EXPECT_EQ(edriel::parsePort("999999"), kDefaultPort);
}

TEST(ParsePort, RejectsNonNumeric) {
    EXPECT_EQ(edriel::parsePort(""), kDefaultPort);
    EXPECT_EQ(edriel::parsePort("abc"), kDefaultPort);
    EXPECT_EQ(edriel::parsePort("12x4"), kDefaultPort);
    EXPECT_EQ(edriel::parsePort("-1"), kDefaultPort);
    EXPECT_EQ(edriel::parsePort(" 30002"), kDefaultPort);  // whitespace junk
    EXPECT_EQ(edriel::parsePort("30002 "), kDefaultPort);  // trailing junk
}

// --- parseMulticastAddress ------------------------------------------------

TEST(ParseMulticastAddress, AcceptsMulticastRange) {
    EXPECT_EQ(edriel::parseMulticastAddress("224.0.0.0"), "224.0.0.0");
    EXPECT_EQ(edriel::parseMulticastAddress("239.255.0.1"), "239.255.0.1");
    EXPECT_EQ(edriel::parseMulticastAddress("239.255.255.255"), "239.255.255.255");
}

TEST(ParseMulticastAddress, RejectsNonMulticastIpv4) {
    EXPECT_EQ(edriel::parseMulticastAddress("192.168.0.1"), kDefaultMc);
    EXPECT_EQ(edriel::parseMulticastAddress("10.0.0.1"), kDefaultMc);
    EXPECT_EQ(edriel::parseMulticastAddress("223.255.255.255"), kDefaultMc);
    EXPECT_EQ(edriel::parseMulticastAddress("240.0.0.1"), kDefaultMc);  // reserved, >239
}

TEST(ParseMulticastAddress, RejectsUnparseable) {
    EXPECT_EQ(edriel::parseMulticastAddress(""), kDefaultMc);
    EXPECT_EQ(edriel::parseMulticastAddress("not-an-ip"), kDefaultMc);
    EXPECT_EQ(edriel::parseMulticastAddress("300.0.0.1"), kDefaultMc);  // octet >255
    EXPECT_EQ(edriel::parseMulticastAddress("239.255.0.1.9"), kDefaultMc);  // 5 octets
    EXPECT_EQ(edriel::parseMulticastAddress("239.255.1"), kDefaultMc);  // 3 octets
    EXPECT_EQ(edriel::parseMulticastAddress("0239.255.0.1"), kDefaultMc);  // leading zero
    EXPECT_EQ(edriel::parseMulticastAddress("239.255.00.1"), kDefaultMc);  // leading zero
}

// --- parseDurationSeconds --------------------------------------------------

TEST(ParseDurationSeconds, AcceptsValidRange) {
    const std::chrono::seconds sentinel(0);  // 0 is never valid -> distinguishes
    EXPECT_EQ(edriel::parseDurationSeconds("1", sentinel), std::chrono::seconds(1));
    EXPECT_EQ(edriel::parseDurationSeconds("2", sentinel), std::chrono::seconds(2));
    EXPECT_EQ(edriel::parseDurationSeconds("10", sentinel), std::chrono::seconds(10));
    EXPECT_EQ(edriel::parseDurationSeconds("86400", sentinel), std::chrono::seconds(86400));
}

TEST(ParseDurationSeconds, FallsBackOnInvalid) {
    const std::chrono::seconds fallback(99);
    EXPECT_EQ(edriel::parseDurationSeconds("", fallback), fallback);              // empty
    EXPECT_EQ(edriel::parseDurationSeconds("abc", fallback), fallback);           // non-numeric
    EXPECT_EQ(edriel::parseDurationSeconds("0", fallback), fallback);             // zero/<=0
    EXPECT_EQ(edriel::parseDurationSeconds("-2", fallback), fallback);            // negative
    EXPECT_EQ(edriel::parseDurationSeconds("86401", fallback), fallback);         // above sane cap
    EXPECT_EQ(edriel::parseDurationSeconds("10x", fallback), fallback);           // trailing junk
    EXPECT_EQ(edriel::parseDurationSeconds(" 10", fallback), fallback);           // leading whitespace
    EXPECT_EQ(edriel::parseDurationSeconds("10 ", fallback), fallback);           // trailing whitespace
    EXPECT_EQ(edriel::parseDurationSeconds("99999999999999999999", fallback),
              fallback);  // overflow
}

// --- loadConfig ------------------------------------------------------------

TEST(LoadConfig, MissingFileYieldsDefaults) {
    const auto path =
        std::filesystem::temp_directory_path() / "edriel_no_such_file_xyz.yml";
    std::remove(path.c_str());  // ensure absent

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, kDefaultPort);
    EXPECT_EQ(cfg.multicastAddress, kDefaultMc);
}

TEST(LoadConfig, ParsesValidFile) {
    const auto path = writeTempConfig(
        "port: 40001\n"
        "multicast_ip: 224.0.0.10\n"
        "discovery_period_seconds: 3\n"
        "participant_timeout_seconds: 12\n"
        "grpc_port: 4700\n"
        "advertise_address:\n"
        "  - 10.5.6.7\n"
        "max_advertised_endpoints: 5\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, 40001);
    EXPECT_EQ(cfg.multicastAddress, "224.0.0.10");
    EXPECT_EQ(cfg.discoverySendPeriod, std::chrono::seconds(3));
    EXPECT_EQ(cfg.participantTimeout, std::chrono::seconds(12));
    EXPECT_EQ(cfg.grpcPort, 4700);
    EXPECT_EQ(cfg.maxAdvertisedEndpoints, 5u);
    ASSERT_EQ(cfg.advertiseAddresses.size(), 1u);
    EXPECT_EQ(cfg.advertiseAddresses[0], "10.5.6.7");
    EXPECT_FALSE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, FallsBackPerKeyOnInvalid) {
    // Only the port is invalid -> multicast stays. Verify per-key fallback.
    // The new reliable keys are absent (their presence isn't the point here);
    // absence makes the diagnostics flag flip, as with any key.
    const auto path = writeTempConfig(
        "port: 0\n"
        "multicast_ip: 234.5.6.7\n"
        "grpc_port: 4700\n"
        "max_advertised_endpoints: 5\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, kDefaultPort);              // invalid -> default
    EXPECT_EQ(cfg.multicastAddress, "234.5.6.7");   // valid -> kept
    EXPECT_EQ(cfg.grpcPort, 4700);                  // valid -> kept
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, MissingKeyKeepsDefaultForThatKey) {
    const auto path = writeTempConfig("port: 48000\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, 48000);                     // present key read
    EXPECT_EQ(cfg.multicastAddress, kDefaultMc);    // absent key -> default
    EXPECT_EQ(cfg.discoverySendPeriod, kDefaultDiscovery);   // absent -> default
    EXPECT_EQ(cfg.participantTimeout, kDefaultTimeout);      // absent -> default
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, DurationKeysFallBackPerKey) {
    // Only the discovery period is invalid -> the timeout stays honored.
    const auto path = writeTempConfig(
        "discovery_period_seconds: 0\n"
        "participant_timeout_seconds: 30\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.discoverySendPeriod, kDefaultDiscovery);  // 0 -> default
    EXPECT_EQ(cfg.participantTimeout, std::chrono::seconds(30));  // valid -> kept
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, DurationKeysRejectOutOfRange) {
    // A value above the sane cap falls back to the default for that key.
    const auto path = writeTempConfig(
        "discovery_period_seconds: 86401\n"
        "participant_timeout_seconds: 15\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.discoverySendPeriod, kDefaultDiscovery);  // > cap -> default
    EXPECT_EQ(cfg.participantTimeout, std::chrono::seconds(15));  // valid -> kept
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, MalformedYamlYieldsDefaults) {
    const auto path = writeTempConfig("port: [not, a, map\n");  // ][ brace unbalanced

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, kDefaultPort);
    EXPECT_EQ(cfg.multicastAddress, kDefaultMc);
}

// --- parseMaxEndpoints ------------------------------------------------------

constexpr uint16_t kDefaultGrpcPort = 4000;
const std::size_t kDefaultMaxEndpoints = edriel::kDefaultMaxAdvertisedEndpoints;

TEST(ParseMaxEndpoints, AcceptsValidRange) {
    EXPECT_EQ(edriel::parseMaxEndpoints("1"), 1u);
    EXPECT_EQ(edriel::parseMaxEndpoints("4"), 4u);
    EXPECT_EQ(edriel::parseMaxEndpoints("64"), 64u);
}

TEST(ParseMaxEndpoints, FallsBackOnInvalid) {
    EXPECT_EQ(edriel::parseMaxEndpoints("0"), kDefaultMaxEndpoints);
    EXPECT_EQ(edriel::parseMaxEndpoints(""), kDefaultMaxEndpoints);
    EXPECT_EQ(edriel::parseMaxEndpoints("abc"), kDefaultMaxEndpoints);
    EXPECT_EQ(edriel::parseMaxEndpoints("-1"), kDefaultMaxEndpoints);
    EXPECT_EQ(edriel::parseMaxEndpoints(" 4"), kDefaultMaxEndpoints);  // junk
}

TEST(ParseMaxEndpoints, ClampsAboveCeiling) {
    // Values above the sane ceiling clamp to the cap, not the fallback.
    EXPECT_EQ(edriel::parseMaxEndpoints("1000"),
              edriel::kMaxAdvertisedEndpointsCap);
}

// --- loadConfig: grpc / advertise keys --------------------------------------

TEST(LoadConfig, ParsesGrpcPort) {
    const auto path = writeTempConfig("grpc_port: 4400\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.grpcPort, 4400);
}

TEST(LoadConfig, InvalidGrpcPortFallsBack) {
    const auto path = writeTempConfig("grpc_port: 0\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.grpcPort, kDefaultGrpcPort);
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, MissingGrpcPortKeepsDefault) {
    const auto path = writeTempConfig("port: 30002\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.grpcPort, kDefaultGrpcPort);
}

TEST(LoadConfig, ParsesScalarAdvertiseAddress) {
    const auto path = writeTempConfig("advertise_address: 10.0.0.9\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    ASSERT_EQ(cfg.advertiseAddresses.size(), 1u);
    EXPECT_EQ(cfg.advertiseAddresses[0], "10.0.0.9");
}

TEST(LoadConfig, ParsesListAdvertiseAddresses) {
    const auto path = writeTempConfig(
        "advertise_address:\n"
        "  - 192.168.1.5\n"
        "  - 10.0.0.3\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    ASSERT_EQ(cfg.advertiseAddresses.size(), 2u);
    EXPECT_EQ(cfg.advertiseAddresses[0], "192.168.1.5");
    EXPECT_EQ(cfg.advertiseAddresses[1], "10.0.0.3");
}

TEST(LoadConfig, MissingAdvertiseAddressLeavesEmpty) {
    const auto path = writeTempConfig("port: 30002\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_TRUE(cfg.advertiseAddresses.empty());
}

TEST(LoadConfig, ParsesMaxAdvertisedEndpoints) {
    const auto path = writeTempConfig("max_advertised_endpoints: 8\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.maxAdvertisedEndpoints, 8u);
}

TEST(LoadConfig, InvalidMaxAdvertisedEndpointsFallsBack) {
    const auto path = writeTempConfig("max_advertised_endpoints: 0\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.maxAdvertisedEndpoints, kDefaultMaxEndpoints);
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, EmptyAdvertiseAddressIsNotFallback) {
    // The shipped config.yml styles `advertise_address:` as an empty value
    // (discover-only). That is a legitimate state, not a fallback: the flag
    // stays false and the address list stays empty.
    const auto path = writeTempConfig(
        "port: 30002\n"
        "multicast_ip: 239.255.0.1\n"
        "discovery_period_seconds: 2\n"
        "participant_timeout_seconds: 10\n"
        "grpc_port: 4000\n"
        "advertise_address:\n"
        "  # - 192.168.1.5\n"
        "max_advertised_endpoints: 4\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_TRUE(cfg.advertiseAddresses.empty());
    EXPECT_FALSE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, GrpcKeysKeepOtherKeys) {
    // New reliable-path keys coexist with the multicast keys (all valid ->
    // no fallback flag fires).
    const auto path = writeTempConfig(
        "port: 31000\n"
        "multicast_ip: 224.0.0.11\n"
        "discovery_period_seconds: 3\n"
        "participant_timeout_seconds: 12\n"
        "grpc_port: 4500\n"
        "advertise_address:\n"
        "  - 10.1.2.3\n"
        "max_advertised_endpoints: 6\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, 31000);
    EXPECT_EQ(cfg.multicastAddress, "224.0.0.11");
    EXPECT_EQ(cfg.discoverySendPeriod, std::chrono::seconds(3));
    EXPECT_EQ(cfg.participantTimeout, std::chrono::seconds(12));
    EXPECT_EQ(cfg.grpcPort, 4500);
    EXPECT_EQ(cfg.maxAdvertisedEndpoints, 6u);
    ASSERT_EQ(cfg.advertiseAddresses.size(), 1u);
    EXPECT_EQ(cfg.advertiseAddresses[0], "10.1.2.3");
    EXPECT_FALSE(cfg.fellBackToDefaults);
}

// --- loadConfig: peers Channel D seed --------------------------------------

TEST(LoadConfig, ParsesPeerEndpointsWithExplicitPorts) {
    const auto path = writeTempConfig(
        "peers:\n"
        "  - 192.168.1.5:4000\n"
        "  - 10.0.0.3:4400\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    ASSERT_EQ(cfg.peerEndpoints.size(), 2u);
    EXPECT_EQ(cfg.peerEndpoints[0], "192.168.1.5:4000");
    EXPECT_EQ(cfg.peerEndpoints[1], "10.0.0.3:4400");
}

TEST(LoadConfig, BareHostPeerDefaultsToGrpcPort) {
    // A bare host without a port inherits the configured grpc_port.
    const auto path = writeTempConfig(
        "grpc_port: 4700\n"
        "peers:\n"
        "  - 192.168.1.9\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    ASSERT_EQ(cfg.peerEndpoints.size(), 1u);
    EXPECT_EQ(cfg.peerEndpoints[0], "192.168.1.9:4700");
}

TEST(LoadConfig, ScalarPeerAccepted) {
    const auto path = writeTempConfig("peers: 192.168.1.5:4000\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    ASSERT_EQ(cfg.peerEndpoints.size(), 1u);
    EXPECT_EQ(cfg.peerEndpoints[0], "192.168.1.5:4000");
}

TEST(LoadConfig, MissingPeersLeavesEmpty) {
    const auto path = writeTempConfig("port: 30002\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_TRUE(cfg.peerEndpoints.empty());
}

TEST(LoadConfig, InvalidPeerEntryIsSkipped) {
    // A non-port suffix is dropped (and the fallback flag is set), while a
    // later valid entry is still honored.
    const auto path = writeTempConfig(
        "peers:\n"
        "  - 192.168.1.5:notaport\n"
        "  - 10.0.0.3:4400\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    ASSERT_EQ(cfg.peerEndpoints.size(), 1u);
    EXPECT_EQ(cfg.peerEndpoints[0], "10.0.0.3:4400");
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

// --- ADR-003 sharded SPSC receive-pipeline keys -----------------------------

constexpr uint32_t kDefaultWorker = edriel::kDefaultWorkerThreads;       // 4
constexpr uint32_t kDefaultReceiver = edriel::kDefaultReceiverThreads;   // 1
const std::size_t kDefaultRingSlots = edriel::kDefaultRxRingSlots;        // 4096

TEST(ParseCountRange, AcceptsInRange) {
    // worker_threads [1,16]
    EXPECT_EQ(edriel::parseCountRange(
                  "4", edriel::kMinWorkerThreads,
                  edriel::kMaxWorkerThreads, edriel::kDefaultWorkerThreads), 4u);
    EXPECT_EQ(edriel::parseCountRange(
                  "16", edriel::kMinWorkerThreads,
                  edriel::kMaxWorkerThreads, edriel::kDefaultWorkerThreads), 16u);
    // receiver_threads [1,4]
    EXPECT_EQ(edriel::parseCountRange(
                  "1", edriel::kMinReceiverThreads,
                  edriel::kMaxReceiverThreads, kDefaultReceiver), 1u);
}

TEST(ParseCountRange, FallsBackOnOutOfRange) {
    EXPECT_EQ(edriel::parseCountRange(
                  "0", edriel::kMinWorkerThreads,
                  edriel::kMaxWorkerThreads, edriel::kDefaultWorkerThreads), edriel::kDefaultWorkerThreads);
    EXPECT_EQ(edriel::parseCountRange(
                  "17", edriel::kMinWorkerThreads,
                  edriel::kMaxWorkerThreads, edriel::kDefaultWorkerThreads), edriel::kDefaultWorkerThreads);
    EXPECT_EQ(edriel::parseCountRange(
                  "5", edriel::kMinReceiverThreads,
                  edriel::kMaxReceiverThreads, kDefaultReceiver), kDefaultReceiver);
}

TEST(ParseCountRange, FallsBackOnJunk) {
    EXPECT_EQ(edriel::parseCountRange(
                  "abc", 1u, 16u, edriel::kDefaultWorkerThreads), edriel::kDefaultWorkerThreads);
    EXPECT_EQ(edriel::parseCountRange(
                  "", 1u, 16u, edriel::kDefaultWorkerThreads), edriel::kDefaultWorkerThreads);
    EXPECT_EQ(edriel::parseCountRange(
                  "-1", 1u, 16u, edriel::kDefaultWorkerThreads), edriel::kDefaultWorkerThreads);
    EXPECT_EQ(edriel::parseCountRange(
                  " 4", 1u, 16u, edriel::kDefaultWorkerThreads), edriel::kDefaultWorkerThreads);
    EXPECT_EQ(edriel::parseCountRange(
                  "4 ", 1u, 16u, edriel::kDefaultWorkerThreads), edriel::kDefaultWorkerThreads);
}

TEST(ParseRingSlots, AcceptsPowerOfTwo) {
    EXPECT_EQ(edriel::parseRingSlots("2"), 2u);
    EXPECT_EQ(edriel::parseRingSlots("4096"), 4096u);
    EXPECT_EQ(edriel::parseRingSlots("1024"), 1024u);
}

TEST(ParseRingSlots, RejectsNonPowerOfTwo) {
    EXPECT_EQ(edriel::parseRingSlots("3"), kDefaultRingSlots);
    EXPECT_EQ(edriel::parseRingSlots("0"), kDefaultRingSlots);
    EXPECT_EQ(edriel::parseRingSlots("1"), kDefaultRingSlots);
    EXPECT_EQ(edriel::parseRingSlots("100"), kDefaultRingSlots);
    EXPECT_EQ(edriel::parseRingSlots("4095"), kDefaultRingSlots);
    EXPECT_EQ(edriel::parseRingSlots("abc"), kDefaultRingSlots);
    EXPECT_EQ(edriel::parseRingSlots("4096 "), kDefaultRingSlots);
}

TEST(LoadConfig, ParsesAdr003Keys) {
    const auto path = writeTempConfig(
        "port: 31000\n"
        "multicast_ip: 224.0.0.11\n"
        "discovery_period_seconds: 3\n"
        "participant_timeout_seconds: 12\n"
        "grpc_port: 4500\n"
        "max_advertised_endpoints: 6\n"
        "receiver_threads: 2\n"
        "worker_threads: 8\n"
        "rx_ring_slots: 1024\n"
        "so_rcvbuf_bytes: 1048576\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.receiverThreads, 2u);
    EXPECT_EQ(cfg.workerThreads, 8u);
    EXPECT_EQ(cfg.rxRingSlots, 1024u);
    EXPECT_EQ(cfg.soRcvbufBytes, 1048576u);
    // Upstream keys present -> no spurious fallback diagnostic fires.
    EXPECT_FALSE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, Adr003KeysFallBackOnInvalid) {
    // worker_threads out of range + rx_ring_slots not a power of two.
    const auto path = writeTempConfig(
        "worker_threads: 99\n"
        "rx_ring_slots: 100\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.workerThreads, kDefaultWorker);                 // invalid -> fallback
    EXPECT_EQ(cfg.receiverThreads, kDefaultReceiver);      // absent -> default
    EXPECT_EQ(cfg.rxRingSlots, kDefaultRingSlots);         // fallback
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, MissingAdr003KeysKeepDefaults) {
    const auto path = writeTempConfig("port: 30002\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.workerThreads, kDefaultWorker);
    EXPECT_EQ(cfg.receiverThreads, kDefaultReceiver);
    EXPECT_EQ(cfg.rxRingSlots, kDefaultRingSlots);
    EXPECT_EQ(cfg.soRcvbufBytes, 0u);
}