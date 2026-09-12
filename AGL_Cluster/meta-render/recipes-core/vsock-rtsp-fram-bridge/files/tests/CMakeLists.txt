cmake_minimum_required(VERSION 3.16)
project(vsock_bridge_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

find_package(PkgConfig REQUIRED)

pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GST REQUIRED gstreamer-1.0)
pkg_check_modules(GSTAPP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GSTRTSP REQUIRED gstreamer-rtsp-server-1.0)

add_executable(test_bridge_core test_bridge_core.c)

target_include_directories(test_bridge_core PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/..     # so ../vsock-rtsp-fram-bridge.c include works
  ${GLIB_INCLUDE_DIRS}
  ${GST_INCLUDE_DIRS}
  ${GSTAPP_INCLUDE_DIRS}
  ${GSTRTSP_INCLUDE_DIRS}
)

target_link_libraries(test_bridge_core PRIVATE
  ${GLIB_LIBRARIES}
  ${GST_LIBRARIES}
  ${GSTAPP_LIBRARIES}
  ${GSTRTSP_LIBRARIES}
)

target_compile_options(test_bridge_core PRIVATE
  -Wall -Wextra -Wpedantic
)

add_test(NAME test_bridge_core COMMAND test_bridge_core)