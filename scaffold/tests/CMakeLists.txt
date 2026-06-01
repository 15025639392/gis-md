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
