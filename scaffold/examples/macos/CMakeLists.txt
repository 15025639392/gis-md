cmake_minimum_required(VERSION 3.20)

# macOS 示例应用 — 使用 Xcode generator 或 Ninja/Makefile
# 用法：cmake -G Xcode -DBUILD_EXAMPLES=ON ../..
#       或 cmake -DBUILD_EXAMPLES=ON -DCMAKE_OSX_ARCHITECTURES=arm64 ../..

add_executable(MinimalGlobe MACOSX_BUNDLE
    MinimalGlobe/AppDelegate.h
    MinimalGlobe/AppDelegate.mm
    MinimalGlobe/MetalView.h
    MinimalGlobe/MetalView.mm
    MinimalGlobe/MacPlatformBridge.h
    MinimalGlobe/MacPlatformBridge.mm
    MinimalGlobe/main.mm
)

target_link_libraries(MinimalGlobe PRIVATE earth_engine_core)

set_target_properties(MinimalGlobe PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/MinimalGlobe/Info.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.earthengine.minimalglobe.macos"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
)

target_link_libraries(MinimalGlobe PRIVATE
    "-framework AppKit"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework Foundation"
)

# 启用 Objective-C++ 编译
set_source_files_properties(
    MinimalGlobe/AppDelegate.mm
    MinimalGlobe/MetalView.mm
    MinimalGlobe/MacPlatformBridge.mm
    MinimalGlobe/main.mm
    PROPERTIES
    COMPILE_FLAGS "-x objective-c++"
)
