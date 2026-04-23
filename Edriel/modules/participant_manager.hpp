/**
 * @file participant_manager.hpp
 * @brief Participant management module for lifecycle tracking
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <chrono>
#include "autoDiscovery.pb.h"

namespace edriel {

/**
 * @brief Participant lifecycle states
 */
enum class ParticipantState {
  UNKNOWN,      // Initial state
  JOINING,      // Joining network
  ONLINE,       // Active and healthy
  OFFLINE,      // Heartbeat expired
  TIMEDOUT      // Timeout threshold reached
};

/**
 * @brief Participant manager for lifecycle tracking
 */
class ParticipantManager {
 public:
  /**
   * @brief Participant data
   */
  struct Participant {
    uint32_t pid;
    uint64_t tid;
    uint64_t uid;
    std::string endpoint;
    ParticipantState state;
    std::chrono::steady_clock::time_point lastHeartbeat;
    std::string status;

    Participant() : pid(0), tid(0), uid(0), state(ParticipantState::UNKNOWN) {
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
      lastHeartbeat = now;
    }
  };

  /**
   * @brief Constructor
   * @param heartbeatTimeout Heartbeat timeout duration
   */
  explicit ParticipantManager(uint32_t heartbeatTimeout = 10000)
    : heartbeatTimeout_(heartbeatTimeout) {}

  /**
   * @brief Register participant
   * @param participant Participant data
   * @return true if registration successful
   */
  bool registerParticipant(const autoDiscovery::ParticipantHeartbeat& participant) {
    auto participantObj = std::make_shared<Participant>();
    participantObj->pid = participant.pid();
    participantObj->tid = participant.tid();
    participantObj->uid = participant.uid();
    participantObj->state = ParticipantState::ONLINE;
    participantObj->lastHeartbeat = std::chrono::steady_clock::now();
    participantObj->status = "online";

    participants_[participant.pid] = participantObj;
    notifyParticipantChange(participant.pid);
    return true;
  }

  /**
   * @brief Update participant heartbeat
   * @param pid Participant ID
   * @return true if update successful
   */
  bool updateHeartbeat(uint32_t pid) {
    auto it = participants_.find(pid);
    if (it != participants_.end()) {
      it->second->lastHeartbeat = std::chrono::steady_clock::now();
      it->second->state = ParticipantState::ONLINE;
      it->second->status = "online";
      return true;
    }
    return false;
  }

  /**
   * @brief Unregister participant
   * @param pid Participant ID
   * @return true if unregistration successful
   */
  bool unregisterParticipant(uint32_t pid) {
    auto it = participants_.find(pid);
    if (it != participants_.end()) {
      notifyParticipantChange(pid);
      participants_.erase(it);
      return true;
    }
    return false;
  }

  /**
   * @brief Get participant
   * @param pid Participant ID
   * @return Shared pointer to participant, or nullptr if not found
   */
  std::shared_ptr<Participant> getParticipant(uint32_t pid) const {
    auto it = participants_.find(pid);
    return (it != participants_.end()) ? it->second : nullptr;
  }

  /**
   * @brief List all participants
   * @return Vector of shared pointers to participants
   */
  std::vector<std::shared_ptr<Participant>> listParticipants() const {
    std::vector<std::shared_ptr<Participant>> result;
    for (const auto& [pid, participant] : participants_) {
      result.push_back(participant);
    }
    return result;
  }

  /**
   * @brief Check if participant exists
   * @param pid Participant ID
   * @return true if participant exists
   */
  bool hasParticipant(uint32_t pid) const {
    return participants_.find(pid) != participants_.end();
  }

  /**
   * @brief Get participant count
   * @return Number of registered participants
   */
  size_t count() const {
    return participants_.size();
  }

  /**
   * @brief Check if participant is online
   * @param pid Participant ID
   * @return true if participant is online
   */
  bool isOnline(uint32_t pid) const {
    auto participant = getParticipant(pid);
    return participant && participant->state == ParticipantState::ONLINE;
  }

 private:
  /**
   * @brief Notify of participant change (TODO: Implement event emission)
   * @param pid Participant ID
   */
  void notifyParticipantChange(uint32_t pid) {
    // TODO: Emit event
  }

  uint32_t heartbeatTimeout_;
  std::unordered_map<uint32_t, std::shared_ptr<Participant>> participants_;
};

}  // namespace edriel
