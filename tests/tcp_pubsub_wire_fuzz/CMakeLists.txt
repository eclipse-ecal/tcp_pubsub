################################################################################
# Copyright (c) Continental. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.
#
# SPDX-License-Identifier: MIT
################################################################################

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
    OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Wire fuzzing requires upstream Clang with the clang++ driver and libFuzzer/AddressSanitizer runtimes.")
endif()

set(_wire_sanitizers "fuzzer,address")
if (NOT WIN32)
    string(APPEND _wire_sanitizers ",undefined")
endif()

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=${_wire_sanitizers}")
check_cxx_source_compiles("
    #include <cstddef>
    #include <cstdint>
    extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
" TCP_PUBSUB_HAS_WIRE_FUZZ_SANITIZERS)
cmake_pop_check_state()
if (NOT TCP_PUBSUB_HAS_WIRE_FUZZ_SANITIZERS)
    message(FATAL_ERROR "Clang cannot link -fsanitize=${_wire_sanitizers}. Install the matching compiler-rt runtimes.")
endif()

# Compile the parser itself with instrumentation, not just the fuzz harness.
add_executable(tcp_pubsub_wire_fuzz
    src/frame_parser_fuzz.cpp
    ../../tcp_pubsub/src/wire/frame_parser.cpp
)
target_compile_features(tcp_pubsub_wire_fuzz PRIVATE cxx_std_17)
target_include_directories(tcp_pubsub_wire_fuzz PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../tcp_pubsub/src")
target_compile_options(tcp_pubsub_wire_fuzz PRIVATE
    -Wall -Wextra -fno-omit-frame-pointer "-fsanitize=${_wire_sanitizers}"
)
target_link_options(tcp_pubsub_wire_fuzz PRIVATE "-fsanitize=${_wire_sanitizers}")
