

cmake_minimum_required(VERSION 3.16)
project(host_broker CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(host_broker
 src/main.cpp
 src/broker.cpp
 src/vsock.cpp
 src/cli.cpp
 src/fram.cpp
)
target_include_directories(host_broker PRIVATE include)
target_compile_options(host_broker PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(host_broker PRIVATE _GNU_SOURCE)
# -----------------------
# Tests (no external deps)
# -----------------------
option(BUILD_TESTS "Build unit tests" ON)
if (BUILD_TESTS)
 enable_testing()
add_executable(host_broker_tests
  tests/test_main.cpp
  tests/test_fram.cpp
  tests/test_io.cpp
  tests/test_cli.cpp
  src/fram.cpp
  src/cli.cpp
)

target_include_directories(host_broker_tests PRIVATE include tests)
 target_compile_options(host_broker_tests PRIVATE -Wall -Wextra -Wpedantic)
 target_compile_definitions(host_broker_tests PRIVATE _GNU_SOURCE)
 add_test(NAME host_broker_tests COMMAND host_broker_tests)
endif()