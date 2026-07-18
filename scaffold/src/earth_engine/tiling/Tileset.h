#pragma once

#include "TilesetTile.h"
#include "TileScheme.h"
#include "TilePlan.h"
#include "TileContentAccess.h"
#include "TileContentCacheManager.h"
#include "TileCacheOwnershipManager.h"
#include "TileContentLifecycleManager.h"
#include "TileContentResourceInvalidator.h"
#include "TileContentRuntime.h"
#include "TileLoadDiagnostics.h"
#include "TileLoadQueue.h"
#include "TileLoadTypes.h"
#include "TileOcclusionCallback.h"
#include "TileSoftwareOcclusionPolicy.h"
#include "GpuUploadQueue.h"
#include "TileMeshPreparationManager.h"
#include "TileOcclusionState.h"
#include "TileRasterUpsampledChildCoordinator.h"
#include "TileRenderCommandManager.h"
#include "TileRenderReferenceReleaser.h"
#include "TileIncrementalFrontier.h"
#include "TileSelectionCounters.h"
#include "TileSelectionMetrics.h"
#include "TileSelectionReuseState.h"
#include "TilesetTerrainProviders.h"
#include "TilesetTileRegistry.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../core/math/Vec3.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderCommand.h"
#include "../scene/FrameState.h"

#include <memory>
#include <cstdint>
#include <optional>
#include <vector>
#include <string>
#include <unordered_set>

namespace earth_engine {

class Renderer;
class RenderDevice;
class IPrepareRendererResources;
struct SelectorFrame;
class ActivatedRasterOverlay;
struct TilesetTestAccess;
class TilesetSelectionFrameFacade;
class TilesetUpdateFrameFacade;
class TilesetUpdateFrameRuntime;
class TileSelectionWorker;

/// cesium-native TilesetOptions subset used by the unified terrain tileset.
/// Defaults intentionally mirror native where the local renderer has the
/// corresponding capability.
struct TilesetOptions {
    double maximumScreenSpaceError = 16.0;
    uint32_t maximumSimultaneousTileLoads = 20;
    bool preloadAncestors = true;
    bool preloadSiblings = true;
    uint32_t loadingDescendantLimit = 20;
    bool forbidHoles = false;
    bool enableFrustumCulling = true;
    bool enableOcclusionCulling = true;
    bool delayRefinementForOcclusion = true;
    bool enableFogCulling = true;
    bool enforceCulledScreenSpaceError = true;
    double culledScreenSpaceError = 64.0;
    // cesium-js cullRequestsWhileMoving:运动期跳过"回来时多半已划走"的瓦片
    // 网络请求。movementRatio = multiplier × 相机本帧位移 / 瓦片直径 ≥1 → 本帧
    // 不发该瓦片请求(下一帧重评估)。cesium-native 无此项(仅靠 load 预算节流),
    // cesium-js 默认开、对所有瓦片(含 replace-refine 地形)生效——这是对地形
    // globe 真正有效的运动降载杠杆(foveated 对 replace-refine 地形是 no-op)。
    // 默认 false = 忠实 cesium-native(不改 golden);true 才启用。
    bool cullRequestsWhileMoving = false;
    double cullRequestsWhileMovingMultiplier = 60.0;
    // Terrain fill proxy (cesium-js TerrainFillMesh model): give each visible
    // tile still loading real terrain a drape-ready ellipsoid proxy so imagery
    // appears on the smooth globe immediately, then swaps to real terrain (which
    // "rises") when it arrives. Default false = unchanged behavior / golden.
    bool enableTerrainFillProxy = false;
    int terrainFillProxyGridSize = 16;
    bool renderTilesUnderCamera = true;
    int64_t maximumCachedBytes = 512LL * 1024 * 1024;
    bool enableLodTransitionPeriod = false;
    float lodTransitionLength = 1.0f;
    bool kickDescendantsWhileFadingIn = true;
    // Kill-switch for moving tile selection off the render thread. Default
    // false → the current synchronous selection path is used unchanged. When
    // true, the render thread dispatches selection to a dedicated worker and
    // applies the result. Any regression → set false to fully revert.
    bool asyncSelection = false;
    // 在 asyncSelection=true 基础上，把影子选择放到专用 worker 线程真异步跑
    // （render 不阻塞，结果延迟 ≥1 帧，worker 忙时本帧沿用上次计划）。默认
    // false = 影子选择在 render 线程同步跑（golden 可逐帧对拍验证选择算法）。
    // true = 性能模式，靠 TSAN + 真机验证；结果滞后使 golden 无法逐帧对拍。
    bool asyncSelectionNonBlocking = false;
    // ③ 增量切面(incremental frontier)总开关。默认 false → 每帧全量遍历
    // 选择(现状,忠实 cesium)。true → 只重评估「margin 带 ∪ dirty 集」的
    // 边界瓦片,跳过可证明稳定的内部(设计见
    // docs/issues/selector-incremental-frontier-design-2026-07-06.md)。
    // Phase 0:此 flag 已就位但增量路径尚未接线,恒走全量。
    bool incrementalSelection = false;
    // §8 等价性基座开关(debug/test-only,默认 false)。true → 每帧在被测
    // 选择路径之后再跑一遍全量参考选择,断言 visibleTiles/loadQueue/counters
    // 逐位相同。increment==full 的 oracle;Phase 0 下是 full==full 自证。
    // release 恒关(零开销)。
    bool verifySelectionEquivalence = false;
    // 每帧主线程加载时间预算（毫秒）。>0 时 FrameResourceBudget 按实测
    // finalize/上传耗时（recordElapsed）截断当帧后续工作，计数上限退为兜底；
    // 0 = 不设时间闸门（cesium-native 出厂默认，但静止帧一帧可串 20 次
    // finalize + 20 次上传形成尖峰，故本引擎默认 8ms）。
    double mainThreadLoadingTimeLimit = 8.0;
    double tileCacheUnloadTimeLimit = 0.0;
    std::vector<FogDensityAtHeight> fogDensityTable = {
        {359.393, 2.0e-5},     {800.749, 2.0e-4},
        {1275.6501, 1.0e-4},   {2151.1192, 7.0e-5},
        {3141.7763, 5.0e-5},   {4777.5198, 4.0e-5},
        {6281.2493, 3.0e-5},   {12364.307, 1.9e-5},
        {15900.765, 1.0e-5},   {49889.0549, 8.5e-6},
        {78026.8259, 6.2e-6},  {99260.7344, 5.8e-6},
        {120036.3873, 5.3e-6}, {151011.0158, 5.2e-6},
        {156091.1953, 5.1e-6}, {203849.3112, 4.2e-6},
        {274866.9803, 4.0e-6}, {319916.3149, 3.4e-6},
        {493552.0528, 2.6e-6}, {628733.5874, 2.2e-6},
        {1000000.0, 0.0},
    };
};

/// cesium-native Tileset equivalent.
/// Manages a unified quadtree of terrain and raster overlay tiles.
class Tileset {
public:
    using OcclusionCallback = TileOcclusionCallback;

    Tileset(std::unique_ptr<TileScheme> tileScheme,
            std::vector<ActivatedRasterOverlay*> rasterOverlays,
            RenderDevice* device,
            TilesetOptions options);
    Tileset(std::unique_ptr<TileScheme> tileScheme,
            std::vector<ActivatedRasterOverlay*> rasterOverlays,
            RenderDevice* device,
            TilesetOptions options,
            std::unique_ptr<TilesetContentProvider> contentProvider);
    ~Tileset();

    void update(const FrameState& frameState,
                IPrepareRendererResources* pPrepRenderer = nullptr);
    void buildRenderCommands(Renderer& renderer,
                             RenderCommandList& commands,
                             uint64_t renderFrameId = 0,
                             const std::vector<TileRenderEntry>*
                                 renderEntriesOverride = nullptr);

    const TilePlan& tilePlan() const { return tilePlan_; }
    bool shouldHoldPresentationFrame() const;
    bool requiresBaseImageryPresentationSurface() const;
    const TileScheme& tileScheme() const { return *tileScheme_; }
    int cachedHeightmapTerrainTilesForLegacySurfacePath() const;
    int pendingRequests() const;
    int64_t totalBytesUsed() const;
    // 北极星测量台:常驻资源字节按类拆分(内容缓存 vs 影像纹理),位姿无关。
    int64_t contentBytesUsed() const;
    int64_t imageryTextureBytesUsed() const;
    TilesetLoadDiagnostics loadDiagnostics() const;

    /// Sample the best loaded terrain height at longitude/latitude.
    /// Returns ellipsoid height in meters, or nullopt when no loaded terrain
    /// tile currently covers the position (no data). Callers that need to
    /// distinguish "no data" from real sea level (e.g. the camera altitude
    /// clamp) use this variant.
    std::optional<float> sampleHeightOptional(
        double lngRad, double latRad) const;

    /// Convenience wrapper: no data falls back to sea level (0). Picking and
    /// other "want a concrete height, sea-level default is fine" callers.
    float sampleHeight(double lngRad, double latRad) const {
        return sampleHeightOptional(lngRad, latRad).value_or(0.0f);
    }

    /// cesium-native: release all render references after GPU submit.
    /// Called by Scene after renderer_->submit(commands) so that
    /// reference counts protect tiles until the GPU has consumed them.
    void releaseRenderReferences();

    /// Explicitly abandon a command batch that was built but will not be
    /// submitted. This is the only legal pre-submit path that releases the
    /// batch's tile references.
    void discardPendingRenderReferences();

    /// cesium-native TileOcclusionRendererProxyPool equivalent input hook.
    /// Renderer/platform code may provide real per-tile occlusion results.
    /// Do not return OcclusionUnavailable for tiles whose result will never
    /// arrive; return NotOccluded so traversal does not wait forever.
    void setOcclusionCallback(OcclusionCallback callback);
    void clearOcclusionCallback();

    /// §8 等价性 oracle 的上一帧结果(verifySelectionEquivalence 开启时更新)。
    /// 空 = 增量/被测选择与全量参考逐位相同;否则为第一处差异描述。debug/测试
    /// 用;flag 关时恒空。
    const std::string& selectionEquivalenceMismatch() const {
        return selectionEquivalenceMismatch_;
    }

    /// ③ 增量切面子树贡献缓存(incrementalSelection 开启时每帧重建)。测试用。
    const TileIncrementalFrontier& incrementalFrontier() const {
        return incrementalFrontier_;
    }

private:
    friend struct TilesetTestAccess;
    friend class TilesetSelectionFrameFacade;
    friend class TilesetUpdateFrameRuntime;

    Tileset(TilesetTerrainProviders terrainProviders,
            std::unique_ptr<TileScheme> tileScheme,
            std::vector<ActivatedRasterOverlay*> rasterOverlays,
            RenderDevice* device,
            TilesetOptions options);
    TileContentRuntimeRequestFrame makeContentRuntimeRequestFrame(
        IPrepareRendererResources* pPrepRenderer);
    TileContentRuntimeUploadFrame makeContentRuntimeUploadFrame(
        IPrepareRendererResources* pPrepRenderer);
    bool hasTerrainQuadtree() const;
    bool plannedRenderEntriesHaveRequiredBaseImagery() const;
    TileLoadRequestOutcome requestMissingContent(
        const std::vector<TileLoadRequest>& loadRequests,
        FrameResourceBudget* budget = nullptr,
        IPrepareRendererResources* pPrepRenderer = nullptr);
    TileLoadRequestOutcome requestMissingContent(
        TileLoadQueue& loadQueue,
        FrameResourceBudget* budget = nullptr,
        IPrepareRendererResources* pPrepRenderer = nullptr);
    bool processPendingLoads(
        bool interactionActive,
        bool resourceSmoothingActive,
        IPrepareRendererResources* pPrepRenderer,
        FrameResourceBudget* budget = nullptr);

    /// Drain the async GPU upload queue.  Must be called after
    /// processPendingLoads in each frame update.
    /// maxUploadsPerFrame 现为失控上限（backstop），真实限流是 drain 内的
    /// 共享主线程时间预算（mainThreadTimeMs 8ms 墙钟）+ 4 张保底 floor。
    bool drainGpuUploadQueue(
        IPrepareRendererResources* pPrepRenderer = nullptr,
        uint32_t maxUploadsPerFrame = 20);

    void markContentResourcesDirty();
    TileOcclusionState checkSingleTileOcclusion(
        const TilesetTile& tile) const;
    TileOcclusionState checkOcclusion(const TilesetTile& tile) const;
    void rotateSelectionActiveTiles(bool resetSelectionState);
    void retirePreviousSelectionReferencesForReuse();
    void trackSelectionActiveTile(
        TilesetTile& tile,
        bool resetSelectionState);
    void releaseSelectionReferences(std::vector<TilesetTile*>& tiles);

    // ---- Incremental selection active-set ----
    // Replaces the per-frame full-registry selection reset sweep. The reset
    // and traversal now touch only tiles whose selectionFrameState is
    // non-default (visited this/last frame), which is O(visible) instead of
    // O(resident tiles). See TileSelectionStateResetter::resetOne.
public:
    // Frame start: reset last frame's active tiles (shift + decay) and prime
    // this frame's accumulator. Called before traversal.
    void resetActiveSelectionState();
    // Traversal visit entry: lazily reset a newly-visited tile once and add it
    // to this frame's active-set (no-op if already handled this frame).
    void onSelectionVisitTile(TilesetTile& tile);
private:

    TilesetTerrainProviders terrainProviders_;
    std::unique_ptr<TileScheme> tileScheme_;
    std::vector<ActivatedRasterOverlay*> rasterOverlays_;
    RenderDevice* device_ = nullptr;
    TilesetOptions options_;

    TilePlan tilePlan_;

    // cesium-native: byte budget instead of fixed tile count.
    // cesium-native TilesetOptions::maximumCachedBytes default.
    static constexpr int64_t kMaximumCachedBytes = 512LL * 1024 * 1024;
    uint64_t frameNumber_ = 0;

    // Cesium Native TreeTraversalState<Tile::Pointer> equivalent. Each vector
    // owns one reference per entry: current traversal and previous traversal.
    // Their union is also the bounded set whose selection history may need a
    // reset, replacing the old full-registry sweep.
    uint64_t selectionActiveFrameId_ = 0;
    std::vector<TilesetTile*> selectionActiveTiles_;
    std::vector<TilesetTile*> selectionActiveTilesPrev_;

    TilesetTileRegistry tileRegistry_;
    TileContentLifecycleManager contentLifecycle_;
    TileContentCacheManager contentCache_;
    GpuUploadQueue gpuUploadQueue_;  // async CPU→GPU pipeline
    uint64_t resourceRevision_ = 1;
    TileSelectionReuseState selectionReuseState_;
    // Last requestMissingContent outcome, surfaced via loadDiagnostics()
    // to diagnose load-queue stalls (which branch dropped the requests).
    TileLoadRequestOutcome lastRequestOutcome_;
    TileContentResourceInvalidator resourceInvalidator_;
    TileContentAccess contentAccess_;
    TileCacheOwnershipManager cacheOwnership_;
    TileRasterUpsampledChildCoordinator rasterUpsampledChildren_;
    std::unordered_set<std::string> tilesFadingOut_;
    uint64_t generation_ = 0;
    bool interactionActiveForFrame_ = false;
    bool resourceSmoothingActiveForFrame_ = false;
    FrameResourceBudget frameResourceBudget_;
    double lastInteractionActiveTimeSeconds_ = -1.0;
    Vec3 lastCameraPosition_ = Vec3::zero();
    Vec3 lastCameraDirection_ = Vec3::zero();  // for view-weighted priority
    // 遮挡检查的相机派生量缓存（按 lastCameraPosition_ 记忆化，P2-11）。
    // checkSingleTileOcclusion 为 const 查询路径，故 mutable。
    mutable TileSoftwareOcclusionPolicy::CameraContext occlusionCameraContext_;
    mutable bool occlusionCameraContextValid_ = false;
    double currentFrameTimeSeconds_ = 0.0;
    OcclusionCallback occlusionCallback_;
    bool cameraMoving_ = false;
    TileLoadQueue loadQueue_;
    TileSelectionCounters selectionCounters_;
    // §8 等价性 oracle 结果(空=相等)。见 selectionEquivalenceMismatch()。
    std::string selectionEquivalenceMismatch_;
    // ③ 增量切面缓存(incrementalSelection 开启时用;默认路径不触碰)。
    TileIncrementalFrontier incrementalFrontier_;
    TileMeshPreparationManager meshPreparation_;
    TileContentRuntime contentRuntime_;
    TileRenderCommandManager renderCommands_;
    std::vector<TileRenderReference> pendingRenderReferences_;
    // Dedicated off-render-thread selection worker (asyncSelection path only).
    // Lazily created on first async frame; joined on destruction. Held by
    // pointer so the sync path pays nothing and Tileset.h needs only a forward
    // declaration.
    std::unique_ptr<TileSelectionWorker> selectionWorker_;
};

} // namespace earth_engine
