/**
 * @file publisher.cpp
 * @brief Example Edriel publisher using a user-defined .proto message type.
 *
 * Registers the "telemetry" topic carrying robot::Telemetry messages and
 * publishes one update per second on the multicast group.
 */

#include <asio.hpp>
#include <iostream>

#include "Edriel.hpp"
#include "robot.pb.h"

int main()
{
    asio::io_context io;
    edriel::Edriel edriel(io);

    if (!edriel.registerPublisherTopic<robot::Telemetry>("telemetry")) {
        std::cerr << "[example-pub] failed to register publisher topic\n";
        return 1;
    }

    // Publish one telemetry frame per second.
    asio::steady_timer publishTimer(io);
    std::function<void(const asio::error_code&)> publishLoop =
        [&](const asio::error_code& ec) {
            if (ec) {
                return;
            }

            robot::Telemetry telemetry;
            telemetry.set_node_id(42);
            telemetry.set_battery_voltage(11.4);
            telemetry.set_wheel_rpm(1830.0);

            if (edriel.sendMessage("telemetry", telemetry)) {
                std::cout << "[example-pub] published telemetry\n";
            } else {
                std::cerr << "[example-pub] publish failed\n";
            }

            publishTimer.expires_after(std::chrono::seconds(1));
            publishTimer.async_wait(publishLoop);
        };

    publishTimer.async_wait(publishLoop);
    edriel.startAutoDiscovery();
    io.run();
    return 0;
}
