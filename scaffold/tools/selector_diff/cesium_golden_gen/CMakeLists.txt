# cesium-native golden 轨迹生成器（见 ../DESIGN.md）。
#
# 独立顶层工程：add_subdirectory 引入本地 cesium-native checkout，
# 依赖经 vcpkg manifest 模式满足（复用 build-selectordiff 已编好的
# vcpkg_installed，配置见 build_golden.sh）。

cmake_minimum_required(VERSION 3.20)
project(cesium_golden_gen LANGUAGES C CXX)

set(CESIUM_NATIVE_DIR "" CACHE PATH "Path to the local cesium-native checkout")
if(NOT CESIUM_NATIVE_DIR)
    message(FATAL_ERROR "CESIUM_NATIVE_DIR must point to the cesium-native checkout")
endif()

add_subdirectory(${CESIUM_NATIVE_DIR} cesium-native EXCLUDE_FROM_ALL)

add_executable(cesium_golden_gen main.cpp)
target_compile_features(cesium_golden_gen PRIVATE cxx_std_20)
# scenarios.h 在上一级目录（selector_diff/），单一场景来源
target_include_directories(cesium_golden_gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(cesium_golden_gen PRIVATE Cesium3DTilesSelection)
