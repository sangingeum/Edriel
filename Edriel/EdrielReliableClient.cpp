/**
 * @file EdrielReliableClient.cpp
 * @brief Implementation of ReliableSubscriberConnection.
 */

#include "EdrielReliableClient.hpp"
#include "Edriel.hpp"

#include <chrono>

namespace edriel {

namespace {
/// Per-candidate connect deadline (bounded so a black-holed candidate cannot
/// stall the connect-in-order scan forever). A refused/closed port fails fast.
constexpr std::chrono::milliseconds kConnectTimeout{ 2000 };
/// Backoff between full scan passes when every candidate was unreachable.
constexpr std::chrono::milliseconds kScanBackoff{ 200 };
}  // namespace

ReliableSubscriberConnection::ReliableSubscriberConnection(
    Edriel& node, SubscriberKey publisher, std::vector<std::string> candidates,
    std::uint32_t selfPid, std::uint64_t selfTid, std::uint64_t selfUid)
    : node_(node)
    , publisher_(publisher)
    , candidates_(std::move(candidates))
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

std::string ReliableSubscriberConnection::currentEndpoint() const {
    std::lock_guard<std::mutex> lock(connectedMutex_);
    return connected_;
}

void ReliableSubscriberConnection::run_() {
    while (!stop_) {
        // Multi-homed connect-in-order (ADR-0002 §6.3): dial each candidate in
        // order; on connect failure advance to the next; the first candidate
        // that reaches the READY channel state wins.
        for (const std::string& target : candidates_) {
            if (stop_) {
                break;
            }

            auto channel =
                grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
            if (!channel->WaitForConnected(std::chrono::system_clock::now()
                                           + kConnectTimeout)) {
                continue;  // this candidate unreachable -> try the next
            }
            {
                std::lock_guard<std::mutex> lock(connectedMutex_);
                connected_ = target;
            }

            auto stub = autoDiscovery::ParticipantStreamService::NewStub(channel);

            autoDiscovery::ParticipantHeartbeat identity;
            identity.set_pid(selfPid_);
            identity.set_tid(selfTid_);
            identity.set_uid(selfUid_);

            // Keep the write half open (no WritesDone) so the publisher's server
            // keeps the stream alive and pushes frames to us.
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
                    {
                        std::lock_guard<std::mutex> lock(ctxMutex_);
                        activeCtx_ = nullptr;
                    }
                    break;  // couldn't open a stream here -> advance candidate
                }

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
                {
                    std::lock_guard<std::mutex> lock(ctxMutex_);
                    activeCtx_ = nullptr;
                }
                if (!stop_ && broken) {
                    break;  // stream dropped -> advance to the next candidate
                }
            }

            {
                std::lock_guard<std::mutex> lock(connectedMutex_);
                connected_.clear();
            }
            if (stop_) {
                break;
            }
            continue;  // try the next candidate in the list
        }

        // The pass ended without a still-live stream (all candidates failed, or
        // the last stream broke). Back off slightly before the next scan so a
        // down/anti-spoofing peer is not hot-polled; reconciliation separately
        // wakes us up on endpoint changes.
        if (!stop_) {
            std::this_thread::sleep_for(kScanBackoff);
        }
    }
}

}  // namespace edriel