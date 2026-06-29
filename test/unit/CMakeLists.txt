# Host-side unit test + coverage build for DumbESPty firmware logic.
#
# This builds a native (host gcc/g++) test binary that compiles selected
# firmware sources from ../../main against lightweight mock headers in ./mocks,
# links them with GoogleTest, and is instrumented for gcov coverage.
#
# It is deliberately SEPARATE from the ESP-IDF firmware build. Only the
# hardware-independent logic of the firmware is exercised here (terminal escape
# parsing, hostname validation, HID keycode mapping, SSH string/hash helpers,
# shell argument parsing, backlight math, etc.). Hardware/RTOS calls are
# satisfied by the mocks under ./mocks.
#
# Usage:
#   cmake -S test/unit -B build/unit -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build/unit
#   ctest --test-dir build/unit --output-on-failure
#   cmake --build build/unit --target coverage   # HTML + XML report

cmake_minimum_required(VERSION 3.16)
project(dumbespty_unit_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../main)
set(MOCKS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mocks)

# ----------------------------------------------------------------------------
# Coverage instrumentation (gcov). Applied only to firmware sources of interest
# via the object library below, plus the test TUs, so the report focuses on the
# code we intend to cover (mocks/gtest excluded from the final report filter).
# ----------------------------------------------------------------------------
set(COVERAGE_FLAGS --coverage -O0 -g -fprofile-arcs -ftest-coverage)

# ----------------------------------------------------------------------------
# GoogleTest via FetchContent (falls back gracefully if already populated).
# ----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG release-1.12.1
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# Common include dirs: mocks FIRST so they shadow any real SDK headers, then
# the firmware source/include dirs.
set(COMMON_INCLUDES
  ${MOCKS_DIR}
  ${MAIN_DIR}
  ${MAIN_DIR}/include
)

# ----------------------------------------------------------------------------
# Mock/stub support library (NOT coverage-instrumented; excluded from report).
# ----------------------------------------------------------------------------
add_library(test_mocks STATIC
  ${MOCKS_DIR}/stubs_esp.cpp
  ${MOCKS_DIR}/stubs_freertos.cpp
  ${MOCKS_DIR}/stubs_lvgl.cpp
  ${MOCKS_DIR}/stubs_firmware.cpp
  ${MOCKS_DIR}/stubs_ssh.cpp
)
target_include_directories(test_mocks PUBLIC ${COMMON_INCLUDES})

# ----------------------------------------------------------------------------
# Firmware sources under test, compiled with coverage instrumentation.
# Cozette font data is compiled in to satisfy cozette_bdf_lookup used by
# terminal.cpp's (unexercised) draw path.
# ----------------------------------------------------------------------------
add_library(firmware_under_test STATIC
  ${MAIN_DIR}/terminal.cpp
  ${MAIN_DIR}/hostname_mgr.cpp
  ${MAIN_DIR}/fonts/cozette_bdf_13.c
)
target_include_directories(firmware_under_test PUBLIC ${COMMON_INCLUDES})
target_compile_options(firmware_under_test PRIVATE ${COVERAGE_FLAGS})
target_link_options(firmware_under_test PUBLIC ${COVERAGE_FLAGS})
target_compile_definitions(firmware_under_test PRIVATE CONFIG_IDF_TARGET_ESP32P4=1)
# NOTE: usb_hid_host.cpp is compiled inside test_usb_hid.cpp (white-box include)
# so its file-local keymap functions can be tested. The mocks provide
# usb/usb_host.h + usb/hid_host.h so its __has_include keymap path is enabled.

# ----------------------------------------------------------------------------
# Test executable.
# ----------------------------------------------------------------------------
add_executable(unit_tests
  test_terminal.cpp
  test_hostname.cpp
  test_usb_hid.cpp
  test_ssh_helpers.cpp
)
target_include_directories(unit_tests PRIVATE ${COMMON_INCLUDES})
target_compile_options(unit_tests PRIVATE ${COVERAGE_FLAGS})
# test_usb_hid.cpp white-box-includes usb_hid_host.cpp; enable its keymap path.
set_source_files_properties(test_usb_hid.cpp PROPERTIES
  COMPILE_DEFINITIONS CONFIG_IDF_TARGET_ESP32P4=1)
target_link_libraries(unit_tests
  PRIVATE firmware_under_test test_mocks GTest::gtest GTest::gtest_main
)
target_link_options(unit_tests PRIVATE ${COVERAGE_FLAGS})

gtest_discover_tests(unit_tests)

# ----------------------------------------------------------------------------
# Coverage report target (gcovr): text summary + HTML + Cobertura XML.
# ----------------------------------------------------------------------------
find_program(GCOVR_EXECUTABLE gcovr
  HINTS $ENV{HOME}/.local/bin /usr/local/bin /usr/bin)

if(GCOVR_EXECUTABLE)
  add_custom_target(coverage
    # Clear stale profile data so re-runs don't trip libgcov timestamp warnings.
    COMMAND ${CMAKE_COMMAND} -E rm -f
            ${CMAKE_BINARY_DIR}/CMakeFiles/unit_tests.dir/test_terminal.cpp.gcda
            ${CMAKE_BINARY_DIR}/CMakeFiles/unit_tests.dir/test_hostname.cpp.gcda
            ${CMAKE_BINARY_DIR}/CMakeFiles/unit_tests.dir/test_usb_hid.cpp.gcda
            ${CMAKE_BINARY_DIR}/CMakeFiles/unit_tests.dir/test_ssh_helpers.cpp.gcda
    COMMAND ctest --output-on-failure
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
    COMMAND ${GCOVR_EXECUTABLE}
            --root ${CMAKE_CURRENT_SOURCE_DIR}/../..
            --filter ".*/main/.*"
            --gcov-ignore-parse-errors
            --print-summary
            --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
            --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
            --txt ${CMAKE_BINARY_DIR}/coverage/coverage.txt
            ${CMAKE_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running tests and generating gcovr coverage report (HTML + Cobertura XML)"
    USES_TERMINAL
  )
else()
  message(WARNING "gcovr not found; 'coverage' target unavailable. Install with: pip install gcovr")
endif()
