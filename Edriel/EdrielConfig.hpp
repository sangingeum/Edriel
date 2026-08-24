/**
 * @file EdrielConfig.hpp
 * @brief Configurable auto-discovery runtime (port, multicast group, cadence).
 *
 * The auto-discovery port, multicast group IP, heartbeat send period, and
 * participant aliveness timeout were historically hardcoded
 * (30002 / 239.255.0.1 / 2s / 10s). They are now read from a config.yml with
 * strict, per-key validation: an invalid value falls back to that key's default
 * rather than aborting startup. Parsing lives in its own translation unit
 * (EdrielConfig.cpp) so the validation contract is unit-testable without an
 * io_context.
 */

#pragma once

#include <chrono>    // std::chrono::seconds
#include <cstdint>   // std::uint16_t
#include <string>

namespace edriel {

// ---------------------------------------------------------------------------
// Config defaults (also the per-key fallback).
// ---------------------------------------------------------------------------

/// Default discovery heartbeat send interval (seconds). Invalid -> 2.
inline constexpr std::chrono::seconds kDefaultDiscoverySendPeriod{ 2 };
/// Default participant aliveness timeout, seconds. Invalid -> 10.
inline constexpr std::chrono::seconds kDefaultParticipantTimeout{ 10 };
/// Sanity upper bound (seconds) for both configurable durations. A value
/// above this is rejected as pathological and falls back to the default.
inline constexpr std::chrono::seconds kMaxConfigurableDuration{ std::chrono::hours(24) };

/**
 * @struct Config
 * @brief Validated auto-discovery endpoint and cadence configuration.
 *
 * Values are always valid: any key that is missing, unparseable, or out of
 * range retains its historical default.
 */
struct Config {
    /// UDP port in [1, 65535]. Invalid/out-of-range -> 30002.
    std::uint16_t port = 30002;
    /// IPv4 multicast group (224.0.0.0 .. 239.255.255.255). Invalid -> "239.255.0.1".
    std::string multicastAddress = "239.255.0.1";
    /// Discovery heartbeat send interval in seconds. Invalid/<=0 -> 2s.
    std::chrono::seconds discoverySendPeriod = kDefaultDiscoverySendPeriod;
    /// Participant aliveness timeout in seconds. Invalid/<=0 -> 10s.
    std::chrono::seconds participantTimeout = kDefaultParticipantTimeout;

    /// True when one or more config.yml keys were missing or invalid and a
    /// default was substituted (diagnostics only). Also drives the
    /// constructor's fallback notice.
    bool fellBackToDefaults = false;
};

/**
 * @brief Parse a positive whole-second duration. Returns `fallback` for
 *        anything that is not a strict decimal integer in
 *        [1, 86400] seconds (0, overflow, non-numeric, above the sane cap).
 */
std::chrono::seconds parseDurationSeconds(const std::string& value,
                                          std::chrono::seconds fallback);

/**
 * @brief Parse an integer port. Returns `fallback` for anything that is not a
 *        strict decimal integer in [1, 65535] (0, overflow, non-numeric).
 */
std::uint16_t parsePort(const std::string& value, std::uint16_t fallback = 30002);

/**
 * @brief Parse an IPv4 multicast address. Returns `fallback` unless `value` is
 *        a strict dotted-quad address in the multicast range 224.0.0.0 .. 239.255.255.255.
 */
std::string parseMulticastAddress(const std::string& value,
                                  const std::string& fallback = "239.255.0.1");

/**
 * @brief Load and validate config from a YAML file at `configPath`.
 *
 * Keys: `port` (integer), `multicast_ip` (string), `discovery_period_seconds`
 * (integer seconds), and `participant_timeout_seconds` (integer seconds). Each
 * missing or invalid key falls back to its default independently; a missing or
 * unparseable file also yields the defaults. Never throws on config content.
 */
Config loadConfig(const std::string& configPath = "config.yml");

}  // namespace edriel