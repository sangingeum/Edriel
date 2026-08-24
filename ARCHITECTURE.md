# Edriel Architecture Documentation

## System Overview

Edriel is a C++20 multi-cast auto-discovery networking library built on ASIO with gRPC and Protobuf integration. The system enables network participants to auto-discover each other, register topics, and exchange messages via multicast broadcasting.

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Edriel Runtime                             │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐   │
│  │ Discovery    │  │ Topic        │  │ Participant           │   │
│  │   Module     │  │ Management   │  │   Management          │   │
│  └──────┬───────┘  └──────┬───────┘  └──────────┬───────────┘   │
│         │                 │                      │               │
│  ┌──────┴───────┐  ┌──────┴───────┐  ┌──────────┴───────────┐   │
│  │ Multicast    │  │ Topic Registry │  │ Participant Registry │   │
│  │ Socket       │  │ Map           │  │ Set                  │   │
│  └──────────────┘  └───────────────┘  └──────────────────────┘   │
│         │                 │                      │               │
│  ┌──────┴───────┐  ┌──────┴───────┘  ┌──────────┴───────────┐   │
│  │   sendMessage │  │  handleTopic │  │  handleHeartbeat      │   │
│  │   (broadcast) │  │   Announce   │  │  (update/timeout)     │   │
│  └──────────────┘  └───────────────┘  └──────────────────────┘   │
│                                                                    │
│  ┌───────────────────────────────────────────────────────────┐   │
│  │                      gRPC Service                          │   │
│  │  StreamParticipants()  ──────→  Participant Heartbeats    │   │
│  │  GetParticipantInfo() ──────→  Participant Queries         │   │
│  └───────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

## Component Descriptions

### 1. Discovery Module (`modules/discovery.hpp`)

Handles multicast-based participant auto-discovery:

**Responsibilities:**
- Send periodic discovery heartbeats to multicast group
- Receive discovery packets from other participants
- Parse and extract participant information
- Manage multicast socket lifecycle
- Bind/join the multicast group using the resolved (configured) port and address

**Key Methods:**
- `sendDiscoveryPacket()`: Serialize and broadcast discovery message
- `receiveDiscoveryPacket()`: Listen for incoming packets
- `parseHeartbeat()`: Extract participant metadata

The multicast group it binds to is not hardcoded: the endpoint is derived from
the runtime `Config` (see §6), which itself is populated from `config.yml`
(`port` / `multicast_ip`) with a fallback to defaults when invalid.

### 2. Topic Management Module (`modules/topic_manager.hpp`)

Manages topic registration and message routing:

**Data Structures:**
- `TopicRegistry`: `std::unordered_map<std::string, std::set<Participant>>`
  - Maps topic name to set of publishing/subscribing participants
  - Supports both publishers and subscribers per topic

**Key Methods:**
- `registerPublisherTopic()`: Add topic to registry as publisher
- `registerSubscriberTopic()`: Add topic to registry as subscriber
- `unregisterPublisherTopic()`: Remove topic from publisher registry
- `unregisterSubscriberTopic()`: Remove topic from subscriber registry
- `notifySubscribers(topic, message)`: Deliver message to topic subscribers

**Topic Key:** `topicName + messageType` (e.g., "temperature+update")

### 3. Participant Management Module (`modules/participant_manager.hpp`)

Tracks participant lifecycle and heartbeats:

**Participant States:**
- `UNKNOWN`: Initial state
- `JOINING`: Participant sending discovery packets
- `ONLINE`: Active participant with recent heartbeat
- `OFFLINE`: Detected but not timed out
- `TIMEDOUT`: Heartbeat timeout exceeded

**Key Methods:**
- `registerParticipant()`: Add new participant to registry
- `unregisterParticipant()`: Remove timed-out participant
- `updateParticipant()`: Update heartbeat timestamp
- `shouldBeRemoved()`: Check if timeout exceeded
- `updateLastSeen()`: Reset timeout timer

**Data Structure:**
```cpp
struct Participant {
    unsigned long pid;       // Participant ID
    uint64_t tid;           // Transaction ID (sequence)
    uint64_t uid;           // Unique identifier
    std::chrono::steady_clock::time_point lastSeen;
    std::set<TopicInfo> publishedTopics;
    std::set<TopicInfo> subscribedTopics;
    std::chrono::seconds timeout; // aliveness timeout, from Config
    // ... operators for set containment ...
};
```

### 4. gRPC Streaming Service

Provides efficient participant data exchange:

**Service Definition:** `autoDiscovery_grpc_service.proto`

```protobuf
service ParticipantStreamService {
  // Server-side streaming: continuous heartbeat data
  rpc StreamParticipants(stream ParticipantHeartbeat) 
      returns (stream ParticipantData);
  
  // Unary: on-demand participant queries
  rpc GetParticipantInfo(ParticipantHeartbeat) 
      returns (ParticipantData);
}
```

**Implementation:** `autoDiscovery_grpc_service.cpp`

**Features:**
- Server-side streaming of participant heartbeats
- Initial response with registered participants
- Periodic polling for updates (100ms interval)
- Connection liveness pings
- Timeout handling (5s connection deadline)

### 5. Message Sender (`sendMessage()`)

Broadcasts messages to multicast group:

**Flow:**
1. Serialize message with magic number prepended
2. Set multicast destination (config-derived port/multicast address)
3. Async send via ASIO
4. Notify topic subscribers

**Signature:**
```cpp
template<typename Topic> requires Topic
bool sendMessage(const std::string& topicName, const Topic& message);
```

### 6. Configuration Module (`EdrielConfig.hpp` / `EdrielConfig.cpp`)

Loads and validates the runtime settings that previously were hardcoded
(port `30002`, multicast group `239.255.0.1`, heartbeat send interval `2s`,
participant aliveness timeout `10s`):

**Responsibilities:**
- Parse `config.yml` via yaml-cpp (`port`, `multicast_ip`,
  `discovery_period_seconds`, `participant_timeout_seconds` keys)
- Validate each key independently and strictly
- Fall back to a key's historical default on any missing/invalid/unparseable value
- Report (via `fellBackToDefaults`) when a default was substituted, for diagnostics
- Keep parsing in its own translation unit so validation is testable without an `io_context`

**Key Methods:**
- `parsePort(value, fallback)`: strict decimal integer in `[1, 65535]`, else `fallback`
- `parseMulticastAddress(value, fallback)`: strict dotted-quad IPv4 in `224.0.0.0..239.255.255.255`, else `fallback`
- `parseDurationSeconds(value, fallback)`: strict positive integer seconds in
  `[1, 86400]` (sane upper bound, 24 h), else `fallback`
- `loadConfig(configPath = "config.yml")`: returns a `Config`; never throws on config content

**Fallback Behavior:** each key is resolved independently — missing, malformed,
or out-of-range values keep that key's default (`port` → `30002`,
`multicastAddress` → `239.255.0.1`, `discoverySendPeriod` → `2s`,
`participantTimeout` → `10s`), and the `fellBackToDefaults` flag is set.
The default `Edriel` constructor calls `loadConfig()`; the resolved values drive
`multicastEndpoint` / `receiverEndpoint`, the multicast `join_group` call, and
the discovery send + cleanup timer cadences.

## Data Flow

### Discovery Flow
```
0. Load config.yml and validate (per-key), falling back to defaults on invalid
1. Timer fires (every config discovery send period; default 2s)
2. Create discovery message with own participant info
3. Serialize to packet + prepend magic number
4. Broadcast to multicast address (config-derived)
5. Wait for responses
6. Parse responses and register new participants
7. Update participant heartbeats
8. Remove timed-out participants (cleanup cadence = timeout/2; default every 5s)
```

### Message Delivery Flow
```
1. User calls sendMessage(topic, message)
2. Serialize message
3. Prepend magic number
4. Lookup subscribers for topic
5. Create multicast packet
6. Async_send_to multicast socket
7. Return on completion
```

### gRPC Streaming Flow
```
Client: Send initial heartbeats (request stream)
Server:  Send initial participant data (response)
Server:  Poll for updates (100ms intervals)
Server:  Send updated heartbeats
Client:  Receive continuous participant data
Server:  Send final status on close
```

## Configuration Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `discoveryPort` (config.yml `port`) | 30002 | Multicast port — overridable, falls back on invalid |
| `multicastAddress` (config.yml `multicast_ip`) | 239.255.0.1 | Multicast group address — overridable, falls back on invalid |
| `recvBufferSize` | 1500 | UDP buffer size |
| `discoverySendPeriod` (config.yml `discovery_period_seconds`) | 2s | Discovery heartbeat interval — overridable, falls back on invalid |
| `participantTimeout` (config.yml `participant_timeout_seconds`) | 10s | Participant aliveness timeout — overridable, falls back on invalid |
| cleanup cadence (`max(timeout/2, 1s)`) | 5s | Participant cleanup timer interval (derived from the configured timeout) |
| `magicNumber` | 0xED75E1ED | Packet integrity check |

## CMake Build System

```cmake
project(Edriel CXX_STANDARD 20)

# Conan dependencies
include(conan_newbuild.cmake)
conan_newbuild_setup()
add_subdirectory(${CONAN_HOST_FOLDER} thirdparty)

# Generated protobuf headers
add_library(EdrielEdriel Edriel.cpp main.cpp)
target_link_libraries(EdrielEdriel PRIVATE asio protobuf grpc++)
target_include_directories(EdrielEdriel 
    PRIVATE ${CMAKE_SOURCE_DIR}/Edriel
    PRIVATE ${PROTOBUF_GENERATED_HEADERS_INCLUDE_DIR}
)

# Test suite
add_subdirectory(test)
```

## Memory Layout

```
Edriel Instance
├── asio::io_context& io_context
├── asio::strand strand
├── std::unique_ptr<udp::socket> autoDiscoverySocket
├── std::unique_ptr<steady_timer> autoDiscoverySendTimer
├── std::unique_ptr<steady_timer> autoDiscoveryCleanUpTimer
├── asio::ip::udp::endpoint multicastEndpoint  // config-derived (multicastAddress:port)
├── asio::ip::udp::endpoint receiverEndpoint   // config-derived (any:port)
├── edriel::Config config_                     // parsed runtime config (port + multicast + cadence)
├── autoDiscovery::Message discoveryMessage
├── std::string discoveryPacket
├── std::atomic_bool isRunning
├── std::mutex runnerMutex
├── std::set<Participant> participants
└── Participant selfParticipant_
```

## Performance Considerations

### Multicast Broadcasting
- Uses ASIO's async operations for non-blocking sends
- Single packet reaches all group members
- Magic number ensures packet integrity
- Minimal serialization overhead

### Memory Efficiency
- Fixed-size receive buffers (1500 bytes)
- Participant data in `std::set` with heap allocation
- Topic registry uses hash map for O(1) lookup

### gRPC Efficiency
- Server-side streaming reduces latency
- Initial response provides immediate data
- Periodic polling balances update frequency vs overhead

## Security Considerations

### Magic Number Validation
- All received packets validated against magic number
- Prevents malformed packet injection
- Can be extended with replay attack prevention (timestamp-based)

### Timeout-Based Cleanup
- Participants without heartbeats for 10s are marked for removal
- Cleanup timer runs every 5s
- Prevents memory leaks from stale participants

## Future Enhancements

### QoS Support
- Reliable vs best-effort message modes
- Message priority levels
- Acknowledgment mechanisms

### Advanced Topic Exchange
- Hash-based topic indexing
- LRU caching for topic state
- Topic compression

### Configuration
- Multi-network interface support
- Live config reload without restart (config is currently read once at construction)
