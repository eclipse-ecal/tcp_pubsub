################################################################################
# Copyright (c) Continental. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.
#
# SPDX-License-Identifier: MIT
################################################################################

cmake_minimum_required(VERSION 3.13...4.0)

project(tcp_pubsub_wire_test)

find_package(GTest REQUIRED)

add_executable(${PROJECT_NAME} src/wire_codec_test.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../tcp_pubsub/src")
target_link_libraries(${PROJECT_NAME} PRIVATE tcp_pubsub_private GTest::gtest_main)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME})
