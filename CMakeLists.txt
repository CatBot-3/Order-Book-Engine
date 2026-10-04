cmake_minimum_required(VERSION 3.21)

project(
  order_book_engine
  VERSION 0.1.0
  DESCRIPTION "ITCH 5.0 feed handler, order book and matching engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OBE_BUILD_APPS "Build the command-line apps" ON)
option(OBE_BUILD_TESTS "Build the test suite" ON)
option(OBE_BUILD_BENCH "Build the benchmarks" ON)
option(OBE_BUILD_FUZZ "Build the libFuzzer targets (Clang only)" OFF)
option(OBE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(OBE_NATIVE "Compile with -march=native. Changes benchmark numbers; record it when used" OFF)
set(OBE_SANITIZE "" CACHE STRING "Sanitizers, comma separated: address,undefined or thread")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ObeFlags)

# The library is header-only. Linking obe::obe gives a target the include path,
# C++20 and the project warning set.
add_library(obe INTERFACE)
add_library(obe::obe ALIAS obe)
target_include_directories(obe INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(obe INTERFACE cxx_std_20)
target_link_libraries(obe INTERFACE obe_warnings)

if(OBE_BUILD_APPS)
  add_subdirectory(apps)
endif()

if(OBE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OBE_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(OBE_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
