cmake_minimum_required(VERSION 3.13)

include(CMakeDependentOption)

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

option(TCP_PUBSUB_USE_BUILTIN_ASIO
       "Use the builtin asio submodule. If set to OFF, asio must be available from somewhere else (e.g. system libs)."
       ON)

option(TCP_PUBSUB_USE_BUILTIN_RECYCLE
       "Use the builtin steinwurf::recycle submodule. If set to OFF, recycle must be available from somewhere else (e.g. system libs)."
       ON)

option(TCP_PUBSUB_BUILD_TESTS
       "Build the tcp_pubsub tests. Requires Gtest::GTest to be findable by CMake."
       OFF)

cmake_dependent_option(TCP_PUBSUB_USE_BUILTIN_GTEST
       "Use the builtin GoogleTest submodule. Only needed if TCP_PUBSUB_BUILD_TESTS is ON. If set to OFF, GoogleTest must be available from somewhere else (e.g. system libs)."
       ON                       # Default value if dependency is met
       "TCP_PUBSUB_BUILD_TESTS" # Dependency
       OFF)                     # Default value if dependency is not met


# Module path for finding asio
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/modules)

# Set Debug postfix
set(CMAKE_DEBUG_POSTFIX            d)
set(CMAKE_MINSIZEREL_POSTFIX       minsize)
set(CMAKE_RELWITHDEBINFO_POSTFIX   reldbg)


# Use builtin asio
if (TCP_PUBSUB_USE_BUILTIN_ASIO)
    include("${CMAKE_CURRENT_LIST_DIR}/thirdparty/asio/build-asio.cmake")
endif()

# Use builtin recycle
if (TCP_PUBSUB_USE_BUILTIN_RECYCLE)
    include("${CMAKE_CURRENT_LIST_DIR}/thirdparty/recycle/build-recycle.cmake")
endif()

# Use builtin gtest
if (TCP_PUBSUB_USE_BUILTIN_GTEST)
    include("${CMAKE_CURRENT_LIST_DIR}/thirdparty/gtest/build-gtest.cmake")
endif()

# For tests we need to make sure that all shared libraries and executables are 
# put into the same directory. Otherwise the tests will fail on windows.
if(TCP_PUBSUB_BUILD_TESTS AND (BUILD_SHARED_LIBS OR (TCP_PUBSUB_LIBRARY_TYPE STREQUAL "SHARED")))
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Module path for finding tcp_pubsub
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tcp_pubsub/Module)

# Add main tcp_pubsub library
add_subdirectory(tcp_pubsub)

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


# Add Tests if enabled
if (TCP_PUBSUB_BUILD_TESTS)
    enable_testing()
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/tests/tcp_pubsub_test")
endif()

# Make this package available for packing with CPack
include("${CMAKE_CURRENT_LIST_DIR}/cpack_config.cmake")
