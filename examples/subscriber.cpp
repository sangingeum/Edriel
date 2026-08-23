/**
 * @file subscriber.cpp
 * @brief Example Edriel subscriber using a user-defined .proto message type.
 *
 * Subscribes to the "telemetry" topic carrying robot::Telemetry messages
 * and prints every update received from the multicast group.
 */

#include <asio.hpp>
#include <iostream>

#include "Edriel.hpp"
#include "robot.pb.h"

int main()
{
    asio::io_context io;
    edriel::Edriel edriel(io);

    if (!edriel.registerSubscriberTopic<robot::Telemetry>(
            "telemetry",
            [](const robot::Telemetry& telemetry) {
                std::cout << "[example-sub] telemetry from node "
                          << telemetry.node_id()
                          << ": battery=" << telemetry.battery_voltage()
                          << "V rpm=" << telemetry.wheel_rpm() << "\n";
            })) {
        std::cerr << "[example-sub] failed to subscribe to topic\n";
        return 1;
    }

    edriel.startAutoDiscovery();
    io.run();
    return 0;
}
