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
    earth_engine/core/math/MathUtils.h
    earth_engine/core/math/AttributeCompression.h
    earth_engine/core/math/Mat4.h
    earth_engine/core/math/Mat4.cpp
    earth_engine/core/math/Ray.h
    earth_engine/core/math/Ray.cpp
    earth_engine/core/math/RayEllipsoidIntersectionInterval.h
    earth_engine/core/math/ClipTriangleAtAxisAlignedThreshold.h
    earth_engine/core/math/ClipTriangleAtAxisAlignedThreshold.cpp
    earth_engine/core/math/IntersectionTests.h
    earth_engine/core/math/IntersectionTests.cpp
    earth_engine/core/math/Rectangle.h
    earth_engine/core/math/Rectangle.cpp
    earth_engine/core/math/Plane.h
    earth_engine/core/math/Plane.cpp
    earth_engine/core/math/CullingVolume.h
    earth_engine/core/math/CullingVolume.cpp
    earth_engine/core/math/AxisAlignedBox.h
    earth_engine/core/math/AxisAlignedBox.cpp
    earth_engine/core/math/BoundingSphere.h
    earth_engine/core/math/OrientedBoundingBox.h
    earth_engine/core/math/BoundingCylinderRegion.h
    earth_engine/core/math/BoundingCylinderRegion.cpp
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
    earth_engine/core/cache/SharedAssetDepot.h
)

# ============================================================
# core/resources
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/core/resources/FrameResourceBudget.h
    earth_engine/core/resources/FrameResourceBudget.cpp
)

# ============================================================
# core/geodesy
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/core/geodesy/Cartographic.h
    earth_engine/core/geodesy/Cartographic.cpp
    earth_engine/core/geodesy/EllipsoidTangentPlane.h
    earth_engine/core/geodesy/EllipsoidTangentPlane.cpp
    earth_engine/core/geodesy/BoundingRegionBuilder.h
    earth_engine/core/geodesy/BoundingRegionBuilder.cpp
    earth_engine/core/geodesy/Ellipsoid.h
    earth_engine/core/geodesy/Ellipsoid.cpp
    earth_engine/core/geodesy/GeographicProjection.h
    earth_engine/core/geodesy/GeographicProjection.cpp
    earth_engine/core/geodesy/WebMercatorProjection.h
    earth_engine/core/geodesy/WebMercatorProjection.cpp
    earth_engine/core/geodesy/Projection.h
    earth_engine/core/geodesy/Projection.cpp
    earth_engine/core/geodesy/QuadtreeGeometricError.h
    earth_engine/core/geodesy/QuadtreeGeometricError.cpp
    earth_engine/core/geodesy/S2CellBoundingVolume.h
    earth_engine/core/geodesy/S2CellBoundingVolume.cpp
    earth_engine/core/geodesy/S2CellID.h
    earth_engine/core/geodesy/S2CellID.cpp
    earth_engine/core/geodesy/SimplePlanarEllipsoidCurve.h
    earth_engine/core/geodesy/SimplePlanarEllipsoidCurve.cpp
    earth_engine/core/geodesy/Transforms.h
    earth_engine/core/geodesy/Transforms.cpp
)

# ============================================================
# core/net
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/core/net/Uri.h
    earth_engine/core/net/Uri.cpp
)

# ============================================================
# globe
# ============================================================

# ============================================================
# content
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/content/GltfExtensions.h
    earth_engine/content/GltfModel.h
    earth_engine/content/GltfModel.cpp
    earth_engine/content/GltfContentProvider.h
    earth_engine/content/GltfContentProvider.cpp
    earth_engine/content/EllipsoidTerrainContentProvider.h
    earth_engine/content/EllipsoidTerrainContentProvider.cpp
    earth_engine/content/EllipsoidTerrainMeshBuilder.h
    earth_engine/content/EllipsoidTerrainMeshBuilder.cpp
    earth_engine/content/HeightmapTerrainContentProvider.h
    earth_engine/content/HeightmapTerrainContentProvider.cpp
    earth_engine/content/CompositeTerrainProvider.h
    earth_engine/content/CompositeTerrainProvider.cpp
    earth_engine/content/GltfTerrainUpsampler.h
    earth_engine/content/GltfTerrainUpsampler.cpp
    earth_engine/content/QuantizedMeshContentLoader.h
    earth_engine/content/QuantizedMeshContentLoader.cpp
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
    earth_engine/renderer/OffscreenPostProcess.h
    earth_engine/renderer/OffscreenPostProcess.cpp
    earth_engine/renderer/RenderDeviceRasterTextureUploader.h
    earth_engine/renderer/RenderDeviceRasterTextureUploader.cpp
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
    earth_engine/scene/Diagnostics.h
    earth_engine/scene/EngineTimingScope.h
    earth_engine/scene/Frustum.h
    earth_engine/scene/Frustum.cpp
    earth_engine/scene/SelectorView.h
    earth_engine/scene/FrameState.h
    earth_engine/scene/SceneSelectorViewBuilder.h
    earth_engine/scene/SceneSelectorViewBuilder.cpp
    earth_engine/scene/PresentationTrace.h
    earth_engine/scene/SceneEnvironmentCoordinator.h
    earth_engine/scene/SceneEnvironmentCoordinator.cpp
    earth_engine/scene/SceneFrameDiagnostics.h
    earth_engine/scene/SceneFrameDiagnostics.cpp
    earth_engine/scene/SceneFrameDiagnosticsAggregator.h
    earth_engine/scene/SceneFrameDiagnosticsAggregator.cpp
    earth_engine/scene/SceneFrameStateBuilder.h
    earth_engine/scene/SceneFrameStateBuilder.cpp
    earth_engine/scene/SceneFrameRuntime.h
    earth_engine/scene/SceneFrameRuntime.cpp
    earth_engine/scene/SceneFrameUpdateCoordinator.h
    earth_engine/scene/SceneFrameUpdateCoordinator.cpp
    earth_engine/scene/SceneInputCoordinator.h
    earth_engine/scene/SceneInputCoordinator.cpp
    earth_engine/scene/SceneInteractionCoordinator.h
    earth_engine/scene/SceneInteractionCoordinator.cpp
    earth_engine/scene/SceneLayerCoordinator.h
    earth_engine/scene/SceneLayerCoordinator.cpp
    earth_engine/scene/ScenePickingCoordinator.h
    earth_engine/scene/ScenePickingCoordinator.cpp
    earth_engine/scene/SceneTerrainQuery.h
    earth_engine/scene/SceneTerrainQuery.cpp
    earth_engine/scene/ScenePresentationTraceBuilder.h
    earth_engine/scene/ScenePresentationTraceBuilder.cpp
    earth_engine/scene/ScenePrimaryTilesetTakeoverPolicy.h
    earth_engine/scene/ScenePrimaryTilesetTakeoverPolicy.cpp
    earth_engine/scene/ScenePrimaryTilesetRenderComposer.h
    earth_engine/scene/ScenePrimaryTilesetRenderComposer.cpp
    earth_engine/scene/SceneTilesetCoordinator.h
    earth_engine/scene/SceneTilesetCoordinator.cpp
    earth_engine/scene/SceneRenderCommandUniformUpdater.h
    earth_engine/scene/SceneRenderCommandUniformUpdater.cpp
    earth_engine/scene/SceneRenderPipeline.h
    earth_engine/scene/SceneRenderPipeline.cpp
    earth_engine/scene/SceneRenderDiagnostics.h
    earth_engine/scene/SceneRenderDiagnostics.cpp
    earth_engine/scene/SceneTelemetryCoordinator.h
    earth_engine/scene/SceneTelemetryCoordinator.cpp
    earth_engine/scene/SceneTilesetDiagnostics.h
    earth_engine/scene/SceneTilesetDiagnostics.cpp
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
# sdk
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/sdk/EarthSceneConfig.h
    earth_engine/sdk/EarthEngineSdkFacade.h
    earth_engine/sdk/EarthEngineSdkFacade.cpp
)

# ============================================================
# tiling
# ============================================================
target_sources(earth_engine_core PRIVATE
    earth_engine/tiling/CrsProfile.h
    earth_engine/tiling/CrsProfile.cpp
    earth_engine/tiling/DecodedHeightmapSampler.h
    earth_engine/tiling/DecodedHeightmapSampler.cpp
    earth_engine/tiling/GltfDrawCommandBuilder.h
    earth_engine/tiling/GltfDrawCommandBuilder.cpp
    earth_engine/tiling/GltfRenderGeometryBuilder.h
    earth_engine/tiling/GltfRenderGeometryBuilder.cpp
    earth_engine/tiling/GltfRenderResourcePreparer.h
    earth_engine/tiling/GltfRenderResourcePreparer.cpp
    earth_engine/tiling/TileFillProxyPreparer.h
    earth_engine/tiling/TileFillProxyPreparer.cpp
    earth_engine/tiling/TileGeomorphHeightDelta.h
    earth_engine/tiling/TileGeomorphHeightDelta.cpp
    earth_engine/tiling/LoadedTerrainHeightSampler.h
    earth_engine/tiling/LoadedTerrainHeightSampler.cpp
    earth_engine/tiling/RasterOverlayScreenSpaceMetrics.h
    earth_engine/tiling/RasterOverlayScreenSpaceMetrics.cpp
    earth_engine/tiling/RenderContentRasterOverlayStateUpdater.h
    earth_engine/tiling/RenderContentRasterOverlayStateUpdater.cpp
    earth_engine/tiling/TileRasterOverlayDetailsGenerator.h
    earth_engine/tiling/TileRasterOverlayDetailsGenerator.cpp
    earth_engine/tiling/TileKey.h
    earth_engine/tiling/TileKey.cpp
    earth_engine/tiling/SchemeId.h
    earth_engine/tiling/SchemeId.cpp
    earth_engine/tiling/TileID.h
    earth_engine/tiling/TileID.cpp
    earth_engine/tiling/ImplicitTileIdUtilities.h
    earth_engine/tiling/ImplicitTileIdUtilities.cpp
    earth_engine/tiling/OctreeTilingScheme.h
    earth_engine/tiling/OctreeTilingScheme.cpp
    earth_engine/tiling/QuadtreeTilingScheme.h
    earth_engine/tiling/QuadtreeTilingScheme.cpp
    earth_engine/tiling/TileLoadState.h
    earth_engine/tiling/TileContentUpsampleKind.h
    earth_engine/tiling/TileRefine.h
    earth_engine/tiling/TileAvailability.h
    earth_engine/tiling/TileAvailability.cpp
    earth_engine/tiling/TileBoundingVolume.h
    earth_engine/tiling/TileBoundingVolume.cpp
    earth_engine/tiling/TileScheme.h
    earth_engine/tiling/TileScheme.cpp
    earth_engine/tiling/SurfaceTile.h
    earth_engine/tiling/SurfaceRasterBinding.h
    earth_engine/tiling/SurfaceRasterBinding.cpp
    earth_engine/tiling/TileSurface.h
    earth_engine/tiling/TileSurface.cpp
    earth_engine/tiling/TilePlan.h
    earth_engine/tiling/TilePlan.cpp
    earth_engine/tiling/TileChildFrameMaterializer.h
    earth_engine/tiling/TileChildMaterializer.h
    earth_engine/tiling/TileContentUnloadCoordinator.h
    earth_engine/tiling/TileRasterUpsampledChildMaterializer.h
    earth_engine/tiling/TileLodTransitionController.h
    earth_engine/tiling/TileLodTransitionFrameUpdater.h
    earth_engine/tiling/TileLodTransitionFrameUpdater.cpp
    earth_engine/tiling/TileRenderPlanFrameRefresher.h
    earth_engine/tiling/TileRenderPlanFrameRefresher.cpp
    earth_engine/tiling/TileRenderPlanFinalizer.h
    earth_engine/tiling/TileContentUploadPolicy.h
    earth_engine/tiling/TileContentUploadPolicy.cpp
    earth_engine/tiling/TileContentStateTransition.h
    earth_engine/tiling/TileContentUploadCommitter.h
    earth_engine/tiling/TileContentUploadCommitter.cpp
    earth_engine/tiling/TileAvailabilityUpdateCommitter.h
    earth_engine/tiling/TileContentCacheManager.h
    earth_engine/tiling/TileContentCacheManager.cpp
    earth_engine/tiling/TileContentResourceInvalidator.h
    earth_engine/tiling/TileContentResourceInvalidator.cpp
    earth_engine/tiling/TileContentRuntime.h
    earth_engine/tiling/TileContentRuntime.cpp
    earth_engine/tiling/TileCacheOwnershipManager.h
    earth_engine/tiling/TileCacheOwnershipManager.cpp
    earth_engine/tiling/TileContentAccess.h
    earth_engine/tiling/TileContentAccess.cpp
    earth_engine/tiling/TileContentLifecycleManager.h
    earth_engine/tiling/TileContentLifecycleManager.cpp
    earth_engine/tiling/TilesetContentLifecycleCoordinator.h
    earth_engine/tiling/TileEmptyContentRegistry.h
    earth_engine/tiling/TileEmptyContentRegistry.cpp
    earth_engine/tiling/TileCacheKey.h
    earth_engine/tiling/TileCreationPolicy.h
    earth_engine/tiling/TileCacheMetrics.h
    earth_engine/tiling/TileCacheMetrics.cpp
    earth_engine/tiling/TilesetProviderDiagnosticsCollector.h
    earth_engine/tiling/TilesetProviderDiagnosticsCollector.cpp
    earth_engine/tiling/TileCacheUnloadCoordinator.h
    earth_engine/tiling/TileUpdateDebugLogInput.h
    earth_engine/tiling/TileRenderDebugLogInput.h
    earth_engine/tiling/TileFrameDebugLogFormatter.h
    earth_engine/tiling/TileFrameDebugLogFormatter.cpp
    earth_engine/tiling/TileFrameInteractionTracker.h
    earth_engine/tiling/TileFrameResourceBudgetPlanner.h
    earth_engine/tiling/TileFrameWorkCoordinator.h
    earth_engine/tiling/TileBoundsMetrics.h
    earth_engine/tiling/TileBoundsMetrics.cpp
    earth_engine/tiling/TileFrameState.h
    earth_engine/tiling/TileFrameState.cpp
    earth_engine/tiling/TileFrameBudgetFallback.h
    earth_engine/tiling/TileLoadDiagnostics.h
    earth_engine/tiling/TileLoadDiagnostics.cpp
    earth_engine/tiling/TileLoadPriorityPolicy.h
    earth_engine/tiling/TileLoadPriorityPolicy.cpp
    earth_engine/tiling/TileIndexState.h
    earth_engine/tiling/TileIndexState.cpp
    earth_engine/tiling/TileLoadLifecycle.h
    earth_engine/tiling/TileLoadLifecycle.cpp
    earth_engine/tiling/TileLoadRequestPlanner.h
    earth_engine/tiling/TileLoadRequestPlanner.cpp
    earth_engine/tiling/TileLoadRequestDispatcher.h
    earth_engine/tiling/TileLoadRequestDispatcher.cpp
    earth_engine/tiling/TileMissingRequestScheduler.h
    earth_engine/tiling/TileLoadScheduler.h
    earth_engine/tiling/TileLoadQueue.h
    earth_engine/tiling/TileLoadQueue.cpp
    earth_engine/tiling/TilePendingLoadQueue.h
    earth_engine/tiling/TilePendingLoadQueue.cpp
    earth_engine/tiling/TileGltfTerrainUpsampledChildMaterializer.h
    earth_engine/tiling/TilePendingLoadCommitCoordinator.h
    earth_engine/tiling/TilePendingLoadProcessor.h
    earth_engine/tiling/TilePendingUploadFrameProcessor.h
    earth_engine/tiling/TilePendingUploadCompletion.h
    earth_engine/tiling/TilePendingUploadCompletion.cpp
    earth_engine/tiling/TileRasterOverlayFrameProcessor.h
    earth_engine/tiling/TileRasterOverlaySignature.h
    earth_engine/tiling/TileRasterUpsampledChildCoordinator.h
    earth_engine/tiling/TileRasterUpsampledChildCoordinator.cpp
    earth_engine/tiling/TileRasterOverlayPrefetcher.h
    earth_engine/tiling/TileRasterOverlayPrefetcher.cpp
    earth_engine/tiling/TileRasterOverlayState.h
    earth_engine/tiling/TileRasterOverlayState.cpp
    earth_engine/tiling/TileRasterOverlayReadinessPolicy.h
    earth_engine/tiling/TileRasterOverlayReadinessPolicy.cpp
    earth_engine/tiling/TilePendingRequestState.h
    earth_engine/tiling/TilePendingRequestState.cpp
    earth_engine/tiling/TileRenderEntryCommandBuilder.h
    earth_engine/tiling/TileRenderCommandPreparer.h
    earth_engine/tiling/TileRenderCommandManager.h
    earth_engine/tiling/TileRenderCommandManager.cpp
    earth_engine/tiling/TileRenderFrameBuilder.h
    earth_engine/tiling/TileRenderFrameCoordinator.h
    earth_engine/tiling/TileRenderFrameContext.h
    earth_engine/tiling/TileRenderFrameContext.cpp
    earth_engine/tiling/TileRenderFrameInputBuilder.h
    earth_engine/tiling/TileRenderFrameMaintenance.h
    earth_engine/tiling/TilesetUpdateFrameFacade.h
    earth_engine/tiling/TilesetUpdateFrameFacade.cpp
    earth_engine/tiling/TilesetUpdateFrameRuntime.h
    earth_engine/tiling/TilesetUpdateFrameRuntime.cpp
    earth_engine/tiling/TilesetRenderFrameExecutor.h
    earth_engine/tiling/TilesetRenderFrameExecutor.cpp
    earth_engine/tiling/TilesetTerrainProviders.h
    earth_engine/tiling/TilesetTerrainProviders.cpp
    earth_engine/tiling/TileRenderContentState.h
    earth_engine/tiling/TileRenderReferenceState.h
    earth_engine/tiling/TileRenderReferenceReleaser.h
    earth_engine/tiling/TileLoadResultMetadata.h
    earth_engine/tiling/TileLoadResultMetadataApplicator.h
    earth_engine/tiling/TileLoadResultMetadataApplicator.cpp
    earth_engine/tiling/TileLoadStatus.h
    earth_engine/tiling/TileLoadTypes.h
    earth_engine/tiling/TileOcclusionState.h
    earth_engine/tiling/TileOcclusionCallback.h
    earth_engine/tiling/TileOcclusionResolver.h
    earth_engine/tiling/TileMeshFrameEnsurer.h
    earth_engine/tiling/TileMeshPreparationManager.h
    earth_engine/tiling/TileMeshPreparationManager.cpp
    earth_engine/tiling/TileRenderablePolicy.h
    earth_engine/tiling/TileRenderablePolicy.cpp
    earth_engine/tiling/TilePriorityMetrics.h
    earth_engine/tiling/TilePriorityMetrics.cpp
    earth_engine/tiling/TileRefinementAvailabilityResolver.h
    earth_engine/tiling/TileSelectionChildTraversal.h
    earth_engine/tiling/TileSelectionCounters.h
    earth_engine/tiling/TileSelectionRasterOverlayPreparer.h
    earth_engine/tiling/TileSelectionCullingPolicy.h
    earth_engine/tiling/TileSelectionCullingPolicy.cpp
    earth_engine/tiling/TileSelectionFrameBuilder.h
    earth_engine/tiling/TileSelectionFrameBuilder.cpp
    earth_engine/tiling/TileSelectionFrameFinalizer.h
    earth_engine/tiling/TileSelectionFrameFinalizationRunner.h
    earth_engine/tiling/TileSelectionFrameFinalizationRunner.cpp
    earth_engine/tiling/TileSelectionFrameState.h
    earth_engine/tiling/TileSelectionFrameRunner.h
    earth_engine/tiling/TileSelectionHistory.h
    earth_engine/tiling/TileSelectionHistory.cpp
    earth_engine/tiling/TileSelectionInputMetrics.h
    earth_engine/tiling/TileSelectionInputMetrics.cpp
    earth_engine/tiling/TileSelectionKickPolicy.h
    earth_engine/tiling/TileSelectionKickPolicy.cpp
    earth_engine/tiling/TileSelectionMetrics.h
    earth_engine/tiling/TileSelectionMetrics.cpp
    earth_engine/tiling/TileSelectionPostTraversalCommitter.h
    earth_engine/tiling/TileSelectionPostTraversalPolicy.h
    earth_engine/tiling/TileSelectionPostTraversalPolicy.cpp
    earth_engine/tiling/TileSelectionPlanAppender.h
    earth_engine/tiling/TileSelectionPlanAppender.cpp
    earth_engine/tiling/TileSelectionPreTraversalPolicy.h
    earth_engine/tiling/TileSelectionPreTraversalPolicy.cpp
    earth_engine/tiling/TileSelectionRefineFlowPolicy.h
    earth_engine/tiling/TileSelectionRefineFlowPolicy.cpp
    earth_engine/tiling/TileSelectionRefinementPolicy.h
    earth_engine/tiling/TileSelectionRefinementPolicy.cpp
    earth_engine/tiling/TileSelectionRenderEntryPolicy.h
    earth_engine/tiling/TileSelectionRenderEntryPolicy.cpp
    earth_engine/tiling/TileSelectionResetPolicy.h
    earth_engine/tiling/TileSelectionResetPolicy.cpp
    earth_engine/tiling/TileSelectionRootPolicy.h
    earth_engine/tiling/TileSelectionRootPolicy.cpp
    earth_engine/tiling/TileSelectionStateResetter.h
    earth_engine/tiling/TileSelectionStateResetter.cpp
    earth_engine/tiling/TileSelectionShadowTree.h
    earth_engine/tiling/TileSelectionShadowTree.cpp
    earth_engine/tiling/TileSelectionShadowRunner.h
    earth_engine/tiling/TileSelectionShadowRunner.cpp
    earth_engine/tiling/TileSelectionWorker.h
    earth_engine/tiling/TileSelectionWorker.cpp
    earth_engine/tiling/TileSelectionReusePolicy.h
    earth_engine/tiling/TileSelectionReusePolicy.cpp
    earth_engine/tiling/TileSelectionReuseState.h
    earth_engine/tiling/TileSelectionSummaryBuilder.h
    earth_engine/tiling/TileSelectionSummaryPolicy.h
    earth_engine/tiling/TileSelectionSummaryPolicy.cpp
    earth_engine/tiling/TileSelectionTraversalCounterPolicy.h
    earth_engine/tiling/TileSelectionTraversalCounterPolicy.cpp
    earth_engine/tiling/TileSelectionTraversalContext.h
    earth_engine/tiling/TileSelectionTraversalContextBuilder.h
    earth_engine/tiling/TileSelectionTraversalContextBuilder.cpp
    earth_engine/tiling/TileSelectionTraversalDetailsBuilder.h
    earth_engine/tiling/TileSelectionTraversalDetailsBuilder.cpp
    earth_engine/tiling/TileSelectionTraversalExecutor.h
    earth_engine/tiling/TileSelectionTraversalExecutor.cpp
    earth_engine/tiling/TilesetSelectionFrameFacade.h
    earth_engine/tiling/TilesetSelectionFrameFacade.cpp
    earth_engine/tiling/TileSelectionVisitPreparation.h
    earth_engine/tiling/TileSelectionVisitPreparation.cpp
    earth_engine/tiling/TileSelectionVisibilitySampler.h
    earth_engine/tiling/TileSelectionVisibilitySampler.cpp
    earth_engine/tiling/TileSoftwareOcclusionPolicy.h
    earth_engine/tiling/TileSoftwareOcclusionPolicy.cpp
    earth_engine/tiling/TileSubtreeRemovalCoordinator.h
    earth_engine/tiling/TileSubtreeWorkTracker.h
    earth_engine/tiling/TileTerminalLoadPolicy.h
    earth_engine/tiling/TileTerminalLoadPolicy.cpp
    earth_engine/tiling/TileTerminalLoadCommitter.h
    earth_engine/tiling/TileTerminalLoadCommitter.cpp
    earth_engine/tiling/TileTerrainHeightRangePolicy.h
    earth_engine/tiling/TileTerrainHeightRangePolicy.cpp
    earth_engine/tiling/TileVisibleRangeFinalizer.h
    earth_engine/tiling/TileVisibleRangeFinalizer.cpp
    earth_engine/tiling/TileTraversalDetails.h
    earth_engine/tiling/TileTraversalDetails.cpp
    earth_engine/tiling/TileUpsampleSourcePreparer.h
    earth_engine/tiling/TileUpdateFrameContextBuilder.h
    earth_engine/tiling/TileUpdateSelectionWorkRunner.h
    earth_engine/tiling/TileUpdateUploadRunner.h
    earth_engine/tiling/TileViewerRequestVolumePolicy.h
    earth_engine/tiling/TileViewerRequestVolumePolicy.cpp
    earth_engine/tiling/TileSubtreeTraversal.h
    earth_engine/tiling/TileUnloadPolicy.h
    earth_engine/tiling/TileUnloadPolicy.cpp
    earth_engine/tiling/TileUnloadQueue.h
    earth_engine/tiling/TileUnloadQueue.cpp
    earth_engine/tiling/TilesetTile.h
    earth_engine/tiling/TilesetTileRegistry.h
    earth_engine/tiling/TilesetTileRegistry.cpp
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
    earth_engine/providers/BingMapsImageryProvider.h
    earth_engine/providers/BingMapsImageryProvider.cpp
    earth_engine/providers/GoogleMapTilesImageryProvider.h
    earth_engine/providers/GoogleMapTilesImageryProvider.cpp
    earth_engine/providers/XYZImageryProvider.h
    earth_engine/providers/XYZImageryProvider.cpp
    earth_engine/providers/WebMapServiceImageryProvider.h
    earth_engine/providers/WebMapServiceImageryProvider.cpp
    earth_engine/providers/WebMapTileServiceImageryProvider.h
    earth_engine/providers/WebMapTileServiceImageryProvider.cpp
    earth_engine/providers/TileMapServiceImageryProvider.h
    earth_engine/providers/TileMapServiceImageryProvider.cpp
    earth_engine/providers/ProviderRequestDiagnosticsAggregator.h
    earth_engine/providers/ProviderRequestDiagnosticsAggregator.cpp
    earth_engine/providers/TerrainProvider.h
    earth_engine/providers/TerrainProvider.cpp
    earth_engine/providers/HeightmapTerrainProvider.h
    earth_engine/providers/HeightmapTerrainProvider.cpp
    earth_engine/providers/QuantizedMeshTerrainProvider.h
    earth_engine/providers/QuantizedMeshTerrainProvider.cpp
    earth_engine/providers/QuantizedMeshLayerJsonFetcher.h
    earth_engine/providers/QuantizedMeshLayerJsonFetcher.cpp
    earth_engine/providers/TileMapServiceUrl.h
    earth_engine/providers/TileMapServiceUrl.cpp
    earth_engine/providers/RasterOverlayTile.h
    earth_engine/providers/RasterOverlayTile.cpp
    earth_engine/providers/RasterTextureUploader.h
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
    earth_engine/layers/CreditSystem.h
    earth_engine/layers/CreditSystem.cpp
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
    earth_engine/platform/bridge/CurlPlatformBridge.h
    earth_engine/platform/bridge/CurlPlatformBridge.cpp
    earth_engine/platform/bridge/CurlMultiRequestScheduler.h
    earth_engine/platform/bridge/CurlMultiRequestScheduler.cpp
    earth_engine/platform/stb_image_impl.cpp
)

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
    # Metal 渲染后端在 iOS / macOS 共用同一套实现
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
    # PlatformBridge 按平台二选一：iOS -> IosPlatformBridge（UIKit 设备信息），
    # macOS -> MacPlatformBridge。两者结构一致（NSURLSession/ImageIO/Keychain），
    # 均使用 ARC。
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_sources(earth_engine_core PRIVATE
            earth_engine/platform/ios/IosPlatformBridge.h
            earth_engine/platform/ios/IosPlatformBridge.mm
        )
        set_source_files_properties(
            earth_engine/platform/ios/IosPlatformBridge.mm
            PROPERTIES
            COMPILE_FLAGS "-x objective-c++ -fobjc-arc"
        )
        find_library(UIKIT_LIBRARY UIKit)
        if(UIKIT_LIBRARY)
            target_link_libraries(earth_engine_core PRIVATE ${UIKIT_LIBRARY})
        endif()
    else()
        target_sources(earth_engine_core PRIVATE
            earth_engine/platform/macos/MacPlatformBridge.h
            earth_engine/platform/macos/MacPlatformBridge.mm
        )
        set_source_files_properties(
            earth_engine/platform/macos/MacPlatformBridge.mm
            PROPERTIES
            COMPILE_FLAGS "-x objective-c++ -fobjc-arc"
        )
    endif()
    # 链接 Metal 和 QuartzCore frameworks
    find_library(METAL_LIBRARY Metal)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(SECURITY_LIBRARY Security)
    find_library(COREGRAPHICS_LIBRARY CoreGraphics)
    find_library(IMAGEIO_LIBRARY ImageIO)
    if(METAL_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${METAL_LIBRARY})
    endif()
    if(QUARTZCORE_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${QUARTZCORE_LIBRARY})
    endif()
    if(FOUNDATION_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${FOUNDATION_LIBRARY})
    endif()
    if(SECURITY_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${SECURITY_LIBRARY})
    endif()
    if(COREGRAPHICS_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${COREGRAPHICS_LIBRARY})
    endif()
    if(IMAGEIO_LIBRARY)
        target_link_libraries(earth_engine_core PRIVATE ${IMAGEIO_LIBRARY})
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
    earth_engine/data/Feature.h
    earth_engine/data/FeatureSpatialIndex.h
    earth_engine/data/FeatureSpatialIndex.cpp
    earth_engine/data/FeatureBucketGrid.h
    earth_engine/data/FeatureBucketGrid.cpp
    earth_engine/data/ConstrainedDelaunay.h
    earth_engine/data/ConstrainedDelaunay.cpp
    earth_engine/data/PolygonTessellator.h
    earth_engine/data/PolygonTessellator.cpp
    earth_engine/data/LineTessellator.h
    earth_engine/data/LineTessellator.cpp
    earth_engine/data/FeatureStore.h
    earth_engine/data/FeatureStore.cpp
    earth_engine/data/GeoJsonImporter.h
    earth_engine/data/GeoJsonImporter.cpp
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
set(_EARTH_ENGINE_VCPKG_TRIPLET "${VCPKG_TARGET_TRIPLET}")
if(NOT _EARTH_ENGINE_VCPKG_TRIPLET)
    set(_EARTH_ENGINE_VCPKG_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}")
endif()
if(ANDROID AND NOT _EARTH_ENGINE_VCPKG_TRIPLET)
    set(_EARTH_ENGINE_VCPKG_TRIPLET "arm64-android")
endif()
if(APPLE AND NOT _EARTH_ENGINE_VCPKG_TRIPLET)
    set(_EARTH_ENGINE_VCPKG_TRIPLET "arm64-osx")
endif()
set(_EARTH_ENGINE_VCPKG_ROOT "${EARTH_ENGINE_VCPKG_ROOT}")
if(NOT _EARTH_ENGINE_VCPKG_ROOT)
    set(_EARTH_ENGINE_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
endif()
if(NOT _EARTH_ENGINE_VCPKG_ROOT)
    set(_EARTH_ENGINE_REPO_ADJACENT_VCPKG
        "${CMAKE_CURRENT_LIST_DIR}/../../../globe/third_party/vcpkg")
    if(EXISTS "${_EARTH_ENGINE_REPO_ADJACENT_VCPKG}/packages")
        set(_EARTH_ENGINE_VCPKG_ROOT "${_EARTH_ENGINE_REPO_ADJACENT_VCPKG}")
    endif()
endif()
if(_EARTH_ENGINE_VCPKG_ROOT AND _EARTH_ENGINE_VCPKG_TRIPLET)
    foreach(_EARTH_ENGINE_VCPKG_PACKAGE glm nlohmann-json stb curl openssl zlib)
        set(_EARTH_ENGINE_VCPKG_PACKAGE_ROOT
            "${_EARTH_ENGINE_VCPKG_ROOT}/packages/${_EARTH_ENGINE_VCPKG_PACKAGE}_${_EARTH_ENGINE_VCPKG_TRIPLET}")
        if(EXISTS "${_EARTH_ENGINE_VCPKG_PACKAGE_ROOT}")
            list(APPEND CMAKE_PREFIX_PATH "${_EARTH_ENGINE_VCPKG_PACKAGE_ROOT}")
        endif()
    endforeach()
endif()

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

if(ANDROID)
    if(_EARTH_ENGINE_VCPKG_ROOT AND _EARTH_ENGINE_VCPKG_TRIPLET)
        set(_EARTH_ENGINE_CURL_CONFIG_DIR
            "${_EARTH_ENGINE_VCPKG_ROOT}/packages/curl_${_EARTH_ENGINE_VCPKG_TRIPLET}/share/curl")
        set(_EARTH_ENGINE_OPENSSL_ROOT
            "${_EARTH_ENGINE_VCPKG_ROOT}/packages/openssl_${_EARTH_ENGINE_VCPKG_TRIPLET}")
        set(_EARTH_ENGINE_OPENSSL_CONFIG_DIR
            "${_EARTH_ENGINE_OPENSSL_ROOT}/share/openssl")
        set(_EARTH_ENGINE_ZLIB_ROOT
            "${_EARTH_ENGINE_VCPKG_ROOT}/packages/zlib_${_EARTH_ENGINE_VCPKG_TRIPLET}")

        if(NOT CURL_DIR AND EXISTS "${_EARTH_ENGINE_CURL_CONFIG_DIR}/CURLConfig.cmake")
            set(CURL_DIR "${_EARTH_ENGINE_CURL_CONFIG_DIR}")
        endif()
        if(NOT OpenSSL_DIR AND EXISTS "${_EARTH_ENGINE_OPENSSL_CONFIG_DIR}/OpenSSLConfig.cmake")
            set(OpenSSL_DIR "${_EARTH_ENGINE_OPENSSL_CONFIG_DIR}")
        endif()
        if(NOT OPENSSL_ROOT_DIR AND EXISTS "${_EARTH_ENGINE_OPENSSL_ROOT}/include/openssl/ssl.h")
            set(OPENSSL_ROOT_DIR "${_EARTH_ENGINE_OPENSSL_ROOT}")
        endif()
        if(NOT OPENSSL_INCLUDE_DIR AND EXISTS "${_EARTH_ENGINE_OPENSSL_ROOT}/include/openssl/ssl.h")
            set(OPENSSL_INCLUDE_DIR "${_EARTH_ENGINE_OPENSSL_ROOT}/include")
        endif()
        if(NOT OPENSSL_SSL_LIBRARY AND EXISTS "${_EARTH_ENGINE_OPENSSL_ROOT}/lib/libssl.a")
            set(OPENSSL_SSL_LIBRARY "${_EARTH_ENGINE_OPENSSL_ROOT}/lib/libssl.a")
        endif()
        if(NOT OPENSSL_CRYPTO_LIBRARY AND EXISTS "${_EARTH_ENGINE_OPENSSL_ROOT}/lib/libcrypto.a")
            set(OPENSSL_CRYPTO_LIBRARY "${_EARTH_ENGINE_OPENSSL_ROOT}/lib/libcrypto.a")
        endif()
        if(NOT ZLIB_ROOT AND EXISTS "${_EARTH_ENGINE_ZLIB_ROOT}/include/zlib.h")
            set(ZLIB_ROOT "${_EARTH_ENGINE_ZLIB_ROOT}")
        endif()
        if(NOT ZLIB_INCLUDE_DIR AND EXISTS "${_EARTH_ENGINE_ZLIB_ROOT}/include/zlib.h")
            set(ZLIB_INCLUDE_DIR "${_EARTH_ENGINE_ZLIB_ROOT}/include")
        endif()
        if(NOT ZLIB_LIBRARY AND EXISTS "${_EARTH_ENGINE_ZLIB_ROOT}/lib/libz.a")
            set(ZLIB_LIBRARY "${_EARTH_ENGINE_ZLIB_ROOT}/lib/libz.a")
        endif()
    endif()
    find_package(CURL CONFIG REQUIRED)
else()
    find_package(CURL CONFIG QUIET)
    if(NOT CURL_FOUND)
        find_package(CURL REQUIRED)
    endif()
endif()
target_link_libraries(earth_engine_core PUBLIC CURL::libcurl)

unset(_EARTH_ENGINE_VCPKG_PACKAGE)
unset(_EARTH_ENGINE_VCPKG_PACKAGE_ROOT)
unset(_EARTH_ENGINE_VCPKG_ROOT)
unset(_EARTH_ENGINE_REPO_ADJACENT_VCPKG)
unset(_EARTH_ENGINE_CURL_CONFIG_DIR)
unset(_EARTH_ENGINE_OPENSSL_ROOT)
unset(_EARTH_ENGINE_OPENSSL_CONFIG_DIR)
unset(_EARTH_ENGINE_ZLIB_ROOT)
unset(_EARTH_ENGINE_VCPKG_TRIPLET)

# stb_image 是 header-only；当前核心库尚未直接使用，缺失时不阻断。
find_package(stb CONFIG QUIET)
if(stb_FOUND)
    target_link_libraries(earth_engine_core PRIVATE stb::stb)
endif()

# ============================================================
# TSAN-instrumented twin for the async-selection race test.
# Gated behind EARTH_ENGINE_ENABLE_TSAN (default OFF) so normal builds/CI are
# untouched. Configure a dedicated build dir with -DEARTH_ENGINE_ENABLE_TSAN=ON.
# Reuses earth_engine_core's fully-resolved source/include/link properties so
# it stays in lockstep without duplicating the source list.
# ============================================================
option(EARTH_ENGINE_ENABLE_TSAN
    "Build the ThreadSanitizer-instrumented async-selection race test" OFF)
if(EARTH_ENGINE_ENABLE_TSAN)
    get_target_property(_ee_tsan_sources earth_engine_core SOURCES)
    get_target_property(_ee_tsan_incs earth_engine_core INCLUDE_DIRECTORIES)
    get_target_property(_ee_tsan_links earth_engine_core LINK_LIBRARIES)
    add_library(earth_engine_core_tsan STATIC ${_ee_tsan_sources})
    target_include_directories(earth_engine_core_tsan PUBLIC ${_ee_tsan_incs})
    if(_ee_tsan_links)
        target_link_libraries(earth_engine_core_tsan PUBLIC ${_ee_tsan_links})
    endif()
    target_compile_features(earth_engine_core_tsan PUBLIC cxx_std_17)
    target_compile_options(earth_engine_core_tsan PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer -g -O1)
    target_link_options(earth_engine_core_tsan PUBLIC -fsanitize=thread)
endif()
