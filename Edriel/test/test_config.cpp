/**
 * @file test_config.cpp
 * @brief Unit tests for config.yml parsing and per-key validation fallback.
 *
 * Exercises parsePort()/parseMulticastAddress() directly plus loadConfig()
 * against real (temporary) files covering valid, invalid, missing, and
 * partially-missing configs.
 */

#include <gtest/gtest.h>

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
        "multicast_ip: 224.0.0.10\n");

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, 40001);
    EXPECT_EQ(cfg.multicastAddress, "224.0.0.10");
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
    EXPECT_TRUE(cfg.fellBackToDefaults);
}

TEST(LoadConfig, MalformedYamlYieldsDefaults) {
    const auto path = writeTempConfig("port: [not, a, map\n");  // ][ brace unbalanced

    const edriel::Config cfg = edriel::loadConfig(path.string());
    EXPECT_EQ(cfg.port, kDefaultPort);
    EXPECT_EQ(cfg.multicastAddress, kDefaultMc);
}