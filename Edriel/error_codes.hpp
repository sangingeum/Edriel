/**
 * @file error_codes.hpp
 * @brief Error codes and error classes for Edriel C++20 multi-cast auto-discovery
 * 
 * Provides specific error codes for failure modes:
 * - Magic number validation errors
 * - Participant lifecycle errors
 * - Topic management errors
 * - gRPC streaming errors
 * - Network/multicast errors
 * - Configuration errors
 */

#pragma once

#include <string>
#include <stdexcept>
#include <cstdint>

namespace edriel {

/**
 * @brief Exception classes for specific error types
 */
class EdrielException : public std::exception {
public:
    EdrielException(const std::string& message)
        : message_(message) {}

    const char* what() const noexcept override {
        return message_.c_str();
    }

protected:
    std::string message_;
};

class MagicNumberException : public EdrielException {
public:
    explicit MagicNumberException(uint32_t expected, uint32_t actual)
        : EdrielException(std::string("Magic number mismatch: expected ") + 
                         std::to_string(expected) + 
                         ", actual: " + std::to_string(actual)) {}
};

/**
 * @brief Timestamp validity window in seconds for replay attack prevention
 */
constexpr uint64_t TIMESTAMP_VALIDITY_SECONDS = 300;  // 5 minutes

class TimestampException : public EdrielException {
public:
    explicit TimestampException(uint64_t received, uint64_t current)
        : EdrielException(std::string("Timestamp invalidity: received ") + 
                         std::to_string(received) + 
                         ", current: " + std::to_string(current) +
                         ", valid for " + std::to_string(TIMESTAMP_VALIDITY_SECONDS) + "s") {}
};

class ParticipantException : public EdrielException {
public:
    explicit ParticipantException(const std::string& participantId, const std::string& message)
        : EdrielException(std::string("Participant [") + participantId + "] " + message) {}
};

class TopicException : public EdrielException {
public:
    TopicException(const std::string& topic, const std::string& message)
        : EdrielException(std::string("Topic [") + topic + "] " + message) {}
};

class gRPCException : public EdrielException {
public:
    gRPCException(const std::string& message)
        : EdrielException(std::string("gRPC Error: ") + message) {}
};

class NetworkException : public EdrielException {
public:
    explicit NetworkException(const std::string& message)
        : EdrielException(std::string("Network Error: ") + message) {}
};

class ConfigurationException : public EdrielException {
public:
    ConfigurationException(const std::string& message)
        : EdrielException(std::string("Configuration Error: ") + message) {}
};

/**
 * @brief Error code enumeration for specific failure modes
 * 
 * Each error code represents a distinct failure scenario that enables
 * appropriate error handling and recovery strategies.
 */
enum class ErrorCode : int {
    // Success (no error)
    SUCCESS = 0,
    
    // Magic number related errors
    MAGIC_NUMBER_INVALID = -1,           // Magic number not matching expected value
    MAGIC_NUMBER_CORRUPTED = -2,         // Magic number bit corruption detected
    MAGIC_NUMBER_MISMATCH = -3,          // Received magic number doesn't match sender
    
    // Timestamp related errors
    TIMESTAMP_INVALID = -4,              // Timestamp outside validity window
    TIMESTAMP_REPLAY_DETECTED = -5,      // Replay attack detected (old timestamp)
    TIMESTAMP_TOO_AHEAD = -6,            // Timestamp in future (clock skew)
    TIMESTAMP_ZERO = -7,                 // Zero timestamp received
    
    // Participant lifecycle errors
    PARTICIPANT_NOT_FOUND = -8,          // Participant doesn't exist
    PARTICIPANT_EXPIRED = -9,            // Participant timeout exceeded
    PARTICIPANT_CONFLICT = -10,          // State transition conflict
    PARTICIPANT_DUPLICATE = -11,         // Duplicate participant registration
    
    // Topic management errors
    TOPIC_NOT_FOUND = -12,               // Topic doesn't exist
    TOPIC_ALREADY_EXISTS = -13,          // Topic registration conflict
    TOPIC_UNREGISTERED = -14,            // Unregistration for non-existent topic
    TOPIC_EMPTY_PUBLISHERS = -15,        // No publishers for topic
    
    // gRPC streaming errors
    GRPC_STREAM_CLOSURE = -16,           // Streaming connection closed
    GRPC_SEND_FAILED = -17,              // Failed to send participant data
    GRPC_RECEIVE_TIMEOUT = -18,          // No data received within timeout
    GRPC_PROTOCOL_MISMATCH = -19,        // Protocol version mismatch
    
    // Network/multicast errors
    NETWORK_SOCKET_ERROR = -20,          // Socket operation failed
    NETWORK_MULTICAST_JOIN_FAILED = -21, // Failed to join multicast group
    NETWORK_MULTICAST_LEAVE_FAILED = -22,// Failed to leave multicast group
    NETWORK_BUFFER_FULL = -23,           // Receive/send buffer full
    NETWORK_DISCONNECTED = -24,          // Multicast connection lost
    
    // Configuration errors
    CONFIG_FILE_NOT_FOUND = -25,         // Configuration file doesn't exist
    CONFIG_FILE_READ_ERROR = -26,        // Error reading configuration file
    CONFIG_FILE_PARSE_ERROR = -27,       // Error parsing configuration
    CONFIG_INVALID_PORT = -28,           // Invalid port number
    CONFIG_INVALID_MAGIC = -29,          // Invalid magic number format
    
    // Message handling errors
    MESSAGE_SERIALIZE_ERROR = -30,       // Failed to serialize message
    MESSAGE_DESERIALIZE_ERROR = -31,     // Failed to deserialize message
    MESSAGE_TOO_LARGE = -32,             // Message exceeds buffer size
    MESSAGE_INVALID_SIZE = -33,          // Message has invalid size
    
    // General errors
    UNKNOWN_ERROR = -999,                // Unknown error condition
    INTERNAL_ERROR = -100                // Internal system error
};

/**
 * @brief Converts ErrorCode to string representation
 */
inline std::string errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS: return "SUCCESS";
        case ErrorCode::MAGIC_NUMBER_INVALID: return "MAGIC_NUMBER_INVALID";
        case ErrorCode::MAGIC_NUMBER_CORRUPTED: return "MAGIC_NUMBER_CORRUPTED";
        case ErrorCode::MAGIC_NUMBER_MISMATCH: return "MAGIC_NUMBER_MISMATCH";
        case ErrorCode::TIMESTAMP_INVALID: return "TIMESTAMP_INVALID";
        case ErrorCode::TIMESTAMP_REPLAY_DETECTED: return "TIMESTAMP_REPLAY_DETECTED";
        case ErrorCode::TIMESTAMP_TOO_AHEAD: return "TIMESTAMP_TOO_AHEAD";
        case ErrorCode::TIMESTAMP_ZERO: return "TIMESTAMP_ZERO";
        case ErrorCode::PARTICIPANT_NOT_FOUND: return "PARTICIPANT_NOT_FOUND";
        case ErrorCode::PARTICIPANT_EXPIRED: return "PARTICIPANT_EXPIRED";
        case ErrorCode::PARTICIPANT_CONFLICT: return "PARTICIPANT_CONFLICT";
        case ErrorCode::PARTICIPANT_DUPLICATE: return "PARTICIPANT_DUPLICATE";
        case ErrorCode::TOPIC_NOT_FOUND: return "TOPIC_NOT_FOUND";
        case ErrorCode::TOPIC_ALREADY_EXISTS: return "TOPIC_ALREADY_EXISTS";
        case ErrorCode::TOPIC_UNREGISTERED: return "TOPIC_UNREGISTERED";
        case ErrorCode::TOPIC_EMPTY_PUBLISHERS: return "TOPIC_EMPTY_PUBLISHERS";
        case ErrorCode::GRPC_STREAM_CLOSURE: return "GRPC_STREAM_CLOSURE";
        case ErrorCode::GRPC_SEND_FAILED: return "GRPC_SEND_FAILED";
        case ErrorCode::GRPC_RECEIVE_TIMEOUT: return "GRPC_RECEIVE_TIMEOUT";
        case ErrorCode::GRPC_PROTOCOL_MISMATCH: return "GRPC_PROTOCOL_MISMATCH";
        case ErrorCode::NETWORK_SOCKET_ERROR: return "NETWORK_SOCKET_ERROR";
        case ErrorCode::NETWORK_MULTICAST_JOIN_FAILED: return "NETWORK_MULTICAST_JOIN_FAILED";
        case ErrorCode::NETWORK_MULTICAST_LEAVE_FAILED: return "NETWORK_MULTICAST_LEAVE_FAILED";
        case ErrorCode::NETWORK_BUFFER_FULL: return "NETWORK_BUFFER_FULL";
        case ErrorCode::NETWORK_DISCONNECTED: return "NETWORK_DISCONNECTED";
        case ErrorCode::CONFIG_FILE_NOT_FOUND: return "CONFIG_FILE_NOT_FOUND";
        case ErrorCode::CONFIG_FILE_READ_ERROR: return "CONFIG_FILE_READ_ERROR";
        case ErrorCode::CONFIG_FILE_PARSE_ERROR: return "CONFIG_FILE_PARSE_ERROR";
        case ErrorCode::CONFIG_INVALID_PORT: return "CONFIG_INVALID_PORT";
        case ErrorCode::CONFIG_INVALID_MAGIC: return "CONFIG_INVALID_MAGIC";
        case ErrorCode::MESSAGE_SERIALIZE_ERROR: return "MESSAGE_SERIALIZE_ERROR";
        case ErrorCode::MESSAGE_DESERIALIZE_ERROR: return "MESSAGE_DESERIALIZE_ERROR";
        case ErrorCode::MESSAGE_TOO_LARGE: return "MESSAGE_TOO_LARGE";
        case ErrorCode::MESSAGE_INVALID_SIZE: return "MESSAGE_INVALID_SIZE";
        case ErrorCode::UNKNOWN_ERROR: return "UNKNOWN_ERROR";
        case ErrorCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
        default: return "UNKNOWN_ERROR";
    }
}

/**
 * @brief Converts ErrorCode to human-readable string
 */
inline std::string errorCodeToStringVerbose(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS: return "Operation completed successfully";
        case ErrorCode::MAGIC_NUMBER_INVALID: return "Invalid magic number detected";
        case ErrorCode::MAGIC_NUMBER_CORRUPTED: return "Magic number corruption detected (bit pattern altered)";
        case ErrorCode::MAGIC_NUMBER_MISMATCH: return "Magic number mismatch between sender and receiver";
        case ErrorCode::TIMESTAMP_INVALID: return "Timestamp outside validity window";
        case ErrorCode::TIMESTAMP_REPLAY_DETECTED: return "Replay attack detected - old timestamp received";
        case ErrorCode::TIMESTAMP_TOO_AHEAD: return "Timestamp in future - clock skew detected";
        case ErrorCode::TIMESTAMP_ZERO: return "Invalid zero timestamp received";
        case ErrorCode::PARTICIPANT_NOT_FOUND: return "Participant not found in registry";
        case ErrorCode::PARTICIPANT_EXPIRED: return "Participant heartbeat timeout exceeded";
        case ErrorCode::PARTICIPANT_CONFLICT: return "Participant state transition conflict";
        case ErrorCode::PARTICIPANT_DUPLICATE: return "Duplicate participant registration attempted";
        case ErrorCode::TOPIC_NOT_FOUND: return "Topic not found in registry";
        case ErrorCode::TOPIC_ALREADY_EXISTS: return "Topic already exists in registry";
        case ErrorCode::TOPIC_UNREGISTERED: return "Unregistration attempted for non-existent topic";
        case ErrorCode::TOPIC_EMPTY_PUBLISHERS: return "No publishers registered for topic";
        case ErrorCode::GRPC_STREAM_CLOSURE: return "gRPC streaming connection closed";
        case ErrorCode::GRPC_SEND_FAILED: return "Failed to send participant data via gRPC";
        case ErrorCode::GRPC_RECEIVE_TIMEOUT: return "gRPC receive timeout - no data received";
        case ErrorCode::GRPC_PROTOCOL_MISMATCH: return "gRPC protocol version mismatch";
        case ErrorCode::NETWORK_SOCKET_ERROR: return "Socket operation failed";
        case ErrorCode::NETWORK_MULTICAST_JOIN_FAILED: return "Failed to join multicast group";
        case ErrorCode::NETWORK_MULTICAST_LEAVE_FAILED: return "Failed to leave multicast group";
        case ErrorCode::NETWORK_BUFFER_FULL: return "Receive buffer full or send buffer exhausted";
        case ErrorCode::NETWORK_DISCONNECTED: return "Multicast connection lost";
        case ErrorCode::CONFIG_FILE_NOT_FOUND: return "Configuration file not found";
        case ErrorCode::CONFIG_FILE_READ_ERROR: return "Error reading configuration file";
        case ErrorCode::CONFIG_FILE_PARSE_ERROR: return "Error parsing configuration file";
        case ErrorCode::CONFIG_INVALID_PORT: return "Invalid port number specified";
        case ErrorCode::CONFIG_INVALID_MAGIC: return "Invalid magic number format";
        case ErrorCode::MESSAGE_SERIALIZE_ERROR: return "Message serialization failed";
        case ErrorCode::MESSAGE_DESERIALIZE_ERROR: return "Message deserialization failed";
        case ErrorCode::MESSAGE_TOO_LARGE: return "Message exceeds maximum buffer size";
        case ErrorCode::MESSAGE_INVALID_SIZE: return "Message has invalid size";
        case ErrorCode::UNKNOWN_ERROR: return "Unknown error occurred";
        case ErrorCode::INTERNAL_ERROR: return "Internal system error";
        default: return "Unknown error code";
    }
}

/**
 * @brief Exception wrapper for error codes
 * 
 * Wraps an error code in an exception with descriptive message.
 */
inline EdrielException makeException(ErrorCode code, const std::string& participantId = "") {
    switch (code) {
        case ErrorCode::MAGIC_NUMBER_INVALID:
        case ErrorCode::MAGIC_NUMBER_CORRUPTED:
        case ErrorCode::MAGIC_NUMBER_MISMATCH:
            return MagicNumberException(0, 0);
            
        case ErrorCode::TIMESTAMP_INVALID:
        case ErrorCode::TIMESTAMP_REPLAY_DETECTED:
        case ErrorCode::TIMESTAMP_TOO_AHEAD:
        case ErrorCode::TIMESTAMP_ZERO:
            return TimestampException(0, 0);
            
        case ErrorCode::PARTICIPANT_NOT_FOUND:
        case ErrorCode::PARTICIPANT_EXPIRED:
        case ErrorCode::PARTICIPANT_CONFLICT:
        case ErrorCode::PARTICIPANT_DUPLICATE:
            return ParticipantException(participantId, errorCodeToStringVerbose(code));
            
        case ErrorCode::TOPIC_NOT_FOUND:
        case ErrorCode::TOPIC_ALREADY_EXISTS:
        case ErrorCode::TOPIC_UNREGISTERED:
        case ErrorCode::TOPIC_EMPTY_PUBLISHERS:
            return TopicException("", errorCodeToStringVerbose(code));
            
        case ErrorCode::GRPC_STREAM_CLOSURE:
        case ErrorCode::GRPC_SEND_FAILED:
        case ErrorCode::GRPC_RECEIVE_TIMEOUT:
        case ErrorCode::GRPC_PROTOCOL_MISMATCH:
            return gRPCException(errorCodeToStringVerbose(code));
            
        case ErrorCode::NETWORK_SOCKET_ERROR:
        case ErrorCode::NETWORK_MULTICAST_JOIN_FAILED:
        case ErrorCode::NETWORK_MULTICAST_LEAVE_FAILED:
        case ErrorCode::NETWORK_BUFFER_FULL:
        case ErrorCode::NETWORK_DISCONNECTED:
            return NetworkException(errorCodeToStringVerbose(code));
            
        case ErrorCode::CONFIG_FILE_NOT_FOUND:
        case ErrorCode::CONFIG_FILE_READ_ERROR:
        case ErrorCode::CONFIG_FILE_PARSE_ERROR:
        case ErrorCode::CONFIG_INVALID_PORT:
        case ErrorCode::CONFIG_INVALID_MAGIC:
            return ConfigurationException(errorCodeToStringVerbose(code));
            
        case ErrorCode::MESSAGE_SERIALIZE_ERROR:
        case ErrorCode::MESSAGE_DESERIALIZE_ERROR:
        case ErrorCode::MESSAGE_TOO_LARGE:
        case ErrorCode::MESSAGE_INVALID_SIZE:
            return EdrielException(errorCodeToStringVerbose(code));
            
        case ErrorCode::UNKNOWN_ERROR:
        case ErrorCode::INTERNAL_ERROR:
        default:
            return EdrielException(errorCodeToStringVerbose(code));
    }
}

} // namespace edriel
