################################################################################
# Copyright (c) Continental. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.
# 
# SPDX-License-Identifier: MIT
################################################################################

cmake_minimum_required(VERSION 3.13...4.0)

project(tcp_pubsub_test)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG  TRUE)

find_package(tcp_pubsub REQUIRED)
find_package(GTest      REQUIRED)

set(sources
    src/atomic_signalable.h
    src/tcp_pubsub_test.cpp
)

add_executable (${PROJECT_NAME}
    ${sources}
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_14)

target_link_libraries (${PROJECT_NAME}
    tcp_pubsub::tcp_pubsub
    GTest::gtest_main
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${sources})

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME})
