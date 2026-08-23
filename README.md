# Edriel

C++20 multicast networking library with automatic participant discovery and
typed publish/subscribe messaging. Participants find each other on the local
network via UDP multicast heartbeats, then exchange protobuf messages on named
topics — you define the message types in your own `.proto` files.

## Prerequisites

- CMake >= 3.15
- Conan 2.x
- GCC or MSVC with C++20 support
- protoc (installed automatically as a Conan dependency)

## Build

```bash
# 1. Install dependencies and generate the toolchain
conan install . -of=build --build=missing            # Release
conan install . -s build_type=Debug -of=build --build=missing   # Debug

# 2. Configure + build
cmake --preset conan-release && cmake --build --preset conan-release    # Linux
cmake --preset conan-default  && cmake --build --preset conan-default   # Windows
```

Run the demo binary:

```bash
./build/Release/Edriel/Edriel        # Linux
./build/Release/Edriel/Edriel.exe    # Windows
```

Run tests:

```bash
ctest --test-dir build/Release/Edriel/test --output-on-failure
```

Run benchmarks (latency + throughput over multicast loopback):

```bash
./build/Release/Edriel/test/benchmark
```

## Benchmark baseline

Measured with `Edriel/test/benchmark.cpp` on Ubuntu 24.04, g++ 13.3, Release
(`-O2`), single node publishing to itself over multicast loopback
(239.255.0.1:30002), 2026-08:

| Metric                          | Value                          |
|---------------------------------|--------------------------------|
| publish→callback latency p50    | ~20–28 µs                      |
| publish→callback latency p99    | ~80–120 µs                     |
| publish→callback latency max    | ~0.1–0.4 ms                    |
| throughput, sent (256B payload) | ~100–115k msgs/s               |
| throughput, received            | ~2–8k msgs/s (loopback, best-effort UDP) |

Latency is measured per message (500 paced samples); throughput is a 2 s
unpaced burst. Received throughput on loopback is limited by the single
receive-completion path on the node's io_context thread — each datagram pays a
protobuf envelope parse plus a payload decode. The receive path is allocation-
and log-free per packet; the per-callback descriptor lookup was hoisted to once
per message.

## Core concepts

- **Participant discovery** — `startAutoDiscovery()` joins the multicast group,
  broadcasts heartbeats every 2 s, and drops participants that stop heartbeating
  for 10 s (the timeout is checked every 5 s).
- **Topics** — a topic is a name plus a protobuf message type. A publisher and
  a subscriber connect only when both name and type match.
- **Messages** — any protobuf message type works; you generate its C++ code
  from your own `.proto` file (see below).

## API overview (`edriel::Edriel`)

```cpp
asio::io_context io;
edriel::Edriel edriel(io);

// Discovery
void startAutoDiscovery();
void stopAutoDiscovery();

// Publishing
bool registerPublisherTopic<T>(const std::string& topicName);
bool unregisterPublisherTopic<T>(const std::string& topicName);
bool sendMessage<T>(const std::string& topicName, const T& message);

// Subscribing
bool registerSubscriberTopic<T>(const std::string& topicName);
bool registerSubscriberTopic<T>(const std::string& topicName,
                                std::function<void(const T&)> callback);
bool unregisterSubscriberTopic<T>(const std::string& topicName);
```

`T` must be a protobuf message type (constrained by the `Topic` concept).
All registration/send functions return `false` on failure.

## Custom messages: using your own `.proto` files

You write your message types yourself; Edriel compiles them into C++ and lets
you use them directly as topics.

### 1. Write a proto file

```protobuf
// proto/robot.proto
syntax = "proto3";
package robot;

message Telemetry {
  uint32 node_id = 1;
  double battery_voltage = 2;
  double wheel_rpm = 3;
}
```

### 2. Generate code with `edriel_add_proto_messages()`

In your `CMakeLists.txt`:

```cmake
include(path/to/Edriel/cmake/EdrielProtoMessages.cmake)

edriel_add_proto_messages(robot_messages
    SRCS ${CMAKE_CURRENT_SOURCE_DIR}/proto/robot.proto
    PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}/proto   # optional, defaults to current source dir
)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE robot_messages)
```

The function runs `protoc` on each `.proto`, builds a static library of the
generated code, and links it against `EdrielLib`. Generated headers are
included by their base name (`robot.pb.h`) or relative to `PROTO_PATH`
(`proto/robot.pb.h`). To import Edriel's own protos from yours, append their
directory to `EDRIEL_PROTO_IMPORT_DIRS` before calling the function.

Note: the target requires `EdrielLib`, so add the Edriel project via
`add_subdirectory()` first.

### 3. Publish and subscribe

```cpp
#include <asio.hpp>
#include "Edriel.hpp"
#include "robot.pb.h"

int main()
{
    asio::io_context io;
    edriel::Edriel edriel(io);

    // Publisher
    edriel.registerPublisherTopic<robot::Telemetry>("telemetry");
    robot::Telemetry t;
    t.set_node_id(42);
    edriel.sendMessage("telemetry", t);

    // Subscriber
    edriel.registerSubscriberTopic<robot::Telemetry>(
        "telemetry",
        [](const robot::Telemetry& msg) {
            std::cout << "node " << msg.node_id() << "\n";
        });

    edriel.startAutoDiscovery();
    io.run();
}
```

## Examples

`examples/` contains a complete working pair built on a user-defined proto
(`examples/proto/robot.proto`):

- `examples/publisher.cpp` — registers a `robot::Telemetry` publisher topic and
  sends one frame per second.
- `examples/subscriber.cpp` — subscribes to the same topic with a typed
  callback that prints every frame.

Build them as part of the normal build and run them in two terminals:

```bash
./build/Release/examples/example_subscriber &
./build/Release/examples/example_publisher
```

## Notes

- Payloads are limited to the ~1500-byte datagram MTU budget per message;
  larger payloads are rejected at send time.
- Messages are delivered best-effort over UDP multicast — no retransmission.
