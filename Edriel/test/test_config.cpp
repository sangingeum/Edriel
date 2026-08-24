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
        "participant_timeout_seconds: 12\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, 40001);
    EXPECT_EQ(cfg.multicastAddress, "224.0.0.10");
    EXPECT_EQ(cfg.discoverySendPeriod, std::chrono::seconds(3));
    EXPECT_EQ(cfg.participantTimeout, std::chrono::seconds(12));
    EXPECT_FALSE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, FallsBackPerKeyOnInvalid) {
    // Only the port is invalid -> multicast stays. Verify per-key fallback.
    const auto path = writeTempConfig(
        "port: 0\n"
        "multicast_ip: 234.5.6.7\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, kDefaultPort);              // invalid -> default
    EXPECT_EQ(cfg.multicastAddress, "234.5.6.7");   // valid -> kept
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