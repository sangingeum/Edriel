/**
 * @file config.hpp
 * @brief Configuration file support for Edriel C++20 multi-cast auto-discovery
 * 
 * Provides YAML/JSON configuration parsing for:
 * - Port configuration
 * - TTL (Time To Live) settings
 * - Timeout configuration
 * - Multicast address
 * - Magic number
 * - Cache size
 */

#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>  // Include json.hpp if available

namespace edriel {

/**
 * @brief Configuration structure for Edriel runtime
 * 
 * Contains all configurable parameters including:
 * - Network settings (port, multicast address)
 * - Timing parameters (TTL, timeouts)
 * - Magic number for packet integrity
 * - Cache and performance settings
 */
struct Config {
    // Network configuration
    uint16_t port{ 30002 };                           ///< Multicast port
    std::string multicastAddress{ "239.255.0.1" };    ///< Multicast group address
    std::string magicNumber{ "0xED75E1ED" };          ///< Magic number as string for YAML/JSON
    
    // Timing configuration
    std::chrono::seconds discoverySendPeriod{ 2 };    ///< Discovery heartbeat interval
    std::chrono::seconds discoveryCleanupPeriod{ 5 }; ///< Cleanup timer interval
    std::chrono::seconds heartbeatTimeout{ 10 };      ///< Participant heartbeat timeout
    std::chrono::seconds timestampValiditySeconds{ 300 };  ///< Timestamp validity window (replay attack prevention)
    
    // Cache configuration
    std::size_t lruCacheSize{ 1024 };                 ///< LRU cache capacity
    std::chrono::seconds cacheExpirationSeconds{ 300 };  ///< Cache entry expiration
    
    // Performance configuration
    std::size_t recvBufferSize{ 1500 };               ///< UDP receive buffer size
    
    /**
     * @brief Constructs Config with default values
     */
    Config() = default;
};

/**
 * @brief Loads configuration from YAML file
 * 
 * @param filename Path to YAML configuration file
 * @return Config loaded configuration, or default config on error
 */
Config loadConfigFromYaml(const std::string& filename);

/**
 * @brief Loads configuration from JSON file
 * 
 * @param filename Path to JSON configuration file
 * @return Config loaded configuration, or default config on error
 */
Config loadConfigFromJson(const std::string& filename);

/**
 * @brief Saves configuration to YAML file
 * 
 * @param filename Path to YAML file
 * @param config Configuration to save
 * @return true if save succeeded
 */
bool saveConfigToYaml(const std::string& filename, const Config& config);

/**
 * @brief Saves configuration to JSON file
 * 
 * @param filename Path to JSON file
 * @param config Configuration to save
 * @return true if save succeeded
 */
bool saveConfigToJson(const std::string& filename, const Config& config);

} // namespace edriel
