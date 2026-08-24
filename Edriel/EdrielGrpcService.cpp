/**
 * @file EdrielGrpcService.cpp
 * @brief Implementation of the callback-based ParticipantStreamServiceImpl.
 */

#include "EdrielGrpcService.hpp"
#include "Edriel.hpp"

namespace edriel {

// ============================================================================
// SubscriberReactor
// ============================================================================

SubscriberReactor::SubscriberReactor(ParticipantStreamServiceImpl& service)
    : service_(service) {
    // Begin draining the dialing subscriber's heartbeat stream.
    StartRead(&heartbeat_);
}

SubscriberReactor::~SubscriberReactor() = default;

void SubscriberReactor::OnReadDone(bool ok) {
    if (!ok) {
        // The client half-closed its side. Drain any queued frames first, then
        // finish cleanly once the outbox is empty (defer to OnWriteDone if a
        // write is in flight).
        bool canFinish = false;
        bool needKick = false;
        {
            std::lock_guard<std::mutex> lock(m_);
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
            Finish(grpc::Status::OK);
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
            Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
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
    {
        std::lock_guard<std::mutex> lock(m_);
        writing_ = false;
    }
    if (!ok) {
        Finish(grpc::Status(grpc::StatusCode::CANCELLED, "stream broken"));
        return;
    }
    // Drain any further queued frames. A client half-close while the outbox is
    // non-empty must not cut the queued presence/data frames short: keep
    // writing, and only finish once the outbox empties.
    startWrite_();
    bool finishNow;
    {
        std::lock_guard<std::mutex> lock(m_);
        finishNow = finishPending_ && !writing_ && outbox_.empty();
    }
    if (finishNow) {
        Finish(grpc::Status::OK);
    }
}

void SubscriberReactor::OnDone() {
    if (initialised_) {
        service_.unregisterSubscriber(key_);
    }
}

void SubscriberReactor::enqueue(autoDiscovery::ParticipantData&& frame) {
    bool needWrite = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        outbox_.push_back(std::move(frame));
        needWrite = !writing_;
    }
    if (needWrite) {
        startWrite_();
    }
}

void SubscriberReactor::startWrite_() {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (writing_ || outbox_.empty()) {
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

bool ParticipantStreamServiceImpl::pushData(const SubscriberKey& key,
                                            autoDiscovery::ParticipantData&& frame) {
    // Holding subsMutex_ across the enqueue guarantees the reactor stays alive
    // for the whole push: OnDone's unregisterSubscriber blocks on the same
    // mutex, and gRPC deletes the reactor only after OnDone returns.
    std::lock_guard<std::mutex> lock(subsMutex_);
    const auto it = subscribers_.find(key);
    if (it == subscribers_.end()) {
        return false;
    }
    it->second->enqueue(std::move(frame));
    return true;
}

void ParticipantStreamServiceImpl::registerSubscriber(const SubscriberKey& key,
                                                      SubscriberReactor* reactor) {
    std::lock_guard<std::mutex> lock(subsMutex_);
    subscribers_[key] = reactor;
}

void ParticipantStreamServiceImpl::unregisterSubscriber(const SubscriberKey& key) {
    std::lock_guard<std::mutex> lock(subsMutex_);
    subscribers_.erase(key);
}

bool ParticipantStreamServiceImpl::hasSubscriber(const SubscriberKey& key) const {
    std::lock_guard<std::mutex> lock(subsMutex_);
    return subscribers_.count(key) != 0;
}

}  // namespace edriel
