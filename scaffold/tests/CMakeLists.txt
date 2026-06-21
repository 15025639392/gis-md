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

add_executable(test_projection
    unit/geodesy/test_projection.cpp
)
target_link_libraries(test_projection PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_projection COMMAND test_projection)

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

add_executable(test_tile_selection_render_entry_policy
    unit/tiling/test_tile_selection_render_entry_policy.cpp
)
target_link_libraries(test_tile_selection_render_entry_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_render_entry_policy COMMAND test_tile_selection_render_entry_policy)

add_executable(test_tile_selection_reset_policy
    unit/tiling/test_tile_selection_reset_policy.cpp
)
target_link_libraries(test_tile_selection_reset_policy PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_selection_reset_policy COMMAND test_tile_selection_reset_policy)

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

add_executable(test_tile_lod_transitions
    unit/tiling/test_tile_lod_transitions.cpp
)
target_link_libraries(test_tile_lod_transitions PRIVATE earth_engine_core GTest::gtest_main)
add_test(NAME test_tile_lod_transitions COMMAND test_tile_lod_transitions)

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
