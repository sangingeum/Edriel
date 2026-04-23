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

# 3. 경로 설정
set(PROTO_DIR "${CMAKE_SOURCE_DIR}/Proto")
set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# 4. Proto 파일 목록
file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

# 생성될 파일들의 목록을 담을 변수 초기화
set(PROTO_SRCS)
set(PROTO_HDRS)
set(GRPC_SRCS)
set(GRPC_HDRS)

# Conan이 제공하는 gRPC 플러그인 실행 파일 경로 가져오기
set(gRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

# 5. 각 .proto 파일마다 컴파일 명령어 생성
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
        COMMAND protobuf::protoc
        ARGS --proto_path=${PROTO_DIR}
             --cpp_out=${OUT_DIR}
             --grpc_out=${OUT_DIR}
             --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN_EXECUTABLE}
             ${PROTO_FILE}
        DEPENDS ${PROTO_FILE} protobuf::protoc gRPC::grpc_cpp_plugin
        COMMENT "Compiling ${FIL_WE}.proto to C++ and gRPC..."
        VERBATIM
    )
endforeach()

# 6. Edriel 라이브러리 타겟 (테스트에서도 링크 가능)
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
)

target_include_directories(EdrielLib PUBLIC
    "${CMAKE_SOURCE_DIR}/Edriel"
    "${OUT_DIR}"
)

# 7. 실행 파일 타겟
add_executable(Edriel
    "${CMAKE_SOURCE_DIR}/Edriel/main.cpp"
)

target_link_libraries(Edriel PRIVATE EdrielLib)

# 8. 테스트 서브디렉토리
add_subdirectory(test)