cmake_minimum_required(VERSION 3.20)
project(MinimalGlobeMac LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD 17)

enable_language(OBJCXX)

# Use the same vcpkg toolchain if available
if(DEFINED VCPKG_ROOT AND EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "")
endif()

# Earth Engine core library
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/earth_engine_core)

# macOS app executable
add_executable(MinimalGlobe MACOSX_BUNDLE
    MinimalGlobe/main.mm
    MinimalGlobe/AppDelegate.h
    MinimalGlobe/AppDelegate.mm
    MinimalGlobe/MetalView.h
    MinimalGlobe/MetalView.mm
)

# Include paths
target_include_directories(MinimalGlobe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/MinimalGlobe
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/earth_engine/platform
)

# Link earth_engine_core + macOS frameworks
target_link_libraries(MinimalGlobe PRIVATE
    earth_engine_core
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework Cocoa"
    "-framework CoreVideo"
    "-framework Foundation"
    "-framework Security"
    "-framework CoreGraphics"
    "-framework ImageIO"
)

# Configure app bundle
set_target_properties(MinimalGlobe PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.earthengine.minimalglobe"
    MACOSX_BUNDLE_BUNDLE_NAME "Minimal Globe"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
)

# Ensure glm include path
if(DEFINED GLM_INCLUDE_DIR)
    target_include_directories(earth_engine_core PUBLIC
        ${GLM_INCLUDE_DIR})
endif()
