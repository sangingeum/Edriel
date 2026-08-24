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
        // The client half-closed its side. Finish cleanly once no write is in
        // flight (defer to OnWriteDone if one is).
        bool canFinish = false;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (writing_) {
                finishPending_ = true;
            } else {
                canFinish = true;
            }
        }
        if (canFinish) {
            Finish(grpc::Status::OK);
        }
        return;
    }

    // A heartbeat arrived. Capture the dialing subscriber's identity once and
    // push current registry presence downstream.
    if (!initialised_) {
        initialised_ = true;
        key_ = SubscriberKey{heartbeat_.pid(), heartbeat_.tid(), heartbeat_.uid()};
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
    bool finishNow;
    {
        std::lock_guard<std::mutex> lock(m_);
        writing_ = false;
        finishNow = finishPending_;  // client half-closed while we were writing
    }
    if (finishNow || !ok) {
        Finish(ok ? grpc::Status::OK
                  : grpc::Status(grpc::StatusCode::CANCELLED, "stream broken"));
        return;
    }
    // Drain any further queued frames.
    startWrite_();
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
        if (writing_ || outbox_.empty() || finishPending_) {
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
