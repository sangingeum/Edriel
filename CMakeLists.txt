cmake_minimum_required(VERSION 3.8)

project(Edriel CXX)

# Source files in current directory
# Executable will be created by subdirectory

# Conan toolchain handles dependency paths
if(EXISTS "${CMAKE_SOURCE_DIR}/conan_toolchain.cmake")
  include("${CMAKE_SOURCE_DIR}/conan_toolchain.cmake")
endif()

add_subdirectory(Edriel)
