# 测试
enable_testing()

include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
)
FetchContent_MakeAvailable(googletest)

if(NOT TARGET GTest::gtest AND TARGET gtest)
    add_library(GTest::gtest ALIAS gtest)
endif()
if(NOT TARGET GTest::gtest_main AND TARGET gtest_main)
    add_library(GTest::gtest_main ALIAS gtest_main)
endif()

# ============================================================
# core tests
# ============================================================
add_executable(test_vec3
    unit/core/test_vec3.cpp
)
target_link_libraries(test_vec3 PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_vec3 COMMAND test_vec3)

add_executable(test_mat4
    unit/core/test_mat4.cpp
)
target_link_libraries(test_mat4 PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_mat4 COMMAND test_mat4)

add_executable(test_plane
    unit/core/test_plane.cpp
)
target_link_libraries(test_plane PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_plane COMMAND test_plane)

add_executable(test_ray
    unit/core/test_ray.cpp
)
target_link_libraries(test_ray PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_ray COMMAND test_ray)

add_executable(test_bounding_sphere
    unit/core/test_bounding_sphere.cpp
)
target_link_libraries(test_bounding_sphere PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_bounding_sphere COMMAND test_bounding_sphere)

add_executable(test_oriented_bounding_box
    unit/core/test_oriented_bounding_box.cpp
)
target_link_libraries(test_oriented_bounding_box PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_oriented_bounding_box COMMAND test_oriented_bounding_box)

add_executable(test_rectangle
    unit/core/test_rectangle.cpp
)
target_link_libraries(test_rectangle PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_rectangle COMMAND test_rectangle)

add_executable(test_frame_resource_budget
    unit/core/test_frame_resource_budget.cpp
)
target_link_libraries(test_frame_resource_budget PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_frame_resource_budget COMMAND test_frame_resource_budget)

add_executable(test_cartographic
    unit/geodesy/test_cartographic.cpp
)
target_link_libraries(test_cartographic PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_cartographic COMMAND test_cartographic)

add_executable(test_projection
    unit/geodesy/test_projection.cpp
)
target_link_libraries(test_projection PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_projection COMMAND test_projection)

add_executable(test_ellipsoid
    unit/geodesy/test_ellipsoid.cpp
)
target_link_libraries(test_ellipsoid PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_ellipsoid COMMAND test_ellipsoid)

add_executable(test_tile_scheme
    unit/tiling/test_tile_scheme.cpp
)
target_link_libraries(test_tile_scheme PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_scheme COMMAND test_tile_scheme)

add_executable(test_camera
    unit/scene/test_camera.cpp
)
target_link_libraries(test_camera PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera COMMAND test_camera)

add_executable(test_frustum
    unit/scene/test_frustum.cpp
)
target_link_libraries(test_frustum PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_frustum COMMAND test_frustum)

add_executable(test_globe
    unit/globe/test_globe.cpp
)
target_link_libraries(test_globe PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_globe COMMAND test_globe)

add_executable(test_camera_controller
    unit/camera/test_camera_controller.cpp
)
target_link_libraries(test_camera_controller PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_controller COMMAND test_camera_controller)

add_executable(test_earth_scene_config
    unit/sdk/test_earth_scene_config.cpp
)
target_link_libraries(test_earth_scene_config PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_earth_scene_config COMMAND test_earth_scene_config)

add_executable(test_tile_surface
    unit/tiling/test_tile_surface.cpp
)
target_link_libraries(test_tile_surface PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_surface COMMAND test_tile_surface)

# ============================================================
# stage 4 tests
# ============================================================

add_executable(test_crs_profile
    unit/tiling/test_crs_profile.cpp
)
target_link_libraries(test_crs_profile PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_crs_profile COMMAND test_crs_profile)

add_executable(test_tms_scheme
    unit/tiling/test_tms_scheme.cpp
)
target_link_libraries(test_tms_scheme PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tms_scheme COMMAND test_tms_scheme)

add_executable(test_renderer_commands
    unit/renderer/test_renderer_commands.cpp
)
target_link_libraries(test_renderer_commands PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_renderer_commands COMMAND test_renderer_commands)

add_executable(test_render_command_streaming_set
    unit/renderer/test_render_command_streaming_set.cpp
)
target_link_libraries(test_render_command_streaming_set PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_render_command_streaming_set COMMAND test_render_command_streaming_set)

# ============================================================
# stage 5 tests
# ============================================================

add_executable(test_geojson_parser
    unit/data/test_geojson_parser.cpp
)
target_link_libraries(test_geojson_parser PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_geojson_parser COMMAND test_geojson_parser)

add_executable(test_gltf_parser
    unit/content/test_gltf_parser.cpp
)
target_link_libraries(test_gltf_parser PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_gltf_parser COMMAND test_gltf_parser)

add_executable(test_overlay_style
    unit/style/test_overlay_style.cpp
)
target_link_libraries(test_overlay_style PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_overlay_style COMMAND test_overlay_style)

add_executable(test_minimal_globe_diagnostics
    unit/debug/test_minimal_globe_diagnostics.cpp
    ../examples/android/MinimalGlobe/MinimalGlobeDiagnostics.cpp
)
target_link_libraries(test_minimal_globe_diagnostics PRIVATE earth_engine_core GTest::gtest_main)
target_include_directories(test_minimal_globe_diagnostics PRIVATE
    ../examples/android/MinimalGlobe
)
add_test(NAME test_minimal_globe_diagnostics COMMAND test_minimal_globe_diagnostics)

add_executable(test_picking
    unit/interaction/test_picking.cpp
)
target_link_libraries(test_picking PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_picking COMMAND test_picking)

add_executable(test_input_manager
    unit/interaction/test_input_manager.cpp
)
target_link_libraries(test_input_manager PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_input_manager COMMAND test_input_manager)

# ============================================================
# stage 6 tests
# ============================================================

add_executable(test_terrain_provider
    unit/providers/test_terrain_provider.cpp
)
target_link_libraries(test_terrain_provider PRIVATE earth_engine_core GTest::gtest_main)
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(test_terrain_provider PRIVATE CURL::libcurl)
else()
    find_library(CURL_LIB curl)
    if(CURL_LIB)
        target_link_libraries(test_terrain_provider PRIVATE ${CURL_LIB})
    endif()
endif()
add_test(NAME test_terrain_provider COMMAND test_terrain_provider)

add_executable(test_raster_overlay_lifecycle
    unit/providers/test_raster_overlay_lifecycle.cpp
)
target_link_libraries(test_raster_overlay_lifecycle PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_raster_overlay_lifecycle COMMAND test_raster_overlay_lifecycle)

add_executable(test_curl_multi_request_scheduler
    unit/platform/test_curl_multi_request_scheduler.cpp
)
target_link_libraries(test_curl_multi_request_scheduler PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_curl_multi_request_scheduler COMMAND test_curl_multi_request_scheduler)

# ============================================================
# environment tests
# ============================================================

add_executable(test_sun_direction
    unit/environment/test_sun_direction.cpp
)
target_link_libraries(test_sun_direction PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_sun_direction COMMAND test_sun_direction)

add_executable(test_time_controller
    unit/environment/test_time_controller.cpp
)
target_link_libraries(test_time_controller PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_time_controller COMMAND test_time_controller)

add_executable(test_atmosphere_scattering
    unit/environment/test_atmosphere_scattering.cpp
)
target_link_libraries(test_atmosphere_scattering PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_atmosphere_scattering COMMAND test_atmosphere_scattering)
