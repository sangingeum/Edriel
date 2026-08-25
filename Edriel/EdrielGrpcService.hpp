/**
 * @file EdrielGrpcService.hpp
 * @brief Callback-based gRPC ParticipantStreamService server for the reliable path.
 *
 * Implements the ADR-0002 gRPC plane on `grpc_port`. Every node serves one
 * instance:
 *   - GetParticipantInfo (unary): a participant's ParticipantData — the
 *     post-connect verifier/refresher (Channel C in ADR-0002).
  *   - StreamParticipants (bidi): a subscriber dials, the server gates its
  *     identity against the participant registry (anti-spoof, §6.2), then
  *     pushes ParticipantData presence then reliable payload frames over the
  *     stream.
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
    /// Called by gRPC when the client cancels the RPC (e.g. its stop()/re-dial
    /// tore down the connection). Retire promptly so the publisher stops
    /// routing pushes to this now-dead stream.
    void OnCancel() override;
    /// Called by gRPC when the RPC is fully finished and the reactor is about
    /// to be destroyed.
    void OnDone() override;

    /// Thread-safe: enqueue a ParticipantData frame for this subscriber and
    /// kick a write if one is not already in flight.
    void enqueue(autoDiscovery::ParticipantData&& frame);

    /// Thread-safe: is this stream still a live, deliverable subscriber? False
    /// once teardown has begun (client cancel or terminal Finish), so the
    /// publisher neither routes frames into a dead stream nor reports a
    /// torn-down dial as connected.
    bool isLive();

    /// Thread-safe: mark this stream superseded by a newer dial of the same
    /// subscriber (same key). Future enqueues are dropped and isLive() turns
    /// false immediately, closing the window where a predecessor reactor could
    /// still swallow frames after its successor has registered.
    void supersede();

private:
    void startWrite_();  // if idle and the outbox is non-empty, StartWrite
    /// Guarded finish: call Finish() at most once. gRPC's callback server
    /// wraps terminal status in a single finish tag (finish_tag_) that it
    /// `Set()`s per Finish call and asserts is clear; a second Finish() on the
    /// same stream therefore aborts (callback_common.h `call_ == nullptr`).
    /// On teardown, gRPC can fail both the pending read and the in-flight
    /// write, dispatching OnReadDone(false) and OnWriteDone(false) on separate
    /// executor threads — without a one-shot guard these two finish the RPC
    /// twice. This records the decision under m_ and issues Finish exactly once.
    void finish_(grpc::Status status);
    /// Deliberate teardown Finish status: CANCELLED when the stream ended by a
    /// genuine client cancel (OnCancel fired), OK on a clean client half-close
    /// (WritesDone, no cancel). gRPC routes a graceful half-close to
    /// OnReadDone(false) WITHOUT OnCancel and a teardown/cancel to both, so the
    /// intent is keyed off the cancel flag, not the callback. Read under m_.
    grpc::Status teardownStatus_();

    ParticipantStreamServiceImpl& service_;
    autoDiscovery::ParticipantHeartbeat heartbeat_;  ///< current read buffer
    autoDiscovery::ParticipantData writeBuffer_;     ///< scratch sink for StartWrite
    bool initialised_{false};  ///< subscriber identity seen yet
    bool finished_{false};     ///< one-shot: terminal Finish already issued
    bool cancelled_{false};    ///< client-cancel teardown: finish CANCELLED not OK
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

    /// Register the subscriber's reactor (called by SubscriberReactor once its
    /// identity is known). If a previous reactor is still registered for the
    /// same key, it is retired so it no longer receives pushes; the table then
    /// maps the key to the newest live connection.
    void registerSubscriber(const SubscriberKey& key, SubscriberReactor* reactor);
    /// Remove `reactor` from the table only if it is still the registered entry
    /// for `key` (a pointer-guarded erase). Called by SubscriberReactor when its
    /// stream terminates. The guard matters during reconnect: an old reactor
    /// whose OnDone fires after a new reactor registered must not erase the new
    /// one.
    void unregisterSubscriber(const SubscriberKey& key, SubscriberReactor* reactor);

private:
    Edriel& owner_;

    mutable std::mutex subsMutex_;
    std::map<SubscriberKey, SubscriberReactor*> subscribers_;
};

}  // namespace edriel