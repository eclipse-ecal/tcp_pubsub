cmake_minimum_required(VERSION 3.5.1)

project(hello_world_publisher)

set(CMAKE_CXX_STANDARD 14)

find_package(tcp_pubsub REQUIRED CONFIG)

set(sources
    src/main.cpp
)

add_executable (${PROJECT_NAME}
    ${sources}
)

target_link_libraries (${PROJECT_NAME}
    tcp_pubsub::tcp_pubsub
)
