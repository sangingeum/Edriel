/**
 * @file EdrielConfig.hpp
 * @brief Configurable auto-discovery endpoint parameters (port + multicast group).
 *
 * The auto-discovery port and multicast group IP were historically hardcoded
 * (30002 / 239.255.0.1). They are now read from a config.yml with strict,
 * per-key validation: an invalid value falls back to that key's default rather
 * than aborting startup. Parsing lives in its own translation unit (EdrielConfig.cpp)
 * so the validation contract is unit-testable without an io_context.
 */

#pragma once

#include <cstdint>
#include <string>

namespace edriel {

/**
 * @struct Config
 * @brief Validated auto-discovery endpoint configuration.
 *
 * Values are always valid: any key that is missing, unparseable, or out of
 * range retains its historical default.
 */
struct Config {
    /// UDP port in [1, 65535]. Invalid/out-of-range -> 30002.
    uint16_t port = 30002;
    /// IPv4 multicast group (224.0.0.0 .. 239.255.255.255). Invalid -> "239.255.0.1".
    std::string multicastAddress = "239.255.0.1";

    /// Set true when at least one supplied value was invalid and a default
    /// substituted (diagnostics only).
    bool fellBackToDefaults = false;
};

/**
 * @brief Parse an integer port. Returns `fallback` for anything that is not a
 *        strict decimal integer in [1, 65535] (0, overflow, non-numeric).
 */
uint16_t parsePort(const std::string& value, uint16_t fallback = 30002);

/**
 * @brief Parse an IPv4 multicast address. Returns `fallback` unless `value` is
 *        a strict dotted-quad address in the multicast range 224.0.0.0 .. 239.255.255.255.
 */
std::string parseMulticastAddress(const std::string& value,
                                  const std::string& fallback = "239.255.0.1");

/**
 * @brief Load and validate config from a YAML file at `configPath`.
 *
 * Keys: `port` (integer) and `multicast_ip` (string). Each missing or invalid
 * key falls back to its default independently; a missing or unparseable file
 * also yields the defaults. Never throws on config content.
 */
Config loadConfig(const std::string& configPath = "config.yml");

}  // namespace edriel