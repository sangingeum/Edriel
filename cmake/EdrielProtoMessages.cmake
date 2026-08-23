# EdrielProtoMessages.cmake
#
# Helper for Edriel users: compile your own .proto files into C++ and link the
# generated code against EdrielLib so the messages can be used directly as
# topics (registerPublisherTopic<MyMsg>(), sendMessage(), ...).
#
# Usage:
#   include(EdrielProtoMessages)
#   edriel_add_proto_messages(my_messages
#       SRCS messages/sensor.proto messages/control.proto
#       # Optional: base directory passed to protoc as --proto_path.
#       # Defaults to ${CMAKE_CURRENT_SOURCE_DIR}.
#       PROTO_PATH ${CMAKE_CURRENT_SOURCE_DIR}
#   )
#   target_link_libraries(my_app PRIVATE my_messages)
#
# Extra import directories (e.g. to import Edriel's own autoDiscovery.proto)
# can be appended by callers before invoking the function:
#   list(APPEND EDRIEL_PROTO_IMPORT_DIRS ${MY_OTHER_PROTO_DIR})

include_guard(GLOBAL)

function(edriel_add_proto_messages TARGET_NAME)
    set(options)
    set(oneValueArgs PROTO_PATH)
    set(multiValueArgs SRCS)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(NOT ARG_SRCS)
        message(FATAL_ERROR "edriel_add_proto_messages(${TARGET_NAME}): SRCS is required")
    endif()
    if(NOT ARG_PROTO_PATH)
        set(ARG_PROTO_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if(NOT TARGET EdrielLib)
        message(FATAL_ERROR "edriel_add_proto_messages(${TARGET_NAME}): EdrielLib target not found. "
                            "Add the Edriel project via add_subdirectory() first.")
    endif()

    # protoc compiler. Edriel/CMakeLists.txt already resolves EDRIEL_PROTOC_EXEC
    # from the Conan build module's protobuf::protoc target (falling back to
    # find_program); if this helper is used standalone, resolve it here too.
    if(NOT EDRIEL_PROTOC_EXEC)
        if(TARGET protobuf::protoc)
            get_target_property(EDRIEL_PROTOC_EXEC protobuf::protoc IMPORTED_LOCATION)
        endif()
        if(NOT EDRIEL_PROTOC_EXEC OR NOT EXISTS "${EDRIEL_PROTOC_EXEC}")
            find_program(EDRIEL_PROTOC_EXEC NAMES protoc)
        endif()
        if(NOT EDRIEL_PROTOC_EXEC)
            message(FATAL_ERROR "edriel_add_proto_messages(${TARGET_NAME}): protoc executable "
                                "not found (protobuf Conan package or PATH).")
        endif()
    endif()

    cmake_path(SET PROTO_BASE NORMALIZE "${ARG_PROTO_PATH}")
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/edriel_generated/${TARGET_NAME}")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    # protoc search paths: the user's proto path first, then any extra dirs.
    set(PROTO_PATH_ARGS "--proto_path=${PROTO_BASE}")
    foreach(EXTRA_DIR ${EDRIEL_PROTO_IMPORT_DIRS})
        list(APPEND PROTO_PATH_ARGS "--proto_path=${EXTRA_DIR}")
    endforeach()

    set(GENERATED_SRCS)
    set(GENERATED_HDRS)
    foreach(PROTO_FILE ${ARG_SRCS})
        if(NOT IS_ABSOLUTE "${PROTO_FILE}")
            cmake_path(ABSOLUTE_PATH PROTO_FILE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
        else()
            cmake_path(SET PROTO_FILE NORMALIZE "${PROTO_FILE}")
        endif()
        get_filename_component(FIL_WE "${PROTO_FILE}" NAME_WE)

        # protoc mirrors the source's path relative to --proto_path in the
        # output dir (proto/sub.proto -> <GEN_DIR>/proto/sub.pb.{h,cc}), so
        # compute each generated file's real location.
        file(RELATIVE_PATH PROTO_REL "${PROTO_BASE}" "${PROTO_FILE}")
        string(FIND "${PROTO_REL}" ".." DOTDOT_IDX)
        if(NOT DOTDOT_IDX EQUAL -1)
            message(FATAL_ERROR "edriel_add_proto_messages(${TARGET_NAME}): "
                                "${PROTO_FILE} is not inside PROTO_PATH (${PROTO_BASE})")
        endif()
        get_filename_component(PROTO_REL_DIR "${PROTO_REL}" DIRECTORY)

        if(PROTO_REL_DIR STREQUAL "")
            set(P_CPP "${GEN_DIR}/${FIL_WE}.pb.cc")
            set(P_HDR "${GEN_DIR}/${FIL_WE}.pb.h")
        else()
            set(P_CPP "${GEN_DIR}/${PROTO_REL_DIR}/${FIL_WE}.pb.cc")
            set(P_HDR "${GEN_DIR}/${PROTO_REL_DIR}/${FIL_WE}.pb.h")
        endif()
        list(APPEND GENERATED_SRCS "${P_CPP}")
        list(APPEND GENERATED_HDRS "${P_HDR}")

        add_custom_command(
            OUTPUT "${P_CPP}" "${P_HDR}"
            COMMAND "${EDRIEL_PROTOC_EXEC}"
            ARGS ${PROTO_PATH_ARGS}
                 --cpp_out=${GEN_DIR}
                 ${PROTO_FILE}
            DEPENDS "${PROTO_FILE}"
            COMMENT "Compiling ${PROTO_REL} (edriel_add_proto_messages)"
            VERBATIM
        )
    endforeach()

    add_library(${TARGET_NAME} STATIC
        ${GENERATED_SRCS}
        ${GENERATED_HDRS}
    )
    # Generated headers live under GEN_DIR, with subdirectories mirroring the
    # proto files' paths relative to PROTO_PATH (e.g. proto/robot.pb.h).
    # GEN_DIR's parent is added too so flat includes ("robot.pb.h") also work.
    target_include_directories(${TARGET_NAME} PUBLIC "${GEN_DIR}" "${GEN_DIR}/..")
    # EdrielLib publicly carries protobuf::libprotobuf, so generated code links fine.
    target_link_libraries(${TARGET_NAME} PUBLIC EdrielLib)
endfunction()
