cmake_minimum_required(VERSION 3.5.1)

# Normalize backslashes from Windows paths
file(TO_CMAKE_PATH "${CMAKE_MODULE_PATH}" CMAKE_MODULE_PATH)
file(TO_CMAKE_PATH "${CMAKE_PREFIX_PATH}" CMAKE_PREFIX_PATH)
message(STATUS "Module Path: ${CMAKE_MODULE_PATH}")
message(STATUS "Prefix Path: ${CMAKE_PREFIX_PATH}")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/modules)

include("${CMAKE_CURRENT_LIST_DIR}/tcp_pubsub/version.cmake")
project(tcp_pubsub VERSION ${TCP_PUBSUB_VERSION_MAJOR}.${TCP_PUBSUB_VERSION_MINOR}.${TCP_PUBSUB_VERSION_PATCH})

# Set Debug postfix
set(CMAKE_DEBUG_POSTFIX            d)
set(CMAKE_MINSIZEREL_POSTFIX       minsize)
set(CMAKE_RELWITHDEBINFO_POSTFIX   reldbg)

add_subdirectory(tcp_pubsub)

add_subdirectory(thirdparty/recycle EXCLUDE_FROM_ALL)

add_subdirectory(samples/performance_publisher)
add_subdirectory(samples/performance_subscriber)
add_subdirectory(samples/hello_world_publisher)
add_subdirectory(samples/hello_world_subscriber)

# add_subdirectory(samples/ecal_to_tcp)
# add_subdirectory(samples/tcp_to_ecal)

# Make this package available for packing with CPack
include(CPack)