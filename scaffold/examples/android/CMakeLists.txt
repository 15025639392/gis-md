cmake_minimum_required(VERSION 3.20)

# Android 示例应用 — 通过 Gradle + NDK 构建
# 此 CMakeLists.txt 仅构建 native 库 (.so)，由 Gradle 的 externalNativeBuild 调用。
# 用法：在 build.gradle 中配置 cmake { path "examples/android/CMakeLists.txt" }

project(minimal_globe_android LANGUAGES CXX)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src ${CMAKE_CURRENT_BINARY_DIR}/earth_engine_core)

add_library(earth_engine_android_jni SHARED
    MinimalGlobe/GLESView.cpp
    MinimalGlobe/AmapVectorConfig.cpp
    MinimalGlobe/MinimalGlobeDemoConfig.cpp
    MinimalGlobe/MinimalGlobeDiagnostics.cpp
)

target_link_libraries(earth_engine_android_jni PRIVATE earth_engine_core)

target_include_directories(earth_engine_android_jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/MinimalGlobe
)

# Android 特定链接
target_link_libraries(earth_engine_android_jni PRIVATE
    android    # ANativeWindow
    GLESv3     # OpenGL ES 3.0
    EGL        # EGL
    log        # __android_log_print
)
