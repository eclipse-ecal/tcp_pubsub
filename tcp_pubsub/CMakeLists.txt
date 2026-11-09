cmake_minimum_required(VERSION 3.23...4.0)

include("${CMAKE_CURRENT_LIST_DIR}/version.cmake")
project(tcp_pubsub VERSION ${TCP_PUBSUB_VERSION_MAJOR}.${TCP_PUBSUB_VERSION_MINOR}.${TCP_PUBSUB_VERSION_PATCH})

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Disable default export of symbols
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

find_package(Threads REQUIRED)
find_package(asio REQUIRED)
find_package(recycle REQUIRED)

# Include GenerateExportHeader that will create export macros for us
include(GenerateExportHeader)

# Public API headers
set (includes
    include/tcp_pubsub/callback_data.h
    include/tcp_pubsub/executor.h
    include/tcp_pubsub/publisher.h
    include/tcp_pubsub/subscriber.h
    include/tcp_pubsub/subscriber_session.h
    include/tcp_pubsub/tcp_pubsub_logger.h
)

# Generated public headers
set (generated_includes
    "${PROJECT_BINARY_DIR}/include/tcp_pubsub/tcp_pubsub_version.h"
    "${PROJECT_BINARY_DIR}/include/tcp_pubsub/tcp_pubsub_export.h"
)

# Private source files
set(sources
    src/executor.cpp
    src/executor_impl.cpp
    src/executor_impl.h
    src/portable_endian.h
    src/protocol_handshake_message.h
    src/publisher.cpp
    src/publisher_impl.cpp
    src/publisher_impl.h
    src/publisher_session.cpp
    src/publisher_session.h
    src/subscriber.cpp
    src/subscriber_impl.cpp
    src/subscriber_impl.h
    src/subscriber_session.cpp
    src/subscriber_session_impl.cpp
    src/subscriber_session_impl.h
    src/tcp_header.h
    src/tcp_pubsub_logger_abstraction.h
    src/wire/frame_builder.cpp
    src/wire/frame_builder.h
    src/wire/frame_parser.cpp
    src/wire/frame_parser.h
    src/wire/handshake.cpp
    src/wire/handshake.h
    src/wire/header.h
)

# Generate version defines
configure_file("tcp_pubsub_version.h.in" "${PROJECT_BINARY_DIR}/include/tcp_pubsub/tcp_pubsub_version.h" @ONLY)

# Object library (used internally by tcp_pubsub and for unit tests)
add_library (${PROJECT_NAME}_private OBJECT)

set_target_properties(${PROJECT_NAME}_private PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_sources(${PROJECT_NAME}_private
    PRIVATE
        ${sources}
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS
            include
            ${PROJECT_BINARY_DIR}/include
        FILES
            ${includes}
            ${generated_includes}
)

target_link_libraries(${PROJECT_NAME}_private
    PUBLIC
        Threads::Threads
        $<$<BOOL:${WIN32}>:ws2_32>
        $<$<BOOL:${WIN32}>:wsock32>

        # Link header-only libs (asio & recycle) as described in this workaround:
        # https://gitlab.kitware.com/cmake/cmake/-/issues/15415#note_633938
        $<BUILD_INTERFACE:asio::asio>
        $<BUILD_INTERFACE:steinwurf::recycle>
)

target_compile_definitions(${PROJECT_NAME}_private
    PRIVATE
        ASIO_STANDALONE
        ASIO_DISABLE_VISIBILITY
        _WIN32_WINNT=0x0601
        $<$<STREQUAL:$<TARGET_PROPERTY:${PROJECT_NAME},TYPE>,SHARED_LIBRARY>:tcp_pubsub_EXPORTS>
        $<$<STREQUAL:$<TARGET_PROPERTY:${PROJECT_NAME},TYPE>,STATIC_LIBRARY>:TCP_PUBSUB_STATIC_DEFINE>
        $<$<STREQUAL:$<TARGET_PROPERTY:${PROJECT_NAME},TYPE>,OBJECT_LIBRARY>:TCP_PUBSUB_STATIC_DEFINE>
)

target_compile_features(${PROJECT_NAME}_private PUBLIC cxx_std_17)

target_compile_options(${PROJECT_NAME}_private PRIVATE
                           $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
                                -Wall -Wextra>
                           $<$<CXX_COMPILER_ID:MSVC>:
                                /W4>)

target_include_directories(${PROJECT_NAME}_private
  PRIVATE
    src/
)

# Main library
if (TCP_PUBSUB_LIBRARY_TYPE STREQUAL "OBJECT")
    add_library (${PROJECT_NAME} ALIAS ${PROJECT_NAME}_private)

    # For an object library, define TCP_PUBSUB_STATIC_DEFINE so the generated header uses static macros
    target_compile_definitions(${PROJECT_NAME}_private
        PUBLIC
            TCP_PUBSUB_STATIC_DEFINE
    )

    generate_export_header(${PROJECT_NAME}_private
      EXPORT_FILE_NAME ${PROJECT_BINARY_DIR}/include/tcp_pubsub/tcp_pubsub_export.h
      BASE_NAME TCP_PUBSUB
      STATIC_DEFINE TCP_PUBSUB_STATIC_DEFINE
    )

    add_library (tcp_pubsub::${PROJECT_NAME} ALIAS ${PROJECT_NAME}_private)
else()
    add_library (${PROJECT_NAME} ${TCP_PUBSUB_LIBRARY_TYPE}
        $<TARGET_OBJECTS:${PROJECT_NAME}_private>
    )

    generate_export_header(${PROJECT_NAME}
      EXPORT_FILE_NAME ${PROJECT_BINARY_DIR}/include/tcp_pubsub/tcp_pubsub_export.h
      BASE_NAME TCP_PUBSUB
    )

    target_sources(${PROJECT_NAME}
        PUBLIC
            FILE_SET HEADERS
            BASE_DIRS
                include
                ${PROJECT_BINARY_DIR}/include
            FILES
                ${includes}
                ${generated_includes}
    )

    target_link_libraries(${PROJECT_NAME}
      PUBLIC
        $<BUILD_INTERFACE:${PROJECT_NAME}_private>
    )

    target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

    set_target_properties(${PROJECT_NAME} PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        OUTPUT_NAME ${PROJECT_NAME}
    )

    add_library (tcp_pubsub::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
endif()



##################################

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES 
    ${includes}
    ${sources}
)

################################################################################
### Installation rules
################################################################################

if (NOT TCP_PUBSUB_LIBRARY_TYPE STREQUAL "OBJECT")
  set(TCP_PUBSUB_INSTALL_CMAKE_DIR "lib/cmake/tcp_pubsub")

  # Install Runtime, Libs & Headers
  install(
    TARGETS ${PROJECT_NAME}
    EXPORT tcp_pubsubTargets
    
    RUNTIME
      DESTINATION "bin"
      COMPONENT tcp_pubsub_runtime

    LIBRARY
      DESTINATION "lib"
      COMPONENT tcp_pubsub_runtime

    ARCHIVE
      DESTINATION "lib"
      COMPONENT tcp_pubsub_dev

    FILE_SET HEADERS
      DESTINATION "include"
      COMPONENT tcp_pubsub_dev
  )

  # Install Target.cmake file (-> dev packag)
  install(
    EXPORT tcp_pubsubTargets 
    FILE tcp_pubsubTargets.cmake 
    DESTINATION ${TCP_PUBSUB_INSTALL_CMAKE_DIR}
    NAMESPACE tcp_pubsub::
    COMPONENT tcp_pubsub_dev
  )

  # Create and install Config.cmake file (-> dev package)

  include(CMakePackageConfigHelpers)

  configure_package_config_file(
    "cmake/tcp_pubsubConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_/tcp_pubsubConfig.cmake"
    INSTALL_DESTINATION ${TCP_PUBSUB_INSTALL_CMAKE_DIR}
    PATH_VARS TCP_PUBSUB_INSTALL_CMAKE_DIR
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_/tcp_pubsubConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )
  install(
    FILES
      "${CMAKE_CURRENT_BINARY_DIR}/cmake_/tcp_pubsubConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/cmake_/tcp_pubsubConfigVersion.cmake"
    DESTINATION ${TCP_PUBSUB_INSTALL_CMAKE_DIR}
    COMPONENT tcp_pubsub_dev
  )
endif()
