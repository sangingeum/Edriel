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
./build/Release/benchmark/benchmark
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

# ADR-003 sharded SPSC receive pipeline (best-effort multicast receive path)
receiver_threads: 1     # socket-draining threads (keep 1; see table)
worker_threads: 4       # shard/ring/registry-shard workers N (the real lever)
rx_ring_slots: 4096     # slots per worker's bounded SPSC ring (power of two)
so_rcvbuf_bytes: 1048576  # SO_RCVBUF on the multicast socket; 0 = OS default
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
| `receiver_threads` | integer in `1..4` (ADR-003 keeps 1; multi-queue NIC only) | `1` |
| `worker_threads` | integer in `1..16` | `4` |
| `rx_ring_slots` | power of two | `4096` |
| `so_rcvbuf_bytes` | integer in `0..1<<30` (`0` = OS default) | `0` |

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

### Receive path: ADR-003 sharded SPSC pipeline

The best-effort multicast *receive* path is a sharded pipeline (ADR-0003). A
single socket-drainer thread (`receiver_threads`, default `1`) copies raw
datagrams off the UDP socket and fans them into `worker_threads` (= `N`)
bounded single-producer/single-consumer rings; each worker owns exactly one
ring plus a shard of the participant/topic registry guarded by its own mutex
(no global lock on the hot path). Frames are pinned to a worker by hashing the
topic name, so all frames for one topic run in order on one worker while
different topics dispatch in parallel. Overflow drops the *oldest* ring slot
and is counted (never silent), and kernel `SO_RXQ_OVFL` overruns are folded
into the same observable counter — so end-to-end loss is always visible via
`droppedFrames()`. `worker_threads=1` reproduces the exact pre-ADR-003
single-threaded ordering for callers that need it. The reliable gRPC path
(ADR-0002) is unchanged and shares the sharded registry by the same topic
hash.

## Benchmark baseline

Measured with `benchmark/benchmark.cpp` on Ubuntu 24.04, g++ 13.3, Release
(`-O2`), over multicast loopback (239.255.0.1:30002), 2026-08, at HEAD
`370b5cb` with `worker_threads=4`. The single-node latency number is the
publish → callback round trip (500 paced samples); the receive figures come
from the decoupled two-node harness:

| Metric                          | Value                          |
|---------------------------------|--------------------------------|
| publish→callback latency p50    | ~27 µs                         |
| publish→callback latency p99    | ~80 µs                         |
| publish→callback latency max    | ~0.1 ms                        |
| Consumer true max received (N=4)| ~527k msgs/s (fresh runs 509k / 527k / 540k) |
| Hard gate (1.5 × ~118k baseline)| ≥ 177k msgs/s — passed in every run |

### True unpushed receive max (N=4, ADR-003)

Measured with `Benchmark.TwoNodeReceiveThroughput` in `benchmark/benchmark.cpp`
(worker_threads=4, 4 shard-distinct topics over multicast loopback, 256 B
payload). This harness *floods* the consumer unpaced — four producers send as
fast as their strands and the loopback wire will carry (~2.2–2.5M frames/s
offered to the consumer socket), so the consumer is genuinely saturated on
every run and the delivered figure is its TRUE unpushed ceiling, not a
producer-paced target.

| Metric (N=4, 2026-08, fresh run @ 370b5cb) | Value                                   |
|--------------------------------------|-----------------------------------------|
| Consumer true max received           | ~527k msgs/s (best 1s delivered window; fresh runs 509k / 527k / 540k) |
| Producer send max (offered)          | ~2.2–2.5M msgs/s (4 producers, flood to loopback) |
| Loss at that operating point         | ~30–40% — a genuinely saturated consumer drops whatever exceeds its ceiling |
| Hard gate (1.5 × ~118k baseline)     | ≥ 177k msgs/s — passed in every run     |

The old keep-pace figure (~211k msgs/s) *paced* the producers at a cold wire
target and reported the consumer keeping up; it under-stated the unpushed
ceiling. The consumer's true unpushed max under a hard flood is ~490–540k
msgs/s (best one-second window; median ~527k across fresh runs). The number
is what a fresh run of the benchmark prints as `CONSUMER TRUE MAX`, so this
README value reproduces directly. Reproduce with:

```bash
./build/Release/benchmark/benchmark --gtest_filter=Benchmark.TwoNodeReceiveThroughput
```

## Reliable-QoS benchmark baseline

Measured with `benchmark/benchmark_reliable.cpp` on the same machine (Ubuntu
24.04, g++ 13.3, Release `-O2`), 2026-08, hermetically: each
publisher/subscriber pair is wired via cross-injected discovery adverts and a
subscriber-initiated gRPC dial over loopback TCP (the
`test_reliable.cpp` pattern) — no multicast, no live network. Payloads are
256 B unless noted. All four benchmarks are standalone gTest binaries and are
NOT part of `ctest`. Fresh back-to-back runs:

| Metric (fresh runs, 2 consecutive)   | Run 1                              | Run 2                              |
|--------------------------------------|------------------------------------|------------------------------------|
| B1 publish→callback latency (128 B)  | p50 71.7 µs, p90 104 µs, p99 149 µs, max 224 µs | p50 68.2 µs, p90 102 µs, p99 187 µs, max 837 µs |
| B2 sustained absorb rate (256 B)     | 934k msgs/s sent = received (0% drop, absorb-limited by the lag backstop) | 1.00M msgs/s sent = received (0% drop, absorb-limited) |
| B3 fan-out, 1 pub → 8 subs (256 B)   | 1.39M msgs/s aggregate, 174,741 delivered per sub (exactly-once, all 8 fed) | 1.51M msgs/s aggregate, 188,947 delivered per sub |
| B4 unpaced flood ceiling (256 B)     | stopped by the 50k-frame lag cap; delivered = pushed (exactly-once held) | same |

Practical readings:

- **Latency**: reliable p50 ~70 µs is roughly 2.5× the best-effort multicast
  p50 (~27 µs); p99 stays well under 0.2 ms. TCP loopback + gRPC CQ hop, not
  a defect.
- **Throughput / flood ceiling**: the reliable path has NO backpressure
  signal today — `SubscriberReactor::outbox_` (publisher side,
  `EdrielGrpcService.cpp`) is unbounded, so an unpaced caller at ~930k–1M
  msgs/s simply buffers whatever the stream cannot write (~340k msgs/s
  write-out observed in early runs, growing memory). The benchmarks cap the
  offered stream with a 50k-frame lag backstop so runs stay hermetic; until
  a bounded outbox + real backpressure exists, treat "reliable" as
  "lossless up to unbounded publisher-side buffering", and the meaningful
  sustained ceiling is the STREAM WRITE-OUT rate (~340k msgs/s measured
  without the backstop), not the offered rate. Known issue; re-base B2's
  `kBaselineAbsorbMsgsPerSec` (90k with backstop headroom) once fixed.
- **Exactly-once** holds in every mode: delivered ≤ pushed everywhere,
  fan-out delivered identical counts per subscriber, flood delivered ==
  pushed under the cap.

Reproduce with:

```bash
./build/Release/benchmark/benchmark_reliable --gtest_filter='ReliableBenchmark.*'
```

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
