# Test targets
add_executable(test_participant
    test_participant.cpp
)

target_link_libraries(test_participant
    PRIVATE Edriel
    gtest_main
    gtest
    gmock
    protobuf::libprotobuf
)

add_test(NAME TestParticipant COMMAND test_participant)

add_executable(test_topic_manager
    test_topic_manager.cpp
)

target_link_libraries(test_topic_manager
    PRIVATE Edriel
    gtest_main
    gtest
    gmock
    protobuf::libprotobuf
)

add_test(NAME TestTopicManager COMMAND test_topic_manager)

add_executable(test_cleaner
    test_cleaner.cpp
)

target_link_libraries(test_cleaner
    PRIVATE Edriel
    gtest_main
    gtest
    gmock
    protobuf::libprotobuf
)

add_test(NAME TestCleaner COMMAND test_cleaner)

add_executable(test_integration
    test_integration.cpp
)

target_link_libraries(test_integration
    PRIVATE Edriel
    gtest_main
    gtest
    gmock
    protobuf::libprotobuf
    pthread
)

add_test(NAME TestIntegration COMMAND test_integration)

# Benchmark target
add_executable(benchmark
    benchmark.cpp
)

target_link_libraries(benchmark
    PRIVATE Edriel
    gtest_main
    gtest
    pthread
)

# Note: Benchmarks use GoogleTest assertions but can be run with GoogleTest
# Add to CMakeLists.txt if you want to enable benchmark tests
