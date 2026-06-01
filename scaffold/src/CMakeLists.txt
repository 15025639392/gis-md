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
# renderer
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/renderer/RenderDevice.h
    earth_engine/renderer/RenderCommand.h
)

# ============================================================
# scene
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/scene/Camera.h
    earth_engine/scene/Camera.cpp
)

# ============================================================
# tiling
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/tiling/TileKey.h
    earth_engine/tiling/TileScheme.h
    earth_engine/tiling/TileScheme.cpp
)

# ============================================================
# platform/bridge
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/platform/bridge/PlatformBridge.h
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
    find_path(GLM_INCLUDE_DIR glm/glm.hpp
        HINTS /opt/homebrew/include /usr/local/include
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(NOT GLM_INCLUDE_DIR)
        message(FATAL_ERROR "GLM headers not found. Install glm or set GLM_INCLUDE_DIR.")
    endif()
    target_include_directories(earth_engine_core PUBLIC ${GLM_INCLUDE_DIR})
endif()

find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(earth_engine_core PUBLIC nlohmann_json::nlohmann_json)
endif()

find_package(CURL CONFIG QUIET)
if(CURL_FOUND)
    target_link_libraries(earth_engine_core PRIVATE CURL::libcurl)
endif()

# stb_image 是 header-only；当前核心库尚未直接使用，缺失时不阻断阶段 1 数学/瓦片测试。
find_package(stb CONFIG QUIET)
if(stb_FOUND)
    target_link_libraries(earth_engine_core PRIVATE stb::stb)
endif()
