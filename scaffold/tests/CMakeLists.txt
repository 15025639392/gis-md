# 测试
enable_testing()

find_package(GTest CONFIG REQUIRED)

# ============================================================
# core tests
# ============================================================
add_executable(test_vec3
    unit/core/test_vec3.cpp
)
target_link_libraries(test_vec3 PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_vec3 COMMAND test_vec3)

add_executable(test_ellipsoid
    unit/geodesy/test_ellipsoid.cpp
)
target_link_libraries(test_ellipsoid PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_ellipsoid COMMAND test_ellipsoid)

add_executable(test_tile_scheme
    unit/tiling/test_tile_scheme.cpp
)
target_link_libraries(test_tile_scheme PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_tile_scheme COMMAND test_tile_scheme)

add_executable(test_camera
    unit/scene/test_camera.cpp
)
target_link_libraries(test_camera PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_camera COMMAND test_camera)

add_executable(test_frustum
    unit/scene/test_frustum.cpp
)
target_link_libraries(test_frustum PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_frustum COMMAND test_frustum)

add_executable(test_globe
    unit/globe/test_globe.cpp
)
target_link_libraries(test_globe PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_globe COMMAND test_globe)

add_executable(test_camera_controller
    unit/camera/test_camera_controller.cpp
)
target_link_libraries(test_camera_controller PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_camera_controller COMMAND test_camera_controller)

add_executable(test_tile_plan
    unit/tiling/test_tile_plan.cpp
)
target_link_libraries(test_tile_plan PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_tile_plan COMMAND test_tile_plan)

# ============================================================
# stage 4 tests
# ============================================================

add_executable(test_crs_profile
    unit/tiling/test_crs_profile.cpp
)
target_link_libraries(test_crs_profile PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_crs_profile COMMAND test_crs_profile)

add_executable(test_tms_scheme
    unit/tiling/test_tms_scheme.cpp
)
target_link_libraries(test_tms_scheme PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_tms_scheme COMMAND test_tms_scheme)

add_executable(test_layer_stack
    unit/layers/test_layer_stack.cpp
)
target_link_libraries(test_layer_stack PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_layer_stack COMMAND test_layer_stack)

# ============================================================
# stage 5 tests
# ============================================================

add_executable(test_geojson_parser
    unit/data/test_geojson_parser.cpp
)
target_link_libraries(test_geojson_parser PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_geojson_parser COMMAND test_geojson_parser)

add_executable(test_overlay_style
    unit/style/test_overlay_style.cpp
)
target_link_libraries(test_overlay_style PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_overlay_style COMMAND test_overlay_style)

add_executable(test_picking
    unit/interaction/test_picking.cpp
)
target_link_libraries(test_picking PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
add_test(NAME test_picking COMMAND test_picking)

# ============================================================
# stage 6 tests
# ============================================================

add_executable(test_terrain_provider
    unit/providers/test_terrain_provider.cpp
)
target_link_libraries(test_terrain_provider PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
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

add_executable(test_terrain_mesh
    unit/terrain/test_terrain_mesh.cpp
)
target_link_libraries(test_terrain_mesh PRIVATE earth_engine_core GTest::gtest GTest::gtest_main)
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(test_terrain_mesh PRIVATE CURL::libcurl)
else()
    find_library(CURL_LIB2 curl)
    if(CURL_LIB2)
        target_link_libraries(test_terrain_mesh PRIVATE ${CURL_LIB2})
    endif()
endif()
add_test(NAME test_terrain_mesh COMMAND test_terrain_mesh)
