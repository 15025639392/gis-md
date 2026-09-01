#include "Scene.h"

#include "../core/async/WorkLedger.h"

#include <cstring>
#include "../debug/PlatformLog.h"
#include "../debug/PerfTimer.h"
#include "Camera.h"
#include "SceneEnvironmentCoordinator.h"
#include "SceneInteractionCoordinator.h"
#include "SceneLayerCoordinator.h"
#include "SceneFrameUpdateCoordinator.h"
#include "SceneRenderPipeline.h"
#include "SceneTelemetryCoordinator.h"
#include "SceneTilesetCoordinator.h"
#include "PresentationTrace.h"
#include "../camera/CameraSystem.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../environment/SkyGradient.h"
#include "../interaction/InputEvent.h"
#include "../layers/FeatureRenderLayer.h"
#include "../layers/VectorLayer.h"
#include "../renderer/GlyphAtlas.h"
#include "../renderer/IconAtlas.h"
#include "../renderer/Renderer.h"
#include "../tiling/Tileset.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../renderer/TerrainPageStore.h"
#include "../renderer/AmapTerrainFillMaskStore.h"
#include "../style/AmapClassicRuntime.h"

#include <utility>
#include <algorithm>
#include <cmath>

namespace earth_engine {
namespace {

Vec3 traceArrayToVec3(const std::array<double, 3>& values) {
    return Vec3(values[0], values[1], values[2]);
}

bool cameraStillMatchesPresentedFrame(const FrameState& frameState,
                                      const PresentationTrace& trace) {
    if (!frameState.camera || trace.camera.frameId == 0) {
        return false;
    }

    const Camera& camera = *frameState.camera;
    const Vec3 previousPosition = traceArrayToVec3(trace.camera.position);
    const Vec3 previousDirection = traceArrayToVec3(trace.camera.direction);
    const double heightScale =
        std::max(1000.0, std::abs(trace.camera.cameraHeightMeters));
    if (camera.position().distanceTo(previousPosition) > heightScale * 0.05) {
        return false;
    }

    constexpr double kTwoDegreesCos = 0.9993908270190958;
    return camera.direction().normalized().dot(
               previousDirection.normalized()) >= kTwoDegreesCos;
}

// presentation hold 的连续帧上限(~1s @60fps)。见 Scene::shouldHoldPresentationFrame。
constexpr int kMaximumConsecutiveHeldFrames = 60;

bool shouldHoldTerrainCoverageTakeover(
    const Tileset& primaryTileset,
    const FrameState& frameState,
    const PresentationTrace& previousTrace) {
    if (!primaryTileset.requiresBaseImageryPresentationSurface() ||
        previousTrace.tilesets.empty() ||
        !cameraStillMatchesPresentedFrame(frameState, previousTrace)) {
        return false;
    }

    // 覆盖度用 finalizer 的丢弃计数,**不用渲染项条数**。
    //
    // 旧实现比的是「本帧渲染项条数 < 上一张呈现帧的条数」。条数不等于覆盖度:
    // 一个祖先 entry 可以覆盖多个选中瓦片(finalizer dedup),LOD 收敛合并也会让
    // 条数下降,这两种情况覆盖分毫未变。而参照值取自 presentationTrace,trace 又
    // 只在真的 present 时才刷新 —— 一旦把正常收敛误判成覆盖倒退,就会 hold,
    // hold 又让参照值永远停在旧值,相机不动便永不恢复(近景默认相机整屏不动的
    // 真因,划一下屏幕才解锁)。
    //
    // finalizer 已经算好了精确信号:这两个计数是「本帧被选中、却一点几何都没拿到」
    // 的瓦片数,为 0 就说明覆盖完整,没有任何需要压的缺口。
    const TilePlan& plan = primaryTileset.tilePlan();
    const int uncoveredSelectedTiles =
        plan.renderEntryDropNotBuildableCount +
        plan.renderEntryDropClipUvCount;
    return uncoveredSelectedTiles > 0;
}

} // namespace

Scene::Scene()
    : camera_(std::make_unique<Camera>()),
      cameraSystem_(std::make_unique<CameraSystem>(camera_.get())),
      layers_(std::make_unique<SceneLayerCoordinator>()),
      tilesets_(std::make_unique<SceneTilesetCoordinator>()),
      interaction_(std::make_unique<SceneInteractionCoordinator>()),
      environment_(std::make_unique<SceneEnvironmentCoordinator>()),
      telemetry_(std::make_unique<SceneTelemetryCoordinator>()) {

    // OpenGlobus PlanetCamera reverse-Z defaults: near=150, far=1e12.
    camera_->setPerspective(
        camera_->verticalFovRadians(),
        150.0,
        1e12);
    // Depth func/clear are configured per-platform by RenderDevice.
    configureCameraSurfacePicker();
    interaction_->setFeatureStateChangeCallback(
        [this](const std::string& layerId,
               const std::string& featureId,
               FeatureState state) {
            layers_->applyFeatureState(layerId, featureId, state);
        });
}

Scene::~Scene() {
    // Tilesets borrow ActivatedRasterOverlay instances owned by the SDK
    // facade/runtime.  Release every tile tree before tearing down the
    // official runtime so no late tile/provider callback can observe a dead
    // overlay or source bundle.
    if (tilesets_) {
        tilesets_->clearAll();
    }
    if (renderer_) {
        renderer_->setTerrainFillMaskStore(nullptr);
    }
    terrainFillMaskStore_ = nullptr;
    amapClassicRuntime_.reset();
    clearMvtVectorSources();
    renderer_.reset();
}

void Scene::setTerrainPageStore(TerrainPageStore* store) {
    terrainPageStore_ = store;
    if (renderer_) {
        renderer_->setTerrainPageStore(store);
    }
}

void Scene::setTerrainDisplacementPool(TerrainDisplacementTemplatePool* pool) {
    if (renderer_) {
        renderer_->setTerrainDisplacementPool(pool);
    }
}

bool Scene::setRenderDevice(RenderDevice* device) {
    renderDevice_ = device;
    if (!device) {
        for (auto& runtime : mvtSources_) {
            if (runtime.source) runtime.source->suspend();
        }
        layers_->setRenderDevice(nullptr);
        renderPipeline_.reset();
        renderer_.reset();
        return false;
    }

    layers_->setRenderDevice(device);

    renderer_ = std::make_unique<Renderer>(device);
    renderPipeline_ = std::make_unique<SceneRenderPipeline>();
    if (!renderer_->initialize()) {
        fprintf(stderr, "[Scene] renderer_->initialize() FAILED\n");
        renderPipeline_.reset();
        return false;
    }
    renderer_->setTerrainFillMaskStore(
        terrainFillMaskStore_);

    environment_->initializeRenderResources(device);

    return true;
}

void Scene::setViewport(int widthPixels, int heightPixels, float dpr) {
    frameRuntime_.setViewport(widthPixels, heightPixels, dpr);

    if (cameraSystem_) {
        cameraSystem_->setViewport(widthPixels, heightPixels);
    }
}

void Scene::update(double deltaSeconds) {
    SceneFrameUpdateCoordinator::update(
        frameRuntime_.makeFrameUpdateInput(
            telemetry_->diagnostics(),
            camera_.get(),
            cameraSystem_.get(),
            renderer_.get(),
            *tilesets_,
            !mvtSources_.empty() || amapClassicRuntime_ != nullptr,
            static_cast<uint32_t>(amapClassicRuntime_
                                      ? 3
                                      : mvtSources_.size()),
            terrainPageStore_ != nullptr &&
                tilesets_->hasAnyRasterOverlay(),
            deltaSeconds,
            interaction_->hasInteractionFocus(),
            interaction_->interactionFocusDirection(),
            interaction_->interactionFocusTimeSeconds(),
            environment_->timeController(),
            environment_->skyGradient()));

    const double mvtStartMs = perf::nowMs();
    const FrameState& frame = frameRuntime_.frameState();
    if (frame.camera) {
        const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
        const Cartographic cameraCarto =
            ellipsoid.cartesianToCartographic(frame.camera->position());
        const Vec3& radii = ellipsoid.radii();
        const double minRadius =
            std::min(radii.x(), std::min(radii.y(), radii.z()));
        const Rectangle viewRect = MvtVectorSource::horizonViewRectangle(
            cameraCarto, minRadius);
        const double cameraHeight = std::max(1.0, cameraCarto.height());
        SceneFrameResourceArbiter& arbiter =
            frameRuntime_.resourceArbiter();
        if (amapClassicRuntime_) {
            amapClassicRuntime_->update(viewRect, cameraHeight, arbiter);
            telemetry_->diagnostics().mvtVectorUpdateMs =
                perf::nowMs() - mvtStartMs;
            return;
        }
        std::vector<MvtVectorSource*> activeSources;
        activeSources.reserve(mvtSources_.size());
        for (auto& runtime : mvtSources_) {
            if (runtime.source) activeSources.push_back(runtime.source.get());
        }
        if (!activeSources.empty()) {
            mvtUpdateCursor_ %= activeSources.size();
        }
        const auto fairShare = [&arbiter](
            SceneFrameResourceStage stage, size_t remainingSources) {
            const uint32_t remaining = arbiter.remaining(
                SceneFrameResourceProducer::Mvt,
                stage,
                FrameResourcePriority::Normal);
            if (remainingSources == 0) return uint32_t{0};
            return static_cast<uint32_t>(
                (static_cast<uint64_t>(remaining) + remainingSources - 1) /
                remainingSources);
        };
        for (size_t offset = 0; offset < activeSources.size(); ++offset) {
            const size_t index =
                (mvtUpdateCursor_ + offset) % activeSources.size();
            const size_t remainingSources = activeSources.size() - offset;
            MvtVectorSource::FrameAdmissionLimits limits;
            limits.networkRequests = fairShare(
                SceneFrameResourceStage::NetworkRequest, remainingSources);
            limits.workerDispatches = fairShare(
                SceneFrameResourceStage::WorkerDispatch, remainingSources);
            limits.gpuUploads = fairShare(
                SceneFrameResourceStage::GpuUpload, remainingSources);
            activeSources[index]->update(
                viewRect, cameraHeight, &arbiter, limits);
        }
        if (!activeSources.empty()) {
            mvtUpdateCursor_ =
                (mvtUpdateCursor_ + 1) % activeSources.size();
        }
    }
    telemetry_->diagnostics().mvtVectorUpdateMs =
        perf::nowMs() - mvtStartMs;
}

void Scene::setSelectorViewOverride(
    std::vector<SelectorView> selectorViews) {
    frameRuntime_.setSelectorViewOverride(std::move(selectorViews));
}

void Scene::clearSelectorViewOverride() {
    frameRuntime_.clearSelectorViewOverride();
}

void Scene::setOcclusionCallback(TileOcclusionCallback callback) {
    tilesets_->setOcclusionCallback(std::move(callback));
}

void Scene::clearOcclusionCallback() {
    tilesets_->clearOcclusionCallback();
}

const Diagnostics& Scene::diagnostics() const {
    return telemetry_->diagnostics();
}

SceneFrameResourceArbiterSnapshot Scene::frameResourceArbiterSnapshot() const {
    return frameRuntime_.resourceArbiter().snapshot();
}

void Scene::recordEngineTiming(
    EngineTimingScope scope,
    double elapsedMs) {
    telemetry_->recordEngineTiming(scope, elapsedMs);
}

void Scene::finishEngineFrame(double elapsedMs) {
    telemetry_->finishEngineFrame(elapsedMs);
}

const PresentationTrace& Scene::presentationTrace() const {
    return telemetry_->presentationTrace();
}

bool Scene::render() {
    if (!renderer_ || !renderPipeline_ || !isReady()) return false;

    SceneRenderPipeline::Result renderResult =
        renderPipeline_->render(SceneRenderPipeline::Context{
        frameRuntime_.frameState(),
        telemetry_->diagnostics(),
        *renderer_,
        frameRuntime_.renderCommands(),
        environment_->skyBox(),
        environment_->atmospherePass(),
        environment_->skyGradient(),
        tilesets_->primary(),
        tilesets_->pendingPrimary(),
        terrainFillMaskStore_,
        tilesets_->contentTilesets(),
        layers_->vectorLayers(),
        layers_->mutableFeatureRenderLayers(),
        [this]() { updatePresentationTrace(); },
        renderDevice_,
        sceneRenderTarget_,
        sceneSurfaceWidthPixels_,
        sceneSurfaceHeightPixels_});
    telemetry_->replaceRenderDiagnostics(renderResult.diagnostics);
    return renderResult.presentable;
}

bool Scene::hasConvergingWork(const char** outReason) const {
    auto hit = [outReason](const char* reason) {
        if (outReason) *outReason = reason;
        return true;
    };

    // ① 相机自主演进(惯性滑行/脚本平移)。手指按着不动不算 —— 那条走事件型
    //    脏位,两条路径分开才不会出现两边都不认领的缝。
    if (cameraSystem_ && cameraSystem_->isSelfAnimating()) {
        return hit("cameraAnimating");
    }

    // ② 瓦片流式:网络在途 + 已到未消化。hasPendingWork() 是带锁的权威读数,
    //    不是每 15 帧刷新的 Diagnostics。
    auto tilesetBusy = [](const Tileset* tileset) {
        if (!tileset) return false;
        if (tileset->pendingRequests() > 0) return true;
        for (const ActivatedRasterOverlay* overlay : tileset->rasterOverlays()) {
            if (overlay && overlay->hasPendingWork()) return true;
        }
        return false;
    };
    if (tilesetBusy(tilesets_->primary())) return hit("terrainPending");
    if (tilesetBusy(tilesets_->pendingPrimary())) return hit("pendingPrimary");
    for (const auto& content : tilesets_->contentTilesets()) {
        if (tilesetBusy(content.get())) return hit("contentPending");
    }

    // ③ 页存储:影像异步到达,停帧就没人跑 drainReadyUploads,到货永远灌不进
    //    array —— 表现是该片区永久停在粗页且零报错。
    if (renderer_ && renderer_->terrainPageStore() &&
        renderer_->terrainPageStore()->hasWorkInFlight()) {
        return hit("pageStoreInFlight");
    }
    if (terrainFillMaskStore_ &&
        terrainFillMaskStore_->hasWorkInFlight()) {
        return hit("amapTerrainFillMaskInFlight");
    }

    // ④ V27 标注收敛:字形烘焙/换代 placement/fade 三段全在渲染帧里推进,
    //    停帧 = 新标注永远停在 opacity=0(冷启动 POI 隐形到用户缩放为止)。
    //    ledger gating 路径由层内 Pumped 票(labelConverge)覆盖;本条是
    //    回落判据的对应项 —— 两条判据必须口径一致,否则 auditWorkLedger
    //    每帧报分歧。
    for (const auto& layer : layers_->featureRenderLayers()) {
        if (layer && layer->visible() && layer->hasPendingLabelWork()) {
            return hit("labelConverge");
        }
    }

    // The WorkLedger path already covers MVT fetch/tessellation/commit/retry.
    // Keep the legacy fallback equally complete for platforms without a frame
    // wake callback, otherwise an inbox can land after the host has gone idle.
    for (const auto& runtime : mvtSources_) {
        if (!runtime.source) continue;
        if (runtime.source->tree().pendingCount() > 0 ||
            runtime.source->hasTessellationInFlight() ||
            runtime.source->pendingCommitCount() > 0 ||
            runtime.source->tree().hasRetryPending()) {
            return hit("mvtVectorPending");
        }
    }
    if (amapClassicRuntime_) {
        if (amapClassicRuntime_->sources().hasPendingWork()) {
            return hit("amapOfficialPending");
        }
    }

    if (outReason) *outReason = "idle";
    return false;
}

bool Scene::hasContinuousProducerWork(const char** outReason) const {
    // 账本外的连续生产者(见头文件)。与 hasConvergingWork 的判据①同源:相机
    // 自演进是"每帧改画面"而非"在途工作",故不发令牌,ledger 模式仍需它出帧。
    // 动态时钟/太阳不在此:它走 Engine::advanceTime/setTime 的事件脏位路径。
    if (cameraSystem_ && cameraSystem_->isSelfAnimating()) {
        if (outReason) *outReason = "cameraAnimating";
        return true;
    }
    if (outReason) *outReason = "idle";
    return false;
}

void Scene::auditWorkLedger() const {
    // **影子校验(Phase C 方向)**:gating 已读账本(Engine::ledgerGatingNeedsFrame),
    // 本函数让旧 hasConvergingWork 反过来检查账本 —— 目的是回答"账本权威后,
    // 旧判据是否还有它自己的多余来源或漏源"。
    // 只打不一致,一致时静默(健康态刷屏会让人学会无视这条日志)。
    const uint64_t frameId = frameState().frameId;
    if (lastWorkLedgerAuditFrameId_ == frameId) {
        return;
    }
    lastWorkLedgerAuditFrameId_ = frameId;

    WorkLedger& ledger = WorkLedger::shared();

    // ① 对账完整性:令牌数 vs 从注册表重新数的真值。不等 = 漏接迁移点。
    std::size_t truth = 0;
    if (tilesets_) {
        if (const Tileset* t = tilesets_->primary()) {
            truth += t->countTilesLoadingContent();
        }
        if (const Tileset* t = tilesets_->pendingPrimary()) {
            truth += t->countTilesLoadingContent();
        }
        for (const auto& content : tilesets_->contentTilesets()) {
            if (content) truth += content->countTilesLoadingContent();
        }
    }
    const int ticketed = ledger.outstandingForLabel("tileContentLoad");
    if (static_cast<std::size_t>(ticketed) != truth) {
        platformLog(LogLevel::Error, "WorkLedger",
                    "audit MISMATCH tileContentLoad ticket=%d registry=%zu "
                    "—— 有 loadState 迁移点没接 syncContentLoadWorkTicket",
                    ticketed, truth);
    }

    // ② 迁移漏接审计。相机自演进不发令牌(它不是"在途"而是持续生产者),
    //    故旧判据只因相机而忙时跳过。两个方向不再做对称比较：
    //      old busy / ledger idle = 旧源尚未迁移，ledger gating 可能冻屏；
    //      ledger busy / old idle = 合法，既可能是 Landing 等待，也可能是
    //        Scene 外的 app-owned vector source；ledger 自身已经能正确调度它。
    const char* oldReason = nullptr;
    const bool oldBusy = hasConvergingWork(&oldReason);
    if (oldBusy && oldReason && std::strcmp(oldReason, "cameraAnimating") == 0) {
        return;
    }
    if (!oldBusy) {
        return;
    }
    bool sourceTicketed = false;
    if (oldReason && std::strcmp(oldReason, "labelConverge") == 0) {
        sourceTicketed =
            ledger.outstandingForLabel("labelConverge") > 0 ||
            // GlyphAtlas publishes individual ready results before the
            // coalesced glyphRasterBatch Landing ticket is released. During
            // that interval the legacy predicate is correctly busy, but the
            // layer Pumped ticket is intentionally not held yet. The batch
            // Landing ticket is the authoritative source and will wake the
            // next frame when the remaining glyphs seal.
            ledger.outstandingForLabel("glyphRasterBatch") > 0 ||
            // A glyph worker may release its Landing ticket after the last
            // layer build but before this audit.  The release itself owns the
            // wake pulse; the layer Pumped ticket is re-established by the
            // next build, so this short window is valid and must not be
            // reported as a missing source ticket.
            // Landing labels share one coalesced wake pulse. Concurrent tile
            // and glyph completions may overwrite the diagnostic last-label,
            // but any unconsumed pulse still guarantees the next build. A
            // genuine missing ticket is reported after that pulse is consumed
            // if the old busy predicate remains true.
            ledger.hasUnconsumedLanding();
    } else if (oldReason &&
               std::strcmp(oldReason, "pageStoreInFlight") == 0) {
        sourceTicketed =
            ledger.outstandingForLabel("terrainPageUpload") > 0;
    } else if (oldReason &&
               std::strcmp(oldReason,
                           "amapTerrainFillMaskInFlight") == 0) {
        sourceTicketed =
            ledger.outstandingForLabel(
                "amapTerrainFillMaskFetch") > 0 ||
            // The callback publishes the page before releasing its Landing
            // ticket.  Until the release pulse is consumed, the legacy store
            // predicate can still observe the just-landed entry as pending on
            // an adjacent snapshot without representing a missing wakeup.
            ledger.hasUnconsumedLanding();
    } else if (oldReason &&
               (std::strcmp(oldReason, "amapOfficialPending") == 0 ||
                std::strcmp(oldReason, "mvtVectorPending") == 0)) {
        // AMap official sources use the same VectorTileSourceT machinery and
        // therefore the same fetch/tessellate/commit/retry ticket labels as a
        // generic MVT source.  This branch is audit classification only; it
        // does not create a second ticket or keep network work at full FPS.
        sourceTicketed =
            ledger.outstandingForLabel("mvtVectorFetch") > 0 ||
            ledger.outstandingForLabel("mvtVectorTessellate") > 0 ||
            ledger.outstandingForLabel("mvtVectorCommit") > 0 ||
            ledger.outstandingForLabel("mvtVectorRetry") > 0 ||
            ledger.hasUnconsumedLanding();
    } else {
        // terrainPending / pendingPrimary / contentPending can be backed by a
        // terrain content request, raster request, raster upload, or tile GPU
        // upload.  App-owned vector tickets deliberately do not satisfy this
        // Scene-source audit.
        sourceTicketed =
            ledger.outstandingForLabel("tileContentLoad") > 0 ||
            ledger.outstandingForLabel("rasterOverlayLoad") > 0 ||
            ledger.outstandingForLabel("rasterOverlayUpload") > 0 ||
            ledger.outstandingForLabel("tileGpuUpload") > 0;
    }
    if (sourceTicketed) return;
    platformLog(LogLevel::Error, "WorkLedger",
                "audit MISSING source ticket old=%s(%s) "
                "ledgerTotals(landing=%d pumped=%d)",
                oldBusy ? "busy" : "idle",
                oldReason ? oldReason : "?",
                ledger.outstanding(WorkLedger::Kind::Landing),
                ledger.outstanding(WorkLedger::Kind::Pumped));
}

void Scene::logCpuResidentAccount() {
    // 每 300 帧(~5s@60fps)一行,只走主 Tileset —— 内容 Tileset 的字节由
    // totalMB 与主账之差间接可见,先不单列。O(注册表) 的走账放在限频闸后,
    // 非采样帧零成本。
    if (++cpuAcctFrameCounter_ % 300u != 0u) {
        return;
    }
    const Tileset* primaryTileset = tilesets_ ? tilesets_->primary() : nullptr;
    if (!primaryTileset) {
        return;
    }
    const bool templateActive =
        renderer_ && renderer_->terrainDisplacementPool() != nullptr;
    Tileset::CpuResidentByteBreakdown acct;
    primaryTileset->accumulateCpuResidentBytes(acct, templateActive);
    const auto mb = [](int64_t bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    };
    platformLog(LogLevel::Info, "CpuAcct",
                "tiles=%d hmTiles=%d ghostTiles=%d noMesh=%d | hm=%.1fMB "
                "meshV=%.1fMB meshI=%.1fMB meshO=%.1fMB ghost=%.1fMB "
                "texPix=%.1fMB tgv=%.1fMB fill=%.1fMB | acctTotal=%.1fMB "
                "cacheTotal=%.1fMB",
                acct.tileCount, acct.heightmapTiles, acct.ghostTiles,
                acct.templateOnlyTiles,
                mb(acct.heightmapBytes),
                mb(acct.meshVertexBytes), mb(acct.meshIndexBytes),
                mb(acct.meshOtherBytes), mb(acct.ghostMeshBytes),
                mb(acct.texturePixelBytes), mb(acct.terrainGpuVertexBytes),
                mb(acct.fillModelBytes),
                mb(acct.heightmapBytes + acct.meshVertexBytes +
                   acct.meshIndexBytes + acct.meshOtherBytes +
                   acct.texturePixelBytes + acct.terrainGpuVertexBytes +
                   acct.fillModelBytes),
                mb(primaryTileset->totalBytesUsed()));
}

bool Scene::shouldHoldPresentationFrame() {
    const Tileset* primaryTileset = tilesets_->primary();
    if (!primaryTileset) {
        consecutiveHeldFrames_ = 0;
        return false;
    }
    bool hold = primaryTileset->shouldHoldPresentationFrame();
    if (!hold) {
        hold = shouldHoldTerrainCoverageTakeover(
            *primaryTileset,
            frameRuntime_.frameState(),
            telemetry_->presentationTrace());
        if (hold) {
            static int sTakeoverLogCount = 0;
            if ((sTakeoverLogCount++ % 60) == 0) {
                platformLog(LogLevel::Info, "EarthPerf",
                            "HoldTakeover entries=%zu visible=%zu",
                            primaryTileset->tilePlan().renderEntries.size(),
                            primaryTileset->tilePlan().visibleTiles.size());
            }
        }
    }
    if (!hold) {
        consecutiveHeldFrames_ = 0;
        return false;
    }

    // 活性兜底。这些闸的本意都是压掉**瞬时**的覆盖/纹理缺口闪烁;缺口一旦不是
    // 瞬时的(瓦片停在 Failed 态、影像始终不来),继续扣就是整屏定格 —— 那比露出
    // 一块缺口糟得多。更要紧的是:hold 会让 presentationTrace 停止刷新,任何以
    // trace 为参照的闸都可能自锁,所以必须有一条无条件的出口。
    //
    // 超限后**不清零**,让后续帧继续放行,直到 hold 条件自己消失 —— 否则会退化成
    // "扣 N 帧、放 1 帧"的抽帧循环。
    if (consecutiveHeldFrames_ <= kMaximumConsecutiveHeldFrames) {
        ++consecutiveHeldFrames_;
    }
    if (consecutiveHeldFrames_ > kMaximumConsecutiveHeldFrames) {
        static int sReleaseLogCount = 0;
        if ((sReleaseLogCount++ % 60) == 0) {
            platformLog(LogLevel::Info, "EarthPerf",
                        "HoldReleasedByCap frames=%d — 缺口非瞬时,照常呈现",
                        consecutiveHeldFrames_);
        }
        return false;
    }
    return true;
}

void Scene::updatePresentationTrace() {
    telemetry_->updatePresentationTrace(
        frameRuntime_.frameState(),
        tilesets_->primary(),
        tilesets_->contentTilesets(),
        frameRuntime_.renderCommands());
}

void Scene::setTileset(std::unique_ptr<Tileset> tileset) {
    tilesets_->setPrimary(std::move(tileset));
    configureCameraSurfacePicker();
}

void Scene::stageTilesetReplacement(std::unique_ptr<Tileset> tileset) {
    tilesets_->stagePrimaryReplacement(std::move(tileset));
}

void Scene::addTileset(std::unique_ptr<Tileset> tileset) {
    tilesets_->addContent(std::move(tileset));
}

void Scene::clearTilesets() {
    tilesets_->clearAll();
}

Tileset* Scene::tileset() const {
    return tilesets_->primary();
}

size_t Scene::additionalTilesetCount() const {
    return tilesets_->contentTilesetCount();
}

bool Scene::hasAnyTileset() const {
    return tilesets_->hasAnyTileset();
}

bool Scene::hasTerrain() const {
    return tilesets_->hasPrimaryTerrain();
}

// ---- 矢量图层管理 ----

void Scene::addVectorLayer(std::unique_ptr<VectorLayer> layer) {
    layers_->addVectorLayer(std::move(layer));
}

std::unique_ptr<VectorLayer> Scene::removeVectorLayer(const std::string& layerId) {
    return layers_->removeVectorLayer(layerId);
}

void Scene::addFeatureRenderLayer(std::unique_ptr<FeatureRenderLayer> layer) {
    if (layer && layer->style().usesOfficialProviderContract() &&
        !layer->hasSealedOfficialProfile()) {
        platformLog(LogLevel::Error, "Scene",
                    "reject unsealed official FeatureRenderLayer '%s'",
                    layer->id().c_str());
        return;
    }
    layers_->addFeatureRenderLayer(std::move(layer));
}

bool Scene::addOfficialFeatureRenderLayer(
    std::unique_ptr<FeatureRenderLayer> layer) {
    if (!layer || !layer->hasSealedOfficialProfile() ||
        !layer->style().usesOfficialProviderContract()) return false;
    layer->setRenderDevice(renderDevice_);
    return layers_->addFeatureRenderLayer(std::move(layer));
}

std::unique_ptr<FeatureRenderLayer> Scene::removeFeatureRenderLayer(
    const std::string& layerId) {
    return removeFeatureRenderLayerInternal(layerId, false);
}

std::unique_ptr<FeatureRenderLayer> Scene::removeOfficialFeatureRenderLayer(
    const std::string& layerId) {
    return removeFeatureRenderLayerInternal(layerId, true);
}

std::unique_ptr<FeatureRenderLayer> Scene::removeFeatureRenderLayerInternal(
    const std::string& layerId, bool allowOfficial) {
    // An MVT source and its sink-bound layer are one lifecycle unit.  Route
    // the legacy generic removal entry point through the source teardown
    // protocol instead of detaching the layer while Scene keeps updating the
    // source.  The source-aware API intentionally returns no detached layer.
    if (removeMvtVectorSource(layerId)) {
        return nullptr;
    }
    if (!allowOfficial) {
        for (const auto& layer : layers_->featureRenderLayers()) {
            if (layer && layer->id() == layerId &&
                layer->hasSealedOfficialProfile()) {
                platformLog(LogLevel::Error, "Scene",
                            "reject standalone removal of official layer '%s'",
                            layerId.c_str());
                return nullptr;
            }
        }
    }
    return layers_->removeFeatureRenderLayer(layerId);
}

bool Scene::addMvtVectorSource(
    std::unique_ptr<MvtVectorSource> source,
    std::unique_ptr<FeatureRenderLayer> layer) {
    if (!source || !layer || layer->id().empty()) return false;
    if (layer->style().usesOfficialProviderContract()) {
        platformLog(LogLevel::Error, "Scene",
                    "reject generic MVT source for official layer '%s'",
                    layer->id().c_str());
        return false;
    }
    const std::string layerId = layer->id();
    for (const auto& runtime : mvtSources_) {
        if (runtime.layerId == layerId) return false;
    }
    for (const auto& existing : layers_->featureRenderLayers()) {
        if (existing && existing->id() == layerId) return false;
    }
    layer->setRenderDevice(renderDevice_);
    layers_->addFeatureRenderLayer(std::move(layer));
    mvtSources_.push_back(MvtRuntime{std::move(source), layerId});
    return true;
}

bool Scene::removeMvtVectorSource(const std::string& layerId) {
    auto it = std::find_if(
        mvtSources_.begin(), mvtSources_.end(),
        [&](const MvtRuntime& runtime) { return runtime.layerId == layerId; });
    if (it == mvtSources_.end()) return false;
    if (it->source) it->source->suspend();
    layers_->removeFeatureRenderLayer(layerId);
    it->source.reset();
    mvtSources_.erase(it);
    return true;
}

size_t Scene::mvtVectorSourceCount() const { return mvtSources_.size(); }

const AmapClassicRuntime* Scene::installAmapClassicRuntime(
    std::unique_ptr<AmapClassicRuntime> runtime) {
    if (!runtime || amapClassicRuntime_) return nullptr;
    const bool hasGenericFeatureLayer = std::any_of(
        layers_->featureRenderLayers().begin(),
        layers_->featureRenderLayers().end(), [](const auto& layer) {
            return layer && !layer->hasSealedOfficialProfile();
        });
    if (layers_->vectorLayerCount() != 0 || !mvtSources_.empty() ||
        hasGenericFeatureLayer || tilesets_->hasAnyTileset() ||
        terrainPageStore_ != nullptr) {
        platformLog(LogLevel::Error, "Scene",
                    "reject AMap official runtime beside generic vector/raster contracts");
        return nullptr;
    }
    amapClassicRuntime_ = std::move(runtime);
    terrainFillMaskStore_ = amapClassicRuntime_->terrainFillMaskStore();
    if (renderer_) {
        renderer_->setTerrainFillMaskStore(terrainFillMaskStore_);
    }
    return amapClassicRuntime_.get();
}

void Scene::removeAmapClassicRuntime() {
    if (renderer_) {
        renderer_->setTerrainFillMaskStore(nullptr);
    }
    terrainFillMaskStore_ = nullptr;
    amapClassicRuntime_.reset();
}

void Scene::clearMvtVectorSources() {
    while (!mvtSources_.empty()) {
        removeMvtVectorSource(mvtSources_.back().layerId);
    }
}

bool Scene::setLabelFontData(std::vector<uint8_t> fontData) {
    if (!renderer_ || !renderer_->glyphAtlas()) return false;
    return renderer_->glyphAtlas()->setFontData(std::move(fontData));
}

bool Scene::addIconImage(const std::string& name,
                         int width,
                         int height,
                         const std::vector<uint8_t>& rgba) {
    if (!renderer_ || !renderer_->iconAtlas()) return false;
    return renderer_->iconAtlas()->addImage(name, width, height, rgba);
}

bool Scene::hasIconImage(const std::string& name) const {
    return renderer_ && renderer_->iconAtlas() &&
           renderer_->iconAtlas()->frame(name) != nullptr;
}

size_t Scene::vectorLayerCount() const {
    return layers_->vectorLayerCount();
}

// ---- 拾取与选择 ----

PickResult Scene::pick(float screenX, float screenY) const {
    return interaction_->pick(interactionContext(), screenX, screenY);
}

void Scene::onHover(const PickResult& result) {
    interaction_->onHover(result);
}

void Scene::onSelect(const PickResult& result) {
    interaction_->onSelect(result);
}

void Scene::clearSelection() {
    interaction_->clearSelection();
}

// ---- 输入回调 ----

void Scene::configureCameraSurfacePicker() {
    if (!cameraSystem_) return;

    interaction_->configureCameraSurfacePicker(
        *cameraSystem_,
        [this]() { return interactionContext(); });
}

SceneInteractionContext Scene::interactionContext() const {
    return frameRuntime_.makeInteractionContext(
        camera_.get(),
        cameraSystem_.get(),
        tilesets_->primary(),
        &layers_->vectorLayers(),
        &layers_->mutableFeatureRenderLayers(),
        &frameRuntime_.frameState());
}

void Scene::onInputEvent(const InputEvent& event) {
    interaction_->onInputEvent(interactionContext(), event);
}

// ---- 环境系统 ----

void Scene::setTime(double julianDate) {
    environment_->setTime(julianDate);
}

void Scene::setSunsetTerrainTint(float warmth, float shadowScale) {
    environment_->setSunsetTerrainTint(warmth, shadowScale);
}

double Scene::time() const {
    return environment_->time();
}

void Scene::advanceTime(double seconds) {
    environment_->advanceTime(seconds);
}

Vec3 Scene::sunDirection() const {
    return environment_->sunDirection();
}

const SkyGradient& Scene::skyGradient() const {
    return *environment_->skyGradient();
}

} // namespace earth_engine
