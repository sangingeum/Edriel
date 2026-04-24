find_package(GTest CONFIG REQUIRED)

# test_participant: Participant struct tests
add_executable(test_participant
    test_participant.cpp
)

target_link_libraries(test_participant PRIVATE
    EdrielLib
    GTest::gtest_main
)

add_test(NAME TestParticipant COMMAND test_participant)

# test_cleaner: Timeout logic tests
add_executable(test_cleaner
    test_cleaner.cpp
)

target_link_libraries(test_cleaner PRIVATE
    GTest::gtest_main
)

add_test(NAME TestCleaner COMMAND test_cleaner)

# test_magnitude: Magic number validation tests
add_executable(test_magnitude
    test_magnitude.cpp
)

target_link_libraries(test_magnitude PRIVATE
    EdrielLib
    GTest::gtest_main
)

add_test(NAME TestMagicNumber COMMAND test_magnitude)

# test_integration: Multi-threaded integration tests
add_executable(test_integration
    test_integration.cpp
)

target_link_libraries(test_integration PRIVATE
    GTest::gtest_main
)

add_test(NAME TestIntegration COMMAND test_integration)

# benchmark: Performance benchmarks
add_executable(benchmark
    benchmark.cpp
)

target_link_libraries(benchmark PRIVATE
    GTest::gtest_main
)
