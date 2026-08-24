/**
 * @file EdrielGrpcService.cpp
 * @brief Implementation of ParticipantStreamServiceImpl.
 */

#include "EdrielGrpcService.hpp"
#include "Edriel.hpp"

namespace edriel {

ParticipantStreamServiceImpl::ParticipantStreamServiceImpl(Edriel& owner)
    : owner_(owner)
{}

grpc::Status ParticipantStreamServiceImpl::GetParticipantInfo(
    grpc::ServerContext* /*context*/,
    const autoDiscovery::ParticipantHeartbeat* request,
    autoDiscovery::ParticipantData* response) {
    if (request == nullptr || response == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "null request/response");
    }

    if (!owner_.lookupParticipantData(request->pid(), request->tid(), request->uid(),
                                      *response)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "unknown participant");
    }
    return grpc::Status::OK;
}

grpc::Status ParticipantStreamServiceImpl::StreamParticipants(
    grpc::ServerContext* /*context*/,
    grpc::ServerReaderWriter<autoDiscovery::ParticipantData,
                             autoDiscovery::ParticipantHeartbeat>* stream) {
    if (stream == nullptr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "null stream");
    }

    // Push current registry presence so a freshly-dialed subscriber learns the
    // peers it can reach (discovery presence first). Reliable payload frames
    // are wired on top of this stream in the M4 send path.
    const std::vector<autoDiscovery::ParticipantData> presence =
        owner_.snapshotParticipantData();
    for (const auto& pd : presence) {
        if (!stream->Write(pd)) {
            // Client went away (or gRPC flow control rejected the write).
            return grpc::Status(grpc::StatusCode::CANCELLED, "client disconnected");
        }
    }

    // Drain the subscriber's ParticipantHeartbeat stream (identity + keepalive)
    // until the client half-closes, then finish the RPC cleanly.
    autoDiscovery::ParticipantHeartbeat heartbeat;
    while (stream->Read(&heartbeat)) {
        // M4 registers the dialing subscriber here and routes reliable payloads
        // to it; M3 merely holds the stream open for the connection.
    }
    return grpc::Status::OK;
}

}  // namespace edriel
