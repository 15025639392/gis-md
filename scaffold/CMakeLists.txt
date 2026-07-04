cmake_minimum_required(VERSION 3.20)
project(earth_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# .mm 源(Metal 后端/平台桥)需要根级启用 OBJCXX:若只由子目录
# (examples/macos)enable,全局 .mm→OBJCXX 语言映射会生效而 src/ 作用域
# 缺编译规则,Ninja 生成期报 CMAKE_OBJCXX_COMPILE_OBJECT 缺失。
if(APPLE AND BUILD_EXAMPLES)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

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

option(BUILD_SSE_PIPELINE_TEST "Build lightweight engine regression test" ON)
if(BUILD_SSE_PIPELINE_TEST)
    add_executable(test_sse_pipeline
        tests/unit/tiling/test_sse_pipeline.cpp
    )
    target_link_libraries(test_sse_pipeline PRIVATE earth_engine_core)
    if(BUILD_TESTING)
        add_test(NAME test_sse_pipeline COMMAND test_sse_pipeline)
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# 示例应用（默认不构建，通过 option 开启）
option(BUILD_EXAMPLES "Build example applications" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
