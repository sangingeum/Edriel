/**
 * @file EdrielConfig.cpp
 * @brief Implementation of config.yml loading and validation for auto-discovery.
 *
 * Uses yaml-cpp (Conan package `yaml-cpp`) to parse a minimal config file with
 * `port` and `multicast_ip` keys. Validation is strict and per-key: a bad or
 * missing value keeps that key's historical default instead of failing startup.
 */

#include "EdrielConfig.hpp"

#include <yaml-cpp/yaml.h>

#include <arpa/inet.h>
#include <charconv>   // std::from_chars
#include <fstream>    // std::ifstream
#include <iterator>   // std::istreambuf_iterator

namespace edriel {

uint16_t parsePort(const std::string& value, uint16_t fallback) {
    unsigned long parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);

    // Strict: the whole string must be a single decimal integer (no leading
    // sign, whitespace, or trailing junk) and must land in the UDP port range.
    if (ec != std::errc{} || ptr != end || parsed < 1 || parsed > 65535) {
        return fallback;
    }
    return static_cast<uint16_t>(parsed);
}

std::string parseMulticastAddress(const std::string& value,
                                  const std::string& fallback) {
    in_addr addr{};
    // inet_pton accepts only strict dotted-quad IPv4 (no leading zeros,
    // no shortcut forms), which is exactly the strictness we want.
    if (inet_pton(AF_INET, value.c_str(), &addr) != 1) {
        return fallback;
    }
    const uint32_t host = ntohl(addr.s_addr);
    const uint8_t firstOctet = static_cast<uint8_t>((host >> 24) & 0xFF);
    if (firstOctet < 224 || firstOctet > 239) {
        return fallback;  // valid IPv4 but not a multicast group
    }
    return value;
}

Config loadConfig(const std::string& configPath) {
    Config config;  // defaults; any invalid key keeps its default

    std::ifstream in(configPath);
    if (!in) {
        return config;
    }

    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (content.empty()) {
        return config;
    }

    try {
        const YAML::Node root = YAML::Load(content);
        if (!root.IsMap()) {
            return config;
        }
        // Sentinel fallback (0 for port, "" for address) lets us tell "invalid
        // value" apart from a validated value that merely equals the built-in
        // default, so the diagnostics flag stays accurate.
        if (const YAML::Node portNode = root["port"];
            portNode && portNode.IsScalar()) {
            const uint16_t parsed = parsePort(portNode.as<std::string>(), 0);
            if (parsed != 0) {
                config.port = parsed;
            } else {
                config.fellBackToDefaults = true;  // invalid value -> default kept
            }
        } else {
            config.fellBackToDefaults = true;      // missing port key
        }
        if (const YAML::Node addrNode = root["multicast_ip"];
            addrNode && addrNode.IsScalar()) {
            const std::string parsed =
                parseMulticastAddress(addrNode.as<std::string>(), "");
            if (!parsed.empty()) {
                config.multicastAddress = parsed;
            } else {
                config.fellBackToDefaults = true;  // invalid value -> default kept
            }
        } else {
            config.fellBackToDefaults = true;      // missing multicast_ip key
        }
    } catch (const YAML::Exception&) {
        // Unparseable YAML -> fall back to the defaults.
        return config;
    }

    return config;
}

}  // namespace edriel