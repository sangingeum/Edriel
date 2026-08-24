/**
 * @file EdrielGrpcService.hpp
 * @brief Synchronous gRPC ParticipantStreamService server for the reliable path.
 *
 * Implements the ADR-0002 gRPC plane that rides on `grpc_port`. Every node
 * serves one instance of this service:
 *   - GetParticipantInfo (unary): returns a participant's ParticipantData —
 *     the post-connect verifier/refresher (Channel C in ADR-0002).
 *   - StreamParticipants (bidi): a subscriber dials and receives a server
 *     stream of ParticipantData (presence first; reliable payload frames in
 *     the M4 send path).
 *
 * The service runs on the standard grpcpp synchronous server (part of the
 * already-linked gRPC plumbing). gRPC's own threads invoke the handlers; they
 * marshal registry reads through Edriel's stateMutex-guarded accessors, so the
 * multicast plane on the single io_context is untouched.
 */

#pragma once

#include <grpcpp/grpcpp.h>

#include "autoDiscovery.pb.h"
#include "autoDiscovery_grpc_service.grpc.pb.h"
#include "autoDiscovery_grpc_service.pb.h"

namespace edriel {

class Edriel;

/**
 * @brief gRPC service implementation for participant data streaming.
 */
class ParticipantStreamServiceImpl final
    : public autoDiscovery::ParticipantStreamService::Service {
public:
    /**
     * @brief Constructs the service bound to an owning Edriel.
     * @param owner Edriel whose registry backs the RPCs
     */
    explicit ParticipantStreamServiceImpl(Edriel& owner);

    /**
     * @brief Unary GetParticipantInfo (Channel C verifier/refresher).
     *
     * Returns the participant's ParticipantData (presence + endpoints) or
     * Status::NOT_FOUND when the identity is not in the registry.
     */
    grpc::Status GetParticipantInfo(
        grpc::ServerContext* context,
        const autoDiscovery::ParticipantHeartbeat* request,
        autoDiscovery::ParticipantData* response) override;

    /**
     * @brief Bidi server-side streaming.
     *
     * Reads the dialing subscriber's ParticipantHeartbeat stream (identity +
     * keepalive) and pushes ParticipantData presence downstream. In the M4
     * reliable send path the publisher additionally pushes DataMessage payload
     * frames over this stream. Returns OK when the client half-closes.
     */
    grpc::Status StreamParticipants(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<autoDiscovery::ParticipantData,
                                 autoDiscovery::ParticipantHeartbeat>* stream) override;

private:
    Edriel& owner_;  ///< Owning node; backs registry reads.
};

}  // namespace edriel
