/**
 * @file EdrielReliableClient.cpp
 * @brief Implementation of ReliableSubscriberConnection.
 */

#include "EdrielReliableClient.hpp"
#include "Edriel.hpp"

#include <chrono>

namespace edriel {

ReliableSubscriberConnection::ReliableSubscriberConnection(
    Edriel& node, SubscriberKey publisher, std::string target,
    std::uint32_t selfPid, std::uint64_t selfTid, std::uint64_t selfUid)
    : node_(node)
    , publisher_(publisher)
    , target_(std::move(target))
    , selfPid_(selfPid)
    , selfTid_(selfTid)
    , selfUid_(selfUid)
{}

ReliableSubscriberConnection::~ReliableSubscriberConnection() {
    stop();
}

void ReliableSubscriberConnection::start() {
    thread_ = std::thread(&ReliableSubscriberConnection::run_, this);
}

void ReliableSubscriberConnection::stop() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(ctxMutex_);
        if (activeCtx_ != nullptr) {
            activeCtx_->TryCancel();
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    // grpc::Channel (grpcpp) is RAII: the stub and channel are released when the
    // dropped shared_ptr members / stub destruct (no Shutdown() in this API).
}

void ReliableSubscriberConnection::run_() {
    channel_ = grpc::CreateChannel(target_, grpc::InsecureChannelCredentials());
    auto stub = autoDiscovery::ParticipantStreamService::NewStub(channel_);

    autoDiscovery::ParticipantHeartbeat identity;
    identity.set_pid(selfPid_);
    identity.set_tid(selfTid_);
    identity.set_uid(selfUid_);

    while (!stop_) {
        auto ctx = std::make_unique<grpc::ClientContext>();
        {
            std::lock_guard<std::mutex> lock(ctxMutex_);
            if (stop_) {
                break;
            }
            activeCtx_ = ctx.get();
        }

        auto stream = stub->StreamParticipants(ctx.get());
        if (!stream->Write(identity)) {
            { std::lock_guard<std::mutex> lock(ctxMutex_); activeCtx_ = nullptr; }
            if (!stop_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));  // backoff
            }
            continue;
        }

        // Keep the write half open (no WritesDone) so the publisher's server
        // keeps the stream alive and pushes frames to us.
        autoDiscovery::ParticipantData frame;
        bool broken = false;
        while (!stop_) {
            if (stream->Read(&frame)) {
                node_.handleReliableDataFrame(frame);
            } else {
                broken = true;  // stream closed / broke / cancelled
                break;
            }
        }
        { std::lock_guard<std::mutex> lock(ctxMutex_); activeCtx_ = nullptr; }

        if (!stop_ && broken) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));  // re-dial
        }
    }
}

}  // namespace edriel