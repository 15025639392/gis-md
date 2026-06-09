cmake_minimum_required(VERSION 3.20)
project(earth_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

# 编译选项
if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# iOS 特定
if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0")
    set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")
endif()

# Android 特定
if(ANDROID)
    set(CMAKE_ANDROID_STL_TYPE "c++_shared")
endif()

# 子目录
add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# 示例应用（默认不构建，通过 option 开启）
option(BUILD_EXAMPLES "Build example applications" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
