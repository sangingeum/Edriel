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

## Configuration (`config.yml`)

The auto-discovery endpoint — the UDP port, the multicast group address — and
its cadence (heartbeat send interval, participant aliveness timeout) are read
from a `config.yml` at the repository root (the process working directory). All
keys are optional; every value is validated strictly per-key, and a value that
is missing, malformed, or out of range falls back silently to the built-in
default rather than failing startup.

```yaml
port: 30002
multicast_ip: 239.255.0.1
discovery_period_seconds: 2
participant_timeout_seconds: 10
grpc_port: 4000
advertise_address:      # optional scalar or list; empty = discover-only
  # - 192.168.1.5
max_advertised_endpoints: 4
peers:                  # optional static endpoints for multicast-blind nodes
  # - 192.168.1.5:4000
```

| Key | Valid range | Falls back to |
|-----|-------------|---------------|
| `port` | integer in `1..65535` | `30002` |
| `multicast_ip` | IPv4 multicast `224.0.0.0` .. `239.255.255.255` | `239.255.0.1` |
| `discovery_period_seconds` | integer seconds in `1..86400` | `2` |
| `participant_timeout_seconds` | integer seconds in `1..86400` | `10` |
| `grpc_port` | integer in `1..65535` | `4000` |
| `advertise_address` | scalar or list of non-empty strings (IP/hostname); empty/absent = discover-only | *empty list* |
| `peers` | scalar or list of `address:port` (or bare host → `grpc_port`) static seeds for multicast-blind subscribers | *empty list* |
| `max_advertised_endpoints` | whole number, capped at `64` | `4` |

The `grpc_port` / `advertise_address` / `max_advertised_endpoints` / `peers` keys
drive the reliable path (ADR-0002). Every node runs one gRPC server on `grpc_port`
and advertises its unicast endpoints (`advertise_address`, plus any auto-discovered
interfaces, capped at `max_advertised_endpoints`) on the multicast heartbeat —
so peers can open reliable streams as soon as they are discovered. `peers` is
the static Channel D seed: a multicast-blind / cross-subnet subscriber that
cannot hear the group dials the configured peer endpoints directly instead. See
*Reliable QoS* below.

A missing, unreadable, or malformed `config.yml` behaves exactly like an
invalid value: the defaults are used and no exception escapes. Because
validation is per-key, a valid `port` is honored even when `multicast_ip` is
bad (and vice-versa). YAML is parsed with yaml-cpp (added to the Conan
dependencies).

The participant cleanup pass isn't independently configurable: it is derived
from the participant timeout so the two stay in step. Given a timeout of `T`
seconds, the cleaner runs every `max(T/2, 1)` seconds — a 10 s timeout cleans
every 5 s, matching the historical 5 s / 10 s ratio.

The library constructor reads the file automatically:

```cpp
asio::io_context io;
edriel::Edriel edriel(io);              // loads config.yml (or the defaults)
edriel::Edriel edrielCfg(io, config);   // explicit edriel::Config
```

`edriel::Config` has `port`, `multicastAddress`, `discoverySendPeriod`
(`std::chrono::seconds`), `participantTimeout` (`std::chrono::seconds`),
`grpcPort`, `advertiseAddresses` (`std::vector<std::string>`),
`peerEndpoints` (`std::vector<std::string>`, the `peers:` Channel D seed),
`maxAdvertisedEndpoints`, and a diagnostic `fellBackToDefaults` flag. The
explicit-constructor overload lets a host application supply validated settings
without a config file being present.

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
  broadcasts heartbeats every `discovery_period_seconds` (default 2 s), and
  drops participants that stop heartbeating for `participant_timeout_seconds`
  (default 10 s; the timeout is checked every `max(timeout/2, 1)` seconds).
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

## Reliable QoS (ADR-0002)

By default topics are **best-effort**: `sendMessage()` writes one multicast
datagram and makes no delivery guarantee. A topic opted into **reliable** QoS
instead carries its traffic over a gRPC unicast path between each subscriber
and the publisher — ordered, exactly-once per (publisher, topic), and
backpressured to the data source.

Opt a topic in by passing `reliable = true` (default `false`) when
registering:

```cpp
// Publisher side: this node serves "status" reliably.
edriel.registerPublisherTopic<robot::Telemetry>("status", /*reliable=*/true);

// Subscriber side: this node dials the publisher(s) of "status" and receives
// exactly-once, in-order frames delivered to the callback.
edriel.registerSubscriberTopic<robot::Telemetry>(
    "status",
    [](const robot::Telemetry& msg) { /* ... */ },
    /*reliable=*/true);
```

All other topics stay on the multicast path exactly as before. A best-effort
topic and a reliable topic with the same name/type are distinct in the
registry, so the two QoS classes do not interfere.

How it works (subscriber-initiated, per ADR-0002):

- Every node runs **one small gRPC `ParticipantStreamService`** on `grpc_port`
  (default 4000). It serves the topics *it* publishes reliably.
- Each node *also* dials the publishers of topics it subscribes to with
  `reliable = true`, opening a bidi `StreamParticipants` stream per publisher.
  On that stream the publisher pushes `ParticipantData` frames whose
  `reliable_data` carries one serialized `DataMessage` per sent message,
  stamped with a per-(publisher, topic) `tid`.
- The subscriber keeps a bounded reorder/dedup window per (publisher, topic),
  delivering each distinct `tid` exactly-once in ascending order.
- Dialing endpoints come from the multicast heartbeat (`Identifier.endpoints`,
  Channel A), refreshed every heartbeat; a publisher that times out is dropped
  and its stream torn down. `GetParticipantInfo` (unary) is available as a
  post-connect verifier/refresher (Channel C).

The reliable path honors the same ~1500-byte payload MTU budget as
best-effort; larger payloads are rejected at send time (fragmentation is
deferred — split large reliable messages app-side).

Related config keys are documented above (`grpc_port`,
`advertise_address`, `max_advertised_endpoints`).

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
