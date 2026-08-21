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

add_executable(test_http_cache
    unit/core/test_http_cache.cpp
)
target_link_libraries(test_http_cache PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_http_cache COMMAND test_http_cache)

add_executable(test_mat4
    unit/core/test_mat4.cpp
)
target_link_libraries(test_mat4 PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_mat4 COMMAND test_mat4)

add_executable(test_math_utils
    unit/core/test_math_utils.cpp
)
target_link_libraries(test_math_utils PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_math_utils COMMAND test_math_utils)

add_executable(test_attribute_compression
    unit/core/test_attribute_compression.cpp
)
target_link_libraries(test_attribute_compression PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_attribute_compression COMMAND test_attribute_compression)

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

add_executable(test_intersection_tests
    unit/core/test_intersection_tests.cpp
)
target_link_libraries(test_intersection_tests PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_intersection_tests COMMAND test_intersection_tests)

add_executable(test_clip_triangle_at_axis_aligned_threshold
    unit/core/test_clip_triangle_at_axis_aligned_threshold.cpp
)
target_link_libraries(test_clip_triangle_at_axis_aligned_threshold PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_clip_triangle_at_axis_aligned_threshold COMMAND test_clip_triangle_at_axis_aligned_threshold)

add_executable(test_culling_volume
    unit/core/test_culling_volume.cpp
)
target_link_libraries(test_culling_volume PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_culling_volume COMMAND test_culling_volume)

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

add_executable(test_bounding_cylinder_region
    unit/core/test_bounding_cylinder_region.cpp
)
target_link_libraries(test_bounding_cylinder_region PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_bounding_cylinder_region COMMAND test_bounding_cylinder_region)

add_executable(test_axis_aligned_box
    unit/core/test_axis_aligned_box.cpp
)
target_link_libraries(test_axis_aligned_box PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_axis_aligned_box COMMAND test_axis_aligned_box)

add_executable(test_rectangle
    unit/core/test_rectangle.cpp
)
target_link_libraries(test_rectangle PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_rectangle COMMAND test_rectangle)

add_executable(test_uri
    unit/core/test_uri.cpp
)
target_link_libraries(test_uri PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_uri COMMAND test_uri)

add_executable(test_credit_system
    unit/layers/test_credit_system.cpp
)
target_link_libraries(test_credit_system PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_credit_system COMMAND test_credit_system)

add_executable(test_feature_tile_mesh
    unit/layers/test_feature_tile_mesh.cpp
)
target_link_libraries(test_feature_tile_mesh PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_feature_tile_mesh COMMAND test_feature_tile_mesh)

add_executable(test_feature_render_layer
    unit/layers/test_feature_render_layer.cpp
)
target_link_libraries(test_feature_render_layer PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_feature_render_layer COMMAND test_feature_render_layer)

add_executable(test_label_placement
    unit/layers/test_label_placement.cpp
)
target_link_libraries(test_label_placement PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_label_placement COMMAND test_label_placement)

add_executable(test_feature_edit_queries
    unit/layers/test_feature_edit_queries.cpp
)
target_link_libraries(test_feature_edit_queries PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_feature_edit_queries COMMAND test_feature_edit_queries)

# ============================================================
# test_platform_integration — macOS 平台层集成测试
# ============================================================
add_executable(test_platform_integration
    unit/platform/test_platform_integration.cpp
)
target_include_directories(test_platform_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers
)
target_link_libraries(test_platform_integration PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_platform_integration COMMAND test_platform_integration)

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

add_executable(test_bounding_region_builder
    unit/geodesy/test_bounding_region_builder.cpp
)
target_link_libraries(test_bounding_region_builder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_bounding_region_builder COMMAND test_bounding_region_builder)

add_executable(test_s2_cell_id
    unit/geodesy/test_s2_cell_id.cpp
)
target_link_libraries(test_s2_cell_id PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_s2_cell_id COMMAND test_s2_cell_id)

add_executable(test_s2_cell_bounding_volume
    unit/geodesy/test_s2_cell_bounding_volume.cpp
)
target_link_libraries(test_s2_cell_bounding_volume PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_s2_cell_bounding_volume COMMAND test_s2_cell_bounding_volume)

add_executable(test_projection
    unit/geodesy/test_projection.cpp
)
target_link_libraries(test_projection PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_projection COMMAND test_projection)

add_executable(test_gcj02_transform
    unit/geodesy/test_gcj02_transform.cpp
)
target_link_libraries(test_gcj02_transform PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_gcj02_transform COMMAND test_gcj02_transform)

add_executable(test_quadtree_geometric_error
    unit/geodesy/test_quadtree_geometric_error.cpp
)
target_link_libraries(test_quadtree_geometric_error PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_quadtree_geometric_error COMMAND test_quadtree_geometric_error)

add_executable(test_ellipsoid
    unit/geodesy/test_ellipsoid.cpp
)
target_link_libraries(test_ellipsoid PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_ellipsoid COMMAND test_ellipsoid)

add_executable(test_terrain_gpu_displacement_precision
    unit/geodesy/test_terrain_gpu_displacement_precision.cpp
)
target_link_libraries(test_terrain_gpu_displacement_precision PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_gpu_displacement_precision COMMAND test_terrain_gpu_displacement_precision)

add_executable(test_ellipsoid_tangent_plane
    unit/geodesy/test_ellipsoid_tangent_plane.cpp
)
target_link_libraries(test_ellipsoid_tangent_plane PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_ellipsoid_tangent_plane COMMAND test_ellipsoid_tangent_plane)

add_executable(test_transforms
    unit/geodesy/test_transforms.cpp
)
target_link_libraries(test_transforms PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_transforms COMMAND test_transforms)

add_executable(test_simple_planar_ellipsoid_curve
    unit/geodesy/test_simple_planar_ellipsoid_curve.cpp
)
target_link_libraries(test_simple_planar_ellipsoid_curve PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_simple_planar_ellipsoid_curve COMMAND test_simple_planar_ellipsoid_curve)

add_executable(test_tile_scheme
    unit/tiling/test_tile_scheme.cpp
)
target_link_libraries(test_tile_scheme PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_scheme COMMAND test_tile_scheme)

add_executable(test_tile_key
    unit/tiling/test_tile_key.cpp
)
target_link_libraries(test_tile_key PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_key COMMAND test_tile_key)

add_executable(test_raster_overlay_details
    unit/tiling/test_raster_overlay_details.cpp
)
target_link_libraries(test_raster_overlay_details PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_raster_overlay_details COMMAND test_raster_overlay_details)

add_executable(test_tile_id
    unit/tiling/test_tile_id.cpp
)
target_link_libraries(test_tile_id PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_id COMMAND test_tile_id)

add_executable(test_quadtree_tiling_scheme
    unit/tiling/test_quadtree_tiling_scheme.cpp
)
target_link_libraries(test_quadtree_tiling_scheme PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_quadtree_tiling_scheme COMMAND test_quadtree_tiling_scheme)

add_executable(test_octree_tiling_scheme
    unit/tiling/test_octree_tiling_scheme.cpp
)
target_link_libraries(test_octree_tiling_scheme PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_octree_tiling_scheme COMMAND test_octree_tiling_scheme)

add_executable(test_implicit_tile_id_utilities
    unit/tiling/test_implicit_tile_id_utilities.cpp
)
target_link_libraries(test_implicit_tile_id_utilities PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_implicit_tile_id_utilities COMMAND test_implicit_tile_id_utilities)

add_executable(test_tile_availability
    unit/tiling/test_tile_availability.cpp
)
target_link_libraries(test_tile_availability PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_availability COMMAND test_tile_availability)


add_executable(test_gltf_terrain_upsampler
    unit/content/test_gltf_terrain_upsampler.cpp
)
target_link_libraries(test_gltf_terrain_upsampler PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_gltf_terrain_upsampler COMMAND test_gltf_terrain_upsampler)

add_executable(test_composite_terrain_provider
    unit/content/test_composite_terrain_provider.cpp
)
target_link_libraries(test_composite_terrain_provider PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_composite_terrain_provider COMMAND test_composite_terrain_provider)

add_executable(test_ellipsoid_terrain_mesh_builder
    unit/content/test_ellipsoid_terrain_mesh_builder.cpp
)
target_link_libraries(test_ellipsoid_terrain_mesh_builder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_ellipsoid_terrain_mesh_builder COMMAND test_ellipsoid_terrain_mesh_builder)

add_executable(test_computed_imagery_uv_risk
    unit/content/test_computed_imagery_uv_risk.cpp
)
target_link_libraries(test_computed_imagery_uv_risk PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_computed_imagery_uv_risk COMMAND test_computed_imagery_uv_risk)

add_executable(test_fill_proxy_uv_risk
    unit/content/test_fill_proxy_uv_risk.cpp
)
target_link_libraries(test_fill_proxy_uv_risk PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_fill_proxy_uv_risk COMMAND test_fill_proxy_uv_risk)

add_executable(test_fill_proxy_build_cost
    unit/content/test_fill_proxy_build_cost.cpp
)
target_link_libraries(test_fill_proxy_build_cost PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_fill_proxy_build_cost COMMAND test_fill_proxy_build_cost)

add_executable(test_terrain_displacement_template
    unit/content/test_terrain_displacement_template.cpp
)
target_link_libraries(test_terrain_displacement_template PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_displacement_template COMMAND test_terrain_displacement_template)

add_executable(test_terrain_cpu_bake_cost
    unit/tiling/test_terrain_cpu_bake_cost.cpp
)
target_link_libraries(test_terrain_cpu_bake_cost PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_cpu_bake_cost COMMAND test_terrain_cpu_bake_cost)

add_executable(test_tile_bounding_volume
    unit/tiling/test_tile_bounding_volume.cpp
)
target_link_libraries(test_tile_bounding_volume PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_bounding_volume COMMAND test_tile_bounding_volume)

add_executable(test_tile_child_materializer
    unit/tiling/test_tile_child_materializer.cpp
)
target_link_libraries(test_tile_child_materializer PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_child_materializer COMMAND test_tile_child_materializer)

add_executable(test_loaded_terrain_height_sampler
    unit/tiling/test_loaded_terrain_height_sampler.cpp
)
target_link_libraries(test_loaded_terrain_height_sampler PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_loaded_terrain_height_sampler COMMAND test_loaded_terrain_height_sampler)

add_executable(test_decoded_heightmap_sampler
    unit/tiling/test_decoded_heightmap_sampler.cpp
)
target_link_libraries(test_decoded_heightmap_sampler PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_decoded_heightmap_sampler COMMAND test_decoded_heightmap_sampler)

add_executable(test_terrain_edge_normal_seam
    unit/tiling/test_terrain_edge_normal_seam.cpp
)
target_link_libraries(test_terrain_edge_normal_seam PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_edge_normal_seam COMMAND test_terrain_edge_normal_seam)

add_executable(test_terrain_bake_f32_seam
    unit/tiling/test_terrain_bake_f32_seam.cpp
)
target_link_libraries(test_terrain_bake_f32_seam PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_bake_f32_seam COMMAND test_terrain_bake_f32_seam)

add_executable(test_tileset_sample_height
    unit/tiling/test_tileset_sample_height.cpp
)
target_link_libraries(test_tileset_sample_height PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tileset_sample_height COMMAND test_tileset_sample_height)

add_executable(test_terrain_heightmap_generation
    unit/tiling/test_terrain_heightmap_generation.cpp
)
target_link_libraries(test_terrain_heightmap_generation PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_heightmap_generation COMMAND test_terrain_heightmap_generation)

add_executable(test_terrain_height_service
    unit/tiling/test_terrain_height_service.cpp
)
target_link_libraries(test_terrain_height_service PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_height_service COMMAND test_terrain_height_service)

add_executable(test_terrain_edge_height_lut
    unit/tiling/test_terrain_edge_height_lut.cpp
)
target_link_libraries(test_terrain_edge_height_lut PRIVATE earth_engine_core)
add_test(NAME test_terrain_edge_height_lut COMMAND test_terrain_edge_height_lut)

add_executable(test_terrain_displacement_edge_lut_upload
    unit/tiling/test_terrain_displacement_edge_lut_upload.cpp
)
target_link_libraries(
    test_terrain_displacement_edge_lut_upload
    PRIVATE earth_engine_core GTest::gtest_main)
add_test(
    NAME test_terrain_displacement_edge_lut_upload
    COMMAND test_terrain_displacement_edge_lut_upload)

add_executable(test_tile_edge_snap_resolver
    unit/tiling/test_tile_edge_snap_resolver.cpp
)
target_link_libraries(test_tile_edge_snap_resolver PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_edge_snap_resolver COMMAND test_tile_edge_snap_resolver)

add_executable(test_tile_pending_load_queue
    unit/tiling/test_tile_pending_load_queue.cpp
)
target_link_libraries(test_tile_pending_load_queue PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_pending_load_queue COMMAND test_tile_pending_load_queue)

add_executable(test_tile_pending_load_processor
    unit/tiling/test_tile_pending_load_processor.cpp
)
target_link_libraries(test_tile_pending_load_processor PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_pending_load_processor COMMAND test_tile_pending_load_processor)

add_executable(test_tile_pending_load_commit_coordinator
    unit/tiling/test_tile_pending_load_commit_coordinator.cpp
)
target_link_libraries(test_tile_pending_load_commit_coordinator PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_pending_load_commit_coordinator COMMAND test_tile_pending_load_commit_coordinator)

add_executable(test_tile_pending_upload_completion
    unit/tiling/test_tile_pending_upload_completion.cpp
)
target_link_libraries(test_tile_pending_upload_completion PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_pending_upload_completion COMMAND test_tile_pending_upload_completion)

add_executable(test_tile_pending_request_state
    unit/tiling/test_tile_pending_request_state.cpp
)
target_link_libraries(test_tile_pending_request_state PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_pending_request_state COMMAND test_tile_pending_request_state)

add_executable(test_tile_load_lifecycle
    unit/tiling/test_tile_load_lifecycle.cpp
)
target_link_libraries(test_tile_load_lifecycle PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_load_lifecycle COMMAND test_tile_load_lifecycle)

add_executable(test_tile_content_lifecycle_manager
    unit/tiling/test_tile_content_lifecycle_manager.cpp
)
target_link_libraries(test_tile_content_lifecycle_manager PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_content_lifecycle_manager COMMAND test_tile_content_lifecycle_manager)

add_executable(test_tile_content_unload_coordinator_basic
    unit/tiling/test_tile_content_unload_coordinator_basic.cpp
)
target_link_libraries(test_tile_content_unload_coordinator_basic PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_content_unload_coordinator_basic COMMAND test_tile_content_unload_coordinator_basic)

add_executable(test_tile_content_unload_coordinator_render
    unit/tiling/test_tile_content_unload_coordinator_render.cpp
)
target_link_libraries(test_tile_content_unload_coordinator_render PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_content_unload_coordinator_render COMMAND test_tile_content_unload_coordinator_render)

add_executable(test_tile_content_unload_coordinator_upsample
    unit/tiling/test_tile_content_unload_coordinator_upsample.cpp
)
target_link_libraries(test_tile_content_unload_coordinator_upsample PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_content_unload_coordinator_upsample COMMAND test_tile_content_unload_coordinator_upsample)

add_executable(test_tile_unload_policy
    unit/tiling/test_tile_unload_policy.cpp
)
target_link_libraries(test_tile_unload_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_unload_policy COMMAND test_tile_unload_policy)

add_executable(test_tile_cache_unload_coordinator
    unit/tiling/test_tile_cache_unload_coordinator.cpp
)
target_link_libraries(test_tile_cache_unload_coordinator PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_cache_unload_coordinator COMMAND test_tile_cache_unload_coordinator)

add_executable(test_tile_cache_metrics
    unit/tiling/test_tile_cache_metrics.cpp
)
target_link_libraries(test_tile_cache_metrics PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_cache_metrics COMMAND test_tile_cache_metrics)

add_executable(test_gpu_upload_queue
    unit/tiling/test_gpu_upload_queue.cpp
)
target_link_libraries(test_gpu_upload_queue PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_gpu_upload_queue COMMAND test_gpu_upload_queue)

add_executable(test_tile_content_cache_manager
    unit/tiling/test_tile_content_cache_manager.cpp
)
target_link_libraries(test_tile_content_cache_manager PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_content_cache_manager COMMAND test_tile_content_cache_manager)

add_executable(test_tile_subtree_tracking
    unit/tiling/test_tile_subtree_tracking.cpp
)
target_link_libraries(test_tile_subtree_tracking PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_subtree_tracking COMMAND test_tile_subtree_tracking)

add_executable(test_tile_load_request_dispatcher
    unit/tiling/test_tile_load_request_dispatcher.cpp
)
target_link_libraries(test_tile_load_request_dispatcher PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_load_request_dispatcher COMMAND test_tile_load_request_dispatcher)

add_executable(test_tile_load_request_planner
    unit/tiling/test_tile_load_request_planner.cpp
)
target_link_libraries(test_tile_load_request_planner PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_load_request_planner COMMAND test_tile_load_request_planner)

add_executable(test_tile_load_scheduler
    unit/tiling/test_tile_load_scheduler.cpp
)
target_link_libraries(test_tile_load_scheduler PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_load_scheduler COMMAND test_tile_load_scheduler)

add_executable(test_tile_motion_cull_policy
    unit/tiling/test_tile_motion_cull_policy.cpp
)
target_link_libraries(test_tile_motion_cull_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_motion_cull_policy COMMAND test_tile_motion_cull_policy)

add_executable(test_tile_load_diagnostics
    unit/tiling/test_tile_load_diagnostics.cpp
)
target_link_libraries(test_tile_load_diagnostics PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_load_diagnostics COMMAND test_tile_load_diagnostics)

add_executable(test_tile_traversal_details
    unit/tiling/test_tile_traversal_details.cpp
)
target_link_libraries(test_tile_traversal_details PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_traversal_details COMMAND test_tile_traversal_details)

add_executable(test_tile_selection_child_traversal
    unit/tiling/test_tile_selection_child_traversal.cpp
)
target_link_libraries(test_tile_selection_child_traversal PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_child_traversal COMMAND test_tile_selection_child_traversal)

add_executable(test_tile_selection_history
    unit/tiling/test_tile_selection_history.cpp
)
target_link_libraries(test_tile_selection_history PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_history COMMAND test_tile_selection_history)

add_executable(test_tile_selection_plan_appender
    unit/tiling/test_tile_selection_plan_appender.cpp
)
target_link_libraries(test_tile_selection_plan_appender PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_plan_appender COMMAND test_tile_selection_plan_appender)

add_executable(test_tile_selection_frame_builder
    unit/tiling/test_tile_selection_frame_builder.cpp
)
target_link_libraries(test_tile_selection_frame_builder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_frame_builder COMMAND test_tile_selection_frame_builder)

add_executable(test_tile_selection_frame_finalizer
    unit/tiling/test_tile_selection_frame_finalizer.cpp
)
target_link_libraries(test_tile_selection_frame_finalizer PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_frame_finalizer COMMAND test_tile_selection_frame_finalizer)

add_executable(test_tile_selection_render_entry_policy
    unit/tiling/test_tile_selection_render_entry_policy.cpp
)
target_link_libraries(test_tile_selection_render_entry_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_render_entry_policy COMMAND test_tile_selection_render_entry_policy)

add_executable(test_tile_render_plan_finalizer
    unit/tiling/test_tile_render_plan_finalizer.cpp
)
target_link_libraries(test_tile_render_plan_finalizer PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_render_plan_finalizer COMMAND test_tile_render_plan_finalizer)

add_executable(test_tile_render_entry
    unit/tiling/test_tile_render_entry.cpp
)
target_link_libraries(test_tile_render_entry PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_render_entry COMMAND test_tile_render_entry)

add_executable(test_tile_render_reference_state
    unit/tiling/test_tile_render_reference_state.cpp
)
target_link_libraries(test_tile_render_reference_state PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_render_reference_state COMMAND test_tile_render_reference_state)

add_executable(test_tile_render_entry_command_builder
    unit/tiling/test_tile_render_entry_command_builder.cpp
)
target_link_libraries(test_tile_render_entry_command_builder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_render_entry_command_builder COMMAND test_tile_render_entry_command_builder)

add_executable(test_tile_render_command_preparer
    unit/tiling/test_tile_render_command_preparer.cpp
)
target_link_libraries(test_tile_render_command_preparer PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_render_command_preparer COMMAND test_tile_render_command_preparer)

add_executable(test_tile_render_frame_builder
    unit/tiling/test_tile_render_frame_builder.cpp
)
target_link_libraries(test_tile_render_frame_builder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_render_frame_builder COMMAND test_tile_render_frame_builder)

add_executable(test_tile_render_frame_maintenance
    unit/tiling/test_tile_render_frame_maintenance.cpp
)
target_link_libraries(test_tile_render_frame_maintenance PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_render_frame_maintenance COMMAND test_tile_render_frame_maintenance)

add_executable(test_tile_render_frame_coordinator
    unit/tiling/test_tile_render_frame_coordinator.cpp
)
target_link_libraries(test_tile_render_frame_coordinator PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_render_frame_coordinator COMMAND test_tile_render_frame_coordinator)

add_executable(test_tile_selection_root_policy
    unit/tiling/test_tile_selection_root_policy.cpp
)
target_link_libraries(test_tile_selection_root_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_root_policy COMMAND test_tile_selection_root_policy)

add_executable(test_tile_selection_reset_policy
    unit/tiling/test_tile_selection_reset_policy.cpp
)
target_link_libraries(test_tile_selection_reset_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_reset_policy COMMAND test_tile_selection_reset_policy)

add_executable(test_tile_selection_summary_policy
    unit/tiling/test_tile_selection_summary_policy.cpp
)
target_link_libraries(test_tile_selection_summary_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_summary_policy COMMAND test_tile_selection_summary_policy)

add_executable(test_tile_selection_summary_builder
    unit/tiling/test_tile_selection_summary_builder.cpp
)
target_link_libraries(test_tile_selection_summary_builder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_summary_builder COMMAND test_tile_selection_summary_builder)

add_executable(test_tile_selection_state_resetter
    unit/tiling/test_tile_selection_state_resetter.cpp
)
target_link_libraries(test_tile_selection_state_resetter PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_state_resetter COMMAND test_tile_selection_state_resetter)

add_executable(test_tile_selection_refinement_policies
    unit/tiling/test_tile_selection_refinement_policies.cpp
)
target_link_libraries(test_tile_selection_refinement_policies PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_refinement_policies COMMAND test_tile_selection_refinement_policies)

add_executable(test_tile_selection_input_metrics
    unit/tiling/test_tile_selection_input_metrics.cpp
)
target_link_libraries(test_tile_selection_input_metrics PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_input_metrics COMMAND test_tile_selection_input_metrics)

add_executable(test_tile_selection_culling_policy
    unit/tiling/test_tile_selection_culling_policy.cpp
)
target_link_libraries(test_tile_selection_culling_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_culling_policy COMMAND test_tile_selection_culling_policy)

add_executable(test_tile_selection_traversal_counters
    unit/tiling/test_tile_selection_traversal_counters.cpp
)
target_link_libraries(test_tile_selection_traversal_counters PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_traversal_counters COMMAND test_tile_selection_traversal_counters)

add_executable(test_tile_selection_traversal_context_builder
    unit/tiling/test_tile_selection_traversal_context_builder.cpp
)
target_link_libraries(test_tile_selection_traversal_context_builder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_traversal_context_builder COMMAND test_tile_selection_traversal_context_builder)

add_executable(test_tile_selection_visit_preparation
    unit/tiling/test_tile_selection_visit_preparation.cpp
)
target_link_libraries(test_tile_selection_visit_preparation PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_visit_preparation COMMAND test_tile_selection_visit_preparation)

add_executable(test_tile_selection_visibility_sampler
    unit/tiling/test_tile_selection_visibility_sampler.cpp
)
target_link_libraries(test_tile_selection_visibility_sampler PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_visibility_sampler COMMAND test_tile_selection_visibility_sampler)

add_executable(test_tile_viewer_request_volume_policy
    unit/tiling/test_tile_viewer_request_volume_policy.cpp
)
target_link_libraries(test_tile_viewer_request_volume_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_viewer_request_volume_policy COMMAND test_tile_viewer_request_volume_policy)

add_executable(test_tile_selection_reuse_policy
    unit/tiling/test_tile_selection_reuse_policy.cpp
)
target_link_libraries(test_tile_selection_reuse_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_reuse_policy COMMAND test_tile_selection_reuse_policy)

add_executable(test_tile_frame_debug_log_formatter
    unit/tiling/test_tile_frame_debug_log_formatter.cpp
)
target_link_libraries(test_tile_frame_debug_log_formatter PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_frame_debug_log_formatter COMMAND test_tile_frame_debug_log_formatter)

add_executable(test_tile_update_selection_work_runner
    unit/tiling/test_tile_update_selection_work_runner.cpp
)
target_link_libraries(test_tile_update_selection_work_runner PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_update_selection_work_runner COMMAND test_tile_update_selection_work_runner)

add_executable(test_tile_frame_work_coordinator
    unit/tiling/test_tile_frame_work_coordinator.cpp
)
target_link_libraries(test_tile_frame_work_coordinator PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_frame_work_coordinator COMMAND test_tile_frame_work_coordinator)

add_executable(test_tile_missing_request_scheduler
    unit/tiling/test_tile_missing_request_scheduler.cpp
)
target_link_libraries(test_tile_missing_request_scheduler PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_missing_request_scheduler COMMAND test_tile_missing_request_scheduler)

add_executable(test_tileset_request_missing_budget
    unit/tiling/test_tileset_request_missing_budget.cpp
)
target_link_libraries(test_tileset_request_missing_budget PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tileset_request_missing_budget COMMAND test_tileset_request_missing_budget)

add_executable(test_tileset_lifecycle
    unit/tiling/test_tileset_lifecycle.cpp
)
target_link_libraries(test_tileset_lifecycle PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tileset_lifecycle COMMAND test_tileset_lifecycle)

add_executable(test_tileset_selection_refinement
    unit/tiling/test_tileset_selection_refinement.cpp
)
target_link_libraries(test_tileset_selection_refinement PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tileset_selection_refinement COMMAND test_tileset_selection_refinement)

add_executable(test_zoom_out_base_coverage
    unit/tiling/test_zoom_out_base_coverage.cpp
)
target_link_libraries(test_zoom_out_base_coverage PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_zoom_out_base_coverage COMMAND test_zoom_out_base_coverage)

# selector 对拍回放测试：include 共享场景头，golden 路径编译期注入
# （golden 由 tools/selector_diff/cesium_golden_gen 离线生成后提交入库）
add_executable(test_selector_cesium_golden_diff
    unit/tiling/test_selector_cesium_golden_diff.cpp
)
target_include_directories(test_selector_cesium_golden_diff PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/selector_diff
)
target_compile_definitions(test_selector_cesium_golden_diff PRIVATE
    SELECTOR_DIFF_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/selector_diff/golden"
)
target_link_libraries(test_selector_cesium_golden_diff PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_selector_cesium_golden_diff COMMAND test_selector_cesium_golden_diff)


add_executable(test_tile_visible_range_finalizer
    unit/tiling/test_tile_visible_range_finalizer.cpp
)
target_link_libraries(test_tile_visible_range_finalizer PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_visible_range_finalizer COMMAND test_tile_visible_range_finalizer)

add_executable(test_tile_occlusion_resolver
    unit/tiling/test_tile_occlusion_resolver.cpp
)
target_link_libraries(test_tile_occlusion_resolver PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_occlusion_resolver COMMAND test_tile_occlusion_resolver)

add_executable(test_tile_software_occlusion
    unit/tiling/test_tile_software_occlusion.cpp
)
target_link_libraries(test_tile_software_occlusion PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_software_occlusion COMMAND test_tile_software_occlusion)

add_executable(test_tile_frame_resource_budget_planner
    unit/tiling/test_tile_frame_resource_budget_planner.cpp
)
target_link_libraries(test_tile_frame_resource_budget_planner PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_frame_resource_budget_planner COMMAND test_tile_frame_resource_budget_planner)

add_executable(test_tileset_provider_diagnostics_collector
    unit/tiling/test_tileset_provider_diagnostics_collector.cpp
)
target_link_libraries(test_tileset_provider_diagnostics_collector PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tileset_provider_diagnostics_collector COMMAND test_tileset_provider_diagnostics_collector)

add_executable(test_camera
    unit/scene/test_camera.cpp
)
target_link_libraries(test_camera PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera COMMAND test_camera)

add_executable(test_dynamic_near_plane
    unit/scene/test_dynamic_near_plane.cpp
)
target_link_libraries(test_dynamic_near_plane PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_dynamic_near_plane COMMAND test_dynamic_near_plane)

add_executable(test_frustum
    unit/scene/test_frustum.cpp
)
target_link_libraries(test_frustum PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_frustum COMMAND test_frustum)

add_executable(test_scene_tileset_diagnostics
    unit/scene/test_scene_tileset_diagnostics.cpp
)
target_link_libraries(test_scene_tileset_diagnostics PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_scene_tileset_diagnostics COMMAND test_scene_tileset_diagnostics)

add_executable(test_scene_telemetry_coordinator
    unit/scene/test_scene_telemetry_coordinator.cpp
)
target_link_libraries(test_scene_telemetry_coordinator PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_scene_telemetry_coordinator COMMAND test_scene_telemetry_coordinator)

add_executable(test_scene_render_diagnostics
    unit/scene/test_scene_render_diagnostics.cpp
)
target_link_libraries(test_scene_render_diagnostics PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_scene_render_diagnostics COMMAND test_scene_render_diagnostics)

add_executable(test_scene_frame_state
    unit/scene/test_scene_frame_state.cpp
)
target_link_libraries(test_scene_frame_state PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_scene_frame_state COMMAND test_scene_frame_state)

add_executable(test_camera_system
    unit/camera/test_camera_system.cpp
)
target_link_libraries(test_camera_system PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_system COMMAND test_camera_system)

add_executable(test_desktop_input_binding
    unit/interaction/test_desktop_input_binding.cpp
)
target_link_libraries(test_desktop_input_binding PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_desktop_input_binding COMMAND test_desktop_input_binding)

add_executable(test_camera_orthographic
    unit/camera/test_camera_orthographic.cpp
)
target_link_libraries(test_camera_orthographic PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_orthographic COMMAND test_camera_orthographic)

add_executable(test_camera_tethered
    unit/camera/test_camera_tethered.cpp
)
target_link_libraries(test_camera_tethered PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_tethered COMMAND test_camera_tethered)

add_executable(test_camera_flight
    unit/camera/test_camera_flight.cpp
)
target_link_libraries(test_camera_flight PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_flight COMMAND test_camera_flight)

add_executable(test_camera_viewpoint
    unit/camera/test_camera_viewpoint.cpp
)
target_link_libraries(test_camera_viewpoint PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_viewpoint COMMAND test_camera_viewpoint)

add_executable(test_camera_controller_selector
    unit/camera/test_camera_controller_selector.cpp
)
target_link_libraries(test_camera_controller_selector PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_controller_selector COMMAND test_camera_controller_selector)

add_executable(test_camera_pose_trace
    unit/camera/test_camera_pose_trace.cpp
)
target_link_libraries(test_camera_pose_trace PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_pose_trace COMMAND test_camera_pose_trace)

add_executable(test_pin_anchor_error
    unit/camera/test_pin_anchor_error.cpp
)
target_link_libraries(test_pin_anchor_error PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_pin_anchor_error COMMAND test_pin_anchor_error)

add_executable(test_camera_collision_climb
    unit/camera/test_camera_collision_climb.cpp
)
target_link_libraries(test_camera_collision_climb PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_camera_collision_climb COMMAND test_camera_collision_climb)

add_executable(test_earth_scene_config
    unit/sdk/test_earth_scene_config.cpp
)
target_link_libraries(test_earth_scene_config PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_earth_scene_config COMMAND test_earth_scene_config)

add_executable(test_raster_overlay_screen_space_metrics
    unit/tiling/test_raster_overlay_screen_space_metrics.cpp
)
target_link_libraries(test_raster_overlay_screen_space_metrics PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_raster_overlay_screen_space_metrics COMMAND test_raster_overlay_screen_space_metrics)

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

add_executable(test_terrain_depth_prepass
    unit/renderer/test_terrain_depth_prepass.cpp
)
target_link_libraries(test_terrain_depth_prepass PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_depth_prepass COMMAND test_terrain_depth_prepass)

add_executable(test_terrain_shader_command
    unit/renderer/test_terrain_shader_command.cpp
)
target_link_libraries(test_terrain_shader_command PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_shader_command COMMAND test_terrain_shader_command)

add_executable(test_virtual_texture_page
    unit/renderer/test_virtual_texture_page.cpp
)
target_link_libraries(test_virtual_texture_page PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_virtual_texture_page COMMAND test_virtual_texture_page)

add_executable(test_virtual_texture_poc
    unit/renderer/test_virtual_texture_poc.cpp
)
target_link_libraries(test_virtual_texture_poc PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_virtual_texture_poc COMMAND test_virtual_texture_poc)

add_executable(test_tile_composite_bake_poc
    unit/renderer/test_tile_composite_bake_poc.cpp
)
target_link_libraries(test_tile_composite_bake_poc PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_composite_bake_poc COMMAND test_tile_composite_bake_poc)

add_executable(test_work_ledger
    unit/core/test_work_ledger.cpp
)
target_link_libraries(test_work_ledger PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_work_ledger COMMAND test_work_ledger)

add_executable(test_terrain_page_store
    unit/renderer/test_terrain_page_store.cpp
)
target_link_libraries(test_terrain_page_store PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_terrain_page_store COMMAND test_terrain_page_store)

add_executable(test_vt_indirection_sample_poc
    unit/renderer/test_vt_indirection_sample_poc.cpp
)
target_link_libraries(test_vt_indirection_sample_poc PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_vt_indirection_sample_poc COMMAND test_vt_indirection_sample_poc)

add_executable(test_gltf_draw_command_cache
    unit/tiling/test_gltf_draw_command_cache.cpp
)
target_link_libraries(test_gltf_draw_command_cache PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_gltf_draw_command_cache COMMAND test_gltf_draw_command_cache)

# ============================================================
# stage 5 tests
# ============================================================

add_executable(test_geojson_parser
    unit/data/test_geojson_parser.cpp
)
target_link_libraries(test_geojson_parser PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_geojson_parser COMMAND test_geojson_parser)

add_executable(test_feature_spatial_index
    unit/data/test_feature_spatial_index.cpp
)
target_link_libraries(test_feature_spatial_index PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_feature_spatial_index COMMAND test_feature_spatial_index)

add_executable(test_feature_store
    unit/data/test_feature_store.cpp
)
target_link_libraries(test_feature_store PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_feature_store COMMAND test_feature_store)

add_executable(test_feature_cluster_index
    unit/data/test_feature_cluster_index.cpp
)
target_link_libraries(test_feature_cluster_index PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_feature_cluster_index COMMAND test_feature_cluster_index)

add_executable(test_style_expression
    unit/data/test_style_expression.cpp
)
target_link_libraries(test_style_expression PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_style_expression COMMAND test_style_expression)

# V26 二期:样式文档(A 案精简子集)——parser fail-loud / filter 对拍 /
# 编译契约(场单色/封顶推导)/ 成本类路由(Uniform 不触发 Re-bake)。
add_executable(test_style_document
    unit/style/test_style_document.cpp
)
target_link_libraries(test_style_document PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_style_document COMMAND test_style_document)

add_executable(test_feature_bucket_grid
    unit/data/test_feature_bucket_grid.cpp
)
target_link_libraries(test_feature_bucket_grid PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_feature_bucket_grid COMMAND test_feature_bucket_grid)

add_executable(test_mvt_decoder
    unit/data/test_mvt_decoder.cpp
)
target_link_libraries(test_mvt_decoder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_mvt_decoder COMMAND test_mvt_decoder)

add_executable(test_line_field_rasterizer
    unit/data/test_line_field_rasterizer.cpp
)
target_link_libraries(test_line_field_rasterizer PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_line_field_rasterizer COMMAND test_line_field_rasterizer)

add_executable(test_vector_tile_rasterizer
    unit/data/test_vector_tile_rasterizer.cpp
)
target_link_libraries(test_vector_tile_rasterizer PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_vector_tile_rasterizer COMMAND test_vector_tile_rasterizer)

add_executable(test_vector_tile_tree
    unit/data/test_vector_tile_tree.cpp
)
target_link_libraries(test_vector_tile_tree PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_vector_tile_tree COMMAND test_vector_tile_tree)

add_executable(test_mvt_feature_converter
    unit/data/test_mvt_feature_converter.cpp
)
target_link_libraries(test_mvt_feature_converter PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_mvt_feature_converter COMMAND test_mvt_feature_converter)

add_executable(test_vector_tile_mesh_builder
    unit/data/test_vector_tile_mesh_builder.cpp
)
target_link_libraries(test_vector_tile_mesh_builder PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_vector_tile_mesh_builder COMMAND test_vector_tile_mesh_builder)

add_executable(test_style_filter
    unit/data/test_style_filter.cpp
)
target_link_libraries(test_style_filter PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_style_filter COMMAND test_style_filter)

add_executable(test_mvt_vector_source
    unit/data/test_mvt_vector_source.cpp
)
target_link_libraries(test_mvt_vector_source PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_mvt_vector_source COMMAND test_mvt_vector_source)

add_executable(test_constrained_delaunay
    unit/data/test_constrained_delaunay.cpp
)
target_link_libraries(test_constrained_delaunay PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_constrained_delaunay COMMAND test_constrained_delaunay)

add_executable(test_polygon_tessellator
    unit/data/test_polygon_tessellator.cpp
)
target_link_libraries(test_polygon_tessellator PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_polygon_tessellator COMMAND test_polygon_tessellator)

add_executable(test_line_tessellator
    unit/data/test_line_tessellator.cpp
)
target_link_libraries(test_line_tessellator PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_line_tessellator COMMAND test_line_tessellator)

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

add_executable(test_contracts
    unit/debug/test_contracts.cpp
)
target_link_libraries(test_contracts PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_contracts COMMAND test_contracts)

add_executable(test_minimal_globe_diagnostics
    unit/debug/test_minimal_globe_diagnostics.cpp
    ../examples/android/MinimalGlobe/MinimalGlobeDiagnostics.cpp
)
target_link_libraries(test_minimal_globe_diagnostics PRIVATE earth_engine_core GTest::gtest_main)
target_include_directories(test_minimal_globe_diagnostics PRIVATE
    ../examples/android/MinimalGlobe
)
add_test(NAME test_minimal_globe_diagnostics COMMAND test_minimal_globe_diagnostics)

# 建筑/次级路 zoom+距离分级:测真配置函数(makeMvtDrapeStyle/
# makeMvtRoadFieldStyle),锁定分级门槛不被改坏。
add_executable(test_mvt_basemap_grading
    unit/data/test_mvt_basemap_grading.cpp
    ../examples/android/MinimalGlobe/MinimalGlobeDemoConfig.cpp
)
target_link_libraries(test_mvt_basemap_grading PRIVATE earth_engine_core GTest::gtest_main)
target_include_directories(test_mvt_basemap_grading PRIVATE
    ../examples/android/MinimalGlobe
)
add_test(NAME test_mvt_basemap_grading COMMAND test_mvt_basemap_grading)

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

add_executable(test_heightmap_terrain
    unit/content/test_heightmap_terrain.cpp
)
target_link_libraries(test_heightmap_terrain PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_heightmap_terrain COMMAND test_heightmap_terrain)

add_executable(test_road_field_source
    unit/providers/test_road_field_source.cpp
)
target_link_libraries(test_road_field_source PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_road_field_source COMMAND test_road_field_source)

add_executable(test_vector_drape_provider
    unit/providers/test_vector_drape_provider.cpp
)
target_link_libraries(test_vector_drape_provider PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_vector_drape_provider COMMAND test_vector_drape_provider)

add_executable(test_raster_overlay_lifecycle
    unit/providers/test_raster_overlay_lifecycle.cpp
)
target_link_libraries(test_raster_overlay_lifecycle PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_raster_overlay_lifecycle COMMAND test_raster_overlay_lifecycle)

add_executable(test_raster_hole_update_cost
    unit/providers/test_raster_hole_update_cost.cpp
)
target_link_libraries(test_raster_hole_update_cost PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_raster_hole_update_cost COMMAND test_raster_hole_update_cost)

add_executable(test_web_map_service_imagery_provider
    unit/providers/test_web_map_service_imagery_provider.cpp
)
target_link_libraries(test_web_map_service_imagery_provider PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_web_map_service_imagery_provider COMMAND test_web_map_service_imagery_provider)

add_executable(test_provider_request_diagnostics_aggregator
    unit/providers/test_provider_request_diagnostics_aggregator.cpp
)
target_link_libraries(test_provider_request_diagnostics_aggregator PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_provider_request_diagnostics_aggregator COMMAND test_provider_request_diagnostics_aggregator)

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

add_executable(test_sunset_warmth_ramp
    unit/environment/test_sunset_warmth_ramp.cpp
)
target_link_libraries(test_sunset_warmth_ramp PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_sunset_warmth_ramp COMMAND test_sunset_warmth_ramp)

add_executable(test_sky_color_model
    unit/environment/test_sky_color_model.cpp
)
target_link_libraries(test_sky_color_model PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_sky_color_model COMMAND test_sky_color_model)

add_executable(test_post_process_model
    unit/renderer/test_post_process_model.cpp
)
target_link_libraries(test_post_process_model PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_post_process_model COMMAND test_post_process_model)

# ============================================================
# 文档一致性(非 C++ 测试)
# ============================================================

# AI_INDEX.md 与源码的一致性,两类检查(性质不同,见脚本头注释):
#   A 行号引用:`File.cpp:1234` 是否命中它声称的符号。已知例外记在
#     tools/ai_index_refs_baseline.txt(每条带理由),新增漂移才 FAIL。
#   B 索引覆盖:>300 行的 .cpp/.mm 必须有专属 ### 小节。A 对这类失效免疫 ——
#     它只校验写下的引用,查不出整个文件从未被收录。
# ⚠️ 两者都**不查文字描述是否属实**:条目在、行号对,但描述的东西已被重构走,
#   只能靠人读源码。测试通过 ≠ AI_INDEX 是对的。
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    # 管线特性契约守卫:同一特性在逐瓦/合批/instanced × GLES/Metal 的搬运语句
    # 是否全数落位(单一治理点 / uniform 表 / apply 写入 / 实例流拷贝 / MSL 镜像
    # 同序)。历史上两次单管线静默漏配(9043e20fe / 80892aca1)的回归锁。
    add_test(
        NAME test_pipeline_feature_contracts
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_pipeline_feature_contracts.py
                --src ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    add_test(
        NAME test_ai_index_refs
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_ai_index_refs.py
                --doc ${CMAKE_CURRENT_SOURCE_DIR}/../../AI_INDEX.md
                --src ${CMAKE_CURRENT_SOURCE_DIR}/../src
                --baseline
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ai_index_refs_baseline.txt)
    # HDR 管线开启态防腐守卫:HDR 决策=长期做/短期挂起(northstar/lighting.md L-V5),
    # 挂起期默认关+仅 GLES+无设备回归 → 编不到/跑不到/测不到,churn 会静默改坏。
    # 本守卫查结构完整(分支/锚点/HDR 变体常量),不查 GLSL 语法(glslc 本机缺失)。
    add_test(
        NAME test_hdr_variant_integrity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_hdr_variant_integrity.py
                --src ${CMAKE_CURRENT_SOURCE_DIR}/../src)
else()
    message(STATUS "未找到 Python3,跳过 test_ai_index_refs / test_hdr_variant_integrity")
endif()
