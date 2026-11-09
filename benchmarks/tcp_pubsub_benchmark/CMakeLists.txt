################################################################################
# Copyright (c) Continental. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.
#
# SPDX-License-Identifier: MIT
################################################################################

cmake_minimum_required(VERSION 3.13...4.0)

project(tcp_pubsub_benchmark)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
find_package(tcp_pubsub REQUIRED)
find_package(Threads    REQUIRED)

# Adapter that connects the harness to one concrete implementation. To
# benchmark another implementation, add an adapter_<name>.cpp and select it here.
set(TCP_PUBSUB_BENCHMARK_ADAPTER "v1" CACHE STRING "Library adapter to benchmark (src/adapter_<name>.cpp)")

set(sources
    src/adapter.h
    src/adapter_${TCP_PUBSUB_BENCHMARK_ADAPTER}.cpp
    src/harness.cpp
    src/harness.h
    src/main.cpp
    src/platform.cpp
    src/platform.h
    src/stats.h
)

add_executable(${PROJECT_NAME} ${sources})

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        tcp_pubsub::tcp_pubsub
        Threads::Threads
)

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE psapi winmm)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${sources})
