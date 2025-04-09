add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/recycle" EXCLUDE_FROM_ALL)

# Prepend asio-module/Findrecycle.cmake to the module path
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_LIST_DIR}/Module")
