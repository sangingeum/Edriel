#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/message.h>


namespace edriel {

// C++20 Concepts for Interface Constraints

// Topic type constraint: must derive from google::protobuf::Message
template<typename T>
concept Topic = std::is_base_of<google::protobuf::Message, T>::value;

// Participant type constraint: must have pid, tid, uid, status, endpoint fields
struct ParticipantConcept {
  static constexpr uint32_t INVALID_PID = 0;
  static constexpr uint64_t INVALID_TID = 0;
  static constexpr uint64_t INVALID_UID = 0;

  uint32_t pid = INVALID_PID;
  uint64_t tid = INVALID_TID;
  uint64_t uid = INVALID_UID;
  std::string status = "offline";
  std::string endpoint;
};

// Participant concept placeholder
// TODO: Implement with proper type constraints
// template<typename T>
// concept Participant = requires(T p) { ... };

// Magic Number constraint for security verification
// Note: MAGIC_NUMBER is defined in Edriel.hpp
constexpr uint32_t MAGIC_NUMBER_SIZE = sizeof(uint32_t);

template<typename T>
concept MessageWithMagic = 
  requires(T msg) {
    // msg must have a magic_number field
    { msg.magic_number() } -> std::same_as<uint32_t>;
    // msg must be mutable for setting magic number
    msg.magic_number() = MAGIC_NUMBER;
  };

// Timestamp constraint for replay attack prevention
template<typename T>
concept HasTimestamp = 
  requires(T msg) {
    { msg.timestamp() } -> std::same_as<std::chrono::steady_clock::time_point>;
  };

// Topic Registry constraint
template<typename T>
concept TopicRegistry = 
  requires(T registry) {
    { registry.addPublisher() } -> std::same_as<void>;
    { registry.addSubscriber() } -> std::same_as<void>;
    { registry.hasPublisher() } -> std::same_as<bool>;
    { registry.hasSubscriber() } -> std::same_as<bool>;
    { registry.getSubscribers() } -> std::same_as<std::set<uint32_t>>;
  };

// Multicast Socket constraint
template<typename T>
concept MulticastSocket = 
  requires(T socket) {
    { socket.create() } -> std::same_as<void>;
    { socket.setReuseAddress() } -> std::same_as<void>;
    { socket.setBroadcast() } -> std::same_as<void>;
    { socket.joinGroup() } -> std::same_as<bool>;
    { socket.async_send_to() } -> std::same_as<std::future<void>>;
  };

// Async Task constraint
template<typename T>
concept AsyncTask = 
  requires(T task) {
    { task.async_start() } -> std::same_as<void>;
    { task.async_stop() } -> std::same_as<void>;
    { task.is_running() } -> std::same_as<bool>;
  };

}  // namespace edriel
