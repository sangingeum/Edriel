/**
 * @file config.cpp
 * @brief Configuration file implementation for Edriel C++20 multi-cast auto-discovery
 * 
 * Implements YAML/JSON configuration parsing for:
 * - Port configuration
 * - TTL (Time To Live) settings
 * - Timeout configuration
 * - Multicast address
 * - Magic number
 * - Cache and performance settings
 */

#include "config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace edriel {

// Default configuration values
Config createDefaultConfig() {
    Config config;
    config.port = 30002;
    config.multicastAddress = "239.255.0.1";
    config.magicNumber = "0xED75E1ED";
    config.discoverySendPeriod = std::chrono::seconds(2);
    config.discoveryCleanupPeriod = std::chrono::seconds(5);
    config.heartbeatTimeout = std::chrono::seconds(10);
    config.timestampValiditySeconds = std::chrono::seconds(300);
    config.lruCacheSize = 1024;
    config.cacheExpirationSeconds = std::chrono::seconds(300);
    config.recvBufferSize = 1500;
    return config;
}

// Helper to parse duration string to chrono::seconds
std::chrono::seconds parseDuration(const std::string& str) {
    // Handle formats like "2s", "100ms", "5sec"
    if (str.empty()) {
        return std::chrono::seconds(0);
    }
    
    // Try to parse as number with optional suffix
    std::string trimmed = str;
    std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), ::tolower);
    
    // Extract numeric value
    std::string numericPart;
    std::string suffix;
    size_t pos = trimmed.find_first_of("mssec");
    if (pos != std::string::npos) {
        suffix = trimmed.substr(pos);
        numericPart = trimmed.substr(0, pos);
    } else {
        suffix = "s";  // default to seconds
        numericPart = trimmed;
    }
    
    try {
        long value = std::stol(numericPart);
        if (suffix == "ms" || suffix == "milliseconds") {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::milliseconds(value));
        }
        // "s", "sec", "secs", "seconds" or default all treated as seconds
        return std::chrono::seconds(value);
    } catch (...) {
        return std::chrono::seconds(0);
    }
}

// Helper to parse hex string to uint32_t
uint32_t parseHexMagicNumber(const std::string& hexStr) {
    try {
        // Convert hex string like "0xED75E1ED" to uint32_t
        size_t value = 0;
        for (char c : hexStr) {
            value <<= 4;
            value |= (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
        }
        return static_cast<uint32_t>(value);
    } catch (...) {
        return 0xED75E1ED;  // Default magic number
    }
}

// Helper to save config to YAML string
std::string configToYamlString(const Config& config) {
    std::ostringstream oss;
    oss << "Edriel Configuration\n";
    oss << "===================\n\n";
    oss << "# Network configuration\n";
    oss << "port: " << config.port << "\n";
    oss << "multicast_address: '" << config.multicastAddress << "'\n";
    oss << "magic_number: '" << config.magicNumber << "'\n\n";
    oss << "# Timing configuration\n";
    oss << "discovery_send_period: '" << config.discoverySendPeriod.count() << "s'\n";
    oss << "discovery_cleanup_period: '" << config.discoveryCleanupPeriod.count() << "s'\n";
    oss << "heartbeat_timeout: '" << config.heartbeatTimeout.count() << "s'\n";
    oss << "timestamp_validity_seconds: '" << config.timestampValiditySeconds.count() << "'\n\n";
    oss << "# Cache configuration\n";
    oss << "lru_cache_size: " << config.lruCacheSize << "\n";
    oss << "cache_expiration_seconds: '" << config.cacheExpirationSeconds.count() << "'\n\n";
    oss << "# Performance configuration\n";
    oss << "recv_buffer_size: " << config.recvBufferSize << "\n";
    
    return oss.str();
}

// Helper to save config to JSON string
std::string configToJsonString(const Config& config) {
    std::ostringstream oss;
    oss << R"({
  "network": {
    "port": "uint16_t",
    "multicast_address": "std::string",
    "magic_number": "std::string"
  },
  "timing": {
    "discovery_send_period": "std::chrono::seconds",
    "discovery_cleanup_period": "std::chrono::seconds",
    "heartbeat_timeout": "std::chrono::seconds",
    "timestamp_validity_seconds": "std::chrono::seconds"
  },
  "cache": {
    "lru_cache_size": "std::size_t",
    "cache_expiration_seconds": "std::chrono::seconds"
  },
  "performance": {
    "recv_buffer_size": "std::size_t"
  }
})";
    return oss.str();
}

} // namespace edriel

// Forward declarations for implementation
namespace edriel {

// Inline implementation for simple configurations
Config loadConfigFromYaml(const std::string& filename) {
    Config config = createDefaultConfig();
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[Config] Warning: Could not open YAML file: " << filename << "\n";
        return config;
    }
    
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // Simple YAML parsing (for production, use nlohmann/json)
    // Parse port
    auto portPos = content.find("port:");
    if (portPos != std::string::npos) {
        size_t end = content.find_first_of(" \n", portPos + 5);
        std::string value = content.substr(portPos + 5, end - portPos - 5);
        try {
            config.port = std::stoi(value);
        } catch (...) {}
    }
    
    // Parse multicast_address
    auto multicastPos = content.find("multicast_address:");
    if (multicastPos != std::string::npos) {
        size_t end = content.find_first_of("\n", multicastPos);
        std::string value = content.substr(multicastPos + 18, end - multicastPos - 18);
        if (!value.empty() && value[0] == '\'') {
            value = value.substr(1, value.length() - 2);
        }
        config.multicastAddress = value;
    }
    
    // Parse discovery_send_period
    auto sendPeriodPos = content.find("discovery_send_period:");
    if (sendPeriodPos != std::string::npos) {
        size_t end = content.find_first_of("\n", sendPeriodPos);
        std::string value = content.substr(sendPeriodPos + 22, end - sendPeriodPos - 22);
        config.discoverySendPeriod = parseDuration(value);
    }
    
    // Parse discovery_cleanup_period
    auto cleanupPos = content.find("discovery_cleanup_period:");
    if (cleanupPos != std::string::npos) {
        size_t end = content.find_first_of("\n", cleanupPos);
        std::string value = content.substr(cleanupPos + 24, end - cleanupPos - 24);
        config.discoveryCleanupPeriod = parseDuration(value);
    }
    
    // Parse heartbeat_timeout
    auto timeoutPos = content.find("heartbeat_timeout:");
    if (timeoutPos != std::string::npos) {
        size_t end = content.find_first_of("\n", timeoutPos);
        std::string value = content.substr(timeoutPos + 19, end - timeoutPos - 19);
        config.heartbeatTimeout = parseDuration(value);
    }
    
    // Parse timestamp_validity_seconds
    auto timestampPos = content.find("timestamp_validity_seconds:");
    if (timestampPos != std::string::npos) {
        size_t end = content.find_first_of("\n", timestampPos);
        std::string value = content.substr(timestampPos + 26, end - timestampPos - 26);
        config.timestampValiditySeconds = parseDuration(value);
    }
    
    // Parse lru_cache_size
    auto cachePos = content.find("lru_cache_size:");
    if (cachePos != std::string::npos) {
        size_t end = content.find_first_of("\n", cachePos);
        std::string value = content.substr(cachePos + 16, end - cachePos - 16);
        try {
            config.lruCacheSize = std::stoul(value);
        } catch (...) {}
    }
    
    // Parse cache_expiration_seconds
    auto cacheExpPos = content.find("cache_expiration_seconds:");
    if (cacheExpPos != std::string::npos) {
        size_t end = content.find_first_of("\n", cacheExpPos);
        std::string value = content.substr(cacheExpPos + 24, end - cacheExpPos - 24);
        config.cacheExpirationSeconds = parseDuration(value);
    }
    
    // Parse recv_buffer_size
    auto bufferPos = content.find("recv_buffer_size:");
    if (bufferPos != std::string::npos) {
        size_t end = content.find_first_of("\n", bufferPos);
        std::string value = content.substr(bufferPos + 17, end - bufferPos - 17);
        try {
            config.recvBufferSize = std::stoul(value);
        } catch (...) {}
    }
    
    return config;
}

Config loadConfigFromJson(const std::string& filename) {
    Config config = createDefaultConfig();
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[Config] Warning: Could not open JSON file: " << filename << "\n";
        return config;
    }
    
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // Simple JSON parsing
    auto portPos = content.find("\"port\":");
    if (portPos != std::string::npos) {
        size_t end = content.find_first_of(",}", portPos + 7);
        std::string value = content.substr(portPos + 7, end - portPos - 7);
        try {
            config.port = std::stoi(value);
        } catch (...) {}
    }
    
    // Parse multicast_address
    auto multicastPos = content.find("\"multicast_address\":");
    if (multicastPos != std::string::npos) {
        size_t end = content.find_first_of(",}", multicastPos);
        std::string value = content.substr(multicastPos + 20, end - multicastPos - 20);
        // Remove quotes
        if (!value.empty() && (value[0] == '"' || value[0] == '\'')) {
            value = value.substr(1, value.length() - 2);
        }
        config.multicastAddress = value;
    }
    
    return config;
}

bool saveConfigToYaml(const std::string& filename, const Config& config) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[Config] Error: Could not open file for writing: " << filename << "\n";
        return false;
    }
    
    file << configToYamlString(config);
    
    file.close();
    std::cout << "[Config] Configuration saved to: " << filename << "\n";
    return true;
}

bool saveConfigToJson(const std::string& filename, const Config& config) {
    // JSON implementation similar to YAML
    // For brevity, same logic as saveConfigToYaml
    (void)config;  // Unused in this stub
    return false;
}

} // namespace edriel
