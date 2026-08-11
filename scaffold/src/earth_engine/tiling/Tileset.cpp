#include "Tileset.h"

#include "../debug/Contracts.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../tiling/LoadedTerrainHeightSampler.h"
#include "../tiling/TileCacheKey.h"
#include "../tiling/TileFrameResourceBudgetPlanner.h"
#include "../tiling/TileOcclusionResolver.h"
#include "../tiling/TileRenderFrameContext.h"
#include "../tiling/TileRenderReferenceReleaser.h"
#include "../tiling/TileRasterOverlayReadinessPolicy.h"
#include "../tiling/TileSelectionStateResetter.h"
#include "../tiling/TileSoftwareOcclusionPolicy.h"
#include "../tiling/TerrainDisplacementTemplatePool.h"
#include "../tiling/TilesetProviderDiagnosticsCollector.h"
#include "../tiling/TilesetRenderFrameExecutor.h"
#include "../tiling/TilesetUpdateFrameFacade.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../debug/PlatformLog.h"

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

namespace earth_engine {

namespace {

constexpr int kSmoothedMainThreadUploadLimit = 4;

FrameResourceBudgetConfig makeFrameResourceBudgetConfig(
    const TilesetOptions& options,
    bool interactionActive,
    bool resourceSmoothingActive) {
    return TileFrameResourceBudgetPlanner::plan(
        TileFrameResourceBudgetPlanInput::withDefaultTransport(
            options.maximumSimultaneousTileLoads,
            options.mainThreadLoadingTimeLimit,
            interactionActive,
            resourceSmoothingActive));
}

} // namespace

Tileset::Tileset(std::unique_ptr<TileScheme> tileScheme,
                 std::vector<ActivatedRasterOverlay*> rasterOverlays,
                 RenderDevice* device,
                 TilesetOptions options)
    : Tileset(
          TilesetTerrainProviders(nullptr),
          std::move(tileScheme),
          std::move(rasterOverlays),
          device,
          std::move(options)) {}

Tileset::Tileset(TilesetTerrainProviders terrainProviders,
                 std::unique_ptr<TileScheme> tileScheme,
                 std::vector<ActivatedRasterOverlay*> rasterOverlays,
                 RenderDevice* device,
                 TilesetOptions options)
    : terrainProviders_(std::move(terrainProviders)),
      tileScheme_(std::move(tileScheme)),
      rasterOverlays_(std::move(rasterOverlays)),
      device_(device),
      options_(std::move(options)),
      resourceInvalidator_(
          resourceRevision_,
          contentCache_),
      contentAccess_(
          terrainProviders_.contentProviderOwnsTerrainQuadtree()
              ? TileContentAccess::forContentTerrain(
                    tileRegistry_,
                    *tileScheme_,
                    *terrainProviders_.contentProvider(),
                    &resourceInvalidator_)
              : TileContentAccess::forNoTerrain(
                    tileRegistry_,
                    *tileScheme_,
                    terrainProviders_.contentProvider(),
                    &resourceInvalidator_)),
      cacheOwnership_(
          contentCache_,
          contentLifecycle_,
          loadQueue_,
          tileRegistry_.tiles(),
          rasterOverlays_,
          resourceSmoothingActiveForFrame_,
          options_.maximumCachedBytes,
          options_.tileCacheUnloadTimeLimit,
          &gpuUploadQueue_),
      rasterUpsampledChildren_(
          contentAccess_,
          resourceInvalidator_),
      meshPreparation_(
          contentLifecycle_,
          resourceInvalidator_,
          loadQueue_,
          terrainProviders_.hasTerrainQuadtree(),
          device_,
          rasterOverlays_),
      contentRuntime_(
          contentLifecycle_,
          contentAccess_,
          meshPreparation_,
          resourceInvalidator_),
      renderCommands_(
          meshPreparation_,
          resourceInvalidator_,
          rasterOverlays_,
          device_) {
    // 根层常驻(漏底根修,见 TileBaseCoveragePin.h):只在承担底图覆盖的
    // tileset 上开启(SDK 场景路径设 options.pinBaseCoverage=true)。
    contentCache_.setBaseCoveragePinned(options_.pinBaseCoverage);
    frameResourceBudget_.beginFrame(
        0,
        makeFrameResourceBudgetConfig(options_, false, false));
    for (ActivatedRasterOverlay* overlay : rasterOverlays_) {
        if (overlay) {
            overlay->ensureTileProvider(device_);
        }
    }
}

Tileset::Tileset(
    std::unique_ptr<TileScheme> tileScheme,
    std::vector<ActivatedRasterOverlay*> rasterOverlays,
    RenderDevice* device,
    TilesetOptions options,
    std::unique_ptr<TilesetContentProvider> contentProvider)
    : Tileset(
          TilesetTerrainProviders(std::move(contentProvider)),
          std::move(tileScheme),
          std::move(rasterOverlays),
          device,
          std::move(options)) {}

bool Tileset::hasTerrainQuadtree() const {
    return terrainProviders_.hasTerrainQuadtree();
}

Tileset::~Tileset() {
    discardPendingRenderReferences();
    releaseSelectionReferences(selectionActiveTiles_);
    releaseSelectionReferences(selectionActiveTilesPrev_);
    // cesium-native keeps TilesetContentManager alive while worker callbacks
    // complete. This local engine has synchronous destruction, so wait until
    // every callback has observed the destroyed state and left the callback.
    contentLifecycle_.shutdown();
}

int Tileset::pendingRequests() const {
    return contentLifecycle_.pendingRequests();
}

int64_t Tileset::totalBytesUsed() const {
    return cacheOwnership_.totalBytesUsed();
}

int64_t Tileset::contentBytesUsed() const {
    return cacheOwnership_.contentBytesUsed();
}

void Tileset::accumulateCpuResidentBytes(CpuResidentByteBreakdown& out,
                                         bool sharedTemplateActive) const {
    // 单个 glTF 模型的 CPU 侧字节按类目累加。与 GltfModel::byteSize() 同一
    // 枚举范围,只是拆开类目;两者若失散,总和对不上 totalBytesUsed 会立刻暴露。
    const auto accumulateModel = [](const GltfModel& model,
                                    int64_t& vertexBytes,
                                    int64_t& indexBytes,
                                    int64_t& otherBytes,
                                    int64_t& texelBytes,
                                    int64_t& terrainGpuBytes) {
        for (const GltfPrimitive& primitive : model.primitives) {
            vertexBytes += static_cast<int64_t>(
                primitive.vertices.size() * sizeof(SurfaceVertex));
            vertexBytes += static_cast<int64_t>(
                primitive.runtime.baseVertices.size() * sizeof(SurfaceVertex));
            indexBytes += static_cast<int64_t>(
                primitive.indices.size() * sizeof(uint32_t));
            terrainGpuBytes += static_cast<int64_t>(
                primitive.terrainGpuVertexBytes.size());
            for (const auto& texCoords : primitive.vertexTexCoords) {
                otherBytes += static_cast<int64_t>(
                    texCoords.size() * sizeof(std::array<float, 2>));
            }
            otherBytes += static_cast<int64_t>(
                primitive.vertexColors.size() * sizeof(std::array<float, 4>));
            otherBytes += static_cast<int64_t>(
                primitive.vertexTangents.size() * sizeof(std::array<float, 4>));
            otherBytes += static_cast<int64_t>(
                primitive.featureIds.size() * sizeof(uint32_t));
            otherBytes += static_cast<int64_t>(
                primitive.instances.size() * sizeof(GltfInstance));
        }
        for (const GltfTexture& texture : model.textures) {
            texelBytes += static_cast<int64_t>(texture.image.pixels.size());
        }
    };
    for (const auto& entry : tileRegistry_.tiles()) {
        const TilesetTile* tile = entry.second.get();
        if (!tile) {
            continue;
        }
        ++out.tileCount;
        const TileRenderContentState& rc = tile->content.renderContent;
        if (const DecodedHeightmap* hm = rc.retainedHeightmap()) {
            ++out.heightmapTiles;
            out.heightmapBytes +=
                TileRenderContentState::estimateHeightmapBytes(*hm);
        }
        if (const GltfModel* model = rc.gltfModelForRead()) {
            for (const GltfPrimitive& primitive : model->primitives) {
                if (primitive.templateGeometryOnly) {
                    ++out.templateOnlyTiles;
                    break;
                }
            }
            const int64_t vertexBefore = out.meshVertexBytes;
            const int64_t indexBefore = out.meshIndexBytes;
            accumulateModel(*model, out.meshVertexBytes, out.meshIndexBytes,
                            out.meshOtherBytes, out.texturePixelBytes,
                            out.terrainGpuVertexBytes);
            // 幽灵判据 = prepare 侧 skipBakedTerrainGeometry 的镜像
            // (GltfRenderResourcePreparer.cpp):这些瓦片 draw 时必换共享
            // 位移模板,其 CPU 网格从不被绘制。
            const DecodedHeightmap* hm = rc.retainedHeightmap();
            if (sharedTemplateActive && rc.isTerrainRenderContent() &&
                hm != nullptr && hm->valid() &&
                terrainReliefFade(tile->key.z) > 0.001f) {
                ++out.ghostTiles;
                out.ghostMeshBytes += (out.meshVertexBytes - vertexBefore) +
                                      (out.meshIndexBytes - indexBefore);
            }
        }
        if (const GltfModel* fill = rc.fillContent()) {
            out.fillModelBytes += fill->byteSize();
        }
    }
}

int64_t Tileset::imageryTextureBytesUsed() const {
    return cacheOwnership_.imageryTextureBytesUsed();
}

void Tileset::setOcclusionCallback(OcclusionCallback callback) {
    occlusionCallback_ = std::move(callback);
}

void Tileset::clearOcclusionCallback() {
    occlusionCallback_ = nullptr;
}

TilesetLoadDiagnostics Tileset::loadDiagnostics() const {
    TilesetLoadDiagnostics diagnostics = TileLoadDiagnosticsCollector::collect(
        loadQueue_,
        contentLifecycle_.loadLifecycle(),
        frameResourceBudget_,
        contentCache_.unloadQueue(),
        tileRegistry_.tiles());
    TilesetProviderDiagnosticsCollector::collectContentAndRaster(
        terrainProviders_.contentProvider(),
        rasterOverlays_)
        .applyTo(diagnostics);
    diagnostics.lastRequestOutcome = lastRequestOutcome_;
    return diagnostics;
}

TileContentRuntimeRequestFrame
Tileset::makeContentRuntimeRequestFrame(
    IPrepareRendererResources* pPrepRenderer) {
    TileContentRuntimeRequestFrame frame{
        rasterOverlays_,
        tileRegistry_.tiles()};
    frame.contentProvider = terrainProviders_.contentProvider();
    frame.device = device_;
    frame.pPrepRenderer = pPrepRenderer;
    frame.frameNumber = frameNumber_;
    frame.maximumSimultaneousTileLoads =
        options_.maximumSimultaneousTileLoads;
    frame.mainThreadLoadingTimeLimit = options_.mainThreadLoadingTimeLimit;
    frame.currentFrameTimeSeconds = currentFrameTimeSeconds_;
    frame.smoothedMainThreadUploadLimit =
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit);
    // 摘 glTF 第一级:与 prepare/draw 两侧的模板门控同源(都问 Renderer 的
    // pool 是否存在),让 provider 在「必走模板」时不造网格。
    frame.terrainSharedTemplateActive =
        pPrepRenderer != nullptr &&
        pPrepRenderer->terrainSharedTemplateActive();
    return frame;
}

TileContentRuntimeUploadFrame Tileset::makeContentRuntimeUploadFrame(
    IPrepareRendererResources* pPrepRenderer) {
    TileContentRuntimeUploadFrame frame{rasterOverlays_};
    frame.contentProvider = terrainProviders_.contentProvider();
    frame.device = device_;
    frame.pPrepRenderer = pPrepRenderer;
    frame.gpuUploadQueue = &gpuUploadQueue_;
    frame.frameNumber = frameNumber_;
    frame.maximumSimultaneousTileLoads =
        options_.maximumSimultaneousTileLoads;
    frame.mainThreadLoadingTimeLimit = options_.mainThreadLoadingTimeLimit;
    frame.currentFrameTimeSeconds = currentFrameTimeSeconds_;
    frame.smoothedMainThreadUploadLimit =
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit);
    frame.allowGhostGeometryRelease = options_.decoupleImageryFromGeometry;
    return frame;
}

TileLoadRequestOutcome Tileset::requestMissingContent(
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget,
    IPrepareRendererResources* pPrepRenderer) {
    lastRequestOutcome_ = contentRuntime_.requestMissingTiles(
        loadRequests,
        makeContentRuntimeRequestFrame(pPrepRenderer),
        budget);
    return lastRequestOutcome_;
}

TileLoadRequestOutcome Tileset::requestMissingContent(
    TileLoadQueue& loadQueue,
    FrameResourceBudget* budget,
    IPrepareRendererResources* pPrepRenderer) {
    lastRequestOutcome_ = contentRuntime_.requestMissingTiles(
        loadQueue,
        makeContentRuntimeRequestFrame(pPrepRenderer),
        budget);
    return lastRequestOutcome_;
}

bool Tileset::processPendingLoads(
    bool interactionActive,
    bool resourceSmoothingActive,
    IPrepareRendererResources* pPrepRenderer,
    FrameResourceBudget* budget) {
    processPendingLoadsFrameId_ = frameResourceBudget_.frameNumber();
    return contentRuntime_.processPendingUploads(
        makeContentRuntimeUploadFrame(pPrepRenderer),
        interactionActive,
        resourceSmoothingActive,
        budget);
}

bool Tileset::drainGpuUploadQueue(
    IPrepareRendererResources* pPrepRenderer,
    uint32_t maxUploadsPerFrame) {
    // 契约(消费侧):drain 必须晚于本帧的 processPendingLoads。
    //
    // worker 解码由 processPendingLoads 派发并推进 GpuUploadQueue,同帧的 drain
    // 才取得到;次序反了不会报错,只会让每次上传恒滞后一帧 —— 表现为"加载总是
    // 慢半拍",而这种钝化的症状最容易被归因成网络或设备。
    // AI_INDEX §20 列了这条,小标题写明 "enforced by call order, not by types"。
    //
    // ⚠️ 与 SubmitBeforeReleaseRefs 同类:内部状态 vs 内部状态,挡的是未来重排,
    // 活性由 coverage 证明,无反例控制组。
    GE_CONTRACT(contracts::Id::LoadsBeforeGpuDrain,
                processPendingLoadsFrameId_ == frameResourceBudget_.frameNumber(),
                "frame=%llu loadsFrame=%llu maxUploads=%u",
                (unsigned long long)frameResourceBudget_.frameNumber(),
                (unsigned long long)processPendingLoadsFrameId_,
                maxUploadsPerFrame);
    return contentRuntime_.drainGpuUploadQueue(
        makeContentRuntimeUploadFrame(pPrepRenderer),
        &frameResourceBudget_,
        maxUploadsPerFrame);
}

void Tileset::markContentResourcesDirty() {
    contentRuntime_.markResourcesDirty();
}

TileOcclusionState Tileset::checkSingleTileOcclusion(
    const TilesetTile& tile) const {
    if (occlusionCallback_) {
        return occlusionCallback_(tile);
    }
    // 相机派生量（迭代法测地转换等）按相机位置记忆化：一帧内逐瓦片
    // 调用共享同一份 CameraContext（P2-11）。
    const Vec3& cameraPosition = lastCameraPosition_;
    if (!occlusionCameraContextValid_ ||
        occlusionCameraContext_.cameraPosition.x() != cameraPosition.x() ||
        occlusionCameraContext_.cameraPosition.y() != cameraPosition.y() ||
        occlusionCameraContext_.cameraPosition.z() != cameraPosition.z()) {
        occlusionCameraContext_ =
            TileSoftwareOcclusionPolicy::CameraContext::fromCameraPosition(
                cameraPosition);
        occlusionCameraContextValid_ = true;
    }
    return TileSoftwareOcclusionPolicy::check(tile, occlusionCameraContext_);
}

TileOcclusionState Tileset::checkOcclusion(const TilesetTile& tile) const {
    return TileOcclusionResolver::check(
        tile,
        [this](const TilesetTile& occlusionTile) {
            return checkSingleTileOcclusion(occlusionTile);
        });
}

void Tileset::rotateSelectionActiveTiles(bool resetSelectionState) {
    ++selectionActiveFrameId_;
    std::swap(selectionActiveTiles_, selectionActiveTilesPrev_);

    if (resetSelectionState) {
        auto resetOnce = [this](TilesetTile* tile) {
            if (!tile ||
                tile->selectionActiveFrameId == selectionActiveFrameId_) {
                return;
            }
            tile->selectionActiveFrameId = selectionActiveFrameId_;
            TileSelectionStateResetter::resetOne(*tile, rasterOverlays_);
        };

        // The vector that became current is two traversals old. Reset it once
        // more to decay stale previousSelectionState before releasing it. A
        // stable tile is also in the previous vector, and the frame stamp keeps
        // that overlap from shifting its selection history twice.
        for (TilesetTile* tile : selectionActiveTiles_) {
            resetOnce(tile);
        }
        for (TilesetTile* tile : selectionActiveTilesPrev_) {
            resetOnce(tile);
        }
    }

    // beginTraversal(): previous.swap(current), current.clear(). Clearing the
    // intrusive-pointer vector releases only the two-traversals-old ownership;
    // previous traversal references remain live while this frame is selected.
    releaseSelectionReferences(selectionActiveTiles_);
}

void Tileset::retirePreviousSelectionReferencesForReuse() {
    releaseSelectionReferences(selectionActiveTilesPrev_);
}

void Tileset::trackSelectionActiveTile(
    TilesetTile& tile,
    bool resetSelectionState) {
    if (resetSelectionState &&
        tile.selectionActiveFrameId != selectionActiveFrameId_) {
        tile.selectionActiveFrameId = selectionActiveFrameId_;
        TileSelectionStateResetter::resetOne(tile, rasterOverlays_);
    }
    if (tile.selectionTraversalFrameId == selectionActiveFrameId_) {
        return;
    }

    tile.selectionTraversalFrameId = selectionActiveFrameId_;
    selectionActiveTiles_.push_back(&tile);
    tile.addReference();
    cacheOwnership_.markIneligibleForUnloading(
        TileCacheKey::forTile(tile.key));
}

void Tileset::releaseSelectionReferences(
    std::vector<TilesetTile*>& tiles) {
    for (TilesetTile* tile : tiles) {
        if (!tile) {
            continue;
        }
        assert(tile->referenceCount() > 0);
        tile->removeReference();
        cacheOwnership_.markEligibleForUnloading(
            tile,
            TileCacheKey::forTile(tile->key));
    }
    tiles.clear();
}

void Tileset::resetActiveSelectionState() {
    rotateSelectionActiveTiles(true);
}

void Tileset::onSelectionVisitTile(TilesetTile& tile) {
    trackSelectionActiveTile(tile, true);
}

std::optional<float> Tileset::sampleHeightOptional(
    double lngRad, double latRad) const {
    // 渲染网格一致:本入口的两个消费者(拾取、相机单点兜底)问的都是
    // "屏幕上那张面"——拾取语义是点到所见地形,相机碰撞是不穿所见地形,
    // 与探针/矢量贴地同源。全分辨率双线性与渲染面的格内起伏差在这里
    // 是亚米级误差来源而非精度提升(决策2,2026-08-07)。
    const auto sample = heightService_.sample(
        lngRad, latRad, TerrainHeightService::Interp::RenderGridConsistent);
    if (!sample) {
        return std::nullopt;
    }
    return sample->height;
}

void Tileset::update(
    const FrameState& frameState,
    IPrepareRendererResources* pPrepRenderer) {
    const double t0 = perf::nowMs();
    TilesetUpdateFrameFacade::update(*this, frameState, pPrepRenderer);
    const double t_tot = perf::nowMs() - t0;
    if (t_tot > 5.0) {
        platformLog(LogLevel::Info, "EarthPerf",
            "Tileset.update_real: %.2f ms", t_tot);
    }
}

void Tileset::buildRenderCommands(Renderer& renderer,
                                  RenderCommandList& commands,
                                  uint64_t renderFrameId,
                                  const std::vector<TileRenderEntry>*
                                      renderEntriesOverride) {
    if (!pendingRenderReferences_.empty()) {
        platformLog(
            LogLevel::Error,
            "Tileset",
            "buildRenderCommands rejected: the previous command batch still "
            "owns render references; submit it or explicitly discard it");
        return;
    }
    ++frameNumber_;
    const uint64_t commandFrameNumber =
        renderFrameId != 0 ? renderFrameId : frameNumber_;
    renderCommands_.beginFrame(
        commandFrameNumber,
        generation_,
        currentFrameTimeSeconds_,
        &tilePlan_.edgeLutTables);
    TilesetRenderFrameExecutor::buildRenderCommands(
        TileRenderFrameContext{
            TileRenderFrameCoordinatorInput{
                tilePlan_,
                rasterOverlays_,
                commandFrameNumber,
                lastCameraPosition_,
                options_.fogDensityTable,
                selectionCounters_.fogCulled,
                resourceSmoothingActiveForFrame_,
                interactionActiveForFrame_,
                renderEntriesOverride},
            renderCommands_,
            cacheOwnership_,
            pendingRenderReferences_},
        renderer,
        commands);
}

bool Tileset::shouldHoldPresentationFrame() const {
    const bool hasSelectedSurfaceWork =
        !tilePlan_.visibleTiles.empty();
    if (!hasSelectedSurfaceWork ||
        !requiresBaseImageryPresentationSurface()) {
        return false;
    }

    if (tilePlan_.renderEntries.empty()) {
        static int sEmptyLogCount = 0;
        if ((sEmptyLogCount++ % 60) == 0) {
            platformLog(
                LogLevel::Info,
                "EarthPerf",
                "HoldEntriesEmpty visible=%zu",
                tilePlan_.visibleTiles.size());
        }
        return true;
    }

    // never-drop(cesium 语义):渲染集里缺 base 影像的 entry 是合法的 base 色
    // 兜底渲染(finalizer 发、命令端出 count=0 纯色面),**不再等影像就绪** ——
    // 有 entry 就立即呈现(画灰不画黑),不冻帧。仅当渲染集彻底为空(冷启动
    // 首帧,真没几何可画)才 hold(上面那条)。旧行为
    // (!plannedRenderEntriesHaveRequiredBaseImagery → hold)会在快拉到影像未
    // 下载完的经度时冻帧 ~2s;never-drop 下该立即呈现兜底面。
    return false;
}

bool Tileset::requiresBaseImageryPresentationSurface() const {
    const bool hasSelectedSurfaceWork =
        !tilePlan_.visibleTiles.empty();
    if (!hasSelectedSurfaceWork) {
        return false;
    }

    for (const ActivatedRasterOverlay* activeOverlay : rasterOverlays_) {
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        const RasterOverlay& overlay = activeOverlay->getOverlay();
        if (overlay.role() == RasterOverlayRole::BaseImagery &&
            overlay.blocksCompleteRenderable()) {
            return true;
        }
    }

    return false;
}

bool Tileset::plannedRenderEntriesHaveRequiredBaseImagery() const {
    // 这条 hold 发生在建命令之前,一旦成立整帧不 present;而 present 不发生时
    // frameId 不前进 → 所有 %120 的心跳诊断全哑火,现场只剩一块不动的屏幕。
    // 所以这里独立限频打印成因,不依赖 frameId。
    int missingTile = 0;
    int notDrawable = 0;
    TileKey firstMissingKey{};
    TileKey firstNotDrawableKey{};
    BaseImageryBlockReason firstReason = BaseImageryBlockReason::None;
    for (const TileRenderEntry& entry : tilePlan_.renderEntries) {
        const TilesetTile* renderTile =
            tileRegistry_.findTile(entry.renderKey);
        if (!renderTile) {
            if (missingTile++ == 0) {
                firstMissingKey = entry.renderKey;
            }
            continue;
        }
        if (!TileRasterOverlayReadinessPolicy::
                terrainSurfaceImageryDrawableReady(
                    *renderTile,
                    rasterOverlays_)) {
            if (notDrawable++ == 0) {
                firstNotDrawableKey = entry.renderKey;
                firstReason =
                    TileRasterOverlayReadinessPolicy::baseImageryBlockReason(
                        *renderTile,
                        rasterOverlays_);
            }
        }
    }
    if (missingTile == 0 && notDrawable == 0) {
        return true;
    }
    static int sHoldLogCount = 0;
    if ((sHoldLogCount++ % 60) == 0) {
        platformLog(
            LogLevel::Info,
            "EarthPerf",
            "HoldPlanDiag entries=%zu missingTile=%d(%d/%d/%d) "
            "notDrawable=%d(%d/%d/%d) reason=%d",
            tilePlan_.renderEntries.size(),
            missingTile,
            firstMissingKey.z,
            firstMissingKey.x,
            firstMissingKey.y,
            notDrawable,
            firstNotDrawableKey.z,
            firstNotDrawableKey.x,
            firstNotDrawableKey.y,
            static_cast<int>(firstReason));
    }
    return false;
}

void Tileset::releaseRenderReferences() {
    // cesium-native: called from Scene after renderer_->submit().
    // Drops the reference that was added in buildRenderCommands for
    // tiles in the current render list, making them eligible for
    // unloading in the next frame.
    discardPendingRenderReferences();
}

void Tileset::discardPendingRenderReferences() {
    TileRenderReferenceReleaser::release(
        pendingRenderReferences_,
        [this](const TilesetTile* tile, const std::string& cacheKey) {
            cacheOwnership_.markEligibleForUnloading(tile, cacheKey);
        });
}

} // namespace earth_engine
