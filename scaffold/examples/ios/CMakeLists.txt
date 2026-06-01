cmake_minimum_required(VERSION 3.20)

# iOS 示例应用 — 使用 Xcode generator
# 用法：cmake -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-ios.cmake ..

add_executable(MinimalGlobe
    MinimalGlobe/AppDelegate.mm
    MinimalGlobe/MetalView.mm
    MinimalGlobe/main.mm
)

target_link_libraries(MinimalGlobe PRIVATE earth_engine_core)

set_target_properties(MinimalGlobe PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.earthengine.minimalglobe"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
)

target_link_libraries(MinimalGlobe PRIVATE
    "-framework UIKit"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
)
