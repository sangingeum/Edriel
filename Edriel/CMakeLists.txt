# CMakeList.txt: Edriel에 대한 CMake 프로젝트, 여기에 소스를 포함하고
# 프로젝트 특정 논리를 정의합니다.
#
find_package(asio CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

function(grpc_generate_cpp srcs hdrs dest)
    if(NOT ARGN)
        message(SEND_ERROR "Error: grpc_generate_cpp() called without any proto files")
        return()
    endif()

    if(NOT TARGET gRPC::grpc_cpp_plugin)
        message(FATAL_ERROR "gRPC::grpc_cpp_plugin target not found")
    endif()
    
    set(gRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

    get_filename_component(PROTO_ABS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Proto" ABSOLUTE)
    set(_protobuf_include_path -I "${PROTO_ABS_DIR}")

    set(${srcs})
    set(${hdrs})
    foreach(fil ${ARGN})
        get_filename_component(abs_fil ${fil} ABSOLUTE)
        get_filename_component(fil_we ${fil} NAME_WE)
        
        list(APPEND ${srcs} "${dest}/${fil_we}.grpc.pb.cc")
        list(APPEND ${hdrs} "${dest}/${fil_we}.grpc.pb.h")

        add_custom_command(
            OUTPUT "${dest}/${fil_we}.grpc.pb.cc"
                   "${dest}/${fil_we}.grpc.pb.h"
            COMMAND protobuf::protoc
            ARGS --grpc_out=${dest}
                 --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN_EXECUTABLE}
                 ${_protobuf_include_path}
                 ${abs_fil}
            DEPENDS ${abs_fil} protobuf::protoc gRPC::grpc_cpp_plugin
            COMMENT "Running gRPC C++ protocol buffer compiler on ${fil}"
            VERBATIM)
    endforeach()

    set_source_files_properties(${${srcs}} ${${hdrs}} PROPERTIES GENERATED TRUE)
    set(${srcs} ${${srcs}} PARENT_SCOPE)
    set(${hdrs} ${${hdrs}} PARENT_SCOPE)
endfunction()


set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Proto")
file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
grpc_generate_cpp(GRPC_SRCS GRPC_HDRS ${CMAKE_CURRENT_BINARY_DIR} ${PROTO_FILES})

# 이 프로젝트의 실행 파일에 소스를 추가합니다.
add_executable (Edriel 
    "Edriel.cpp" 
    "Edriel.hpp" 
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_link_libraries(Edriel PRIVATE asio::asio protobuf::libprotobuf gRPC::grpc++ gRPC::grpc++_reflection)
target_include_directories(Edriel PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")


# Server
add_executable (Client 
    "Client.cpp" 
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${GRPC_SRCS}
    ${GRPC_HDRS}
)

target_link_libraries(Client PRIVATE asio::asio protobuf::libprotobuf gRPC::grpc++ gRPC::grpc++_reflection)
target_include_directories(Client PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Client
add_executable (Server 
    "Server.cpp" 
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${GRPC_SRCS}
    ${GRPC_HDRS}
)

target_link_libraries(Server PRIVATE asio::asio protobuf::libprotobuf gRPC::grpc++ gRPC::grpc++_reflection)
target_include_directories(Server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")



if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET Edriel PROPERTY CXX_STANDARD 20)
  set_property(TARGET Client PROPERTY CXX_STANDARD 20)
  set_property(TARGET Server PROPERTY CXX_STANDARD 20)
endif()

# TODO: 필요한 경우 테스트를 추가하고 대상을 설치합니다.
