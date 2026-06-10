cmake_minimum_required(VERSION 3.20)

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(ios)
endif()

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_subdirectory(macos)
endif()
