/**
 * @file EdrielGrpcService.cpp
 * @brief Implementation of the callback-based ParticipantStreamServiceImpl.
 */

#include "EdrielGrpcService.hpp"
#include "Edriel.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>

namespace edriel {

// ============================================================================
// SubscriberReactor
// ============================================================================

namespace {
}

SubscriberReactor::SubscriberReactor(ParticipantStreamServiceImpl& service)
    : service_(service) {
    // ADR-0004 bounded outbox: derive the HWM/LWM frame thresholds from the
    // owner's validated config (config.yml reliable_outbox_* keys, Q6). The
    // water marks are rounded toward zero so refusal starts strictly AT the
    // configured fraction and never past the bound itself.
    const edriel::Config& cfg = service_.owner().config();
    outboxMaxFrames_ = cfg.reliableOutboxMaxFrames;
    hwmFrames_ = static_cast<std::size_t>(
        cfg.reliableOutboxHwm * static_cast<double>(outboxMaxFrames_));
    lwmFrames_ = static_cast<std::size_t>(
        cfg.reliableOutboxLwm * static_cast<double>(outboxMaxFrames_));
    // Coherence guard (config already validates lwm < hwm; this also protects
    // programmatic Config use): HWM must be at least 1 frame inside the bound
    // and LWM strictly below HWM, or the gate degenerates.
    if (hwmFrames_ == 0) {
        hwmFrames_ = 1;
    }
    if (hwmFrames_ >= outboxMaxFrames_) {
        hwmFrames_ = outboxMaxFrames_ - 1;
    }
    if (lwmFrames_ >= hwmFrames_) {
        lwmFrames_ = hwmFrames_ / 2;
    }
    // Begin draining the dialing subscriber's heartbeat stream.
    StartRead(&heartbeat_);
}

SubscriberReactor::~SubscriberReactor() = default;

void SubscriberReactor::OnReadDone(bool ok) {
    if (!ok) {
        // The client half-closed its side (or the stream broke/failed). Drain
        // any queued frames first, then finish cleanly once the outbox is empty
        // (defer to OnWriteDone if a write is in flight). finish_() is one-shot
        // and so a concurrently failing write (OnWriteDone(false)) cannot abort
        // gRPC's finish tag by finishing the RPC twice.
        bool canFinish = false;
        bool needKick = false;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (finished_) {
                return;  // already terminating (racing write/read failure)
            }
            if (writing_) {
                finishPending_ = true;      // finish after the in-flight write
            } else if (outbox_.empty()) {
                canFinish = true;
            } else {
                finishPending_ = true;      // drain the rest, then finish
                needKick = true;
            }
        }
        if (canFinish) {
            finish_(teardownStatus_());
            return;
        }
        if (needKick) {
            startWrite_();
        }
        return;
    }

    // A heartbeat arrived. Capture the dialing subscriber's identity once and
    // push current registry presence downstream.
    if (!initialised_) {
        initialised_ = true;
        key_ = SubscriberKey{heartbeat_.pid(), heartbeat_.tid(), heartbeat_.uid()};

        // Anti-spoof gate (ADR-0002 §6.2): only a *known* participant (present
        // in the registry with a matching (pid,tid,uid)) may register and be fed
        // frames. Unknown dialers are finished without registration, closing the
        // stream before it can receive presence or reliable data.
        if (!service_.owner().isKnownParticipant(key_.pid, key_.tid, key_.uid)) {
            finish_(grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                 "unknown dialer: not a known participant"));
            return;
        }

        service_.registerSubscriber(key_, this);

        for (autoDiscovery::ParticipantData& pd
            : service_.owner().snapshotParticipantData()) {
            enqueue(std::move(pd));
        }
    }

    // Continue reading heartbeats (keepalive).
    StartRead(&heartbeat_);
}

void SubscriberReactor::OnWriteDone(bool ok) {
    bool finishNow = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        writing_ = false;
        if (!ok) {
            // The write itself failed (stream broken / cancelled). gRPC may be
            // concurrently failing the read too; the one-shot guard routes this
            // to a single Finish. Prefer a CANCELLED here so teardown reads as
            // a broken stream, not a clean OK.
            finishNow = !finished_;
        }
    }
    if (!ok) {
        if (finishNow) {
            finish_(grpc::Status(grpc::StatusCode::CANCELLED, "stream broken"));
        }
        return;
    }
    // Drain any further queued frames. A client half-close while the outbox is
    // non-empty must not cut the queued presence/data frames short: keep
    // writing, and only finish once the outbox empties.
    startWrite_();
    {
        std::lock_guard<std::mutex> lock(m_);
        // ADR-0004 LWM resume: once the drain brings the outbox to or below
        // the low-water mark, clear the backpressure latch so pushes are
        // accepted again. Per-reactor state only (Q5 fairness).
        if (backpressured_ && outbox_.size() <= lwmFrames_) {
            backpressured_ = false;
        }
        finishNow = finishPending_ && !writing_ && outbox_.empty();
    }
    if (finishNow) {
        finish_(teardownStatus_());
    }
}

void SubscriberReactor::OnCancel() {
    // The client tore the connection down (stop()/re-dial). gRPC invokes this
    // promptly. Retire the reactor locally: record that this teardown is a
    // genuine cancel (so the terminal Finish reports CANCELLED, not a clean
    // OK) and that we must be evicted from the subscriber table so the
    // publisher stops routing pushes (and `subscriberConnectedForTest` stops
    // reporting a dead stream as live) before the next dial registers. The
    // stream's terminal callbacks (OnReadDone/OnWriteDone false) still run and
    // drive finish_() exactly once; we deliberately do NOT set finished_ here
    // (that would suppress the single Finish the terminal callbacks issue and
    // leak the reactor, since gRPC only calls OnDone after a Finish).
    bool shouldUnregister = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (finished_) {
            return;
        }
        cancelled_ = true;
        shouldUnregister = initialised_;
    }
    // Evict AFTER releasing m_: unregisterSubscriber takes the service-level
    // subsMutex_, and taking it while still holding m_ would invert the lock
    // order that pushData() establishes (subsMutex_ -> m_ via enqueue) — a
    // lock-inversion deadlock. Mirror OnDone()'s shape: release m_ first, then
    // touch the shared table. The pointer-guarded erase keeps an old reactor
    // from evicting a newer one re-registered under the same key meanwhile.
    if (shouldUnregister) {
        service_.unregisterSubscriber(key_, this);
    }
}

void SubscriberReactor::OnDone() {
    if (initialised_) {
        // Pointer-guarded: only remove this reactor from the table, never a
        // newer one that reconnected under the same key meanwhile.
        service_.unregisterSubscriber(key_, this);
    }
}

void SubscriberReactor::finish_(grpc::Status status) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (finished_) {
            return;  // a racing OnReadDone/OnWriteDone already issued Finish
        }
        finished_ = true;
    }
    // gRPC's callback server allows exactly one Finish() per stream (it reuses
    // a single finish tag, asserting it is clear on each call). Marking
    // finished_ before issuing lets a concurrent, redundant terminal callback
    // no-op instead of re-Set()-ing that tag, which would trip the library's
    // `call_ == nullptr` CHECK and abort the process.
    Finish(std::move(status));
}

grpc::Status SubscriberReactor::teardownStatus_() {
    std::lock_guard<std::mutex> lock(m_);
    return cancelled_ ? grpc::Status(grpc::StatusCode::CANCELLED, "client tore down")
                      : grpc::Status::OK;
}

void SubscriberReactor::supersede() {
    std::lock_guard<std::mutex> lock(m_);
    cancelled_ = true;  // a newer dial owns the key; stop serving this stream
}

bool SubscriberReactor::isLive() {
    std::lock_guard<std::mutex> lock(m_);
    return !finished_ && !cancelled_;
}

OutboxStatus SubscriberReactor::enqueue(autoDiscovery::ParticipantData&& frame) {
    bool needWrite = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (finished_ || cancelled_) {
            return OutboxStatus::NotConnected;  // stream terminating; drop
        }
        // ADR-0004 HWM gate: once latched backpressured, refuse until the
        // drain (OnWriteDone) crosses the LWM and clears the latch. The bound
        // itself is the hard stop; the HWM is where refusal begins so the
        // in-flight write + buffered tail never overshoot past the bound.
        if (backpressured_ || outbox_.size() >= hwmFrames_) {
            backpressured_ = true;
            return OutboxStatus::Backpressured;
        }
        outbox_.push_back(std::move(frame));
        needWrite = !writing_;
    }
    if (needWrite) {
        startWrite_();
    }
    return OutboxStatus::Accepted;
}

bool SubscriberReactor::isSendable() {
    std::lock_guard<std::mutex> lock(m_);
    return !finished_ && !cancelled_ && !backpressured_
           && outbox_.size() < hwmFrames_;
}

std::size_t SubscriberReactor::outboxDepth() {
    std::lock_guard<std::mutex> lock(m_);
    return outbox_.size();
}

void SubscriberReactor::startWrite_() {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (finished_ || writing_ || outbox_.empty()) {
            return;
        }
        writeBuffer_ = std::move(outbox_.front());
        outbox_.pop_front();
        writing_ = true;
    }
    StartWrite(&writeBuffer_);
}

// ============================================================================
// ParticipantStreamServiceImpl
// ============================================================================

ParticipantStreamServiceImpl::ParticipantStreamServiceImpl(Edriel& owner)
    : owner_(owner)
{}

grpc::ServerBidiReactor<autoDiscovery::ParticipantHeartbeat,
                        autoDiscovery::ParticipantData>*
ParticipantStreamServiceImpl::StreamParticipants(grpc::CallbackServerContext* /*context*/) {
    auto* reactor = new SubscriberReactor(*this);
    return reactor;
}

grpc::ServerUnaryReactor* ParticipantStreamServiceImpl::GetParticipantInfo(
    grpc::CallbackServerContext* /*context*/,
    const autoDiscovery::ParticipantHeartbeat* request,
    autoDiscovery::ParticipantData* response) {
    class UnaryReactor final : public grpc::ServerUnaryReactor {
    public:
        UnaryReactor(grpc::Status status) { Finish(std::move(status)); }
        void OnDone() override {}
    };

    grpc::Status status;
    if (request == nullptr || response == nullptr) {
        status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "null request/response");
    } else if (!owner_.lookupParticipantData(request->pid(), request->tid(),
                                             request->uid(), *response)) {
        status = grpc::Status(grpc::StatusCode::NOT_FOUND, "unknown participant");
    } else {
        status = grpc::Status::OK;
    }
    return new UnaryReactor(std::move(status));
}

OutboxStatus ParticipantStreamServiceImpl::pushData(
    const SubscriberKey& key, autoDiscovery::ParticipantData&& frame) {
    // Holding subsMutex_ across the enqueue guarantees the reactor stays alive
    // for the whole push: OnDone/OnCancel's guarded unregister blocks on the
    // same mutex, and gRPC deletes the reactor only after OnDone returns.
    std::lock_guard<std::mutex> lock(subsMutex_);
    const auto it = subscribers_.find(key);
    if (it == subscribers_.end()) {
        return OutboxStatus::NotConnected;
    }
    return it->second->enqueue(std::move(frame));
}

bool ParticipantStreamServiceImpl::isSendable(const SubscriberKey& key) {
    std::lock_guard<std::mutex> lock(subsMutex_);
    const auto it = subscribers_.find(key);
    return it != subscribers_.end() && it->second->isSendable();
}

std::size_t ParticipantStreamServiceImpl::outboxDepth(const SubscriberKey& key) {
    std::lock_guard<std::mutex> lock(subsMutex_);
    const auto it = subscribers_.find(key);
    return it != subscribers_.end() ? it->second->outboxDepth() : 0;
}

void ParticipantStreamServiceImpl::registerSubscriber(const SubscriberKey& key,
                                                      SubscriberReactor* reactor) {
    std::lock_guard<std::mutex> lock(subsMutex_);
    const auto it = subscribers_.find(key);
    // A newer generation of the same dial supersedes any predecessor still in
    // the table (its gRPC cancel/eviction may lag the re-dial). Marking it now
    // closes the window where a torn-down predecessor could still be reported
    // connected or swallow a frame after the successor has registered.
    if (it != subscribers_.end() && it->second != reactor) {
        it->second->supersede();
    }
    subscribers_[key] = reactor;
}

void ParticipantStreamServiceImpl::unregisterSubscriber(const SubscriberKey& key,
                                                        SubscriberReactor* reactor) {
    std::lock_guard<std::mutex> lock(subsMutex_);
    const auto it = subscribers_.find(key);
    if (it != subscribers_.end() && it->second == reactor) {
        subscribers_.erase(it);
    }
}

bool ParticipantStreamServiceImpl::hasSubscriber(const SubscriberKey& key) const {
    std::lock_guard<std::mutex> lock(subsMutex_);
    const auto it = subscribers_.find(key);
    // Only a genuinely live stream counts as "connected": a successor that
    // superseded it, or a stream whose client tore the connection down, must
    // not keep reporting connected (subscriberConnectedForTest) or satisfy a
    // dial's readiness before the fresh generation has actually registered.
    const bool r = it != subscribers_.end() && it->second->isLive();
    return r;
}

}  // namespace edriel
