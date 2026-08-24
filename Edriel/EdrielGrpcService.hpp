/**
 * @file EdrielGrpcService.hpp
 * @brief Callback-based gRPC ParticipantStreamService server for the reliable path.
 *
 * Implements the ADR-0002 gRPC plane on `grpc_port`. Every node serves one
 * instance:
 *   - GetParticipantInfo (unary): a participant's ParticipantData — the
 *     post-connect verifier/refresher (Channel C in ADR-0002).
 *   - StreamParticipants (bidi): a subscriber dials, the server pushes
 *     ParticipantData presence then reliable payload frames over the stream.
 *
 * The service uses the grpcpp **callback** API (ServerBidiReactor), which is
 * the gRPC-recommended design when a server must push data asynchronously from
 * producer threads (the publisher's io_context) into a long-lived stream — the
 * synchronous ServerReaderWriter is not concurrent-safe. The reactor's
 * per-subscriber outbox is drained to StartWrite, so StartWrite can be called
 * from any thread safely.
 *
 * Thread safety: reactors are handed to gRPC via `new`; gRPC invokes OnDone()
 * and then deletes them. The subscriber table in the service therefore guards
 * reactor access with a mutex, and every push holds that mutex across the whole
 * enqueue so a reactor cannot be destroyed mid-push (OnDone blocks on the same
 * mutex to unregister, so deletion only happens after all in-flight pushes
 * release it).
 */

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <tuple>

#include <grpcpp/grpcpp.h>

#include "autoDiscovery.pb.h"
#include "autoDiscovery_grpc_service.grpc.pb.h"
#include "autoDiscovery_grpc_service.pb.h"

namespace edriel {

class Edriel;
class ParticipantStreamServiceImpl;

/// Identity of a dialing subscriber, keying its gRPC stream.
struct SubscriberKey {
    std::uint32_t pid;
    std::uint64_t tid;
    std::uint64_t uid;

    friend bool operator<(const SubscriberKey& a, const SubscriberKey& b) {
        return std::tie(a.pid, a.tid, a.uid) < std::tie(b.pid, b.tid, b.uid);
    }
    friend bool operator==(const SubscriberKey& a, const SubscriberKey& b) {
        return a.pid == b.pid && a.tid == b.tid && a.uid == b.uid;
    }
};

/// Per-dial callback reactor for the subscriber's stream.
class SubscriberReactor final
    : public grpc::ServerBidiReactor<autoDiscovery::ParticipantHeartbeat,
                                     autoDiscovery::ParticipantData> {
public:
    explicit SubscriberReactor(ParticipantStreamServiceImpl& service);
    ~SubscriberReactor() override;

    /// Called by gRPC as the client's heartbeats arrive.
    void OnReadDone(bool ok) override;
    /// Called by gRPC after each StartWrite completes.
    void OnWriteDone(bool ok) override;
    /// Called by gRPC when the RPC is fully finished and the reactor is about
    /// to be destroyed.
    void OnDone() override;

    /// Thread-safe: enqueue a ParticipantData frame for this subscriber and
    /// kick a write if one is not already in flight.
    void enqueue(autoDiscovery::ParticipantData&& frame);

private:
    void startWrite_();  // if idle and the outbox is non-empty, StartWrite

    ParticipantStreamServiceImpl& service_;
    autoDiscovery::ParticipantHeartbeat heartbeat_;  ///< current read buffer
    autoDiscovery::ParticipantData writeBuffer_;     ///< scratch sink for StartWrite
    bool initialised_{false};  ///< subscriber identity seen yet
    SubscriberKey key_{0, 0, 0};

    std::mutex m_;
    std::deque<autoDiscovery::ParticipantData> outbox_;
    bool writing_ = false;
    bool finishPending_ = false;  ///< client half-closed while a write was in flight
};

/// Callback gRPC service: GetParticipantInfo (unary) + StreamParticipants (bidi).
class ParticipantStreamServiceImpl final
    : public autoDiscovery::ParticipantStreamService::CallbackService {
public:
    explicit ParticipantStreamServiceImpl(Edriel& owner);

    /// Bidi: create a SubscriberReactor, register the dialing subscriber once
    /// identified, push presence, stream reliable payload frames from outbox.
    grpc::ServerBidiReactor<autoDiscovery::ParticipantHeartbeat,
                            autoDiscovery::ParticipantData>*
    StreamParticipants(grpc::CallbackServerContext* context) override;

    /// Unary: build the participant's ParticipantData or NOT_FOUND.
    grpc::ServerUnaryReactor* GetParticipantInfo(
        grpc::CallbackServerContext* context,
        const autoDiscovery::ParticipantHeartbeat* request,
        autoDiscovery::ParticipantData* response) override;

    /// Push a reliable payload frame to the subscriber identified by `key`.
    /// Returns false if no such subscriber is currently connected.
    bool pushData(const SubscriberKey& key, autoDiscovery::ParticipantData&& frame);

    /// Owning node (used to build presence from the registry).
    Edriel& owner() const { return owner_; }

    /// Whether a subscriber with `key` currently has a live stream (test hook
    /// and liveness probe).
    bool hasSubscriber(const SubscriberKey& key) const;

    /// Register/unregister a subscriber's reactor (called by SubscriberReactor).
    void registerSubscriber(const SubscriberKey& key, SubscriberReactor* reactor);
    void unregisterSubscriber(const SubscriberKey& key);

private:
    Edriel& owner_;

    mutable std::mutex subsMutex_;
    std::map<SubscriberKey, SubscriberReactor*> subscribers_;
};

}  // namespace edriel