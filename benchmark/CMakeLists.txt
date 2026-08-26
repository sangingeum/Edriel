# Edriel benchmarks: standalone latency/throughput harness.
#
# Separate from the unit-test tree on purpose:
#   - Own top-level source dir (benchmark/) sibling to examples/ and Edriel/.
#   - Builds a standalone 'benchmark' executable (not registered under CTest,
#     so it does not contribute to the default `ctest` run).
#   - Compiles benchmark.proto (package benchmark, message Ping) via the same
#     edriel_add_proto_messages() helper the examples use.
#
# Run it explicitly after a build: ./<build>/benchmark/benchmark
#   (from the repo root so config.yml -> worker_threads=4 is picked up)

include("${CMAKE_SOURCE_DIR}/cmake/EdrielProtoMessages.cmake")

# benchmark.proto lives in this dir (package benchmark::Ping).
edriel_add_proto_messages(benchmark_messages
    SRCS benchmark.proto
    PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(GTest CONFIG REQUIRED)

add_executable(benchmark
    benchmark.cpp
)

# House warn bar is mandatory for the standalone benchmark target.
target_compile_options(benchmark PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(benchmark PRIVATE
    EdrielLib
    benchmark_messages
    GTest::gtest_main
)

# Intentionally NOT registered with add_test(): the benchmark is a manual/perf
# tool, not part of the unit/integration test suite.