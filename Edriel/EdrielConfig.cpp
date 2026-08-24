/**
 * @file EdrielConfig.cpp
 * @brief Implementation of config.yml loading and validation for auto-discovery.
 *
 * Uses yaml-cpp (Conan package `yaml-cpp`) to parse a minimal config file with
 * `port`, `multicast_ip`, `discovery_period_seconds`,
 * `participant_timeout_seconds`, `grpc_port`, `advertise_address`, and
 * `max_advertised_endpoints` keys. Validation is strict and per-key: a bad
 * or missing value keeps that key's historical default instead of failing
 * startup.
 */

#include "EdrielConfig.hpp"

#include <yaml-cpp/yaml.h>

#include <charconv>   // std::from_chars
#include <chrono>     // std::chrono::seconds
#include <cstddef>    // std::size_t
#include <fstream>    // std::ifstream
#include <iterator>   // std::istreambuf_iterator
#include <string_view> // std::string_view

namespace edriel {

std::chrono::seconds parseDurationSeconds(const std::string& value,
                                          std::chrono::seconds fallback) {
    unsigned long parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);

    // Strict: the whole string must be a single positive decimal integer (no
    // leading sign, whitespace, or trailing junk), within the sane upper bound
    // defined in the header (24h in seconds).
    if (ec != std::errc{} || ptr != end || parsed < 1 ||
        std::chrono::seconds(static_cast<long long>(parsed)) > kMaxConfigurableDuration) {
        return fallback;
    }
    return std::chrono::seconds(static_cast<long long>(parsed));
}

std::uint16_t parsePort(const std::string& value, std::uint16_t fallback) {
    unsigned long parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);

    // Strict: the whole string must be a single decimal integer (no leading
    // sign, whitespace, or trailing junk) and must land in the UDP port range.
    if (ec != std::errc{} || ptr != end || parsed < 1 || parsed > 65535) {
        return fallback;
    }
    return static_cast<std::uint16_t>(parsed);
}

std::size_t parseMaxEndpoints(const std::string& value, std::size_t fallback) {
    unsigned long parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);

    // Strict: a single positive whole number within the sane ceiling.
    if (ec != std::errc{} || ptr != end || parsed < 1) {
        return fallback;
    }
    if (parsed >= kMaxAdvertisedEndpointsCap) {
        return kMaxAdvertisedEndpointsCap;
    }
    return static_cast<std::size_t>(parsed);
}

std::string parseMulticastAddress(const std::string& value,
                                  const std::string& fallback) {
    // Parse the dotted-quad IPv4 by hand: inet_pton/ntohl are POSIX-only and
    // not available on MSVC. Accept exactly four decimal octets, each in
    // [0, 255] with no leading zeros; the first octet must be in the multicast
    // range 224..239.
    std::string_view rest = value;
    std::uint8_t firstOctet = 0;
    std::size_t parts = 0;
    bool hasFirst = false;
    for (;;) {
        const std::size_t dot = rest.find('.');
        const std::string_view octet = rest.substr(0, dot);
        if (octet.empty() || octet.size() > 3 ||
            (octet.size() > 1 && octet.front() == '0')) {
            return fallback;  // empty part, octet > 999, or leading zero
        }
        unsigned parsed = 0;
        const char* const octetBegin = octet.data();
        const auto [ptr, ec] =
            std::from_chars(octetBegin, octetBegin + octet.size(), parsed, 10);
        if (ec != std::errc{} || ptr != (octetBegin + octet.size()) ||
            parsed > 255) {
            return fallback;  // non-decimal, trailing junk, or octet > 255
        }
        if (!hasFirst) {
            firstOctet = static_cast<std::uint8_t>(parsed);
            hasFirst = true;
        }
        ++parts;
        if (dot == std::string_view::npos) {
            break;
        }
        rest = rest.substr(dot + 1);
    }
    if (parts != 4) {
        return fallback;  // fewer or more than four octets
    }
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
        // Sentinel fallback (0 for port, "" for address, 0s for durations)
        // lets us tell "invalid value" apart from a validated value that
        // merely equals the built-in default, so the diagnostics flag stays
        // accurate.
        if (const YAML::Node portNode = root["port"];
            portNode && portNode.IsScalar()) {
            const std::uint16_t parsed = parsePort(portNode.as<std::string>(), 0);
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
        if (const YAML::Node discNode = root["discovery_period_seconds"];
            discNode && discNode.IsScalar()) {
            const std::chrono::seconds parsed =
                parseDurationSeconds(discNode.as<std::string>(), std::chrono::seconds(0));
            if (parsed != std::chrono::seconds(0)) {
                config.discoverySendPeriod = parsed;
            } else {
                config.fellBackToDefaults = true;  // invalid value -> default kept
            }
        } else {
            config.fellBackToDefaults = true;      // missing discovery_period_seconds key
        }
        if (const YAML::Node toNode = root["participant_timeout_seconds"];
            toNode && toNode.IsScalar()) {
            const std::chrono::seconds parsed =
                parseDurationSeconds(toNode.as<std::string>(), std::chrono::seconds(0));
            if (parsed != std::chrono::seconds(0)) {
                config.participantTimeout = parsed;
            } else {
                config.fellBackToDefaults = true;  // invalid value -> default kept
            }
        } else {
            config.fellBackToDefaults = true;      // missing participant_timeout_seconds key
        }
        if (const YAML::Node grpcNode = root["grpc_port"];
            grpcNode && grpcNode.IsScalar()) {
            const std::uint16_t parsed = parsePort(grpcNode.as<std::string>(), 0);
            if (parsed != 0) {
                config.grpcPort = parsed;
            } else {
                config.fellBackToDefaults = true;  // invalid value -> default kept
            }
        } else {
            config.fellBackToDefaults = true;      // missing grpc_port key
        }
        if (const YAML::Node capNode = root["max_advertised_endpoints"];
            capNode && capNode.IsScalar()) {
            const std::size_t sentinel = 0;  // 0 is never a valid cap
            const std::size_t parsed =
                parseMaxEndpoints(capNode.as<std::string>(), sentinel);
            if (parsed != 0) {
                config.maxAdvertisedEndpoints = parsed;
            } else {
                config.fellBackToDefaults = true;  // invalid value -> default kept
            }
        } else {
            config.fellBackToDefaults = true;      // missing max_advertised_endpoints key
        }
        if (const YAML::Node advNode = root["advertise_address"]; advNode) {
            // Accept a scalar (one address) or a sequence (multi-homed). An
            // absent or empty value means "no configured addresses" — a
            // legitimate discover-only state, NOT a fallback. Only a genuinely
            // malformed node (e.g. a map, or non-string list items) flags the
            // fallback diagnostics.
            bool anyInvalid = false;
            if (advNode.IsNull()) {
                // `advertise_address:` with no value -> discover-only, valid.
            } else if (advNode.IsScalar()) {
                const std::string address = advNode.as<std::string>();
                if (!address.empty()) {
                    config.advertiseAddresses.push_back(address);
                }
            } else if (advNode.IsSequence()) {
                for (const YAML::Node& item : advNode) {
                    if (!item.IsScalar()) {
                        anyInvalid = true;
                        continue;
                    }
                    const std::string address = item.as<std::string>();
                    if (!address.empty()) {
                        config.advertiseAddresses.push_back(address);
                    }
                }
            } else {
                anyInvalid = true;
            }
            if (anyInvalid) {
                config.fellBackToDefaults = true;
            }
        } else {
            // key absent -> empty address list (advertise discovered interfaces),
            // not a fallback.
        }
    } catch (const YAML::Exception&) {
        // Unparseable YAML -> fall back to the defaults.
        return config;
    }

    return config;
}

}  // namespace edriel