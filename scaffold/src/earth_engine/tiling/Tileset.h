#pragma once

#include <limits>

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
#include "../core/async/WorkLedger.h"
#include "TileMeshPreparationManager.h"
#include "TileOcclusionState.h"
#include "TileRasterUpsampledChildCoordinator.h"
#include "TileRenderCommandManager.h"
#include "TileRenderReferenceReleaser.h"
#include "TileSelectionCounters.h"
#include "TileSelectionMetrics.h"
#include "TileSelectionReuseState.h"
#include "TilesetTerrainProviders.h"
#include "TilesetTileRegistry.h"
#include "TerrainHeightService.h"
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
    // 根层常驻 + 启动预载(漏底根修):z ≤ kPinnedBaseCoverageMaxZoom 的瓦片
    // 一经加载豁免预算驱逐;开启后 update 期还会把全球根层(z≤2)种入
    // Preload 队列并单独推进其影像(见 TilesetUpdateFrameRuntime 的
    // runBaseCoveragePreload)。只应对承担底图覆盖的地形 tileset 开启
    // (SDK 场景路径设 true);内容树默认 false。语义全文见
    // TileBaseCoveragePin.h。
    bool pinBaseCoverage = false;
    // Terrain fill proxy (cesium-js TerrainFillMesh model): give each visible
    // tile still loading real terrain a drape-ready ellipsoid proxy so imagery
    // appears on the smooth globe immediately, then swaps to real terrain (which
    // "rises") when it arrives. Default false = unchanged behavior / golden.
    bool enableTerrainFillProxy = false;
    int terrainFillProxyGridSize = 16;
    // 北极星 Phase 2a 断纹理/几何耦合(见 EarthSceneConfig::decoupleImageryFromGeometry)。
    // true → 影像 isMoreDetailAvailable 不再捏造上采样地形子瓦片,几何 cap 在 DEM
    // native max LOD。默认 false = 忠实 cesium(不改 golden)。
    bool decoupleImageryFromGeometry = false;
    // 接边错位诊断探针(SeamDiag)。默认关 —— 常开每帧 measure 约 4ms 是 selPlan
    // 大头(真机 tether 实测),无缝北极星已收官,探针只为再启动接边 A/B 时用。
    // 见 TileRenderPlanFrameRefresher::refresh 的 logEdgeMismatch 门控。
    bool seamEdgeMismatchProbe = false;
    bool renderTilesUnderCamera = true;
    int64_t maximumCachedBytes = 512LL * 1024 * 1024;
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
    /// 地形细化阈值(SSE),SVT B2a 门② 用它把瓦片屏幕 SSE 换算成「未 cap 时会
    /// 细化到的几何 LOD」= 屏幕合适的影像 zoom(匹配耦合态清晰度)。
    double maximumScreenSpaceError() const {
        return options_.maximumScreenSpaceError;
    }
    // 北极星 SVT B2a:页面 determination 需读影像 provider(front()->getTileProvider())。
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays() const {
        return rasterOverlays_;
    }
    // Phase 2c:GPU 位移开关切换时,失效所有地形瓦片的缓存 draw 命令,强制下帧
    // 按新状态重建(开→改绑共享位移模板;关→回落 CPU baked VBO)。无此失效,已
    // 缓存命令永不重建(rebuildCachedDrawCommands 仅在缓存空时触发),运行时开关
    // 对已加载瓦片零效果;且关闭前失效可清掉命令对 pool GPU buffer 的裸指针引用,
    // 消除 pool 释放后的悬垂句柄崩溃。
    void invalidateTerrainDrawCommands() {
        for (auto& entry : tileRegistry_.tiles()) {
            TilesetTile* tile = entry.second.get();
            if (tile &&
                tile->content.renderContent.isTerrainRenderContent()) {
                tile->content.renderContent.invalidateCachedDrawCommands();
            }
        }
    }

    /// 位移池 ON→OFF 时必须调:摘过幽灵网格的瓦片**没有顶点可回落**,
    /// 光靠 markContentResourcesDirty 让 prepare 重建 legacy VBO 是建不出来的
    /// (顶点已释放)。把它们的渲染内容整个清掉 → 走正常重载路径重新解码。
    /// 返回被清的瓦片数(诊断用)。摘除只发生在生产配置下,本函数只服务
    /// 运行时调试开关,稳态零调用。
    std::size_t reloadGhostReleasedTerrainContent(
        IPrepareRendererResources* pPrepRenderer) {
        std::size_t reloaded = 0;
        for (auto& entry : tileRegistry_.tiles()) {
            TilesetTile* tile = entry.second.get();
            if (!tile || !tile->content.renderContent.ghostGeometryReleased()) {
                continue;
            }
            tile->content.renderContent.clearRenderContent();
            tile->rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
            tile->content.loadState = TileLoadState::Unloaded;
            tile->content.contentKind = TileContentKind::Unknown;
            tile->notifyChildMaterializationStateChanged();
            ++reloaded;
        }
        return reloaded;
    }
    /// 审计用:从**权威容器**重新数一遍"正在把内容加载到终态"的瓦片。
    /// 与 WorkLedger 里 tileContentLoad 的在途数对拍 —— 二者不等 = 有一个
    /// loadState 迁移点没接对账,而那种漏接是静默的(症状要到某次冷启动才
    /// 以"画面冻住零报错"的形式冒出来)。O(注册表),仅诊断周期调用。
    std::size_t countTilesLoadingContent() const {
        std::size_t n = 0;
        for (const auto& entry : tileRegistry_.tiles()) {
            const TilesetTile* tile = entry.second.get();
            if (tile && tile->content.loadState ==
                            TileLoadState::ContentLoading) {
                ++n;
            }
        }
        return n;
    }

    /// 审计用:CPU 常驻字节按类目走账(O(注册表),仅诊断周期调用)。
    /// 回答"内容层每瓦片 ~1.6MB 常驻推算是否成立"——totalBytesUsed 只有总数,
    /// 无法区分 heightmap / 幽灵网格 / 纹理像素。GPU buffer/texture 字节
    /// **不计入**(那是另一本账,见 contentBytesUsed 的混计口径)。
    struct CpuResidentByteBreakdown {
        int64_t heightmapBytes = 0;   // retainedHeightmap(float 高度阵)
        int64_t meshVertexBytes = 0;  // SurfaceVertex 阵(primitive.vertices + runtime.baseVertices)
        int64_t meshIndexBytes = 0;   // uint32 索引阵
        int64_t meshOtherBytes = 0;   // texcoords/colors/tangents/featureIds/instances
        int64_t texturePixelBytes = 0;  // 解码后 CPU 侧纹理像素(GltfTexture.image)
        int64_t terrainGpuVertexBytes = 0;  // 预建 32B 顶点字节(应稳态 0,clear 后)
        int64_t fillModelBytes = 0;   // fill 代理 glTF
        // 幽灵子集:共享模板激活时 draw 必换模板、从不被绘制的地形网格
        // (镜像 GltfRenderResourcePreparer 的 skipBakedTerrainGeometry 判据)。
        int64_t ghostMeshBytes = 0;
        int tileCount = 0;       // 注册表内瓦片总数
        int heightmapTiles = 0;  // 带 retainedHeightmap 的瓦片数
        int ghostTiles = 0;      // 幽灵网格瓦片数
        // **机制信号**:内容层根本没造网格的瓦片数(templateGeometryOnly)。
        // 与 ghostTiles 的区别是「没造」vs「造了又释放」—— 两者字节读数完全
        // 相同,只有这个计数能把它们分开(见"没出问题≠没走到那条路")。
        int templateOnlyTiles = 0;
    };
    void accumulateCpuResidentBytes(CpuResidentByteBreakdown& out,
                                    bool sharedTemplateActive) const;

    bool shouldHoldPresentationFrame() const;
    bool requiresBaseImageryPresentationSurface() const;
    const TileScheme& tileScheme() const { return *tileScheme_; }
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

    /// CPU 统一高程采样服务(索引化点查询/质量标签/渲染网格一致模式)。
    /// 相机探针、矢量贴地、拾取的共同高度来源;仅渲染线程。
    const TerrainHeightService& heightService() const {
        return heightService_;
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

    /// 标记内容资源脏(下轮 upload 生命周期重走 prepare)。P5b:Engine 在运行时
    /// 关闭 GPU 位移(pool reset)后调用,让模板活跃期间跳过 per-tile VBO 的
    /// fine 地形瓦片重建 legacy VBO 补洞(原为 friend-only,提为 public API)。
    void markContentResourcesDirty();

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
    // NSDMI:声明序在 tileScheme_/tileRegistry_ 之后,构造时二者已就绪;
    // 服务与 Tileset 同生共死是其索引持瓦片裸指针的安全前提。
    TerrainHeightService heightService_{tileRegistry_, *tileScheme_};
    TileContentLifecycleManager contentLifecycle_;
    TileContentCacheManager contentCache_;
    GpuUploadQueue gpuUploadQueue_;  // async CPU→GPU pipeline
    /// Phase B:GPU 上传积压的 Pumped 对账槽。ContentLoaded→Done 的上传尾此前
    /// 靠 kSettleFrames 兜(gating idle 后多渲几帧);积压 > settle 容量时会漏。
    /// 队列非空即持 Pumped 令牌,强制出帧直到排空,不再依赖 settle。demo
    /// (decoupleImageryFromGeometry=true)队列恒空→令牌恒不取→零影响。
    WorkTicketSlot gpuUploadSlot_;
    uint64_t resourceRevision_ = 1;
    TileSelectionReuseState selectionReuseState_;
    // Last requestMissingContent outcome, surfaced via loadDiagnostics()
    // to diagnose load-queue stalls (which branch dropped the requests).
    TileLoadRequestOutcome lastRequestOutcome_;
    TileContentResourceInvalidator resourceInvalidator_;
    TileContentAccess contentAccess_;
    TileCacheOwnershipManager cacheOwnership_;
    TileRasterUpsampledChildCoordinator rasterUpsampledChildren_;
    uint64_t generation_ = 0;
    // 根层预载一次性种子(见 TilesetUpdateFrameRuntime::runBaseCoveragePreload)。
    bool baseCoveragePreloadSeeded_ = false;
    bool interactionActiveForFrame_ = false;
    bool resourceSmoothingActiveForFrame_ = false;
    FrameResourceBudget frameResourceBudget_;
    // 本帧 processPendingLoads 已跑过的帧号,供 LoadsBeforeGpuDrain 契约比对。
    uint64_t processPendingLoadsFrameId_ = std::numeric_limits<uint64_t>::max();
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
    TileMeshPreparationManager meshPreparation_;
    TileContentRuntime contentRuntime_;
    TileRenderCommandManager renderCommands_;
    std::vector<TileRenderReference> pendingRenderReferences_;
};

} // namespace earth_engine
