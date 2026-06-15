# earth_engine_core — 跨平台静态库
add_library(earth_engine_core STATIC)

target_include_directories(earth_engine_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ============================================================
# core/math
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/core/math/Vec3.h
    earth_engine/core/math/Vec3.cpp
    earth_engine/core/math/Mat4.h
    earth_engine/core/math/Mat4.cpp
    earth_engine/core/math/Ray.h
    earth_engine/core/math/Ray.cpp
    earth_engine/core/math/Rectangle.h
    earth_engine/core/math/Rectangle.cpp
    earth_engine/core/math/Plane.h
    earth_engine/core/math/BoundingSphere.h
    earth_engine/core/math/OrientedBoundingBox.h
)

# ============================================================
# core/async
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/core/async/AsyncSystem.h
)

# ============================================================
# core/cache
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/core/cache/HttpCache.h
    earth_engine/core/cache/PersistentCache.h
)

# ============================================================
# core/geodesy
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/core/geodesy/Cartographic.h
    earth_engine/core/geodesy/Ellipsoid.h
    earth_engine/core/geodesy/Ellipsoid.cpp
    earth_engine/core/geodesy/Transforms.h
    earth_engine/core/geodesy/Transforms.cpp
)

# ============================================================
# globe
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/globe/Globe.h
    earth_engine/globe/Globe.cpp
)

# ============================================================
# content
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/content/GltfModel.h
    earth_engine/content/GltfModel.cpp
    earth_engine/content/GltfContentProvider.h
    earth_engine/content/GltfContentProvider.cpp
)

# ============================================================
# camera
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/camera/CameraController.h
    earth_engine/camera/CameraController.cpp
)

# ============================================================
# renderer
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/renderer/RenderDevice.h
    earth_engine/renderer/RenderCommand.h
    earth_engine/renderer/RenderCommand.cpp
    earth_engine/renderer/Renderer.h
    earth_engine/renderer/Renderer.cpp
    earth_engine/renderer/IPrepareRendererResources.h
    earth_engine/renderer/TileTextureCache.h
    earth_engine/renderer/TileTextureCache.cpp
)

# ============================================================
# scene
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/scene/Camera.h
    earth_engine/scene/Camera.cpp
    earth_engine/scene/Frustum.h
    earth_engine/scene/Frustum.cpp
    earth_engine/scene/FrameState.h
    earth_engine/scene/Scene.h
    earth_engine/scene/Scene.cpp
)

# ============================================================
# engine (top-level)
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/Engine.h
    earth_engine/Engine.cpp
)

# ============================================================
# tiling
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/tiling/CrsProfile.h
    earth_engine/tiling/CrsProfile.cpp
    earth_engine/tiling/TileKey.h
    earth_engine/tiling/TileKey.cpp
    earth_engine/tiling/TileRefine.h
    earth_engine/tiling/TileBoundingVolume.h
    earth_engine/tiling/TileScheme.h
    earth_engine/tiling/TileScheme.cpp
    earth_engine/tiling/SurfaceTile.h
    earth_engine/tiling/TileSurface.h
    earth_engine/tiling/TileSurface.cpp
    earth_engine/tiling/TilePlan.h
    earth_engine/tiling/TilePlan.cpp
    earth_engine/tiling/TilesetTile.h
    earth_engine/tiling/RasterMappedToTilesetTile.h
    earth_engine/tiling/RasterMappedToTilesetTile.cpp
    earth_engine/tiling/Tileset.h
    earth_engine/tiling/Tileset.cpp
)

# ============================================================
# providers
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/providers/ImageryProvider.h
    earth_engine/providers/DebugImageryProvider.h
    earth_engine/providers/DebugImageryProvider.cpp
    earth_engine/providers/XYZImageryProvider.h
    earth_engine/providers/XYZImageryProvider.cpp
    earth_engine/providers/TerrainProvider.h
    earth_engine/providers/TerrainProvider.cpp
    earth_engine/providers/HeightmapTerrainProvider.h
    earth_engine/providers/HeightmapTerrainProvider.cpp
    earth_engine/providers/QuantizedMeshTerrainProvider.h
    earth_engine/providers/QuantizedMeshTerrainProvider.cpp
    earth_engine/providers/RasterOverlayTile.h
    earth_engine/providers/RasterOverlayTile.cpp
    earth_engine/providers/RasterOverlayTileProvider.h
    earth_engine/providers/RasterOverlayTileProvider.cpp
)

# ============================================================
# layers
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/layers/VectorLayer.h
    earth_engine/layers/VectorLayer.cpp
    earth_engine/layers/RasterOverlay.h
    earth_engine/layers/RasterOverlay.cpp
    earth_engine/layers/ActivatedRasterOverlay.h
    earth_engine/layers/ActivatedRasterOverlay.cpp
)

# ============================================================
# debug
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/debug/PerfTimer.h
)

# ============================================================
# platform/bridge
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/platform/bridge/PlatformBridge.h
    earth_engine/platform/stb_image_impl.cpp
)

if(NOT ANDROID)
    target_sources(earth_engine_core PRIVATE
        earth_engine/platform/bridge/CurlPlatformBridge.h
        earth_engine/platform/bridge/CurlPlatformBridge.cpp
    )
endif()

# ============================================================
# platform backends (条件编译)
# ============================================================
if(ANDROID)
    target_sources(earth_engine_core PRIVATE
        earth_engine/platform/android/RenderDeviceGLES.h
        earth_engine/platform/android/RenderDeviceGLES.cpp
        earth_engine/platform/android/AndroidPlatformBridge.h
        earth_engine/platform/android/AndroidPlatformBridge.cpp
    )
    # Android NDK 自带 GLES3
    find_library(GLESv3_LIBRARY GLESv3)
    if(GLESv3_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${GLESv3_LIBRARY})
    endif()
    find_library(EGL_LIBRARY EGL)
    if(EGL_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${EGL_LIBRARY})
    endif()
    find_library(LOG_LIBRARY log)
    if(LOG_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${LOG_LIBRARY})
    endif()
    find_library(JNIGRAPHICS_LIBRARY jnigraphics)
    if(JNIGRAPHICS_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${JNIGRAPHICS_LIBRARY})
    endif()
elseif(APPLE)
    # iOS / macOS — HTTP 通过 PlatformBridge (NSURLSession)，禁用 libcurl
    target_compile_definitions(earth_engine_core PRIVATE EARTH_ENGINE_HAS_LIBCURL=0)
    target_sources(earth_engine_core PRIVATE
        earth_engine/platform/ios/RenderDeviceMetal.h
        earth_engine/platform/ios/RenderDeviceMetal.mm
    )
    # 启用 Objective-C++ 编译
    set_source_files_properties(
        earth_engine/platform/ios/RenderDeviceMetal.mm
        PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )
    # 链接 Metal 和 QuartzCore frameworks
    find_library(METAL_LIBRARY Metal)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    if(METAL_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${METAL_LIBRARY})
    endif()
    if(QUARTZCORE_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${QUARTZCORE_LIBRARY})
    endif()
endif()

# ============================================================
# style
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/style/OverlayStyle.h
    earth_engine/style/OverlayStyle.cpp
)

# ============================================================
# data
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/data/GeoJsonParser.h
    earth_engine/data/GeoJsonParser.cpp
)

# ============================================================
# terrain
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/terrain/TerrainTile.h
    earth_engine/terrain/TerrainTile.cpp
    earth_engine/terrain/QuantizedMeshParser.h
    earth_engine/terrain/QuantizedMeshParser.cpp
)

# ============================================================
# interaction
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/interaction/InputEvent.h
    earth_engine/interaction/InputManager.h
    earth_engine/interaction/InputManager.cpp
    earth_engine/interaction/PickingService.h
    earth_engine/interaction/PickingService.cpp
    earth_engine/interaction/SelectionManager.h
    earth_engine/interaction/SelectionManager.cpp
)

# ============================================================
# environment
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/environment/AtmosphereParameters.h
    earth_engine/environment/AtmosphereBackgroundPass.h
    earth_engine/environment/AtmosphereBackgroundPass.cpp
    earth_engine/environment/SkyBox.h
    earth_engine/environment/SkyBox.cpp
    earth_engine/environment/TimeController.h
    earth_engine/environment/TimeController.cpp
    earth_engine/environment/SunDirection.h
    earth_engine/environment/SunDirection.cpp
    earth_engine/environment/SkyGradient.h
    earth_engine/environment/SkyGradient.cpp
)

# ============================================================
# threading
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/threading/CancellationToken.h
)

# ============================================================
# 依赖库
# ============================================================
find_package(glm CONFIG QUIET)
if(glm_FOUND)
    target_link_libraries(earth_engine_core PUBLIC glm::glm)
else()
    set(_EARTH_ENGINE_GLM_HINTS
        ${GLM_INCLUDE_DIR}
        $ENV{GLM_INCLUDE_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../third_party/glm/include
        $ENV{VCPKG_ROOT}/installed/$ENV{VCPKG_DEFAULT_TRIPLET}/include
        $ENV{VCPKG_ROOT}/packages/glm_arm64-android/include
        ${CMAKE_CURRENT_LIST_DIR}/../../../globe/third_party/vcpkg/packages/glm_arm64-android/include
    )
    find_path(GLM_INCLUDE_DIR glm/glm.hpp
        HINTS
            ${_EARTH_ENGINE_GLM_HINTS}
            /opt/homebrew/include
            /usr/local/include
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(NOT GLM_INCLUDE_DIR)
        message(FATAL_ERROR "GLM headers not found. Install glm or set GLM_INCLUDE_DIR.")
    endif()
    target_include_directories(earth_engine_core PUBLIC ${GLM_INCLUDE_DIR})
    unset(_EARTH_ENGINE_GLM_HINTS)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(earth_engine_core PUBLIC nlohmann_json::nlohmann_json)
else()
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
        HINTS
            ${CMAKE_CURRENT_LIST_DIR}/../third_party_py/nlohmann_json/include
            /opt/homebrew/include
            /usr/local/include
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(NOT NLOHMANN_JSON_INCLUDE_DIR)
        message(FATAL_ERROR "nlohmann/json.hpp not found. Install nlohmann_json or set NLOHMANN_JSON_INCLUDE_DIR.")
    endif()
    target_include_directories(earth_engine_core PUBLIC ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

find_package(CURL CONFIG QUIET)
if(CURL_FOUND)
    target_link_libraries(earth_engine_core PUBLIC CURL::libcurl)
endif()

# stb_image 是 header-only；当前核心库尚未直接使用，缺失时不阻断。
find_package(stb CONFIG QUIET)
if(stb_FOUND)
    target_link_libraries(earth_engine_core PRIVATE stb::stb)
endif()
