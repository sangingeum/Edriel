include("${CMAKE_SOURCE_DIR}/cmake/EdrielProtoMessages.cmake")

# Payload types used by test_pubsub and the benchmarks.
edriel_add_proto_messages(test_messages
    SRCS test_messages.proto
    PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(GTest CONFIG REQUIRED)

# House warn bar for project-owned test targets (strict, local to this dir so
# user code that re-includes EdrielLib is unaffected).
set(EDRIEL_STRICT_FLAGS -Wall -Wextra -Wpedantic -Werror)

# test_pubsub: Topic registry / pub-sub data path tests
add_executable(test_pubsub
    test_pubsub.cpp
)

target_compile_options(test_pubsub PRIVATE ${EDRIEL_STRICT_FLAGS})

target_link_libraries(test_pubsub PRIVATE
    EdrielLib
    test_messages
    GTest::gtest_main
)

add_test(NAME TestPubSub COMMAND test_pubsub)

# test_participant: Participant struct tests
add_executable(test_participant
    test_participant.cpp
)

target_compile_options(test_participant PRIVATE ${EDRIEL_STRICT_FLAGS})

target_link_libraries(test_participant PRIVATE
    EdrielLib
    GTest::gtest_main
)

add_test(NAME TestParticipant COMMAND test_participant)

# test_cleaner: Timeout logic tests
add_executable(test_cleaner
    test_cleaner.cpp
)

target_compile_options(test_cleaner PRIVATE ${EDRIEL_STRICT_FLAGS})

target_link_libraries(test_cleaner PRIVATE
    GTest::gtest_main
)

add_test(NAME TestCleaner COMMAND test_cleaner)

# test_magnitude: Magic number validation tests
add_executable(test_magnitude
    test_magnitude.cpp
)

target_compile_options(test_magnitude PRIVATE ${EDRIEL_STRICT_FLAGS})

target_link_libraries(test_magnitude PRIVATE
    EdrielLib
    GTest::gtest_main
)

add_test(NAME TestMagicNumber COMMAND test_magnitude)

# test_integration: Multi-threaded integration tests
add_executable(test_integration
    test_integration.cpp
)

target_compile_options(test_integration PRIVATE ${EDRIEL_STRICT_FLAGS})

target_link_libraries(test_integration PRIVATE
    GTest::gtest_main
)

add_test(NAME TestIntegration COMMAND test_integration)

# test_config: config.yml parsing and per-key fallback
add_executable(test_config
    test_config.cpp
)

target_compile_options(test_config PRIVATE ${EDRIEL_STRICT_FLAGS})

target_link_libraries(test_config PRIVATE
    EdrielLib
    GTest::gtest_main
)

add_test(NAME TestConfig COMMAND test_config)

# test_endpoint: ADR-0002 Endpoint / endpoints / reliable proto wire tests
add_executable(test_endpoint
    test_endpoint.cpp
)

target_compile_options(test_endpoint PRIVATE ${EDRIEL_STRICT_FLAGS})

target_link_libraries(test_endpoint PRIVATE
    EdrielLib
    GTest::gtest_main
)

add_test(NAME TestEndpoint COMMAND test_endpoint)

# benchmark: Performance benchmarks
add_executable(benchmark
    benchmark.cpp
)

target_compile_options(benchmark PRIVATE ${EDRIEL_STRICT_FLAGS})

target_link_libraries(benchmark PRIVATE
    EdrielLib
    EdrielTestMessages
    GTest::gtest_main
)

# benchmark.cpp includes protobuf headers generated into Edriel's binary dir.
target_include_directories(benchmark PRIVATE
    "${CMAKE_BINARY_DIR}"
)
