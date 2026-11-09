################################################################################
# Copyright (c) Continental. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.
#
# SPDX-License-Identifier: MIT
################################################################################

cmake_minimum_required(VERSION 3.13...4.0)

project(tcp_pubsub_micro_benchmark)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
find_package(tcp_pubsub REQUIRED)
find_package(Threads    REQUIRED)

if (NOT TARGET benchmark::benchmark)
    find_package(benchmark REQUIRED)
endif()

# The reference ("floor") benchmarks use raw asio sockets, independent of tcp_pubsub
if (NOT TARGET asio::asio)
    find_package(asio REQUIRED)
endif()

# Same adapter as the end-to-end harness
set(TCP_PUBSUB_BENCHMARK_ADAPTER "v1" CACHE STRING "Library adapter to benchmark (benchmarks/tcp_pubsub_benchmark/src/adapter_<name>.cpp)")
set(HARNESS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../tcp_pubsub_benchmark/src")

set(sources
    src/micro_api.cpp
    src/micro_common.h
    src/micro_floor.cpp
    src/micro_main.cpp
    ${HARNESS_SRC}/adapter.h
    ${HARNESS_SRC}/adapter_${TCP_PUBSUB_BENCHMARK_ADAPTER}.cpp
    ${HARNESS_SRC}/platform.cpp
    ${HARNESS_SRC}/platform.h
)

add_executable(${PROJECT_NAME} ${sources})

target_include_directories(${PROJECT_NAME} PRIVATE ${HARNESS_SRC})

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        ASIO_STANDALONE
        _WIN32_WINNT=0x0601
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        tcp_pubsub::tcp_pubsub
        benchmark::benchmark
        asio::asio
        Threads::Threads
)

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE psapi winmm ws2_32 mswsock)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX "src" FILES src/micro_api.cpp src/micro_common.h src/micro_floor.cpp src/micro_main.cpp)
