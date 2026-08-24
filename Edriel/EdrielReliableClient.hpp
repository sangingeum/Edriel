/**
 * @file EdrielReliableClient.hpp
 * @brief Subscriber-initiated dial to a publisher for the reliable path.
 *
 * One ReliableSubscriberConnection represents this node dialing one publisher's
 * server (ADR-0002, subscriber-initiated). It opens a bidi StreamParticipants
 * stream, sends this node's heartbeat identity, and reads ParticipantData
 * frames; payload-bearing frames (reliable_data) are routed into the owning
 * Edriel's exactly-once reorder/dedup window and dispatched to local callbacks.
 *
 * Runs on its own std::thread (the gRPC sync client read loop blocks). The
 * owning node reconciles the connection set against registry state; stop()
 * cancels the active ClientContext to unblock a pending Read and joins.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "autoDiscovery_grpc_service.grpc.pb.h"
#include "autoDiscovery_grpc_service.pb.h"
#include "EdrielGrpcService.hpp"  // SubscriberKey

namespace edriel {

class Edriel;

/**
 * @brief One dial from this (subscriber) node to a publisher's gRPC server.
 */
class ReliableSubscriberConnection {
public:
    /**
     * @param node Owning node (receives dispatched frames)
     * @param publisher Publisher identity dialed
     * @param target address:port of the publisher's server
     * @param selfPid/selfTid/selfUid this node's identity (sent as heartbeat)
     */
    ReliableSubscriberConnection(Edriel& node, SubscriberKey publisher,
                                 std::string target,
                                 std::uint32_t selfPid, std::uint64_t selfTid,
                                 std::uint64_t selfUid);

    ~ReliableSubscriberConnection();

    /// Spawns the reading thread.
    void start();

    /// Stops the reading thread, cancels the in-flight read, and joins.
    void stop();

private:
    void run_();

    Edriel& node_;
    SubscriberKey publisher_;
    std::string target_;
    std::uint32_t selfPid_;
    std::uint64_t selfTid_;
    std::uint64_t selfUid_;

    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::shared_ptr<grpc::Channel> channel_;

    std::mutex ctxMutex_;                 ///< guards activeCtx_ (cancel from stop)
    grpc::ClientContext* activeCtx_ = nullptr;
};

}  // namespace edriel