# Edriel examples: user-defined .proto messages used end-to-end.
#
# Demonstrates the edriel_add_proto_messages() helper: the user's own
# examples/proto/robot.proto is compiled with protoc into a library that
# links against EdrielLib, and the generated robot::Telemetry type is
# used directly as a topic in the publisher/subscriber apps.

include("${CMAKE_SOURCE_DIR}/cmake/EdrielProtoMessages.cmake")
edriel_add_proto_messages(robot_messages
    SRCS ${CMAKE_CURRENT_SOURCE_DIR}/proto/robot.proto
    PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}/proto
)

add_executable(example_publisher publisher.cpp)
target_compile_options(example_publisher PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(example_publisher PRIVATE robot_messages)

add_executable(example_subscriber subscriber.cpp)
target_compile_options(example_subscriber PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(example_subscriber PRIVATE robot_messages)
