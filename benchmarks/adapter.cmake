################################################################################
# Copyright (c) Continental. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for details.
#
# SPDX-License-Identifier: MIT
################################################################################

set(TCP_PUBSUB_BENCHMARK_ADAPTER "v2" CACHE STRING "Library adapter to benchmark (benchmarks/tcp_pubsub_benchmark/src/adapter_<name>.cpp)")

set(_tcp_pubsub_benchmark_adapter "${TCP_PUBSUB_BENCHMARK_ADAPTER}")
if (TCP_PUBSUB_BENCHMARK_ADAPTER STREQUAL "v1")
    message(DEPRECATION "Benchmark adapter v1 was renamed to v2 (the tcp_pubsub 2.0.1 API). Use -DTCP_PUBSUB_BENCHMARK_ADAPTER=v2.")
    set(_tcp_pubsub_benchmark_adapter "v2")
endif()

set(TCP_PUBSUB_BENCHMARK_ADAPTER_SOURCE "${CMAKE_CURRENT_LIST_DIR}/tcp_pubsub_benchmark/src/adapter_${_tcp_pubsub_benchmark_adapter}.cpp")
if (NOT EXISTS "${TCP_PUBSUB_BENCHMARK_ADAPTER_SOURCE}")
    message(FATAL_ERROR "Unknown TCP_PUBSUB_BENCHMARK_ADAPTER '${TCP_PUBSUB_BENCHMARK_ADAPTER}': no source at ${TCP_PUBSUB_BENCHMARK_ADAPTER_SOURCE}")
endif()
