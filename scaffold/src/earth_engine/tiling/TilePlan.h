#pragma once

#include "TileKey.h"
#include <string>
#include <vector>
#include <memory>

namespace earth_engine {

class TileScheme;
class Camera;

/// TilePlan is the shared, frame-derived candidate set for one tile scheme.
/// It does not decide provider requests or final rendering for a layer.
struct TileTransition {
    TileKey key;
    float opacity = 1.0f;
    int fadingNodeCount = 0;
};

struct TilePlan {
    uint64_t frameId = 0;
    int zoom = 0;
    int minVisibleZoom = 0;
    int maxVisibleZoom = 0;
    bool equalZoomApplied = false;
    double lodSizePixels = 0.0;
    double minLodSizePixels = 0.0;
    double maxLodSizePixels = 0.0;
    std::vector<TileKey> visibleTiles;
    std::vector<TileTransition> tileTransitions;
    int renderingNodeCount = 0;
    int walkthroughNodeCount = 0;
    int notRenderingNodeCount = 0;
    int cameraInsideNodeCount = 0;
    int inFrustumNodeCount = 0;
    int horizonTangentPreservedCount = 0;
    int equalZoomSecondPassNodeCount = 0;
    int fadingNodeCount = 0;
    int neighborLinkCount = 0;
    int mercatorTileCount = 0;
    int northPolarTileCount = 0;
    int southPolarTileCount = 0;
};

enum class TileRenderSource {
    Exact,
    ParentFallback
};

enum class TileReadinessState {
    Missing,
    ParentFallback,
    Ready
};

struct RenderTileRef {
    TileKey targetKey;
    TileKey textureKey;
    TileRenderSource source = TileRenderSource::Exact;
    TileReadinessState readiness = TileReadinessState::Missing;
    float transitionOpacity = 1.0f;
};

struct TileFallback {
    TileKey targetKey;
    TileKey fallbackKey;
};

/// Per-layer plan derived from TilePlan plus that layer's cache/provider state.
struct LayerTilePlan {
    std::string layerId;
    std::string providerId;
    uint64_t frameId = 0;
    int zoom = 0;
    int minVisibleZoom = 0;
    int maxVisibleZoom = 0;
    bool equalZoomApplied = false;
    double lodSizePixels = 0.0;
    std::vector<TileKey> visibleTiles;
    std::vector<TileTransition> tileTransitions;
    std::vector<TileKey> desiredTiles;
    std::vector<TileKey> requestTiles;
    std::vector<RenderTileRef> renderTiles;
    std::vector<TileFallback> fallbackTiles;
    int readyTileCount = 0;
    int parentFallbackReadyTileCount = 0;
    int missingTileCount = 0;
    int unsupportedTileCount = 0;
    int transitionTileCount = 0;
    int quadtreeFadingNodeCount = 0;
    int quadtreeNeighborLinkCount = 0;
    int quadtreeRenderingNodeCount = 0;
    int quadtreeWalkthroughNodeCount = 0;
    int quadtreeNotRenderingNodeCount = 0;
    int quadtreeCameraInsideNodeCount = 0;
    int quadtreeInFrustumNodeCount = 0;
    int quadtreeHorizonTangentPreservedCount = 0;
    int quadtreeEqualZoomSecondPassNodeCount = 0;
};

/// 瓦片计划计算器。
/// 根据相机状态和瓦片体系，计算当前帧应该显示哪些瓦片。
class TilePlanBuilder {
public:
    /// 计算可见瓦片集合。
    /// @param camera 当前相机
    /// @param scheme 瓦片体系
    /// @param viewportWidthPixels 视口宽度
    /// @param viewportHeightPixels 视口高度
    /// @return 瓦片计划（包含目标 zoom 和 visibleTiles）
    static TilePlan compute(const Camera& camera,
                            const TileScheme& scheme,
                            double viewportWidthPixels,
                            double viewportHeightPixels,
                            int previousZoom = -1);

    /// 根据相机距地表高度估算合适的 zoom 层级。
    /// @param cameraHeightMeters 相机距椭球面的高度（米）
    /// @param viewportHeightPixels 视口高度（像素）
    /// @param verticalFovRadians 垂直视场角
    /// @param tileSize tile 的像素尺寸
    /// @return 建议的 zoom 层级（clamped 到 [minZoom, maxZoom]）
    static int zoomLevelFromHeight(double cameraHeightMeters,
                                   double viewportHeightPixels,
                                   double verticalFovRadians,
                                   int tileSize,
                                   int minZoom,
                                   int maxZoom,
                                   int previousZoom = -1);

    static TileKey parentKey(const TileKey& key);
};

} // namespace earth_engine
