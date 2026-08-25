/**
 * @file EdrielReliableClient.cpp
 * @brief Implementation of ReliableSubscriberConnection.
 */

#include "EdrielReliableClient.hpp"
#include "Edriel.hpp"

#include <chrono>
#include <cstdio>

namespace edriel {

namespace {
/// Per-candidate connect deadline (bounded so a black-holed candidate cannot
/// stall the connect-in-order scan forever). A refused/closed port fails fast.
constexpr std::chrono::milliseconds kConnectTimeout{ 2000 };
/// Per-teardown drain deadline: bound stream->Finish() so a wedged server
/// cannot block stop()/dtor indefinitely on a never-completing RPC.
constexpr std::chrono::milliseconds kFinishTimeout{ 2000 };
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
                // A fresh stream: the reconnect boundary is an explicit loss
                // episode (whatever the prior stream was mid-buffering is gone,
                // with no NACK/replay layer to recover it). Baseline the
                // receiver's reorder windows for this publisher so the first
                // frame of the new stream resumes delivery instead of stalling
                // the window on an unreachable gap.
                node_.noteReliableStreamEstablished(publisher_.uid);

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
                // Drain the RPC to its server-side completion before moving on,
                // on EVERY exit path. A sync ClientReaderWriter abandoned
                // without Finish() only tears down the client half; the server's
                // callback reactor (SubscriberReactor) keeps the call (and its
                // subscriber-table entry) alive until gRPC asynchronously
                // delivers OnCancel/OnDone. Finish() blocks until the server
                // has fully closed the RPC (its OnDone run, entry evicted), so
                // by the time stop() joins this thread the old subscriber entry
                // is gone — otherwise the next re-dial's `waitUntil(connected)`
                // can briefly observe the stale, dying reactor as connected and
                // push a frame into a table that then empties before the fresh
                // generation registers (a dropped-frame race).
                //
                // This must run even when stop_ ended the loop WITHOUT a broken
                // read (e.g. right after a frame was delivered and the loop
                // re-armed): that path used to skip Finish and leave eviction to
                // a lagging async OnCancel, which is exactly the race above.
                ctx->set_deadline(std::chrono::system_clock::now()
                                  + kFinishTimeout);
                // Consume any frames the server is still flushing as it closes,
                // holding the drain until it has really shut the stream down.
                autoDiscovery::ParticipantData scratch;
                while (stream->Read(&scratch)) {}
                stream->Finish();  // drain to server Done; status discarded
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