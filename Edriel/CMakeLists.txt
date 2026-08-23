cmake_minimum_required(VERSION 3.8)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(Edriel CXX)

# 1. C++ 표준 설정
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. Conan 패키지 로드 (CONFIG REQUIRED 필수)
find_package(asio CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(asio-grpc CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

# 3. Proto codegen toolchain ---------------------------------------------------
# protoc: prefer the executable target Conan's protobuf package registers via its
# build module (protobuf::protoc). Not all conan/CMakeDeps versions create that
# target, so fall back to find_program. This keeps the codegen independent of
# how CMakeDeps chooses to expose the compiler.
if(TARGET protobuf::protoc)
    get_target_property(EDRIEL_PROTOC_EXEC protobuf::protoc IMPORTED_LOCATION)
endif()
if(NOT EDRIEL_PROTOC_EXEC OR NOT EXISTS "${EDRIEL_PROTOC_EXEC}")
    find_program(EDRIEL_PROTOC_EXEC NAMES protoc)
endif()
if(NOT EDRIEL_PROTOC_EXEC)
    message(FATAL_ERROR "Edriel: protoc executable not found. Install the protobuf "
                        "Conan package or add protoc to PATH.")
endif()

# grpc_cpp_plugin: idem, for the --plugin=... protoc argument.
if(TARGET gRPC::grpc_cpp_plugin)
    get_target_property(EDRIEL_GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION)
endif()
if(NOT EDRIEL_GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "Edriel: gRPC C++ plugin (grpc_cpp_plugin) not found.")
endif()

# 4. EdrielProtoMessages: helper for users adding their own .proto messages
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# 5. Proto file list (excluding test/benchmark protos handled below)
set(PROTO_DIR "${CMAKE_SOURCE_DIR}/Proto")
set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")
file(GLOB TEST_PROTO_FILES "${PROTO_DIR}/*test*.proto" "${PROTO_DIR}/*bench*.proto")
if(TEST_PROTO_FILES)
    list(REMOVE_ITEM PROTO_FILES ${TEST_PROTO_FILES})
endif()

# Test/benchmark messages: compiled the same way but linked only where needed.
set(TEST_PROTO_SRCS)
set(TEST_PROTO_HDRS)
foreach(PROTO_FILE ${TEST_PROTO_FILES})
    get_filename_component(FIL_WE ${PROTO_FILE} NAME_WE)
    list(APPEND TEST_PROTO_SRCS "${OUT_DIR}/${FIL_WE}.pb.cc")
    list(APPEND TEST_PROTO_HDRS "${OUT_DIR}/${FIL_WE}.pb.h")
    add_custom_command(
        OUTPUT "${OUT_DIR}/${FIL_WE}.pb.cc" "${OUT_DIR}/${FIL_WE}.pb.h"
        COMMAND "${EDRIEL_PROTOC_EXEC}"
        ARGS --proto_path=${PROTO_DIR}
             --cpp_out=${OUT_DIR}
             ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Compiling ${FIL_WE}.proto (test/bench) to C++..."
        VERBATIM
    )
endforeach()

add_library(EdrielTestMessages STATIC ${TEST_PROTO_SRCS} ${TEST_PROTO_HDRS})
target_link_libraries(EdrielTestMessages PUBLIC protobuf::libprotobuf)
target_include_directories(EdrielTestMessages PUBLIC "${OUT_DIR}")
target_compile_options(EdrielTestMessages PRIVATE -Wall -Wextra -Wpedantic -Werror)

# 생성될 파일들의 목록을 담을 변수 초기화
set(PROTO_SRCS)
set(PROTO_HDRS)
set(GRPC_SRCS)
set(GRPC_HDRS)

# 6. 각 .proto 파일마다 컴파일 명령어 생성
set(gRPC_CPP_PLUGIN_EXECUTABLE "${EDRIEL_GRPC_CPP_PLUGIN}")
foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(FIL_WE ${PROTO_FILE} NAME_WE)

    set(P_CPP "${OUT_DIR}/${FIL_WE}.pb.cc")
    set(P_HDR "${OUT_DIR}/${FIL_WE}.pb.h")
    set(G_CPP "${OUT_DIR}/${FIL_WE}.grpc.pb.cc")
    set(G_HDR "${OUT_DIR}/${FIL_WE}.grpc.pb.h")

    list(APPEND PROTO_SRCS ${P_CPP})
    list(APPEND PROTO_HDRS ${P_HDR})
    list(APPEND GRPC_SRCS ${G_CPP})
    list(APPEND GRPC_HDRS ${G_HDR})

    add_custom_command(
        OUTPUT ${P_CPP} ${P_HDR} ${G_CPP} ${G_HDR}
        COMMAND "${EDRIEL_PROTOC_EXEC}"
        ARGS --proto_path=${PROTO_DIR}
             --cpp_out=${OUT_DIR}
             --grpc_out=${OUT_DIR}
             --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN_EXECUTABLE}
             ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Compiling ${FIL_WE}.proto to C++ and gRPC..."
        VERBATIM
    )
endforeach()

# 7. Edriel 라이브러리 타겟 (테스트에서도 링크 가능)
file(GLOB EDRIEL_LIB_SRCS "${CMAKE_SOURCE_DIR}/Edriel/*.cpp")
list(REMOVE_ITEM EDRIEL_LIB_SRCS "${CMAKE_SOURCE_DIR}/Edriel/main.cpp")

add_library(EdrielLib STATIC
    ${EDRIEL_LIB_SRCS}
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${GRPC_SRCS}
    ${GRPC_HDRS}
)

target_link_libraries(EdrielLib PUBLIC
    asio::asio
    protobuf::libprotobuf
    gRPC::grpc++
    asio-grpc::asio-grpc
    yaml-cpp::yaml-cpp
    Threads::Threads
)

target_include_directories(EdrielLib PUBLIC
    "${CMAKE_SOURCE_DIR}/Edriel"
    "${OUT_DIR}"
)

# Warnings: target-scoped so user code linking EdrielLib is unaffected.
target_compile_options(EdrielLib PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

# 8. 실행 파일 타겟
add_executable(Edriel
    "${CMAKE_SOURCE_DIR}/Edriel/main.cpp"
)

target_compile_options(Edriel PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(Edriel PRIVATE
    EdrielLib
)

# 9. 테스트 서브디렉토리
enable_testing()
add_subdirectory(test)

# 10. 예제 (사용자 정의 .proto 메시지 사용 예)
add_subdirectory("${CMAKE_SOURCE_DIR}/examples" "${CMAKE_BINARY_DIR}/examples")