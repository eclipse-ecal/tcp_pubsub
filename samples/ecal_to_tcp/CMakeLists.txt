cmake_minimum_required(VERSION 3.5.1)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

project(ecal_to_tcp)

set(CMAKE_CXX_STANDARD 14)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG  TRUE)
find_package(tcp_pubsub REQUIRED)
find_package(eCAL REQUIRED)
find_package(Threads REQUIRED)

set(sources
    src/main.cpp
)

add_executable (${PROJECT_NAME}
    ${sources}
)

target_link_libraries (${PROJECT_NAME}
    tcp_pubsub::tcp_pubsub
    eCAL::core
    Threads::Threads
)
