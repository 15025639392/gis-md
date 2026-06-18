#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Vec3.h"
#include "Frustum.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class Camera;

/// 运行时诊断数据（MVP 要求：FPS、draw calls、visible tiles、request queue）。
/// 每帧由 Scene 填充，通过 Engine::diagnostics() 暴露。
struct Diagnostics {
    double fps = 0.0;
    double frameTimeMs = 0.0;
    double engineFrameCpuMs = 0.0;
    double engineBeginFrameMs = 0.0;
    double sceneUpdateMs = 0.0;
    double sceneRenderMs = 0.0;
    double engineEndFrameMs = 0.0;
    double cameraUpdateMs = 0.0;
    double environmentUpdateMs = 0.0;
    double basemapStackUpdateMs = 0.0;
    double terrainUpdateMs = 0.0;
    double contentTilesetUpdateMs = 0.0;
    double renderCommandBuildMs = 0.0;
    double renderSubmitMs = 0.0;
    int drawCalls = 0;
    int visibleTiles = 0;
    int contentTilesets = 0;
    int contentVisibleTiles = 0;
    int cachedTextures = 0;
    int queuedRequests = 0;
    int loadingRequests = 0;
    int loadQueuePreloadRequests = 0;
    int loadQueueNormalRequests = 0;
    int loadQueueUrgentRequests = 0;
    int pendingTerrainRequests = 0;
    int pendingTerrainUploads = 0;
    int pendingTerrainTerminalResults = 0;
    int pendingContentRequests = 0;
    int pendingContentUploads = 0;
    int pendingContentTerminalResults = 0;
    int gpuTextureCount = 0;
    int renderSurfaceTiles = 0;
    int renderGltfPrimitives = 0;
    int surfaceMeshCount = 0;
    int imageryAttachments = 0;
    int imageryExactAttachments = 0;
    int imageryParentFallbackAttachments = 0;
    int imageryMissingTiles = 0;
    int imageryUnsupportedTiles = 0;
    int imageryTransitionTiles = 0;
    int imageryKickedTiles = 0;
    int imageryAncestorRetainedTiles = 0;
    int imageryMinTargetZoom = 0;
    int imageryMaxTargetZoom = 0;
    int imageryMinTextureZoom = 0;
    int imageryMaxTextureZoom = 0;
    double lodSizePixels = 0.0;
    int minVisibleZoom = 0;
    int maxVisibleZoom = 0;
    int quadtreeEqualZoomLayers = 0;
    int quadtreeFadingNodes = 0;
    int quadtreeNeighborLinks = 0;
    int quadtreeNeighborBalancedTiles = 0;
    int quadtreeRenderingNodes = 0;
    int quadtreeWalkthroughNodes = 0;
    int quadtreeNotRenderingNodes = 0;
    int quadtreeSelectionRenderedNodes = 0;
    int quadtreeSelectionRefinedNodes = 0;
    int quadtreeSelectionKickedNodes = 0;
    int quadtreeSelectionOccludedNodes = 0;
    int quadtreeSelectionWaitingForOcclusionResultsNodes = 0;
    int quadtreeCulledTilesVisited = 0;
    int quadtreeSelectionAncestorMeetsSseNodes = 0;
    int quadtreeCameraInsideNodes = 0;
    int quadtreeInFrustumNodes = 0;
    int quadtreeHorizonTangentPreservedNodes = 0;
    int quadtreeEqualZoomSecondPassNodes = 0;
    int mercatorTileCount = 0;
    int northPolarTileCount = 0;
    int southPolarTileCount = 0;
    int surfaceMeshBytes = 0;
    int terrainCachedTiles = 0;
    int terrainLoadUnloadingTiles = 0;
    int terrainLoadFailedTemporarilyTiles = 0;
    int terrainLoadUnloadedTiles = 0;
    int terrainLoadContentLoadingTiles = 0;
    int terrainLoadContentLoadedTiles = 0;
    int terrainLoadDoneTiles = 0;
    int terrainLoadFailedTiles = 0;
    int terrainContentUnknownTiles = 0;
    int terrainContentEmptyTiles = 0;
    int terrainContentExternalTiles = 0;
    int terrainContentRenderTiles = 0;
    int terrainUnloadQueueTiles = 0;
    int missingRasterOverlayProjections = 0;
    uint64_t terrainGeneration = 0;
    int terrainSurfaceMeshes = 0;
    int terrainParentFallbackMeshes = 0;
    int terrainReadySurfaceMeshes = 0;
    int terrainTransitionSurfaceMeshes = 0;
    int ellipsoidSurfaceMeshes = 0;
    int staleSurfaceCommands = 0;
    int missingGenerationSurfaceCommands = 0;
    uint64_t minSurfaceGeneration = 0;
    uint64_t maxSurfaceGeneration = 0;
};

/// 每帧渲染上下文。
/// 由 Scene::update() 计算，传递给各 Layer 和 Renderer。
struct FrameState {
    /// 渲染模式（当前仅 3D Globe）
    enum class Mode { Mode3D };

    uint64_t frameId = 0;
    double timeSeconds = 0.0;   // 自引擎启动以来的秒数
    double deltaSeconds = 0.0;  // 上一帧到本帧的秒数
    Mode mode = Mode::Mode3D;

    const Camera* camera = nullptr;

    /// cesium-native TilesetFrameState::frustums equivalent for selection.
    /// Scene populates the default main-camera view. Empty means no frustums,
    /// matching cesium-native's no-selection update path.
    struct SelectorView {
        Vec3 position = Vec3::zero();
        Vec3 direction = Vec3::zero();
        Frustum frustum;
        Mat4 projectionMatrix;
        int viewportHeightPixels = 0;
    };
    std::vector<SelectorView> selectorViews;

    /// 太阳方向（ECEF 单位向量，地心→太阳），环境系统填充
    struct { float x = 0.35f; float y = 0.45f; float z = 0.82f; } lightDir;

    /// 天空 clear 颜色（RGBA），环境系统填充
    float clearR = 0.02f, clearG = 0.02f, clearB = 0.08f, clearA = 1.0f;

    int viewportWidthPixels = 0;
    int viewportHeightPixels = 0;
    float devicePixelRatio = 1.0f;

    bool hasInteractionFocus = false;
    Vec3 interactionFocusDirection = Vec3::zero();

    Diagnostics diagnostics;
};

} // namespace earth_engine
