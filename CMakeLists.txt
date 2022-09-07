cmake_minimum_required(VERSION 3.5.1)

# Project call
include("${CMAKE_CURRENT_LIST_DIR}/tcp_pubsub/version.cmake")
project(tcp_pubsub VERSION ${TCP_PUBSUB_VERSION_MAJOR}.${TCP_PUBSUB_VERSION_MINOR}.${TCP_PUBSUB_VERSION_PATCH})

# Normalize backslashes from Windows paths
file(TO_CMAKE_PATH "${CMAKE_MODULE_PATH}" CMAKE_MODULE_PATH)
file(TO_CMAKE_PATH "${CMAKE_PREFIX_PATH}" CMAKE_PREFIX_PATH)
message(STATUS "Module Path: ${CMAKE_MODULE_PATH}")
message(STATUS "Prefix Path: ${CMAKE_PREFIX_PATH}")

# CMake Options
option(TCP_PUBSUB_BUILD_SAMPLES
       "Build project samples"
       ON)

option(TCP_PUBSUB_BUILD_ECAL_SAMPLES
       "Build eCAL-based project samples. Requires eCAL to be findable by CMake."
       OFF)

# Module path for finding asio
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/modules)

# Set Debug postfix
set(CMAKE_DEBUG_POSTFIX            d)
set(CMAKE_MINSIZEREL_POSTFIX       minsize)
set(CMAKE_RELWITHDEBINFO_POSTFIX   reldbg)

# Add main tcp_pubsub library
add_subdirectory(tcp_pubsub)

# Recycle dependency. It's header only and not in the API, so we add it with EXCLUDE_FOR_ALL, so it won't be installed
add_subdirectory(thirdparty/recycle EXCLUDE_FROM_ALL)

# Generic samples
if (TCP_PUBSUB_BUILD_SAMPLES)
    add_subdirectory(samples/performance_publisher)
    add_subdirectory(samples/performance_subscriber)
    add_subdirectory(samples/hello_world_publisher)
    add_subdirectory(samples/hello_world_subscriber)
endif()

# Specific eCAL Samples that tunnel an eCAL Topic through TCP
if(TCP_PUBSUB_BUILD_ECAL_SAMPLES)
    add_subdirectory(samples/ecal_to_tcp)
    add_subdirectory(samples/tcp_to_ecal)
endif()

# Make this package available for packing with CPack
include("${CMAKE_CURRENT_LIST_DIR}/cpack_config.cmake")
