#include "SceneRenderPipeline.h"
#include "Camera.h"
#include "SceneFrameDiagnosticsAggregator.h"
#include "ScenePrimaryTilesetRenderComposer.h"
#include "SceneRenderCommandUniformUpdater.h"
#include "SceneRenderDiagnostics.h"
#include "../debug/Contracts.h"
#include "../debug/Policies.h"
#include "../debug/PerfTimer.h"
#include "../debug/PlatformLog.h"
#include "../environment/AtmosphereBackgroundPass.h"
#include "../environment/SkyBox.h"
#include "../environment/SkyGradient.h"
#include "../layers/FeatureRenderLayer.h"
#include "../layers/VectorLayer.h"
#include "../renderer/Renderer.h"
#include "../tiling/TileBoundsMetrics.h"
#include "../tiling/Tileset.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace earth_engine {
namespace {

bool isTerrainSurfaceCommand(const RenderCommand& command) {
    return command.kind == RenderCommandKind::GltfPrimitive &&
           command.terrainRenderContent;
}

int countTerrainSurfaceCommands(const RenderCommandList& commands) {
    return static_cast<int>(
        std::count_if(
            commands.begin(),
            commands.end(),
            isTerrainSurfaceCommand));
}

bool isTerrainSurfaceCommandWithBaseImagery(const RenderCommand& command) {
    return isTerrainSurfaceCommand(command) &&
           command.gltfRasterOverlayTextureCount > 0 &&
           command.surfaceBaseRasterState > 0;
}

int terrainRenderEntryCount(const Tileset* tileset) {
    if (!tileset) {
        return 0;
    }
    return static_cast<int>(tileset->tilePlan().renderEntries.size());
}

void applyTerrainRenderEntryDiagnostics(
    const Tileset* tileset,
    Diagnostics& diagnostics) {
    diagnostics.terrainRenderEntriesPlanned = terrainRenderEntryCount(tileset);
    diagnostics.terrainRenderEntriesSelectedPlanned = 0;
    diagnostics.terrainRenderEntriesAncestorFallback = 0;
    diagnostics.terrainRenderEntriesSynchronousPrep = 0;
    diagnostics.terrainRenderEntriesDeferredPrep = 0;
    diagnostics.terrainRenderEntriesDrawn = 0;
    diagnostics.terrainRenderEntriesSelectedDrawn = 0;
    diagnostics.terrainRenderEntriesMissed = 0;
    diagnostics.terrainRenderEntriesSelectedMissed = 0;
    diagnostics.terrainRenderEntriesDeferred = 0;
    diagnostics.terrainRenderEntriesSelectedDeferred = 0;
    diagnostics.terrainSelectedForRenderTiles = 0;
    diagnostics.terrainRenderEntryDropClipUv = 0;
    diagnostics.terrainRenderEntryDropNotBuildable = 0;
    diagnostics.terrainRenderEntryDropNoGeometry = 0;
    diagnostics.terrainRenderEntryDropNoMapping = 0;
    diagnostics.terrainRenderEntryDropNoReadyTexture = 0;
    diagnostics.terrainRenderEntryDropTexcoordInvalid = 0;
    diagnostics.terrainRenderEntryDropOther = 0;
    diagnostics.terrainRenderEntryDropMinZoom = 0;
    diagnostics.terrainRenderEntryDropMaxZoom = 0;
    diagnostics.terrainRenderEntryDropNoTexZoom = -1;
    diagnostics.terrainRenderEntryDropNoTexLoadingState = -1;
    diagnostics.terrainRenderEntryDropNoTexReadyState = -1;
    diagnostics.terrainRenderEntryDropNoTexReadyHasTexture = 0;
    diagnostics.terrainRenderEntryDropNoTexAncestorDepth = 0;
    diagnostics.terrainRenderEntryDropNoTexAncestorsWithMapping = 0;
    diagnostics.terrainRenderEntryDropNoTexAncestorsWithTexture = 0;
    diagnostics.terrainRenderEntryDropNoTexMappingState = -1;
    diagnostics.terrainRenderEntryDropNoTexAuthoritativeUpdates = 0;
    diagnostics.terrainRenderEntryDropNoTexTileLoadState = -99;
    diagnostics.terrainRenderEntryDropNoTexTileContentKind = -1;
    diagnostics.terrainRenderEntriesMissingSelected = 0;
    diagnostics.terrainRenderEntriesMissingRender = 0;
    diagnostics.terrainZeroDrawNoContentNoFill = 0;
    diagnostics.terrainZeroDrawFillNoCommands = 0;
    diagnostics.terrainZeroDrawContentNoCommands = 0;
    if (!tileset) {
        return;
    }

    const TilePlan& plan = tileset->tilePlan();
    diagnostics.terrainRenderEntriesSelectedPlanned =
        plan.renderEntrySelectedPlannedCommandCount;
    diagnostics.terrainRenderEntriesAncestorFallback =
        plan.renderEntryAncestorFallbackCount;
    diagnostics.terrainRenderEntriesSynchronousPrep =
        plan.renderEntrySynchronousPrepCount;
    diagnostics.terrainRenderEntriesDeferredPrep =
        plan.renderEntryDeferredPrepCount;
    diagnostics.terrainRenderEntriesDrawn =
        plan.renderEntryCommandDrawCount;
    diagnostics.terrainRenderEntriesSelectedDrawn =
        plan.renderEntrySelectedCommandDrawCount;
    diagnostics.terrainRenderEntriesMissed =
        plan.renderEntryCommandMissedDrawCount;
    diagnostics.terrainRenderEntriesSelectedMissed =
        plan.renderEntrySelectedCommandMissedDrawCount;
    diagnostics.terrainRenderEntriesDeferred =
        plan.renderEntryCommandDeferredCount;
    diagnostics.terrainRenderEntriesSelectedDeferred =
        plan.renderEntrySelectedCommandDeferredCount;
    diagnostics.terrainSelectedForRenderTiles =
        static_cast<int>(plan.tilesToRenderThisFrame.size());
    diagnostics.terrainRenderEntryDropClipUv =
        plan.renderEntryDropClipUvCount;
    diagnostics.terrainRenderEntryDropNotBuildable =
        plan.renderEntryDropNotBuildableCount;
    diagnostics.terrainRenderEntryDropNoGeometry =
        plan.renderEntryDropNoGeometryCount;
    diagnostics.terrainRenderEntryDropNoMapping =
        plan.renderEntryDropNoMappingCount;
    diagnostics.terrainRenderEntryDropNoReadyTexture =
        plan.renderEntryDropNoReadyTextureCount;
    diagnostics.terrainRenderEntryDropTexcoordInvalid =
        plan.renderEntryDropTexcoordInvalidCount;
    diagnostics.terrainRenderEntryDropOther =
        plan.renderEntryDropOtherCount;
    diagnostics.terrainRenderEntryDropMinZoom =
        plan.renderEntryDropMinZoom;
    diagnostics.terrainRenderEntryDropMaxZoom =
        plan.renderEntryDropMaxZoom;
    diagnostics.terrainRenderEntryDropNoTexZoom =
        plan.renderEntryDropNoTexZoom;
    diagnostics.terrainRenderEntryDropNoTexLoadingState =
        plan.renderEntryDropNoTexLoadingState;
    diagnostics.terrainRenderEntryDropNoTexReadyState =
        plan.renderEntryDropNoTexReadyState;
    diagnostics.terrainRenderEntryDropNoTexReadyHasTexture =
        plan.renderEntryDropNoTexReadyHasTexture;
    diagnostics.terrainRenderEntryDropNoTexAncestorDepth =
        plan.renderEntryDropNoTexAncestorDepth;
    diagnostics.terrainRenderEntryDropNoTexAncestorsWithMapping =
        plan.renderEntryDropNoTexAncestorsWithMapping;
    diagnostics.terrainRenderEntryDropNoTexAncestorsWithTexture =
        plan.renderEntryDropNoTexAncestorsWithTexture;
    diagnostics.terrainRenderEntryDropNoTexMappingState =
        plan.renderEntryDropNoTexMappingState;
    diagnostics.terrainRenderEntryDropNoTexAuthoritativeUpdates =
        plan.renderEntryDropNoTexAuthoritativeUpdates;
    diagnostics.terrainRenderEntryDropNoTexTileLoadState =
        plan.renderEntryDropNoTexTileLoadState;
    diagnostics.terrainRenderEntryDropNoTexTileContentKind =
        plan.renderEntryDropNoTexTileContentKind;
    diagnostics.terrainRenderEntriesMissingSelected =
        plan.renderEntryCommandMissingSelectedCount;
    diagnostics.terrainRenderEntriesMissingRender =
        plan.renderEntryCommandMissingRenderCount;
    diagnostics.terrainZeroDrawNoContentNoFill =
        plan.renderEntryZeroDrawNoContentNoFillCount;
    diagnostics.terrainZeroDrawFillNoCommands =
        plan.renderEntryZeroDrawFillNoCommandsCount;
    diagnostics.terrainZeroDrawContentNoCommands =
        plan.renderEntryZeroDrawContentNoCommandsCount;
}

} // namespace

SceneRenderPipeline::Result SceneRenderPipeline::render(Context context) {
    const double renderStartMs = perf::nowMs();
    context.commands.clear();
    reserveCommands(context);

    double skyMs = 0.0;
    double atmosphereMs = 0.0;
    double layerCommandsMs = 0.0;
    double vectorCommandsMs = 0.0;
    double batchMs = 0.0;
    double mvpUniformsMs = 0.0;
    double sortMs = 0.0;
    double surfaceDiagnosticsMs = 0.0;
    double validateMs = 0.0;
    double diagnosticsMs = 0.0;

    // T2:命令构建**之前**把地形深度纹理推给 Renderer —— 符号命令要在
    // buildLayerCommands 里绑它。纹理对象跨帧稳定,内容由随后的
    // runTerrainDepthPrepass 写入当帧深度,故符号采到的不是陈旧数据。
    prepareTerrainOcclusion(context);
    buildSkyCommands(context, skyMs);
    buildAtmosphereCommands(context, atmosphereMs);
    buildLayerCommands(context, layerCommandsMs, vectorCommandsMs);
    // Fill the web-mercator polar gap (±85°→pole) so it degrades to a flat
    // polar surface instead of showing through to space at globe scale.
    // Appended before the MVP pass so the caps receive per-frame uniforms.
    polarCap_.appendCommands(
        context.commands, context.renderer, context.renderDevice,
        context.frameState.frameId);
    // 地形实例化合批(Step3):MVP compose 之前把资格瓦片合成 instanced 命令
    // (批命令置 hasTerrainDisplacementFrame → updater 算 mvp=viewProj·frame0)。
    assembleTerrainBatches(context, batchMs);
    applyMvpUniforms(context, mvpUniformsMs);
    sortAndValidate(context, sortMs, surfaceDiagnosticsMs, validateMs);
    aggregateDiagnostics(context, diagnosticsMs);

    context.diagnostics.renderCommandBuildMs =
        perf::nowMs() - renderStartMs;

    const bool presentable = !shouldHoldPresentationAfterCommandBuild(context);

    if (context.beforeSubmit && presentable) {
        context.beforeSubmit();
    }

    if (presentable) {
        const double submitStartMs = perf::nowMs();
        runTerrainDepthPrepass(context);
        context.renderer.submit(context.commands);
        submitDoneFrameId_ = context.frameState.frameId;
        context.diagnostics.renderSubmitMs =
            perf::nowMs() - submitStartMs;
    } else {
        context.diagnostics.renderSubmitMs = 0.0;
    }

    releaseRenderReferences(context, presentable);

    char buildDetail[448];
    std::snprintf(buildDetail, sizeof(buildDetail),
        "sky=%.2f atmo=%.2f layers=%.2f vector=%.2f clampH=%.0f/%.0f(t%d/l%d) batch=%.2f(b%d/c%d) mvp=%.2f sort=%.2f surfDiag=%.2f validate=%.2f diag=%.2f commands=%zu",
        skyMs,
        atmosphereMs,
        layerCommandsMs,
        vectorCommandsMs,
        // 贴地体的高度范围(米)。**没有它就无法区分「贴地正常」与「范围恰好
        // 蒙对」** —— 相机飞到别的地形该跟着变;恒为 0/0 = 一次都没汇总到。
        lastClampMinHeight_,
        lastClampMaxHeight_,
        lastClampTightTiles_,
        lastClampLooseTiles_,
        batchMs,
        context.diagnostics.terrainBatches,
        context.diagnostics.batchedTerrainCommands,
        mvpUniformsMs,
        sortMs,
        surfaceDiagnosticsMs,
        validateMs,
        diagnosticsMs,
        context.commands.size());
    perf::logTiming(context.frameState.frameId,
                    "Scene.render.buildBreakdown",
                    context.diagnostics.renderCommandBuildMs,
                    buildDetail);

    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "build=%.2f submit=%.2f draw=%d surface=%d mesh=%d hold=%d",
        context.diagnostics.renderCommandBuildMs,
        context.diagnostics.renderSubmitMs,
        context.diagnostics.drawCalls,
        context.diagnostics.renderSurfaceTiles,
        context.diagnostics.surfaceMeshCount,
        presentable ? 0 : 1);
    perf::logTiming(context.frameState.frameId,
                    "Scene.render.total",
                    perf::nowMs() - renderStartMs,
                    detail);
    return Result{context.diagnostics, presentable};
}

void SceneRenderPipeline::reserveCommands(Context& context) const {
    size_t expectedCommands = 4 + context.vectorLayers.size() * 4;
    // FeatureRenderLayer:每 GPU 桶至多 fill+line 两命令
    for (const auto& fLayer : context.featureRenderLayers) {
        expectedCommands += fLayer->gpuBucketCount() * 2;
    }
    auto addExpectedTilesetCommands = [&](const Tileset* tileset) {
        if (!tileset) return;
        expectedCommands += tileset->tilePlan().renderEntries.size();
    };
    addExpectedTilesetCommands(context.terrainTileset);
    addExpectedTilesetCommands(context.pendingTerrainTileset);
    for (const auto& tileset : context.additionalTilesets) {
        addExpectedTilesetCommands(tileset.get());
    }
    if (context.commands.capacity() < expectedCommands) {
        context.commands.reserve(expectedCommands);
    }
}

void SceneRenderPipeline::buildSkyCommands(
    Context& context,
    double& skyMs) const {
    const double startMs = perf::nowMs();
    if (context.skyBox && context.skyBox->isReady() &&
        context.skyGradient && context.frameState.camera) {
        const auto& cam = *context.frameState.camera;
        Mat4 vm = cam.viewMatrix();
        const double* vmPtr = glm::value_ptr(vm.raw());
        float viewMatrix[16];
        for (int i = 0; i < 16; ++i) viewMatrix[i] = static_cast<float>(vmPtr[i]);

        const float vpW =
            static_cast<float>(context.frameState.viewportWidthPixels);
        const float vpH =
            static_cast<float>(context.frameState.viewportHeightPixels);
        Mat4 pm = cam.projectionMatrix(
            static_cast<double>(vpW), static_cast<double>(vpH));
        const double* pmPtr = glm::value_ptr(pm.raw());
        float projMatrix[16];
        for (int i = 0; i < 16; ++i) projMatrix[i] = static_cast<float>(pmPtr[i]);

        float nightFactor = static_cast<float>(
            context.skyGradient->sunElevation() < -0.05
                ? std::clamp(
                      std::exp(context.skyGradient->sunElevation() * 8.0),
                      0.0,
                      1.0)
                : 0.0);
        double spaceFactor =
            std::clamp((cam.getHeight() - 120000.0) / 780000.0, 0.0, 1.0);
        spaceFactor = spaceFactor * spaceFactor * (3.0 - 2.0 * spaceFactor);
        nightFactor = std::max(nightFactor, static_cast<float>(spaceFactor));

        context.commands.push_back(context.skyBox->buildCommand(
            viewMatrix, projMatrix, cam.isOrthographic(), nightFactor));
    }
    skyMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::buildAtmosphereCommands(
    Context& context,
    double& atmosphereMs) const {
    const double startMs = perf::nowMs();
    if (context.atmospherePass && context.atmospherePass->isReady() &&
        context.skyGradient && context.frameState.camera) {
        const auto& cam = *context.frameState.camera;
        const float vpW =
            static_cast<float>(context.frameState.viewportWidthPixels);
        const float vpH =
            static_cast<float>(context.frameState.viewportHeightPixels);
        Vec3 sunDir(context.frameState.lightDir.x,
                    context.frameState.lightDir.y,
                    context.frameState.lightDir.z);

        context.commands.push_back(context.atmospherePass->buildCommand(
            cam.position(),
            static_cast<float>(cam.verticalFovRadians()),
            static_cast<int>(vpW),
            static_cast<int>(vpH),
            cam.right(),
            cam.up(),
            cam.direction(),
            sunDir,
            context.skyGradient->parameters()));
    }
    atmosphereMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::buildLayerCommands(
    Context& context,
    double& layerCommandsMs,
    double& vectorCommandsMs) const {
    // Tileset 命令来自各 tile 的常驻命令缓存(见 GltfDrawCommandBuilder),
    // 直接追加进帧列表:每命令每帧一次拷贝,无中转缓存。
    const double layerStartMs = perf::nowMs();
    if (context.terrainTileset) {
        if (context.pendingTerrainTileset) {
            const ScenePrimaryTilesetRenderComposition composition =
                ScenePrimaryTilesetRenderComposer::compose(
                    *context.terrainTileset,
                    *context.pendingTerrainTileset);
            const int currentEntryCount =
                static_cast<int>(composition.currentEntries.size());
            const int pendingEntryCount =
                static_cast<int>(composition.pendingEntries.size());
            if (currentEntryCount != lastPrimaryCurrentEntryCount_ ||
                pendingEntryCount != lastPrimaryPendingEntryCount_) {
                platformLog(
                    LogLevel::Info,
                    "Tileset",
                    "Primary regional composition current=%d pending=%d",
                    currentEntryCount,
                    pendingEntryCount);
                lastPrimaryCurrentEntryCount_ = currentEntryCount;
                lastPrimaryPendingEntryCount_ = pendingEntryCount;
            }
            context.terrainTileset->buildRenderCommands(
                context.renderer,
                context.commands,
                context.frameState.frameId,
                &composition.currentEntries);
            if (composition.hasPendingCoverage()) {
                context.pendingTerrainTileset->buildRenderCommands(
                    context.renderer,
                    context.commands,
                    context.frameState.frameId,
                    &composition.pendingEntries);
            }
        } else {
            lastPrimaryCurrentEntryCount_ = -1;
            lastPrimaryPendingEntryCount_ = -1;
            context.terrainTileset->buildRenderCommands(
                context.renderer,
                context.commands,
                context.frameState.frameId);
        }
    } else if (context.pendingTerrainTileset) {
        context.pendingTerrainTileset->buildRenderCommands(
            context.renderer,
            context.commands,
            context.frameState.frameId);
    }
    for (auto& tileset : context.additionalTilesets) {
        if (tileset) {
            tileset->buildRenderCommands(
                context.renderer,
                context.commands,
                context.frameState.frameId);
        }
    }
    layerCommandsMs = perf::nowMs() - layerStartMs;

    const double vectorStartMs = perf::nowMs();
    for (auto& vLayer : context.vectorLayers) {
        if (vLayer->visible()) {
            vLayer->buildRenderCommands(
                context.frameState, context.renderer, context.commands);
        }
    }
    // 贴地(P3):把主地形的区域采样器/代次注入矢量层。每帧刷新以跟随
    // pending 地形接管等主 tileset 指针变化;闭包只在渲染线程当帧使用。
    //
    // E4 裁决(2026-08-07):primary 存活期矢量贴地与相机同用 primary,高度
    // 基准一致;pending 回落只覆盖 primary==null 的空窗,而 coordinator 对
    // 该情形立即提交接管(空窗 ≤1 帧),相机侧(SceneInteractionCoordinator
    // 只看 terrainTileset)无需对称覆盖。⚠️未来接通 stageTilesetReplacement
    // (当前全仓零生产调用者)若把接管改成非立即,须同步重估两侧基准一致性。
    Tileset* terrainForClamp = context.terrainTileset
                                   ? context.terrainTileset
                                   : context.pendingTerrainTileset;
    // worker 侧贴地(stencil 挤出体)要的**区域高度范围**:从本帧可见地形
    // 瓦片的包围体汇总。O(可见瓦片数),不做任何地形采样 —— 这是 worker
    // 能贴地的前提(它拿不到采样器,但拿得到一对标量)。
    //
    // 用可见瓦片而非固定值:平原上范围窄 → 体矮 → fill rate 低;山地自动
    // 变高。
    //
    // ⚠️ **loose 瓦片必须单独成桶,不能并进主汇总**。looseFittingHeights 的
    // 「范围」根本不是测出来的高度,是 -1000/9000 这对占位常量(见
    // TileBoundsMetrics::kDefaultTerrainMin/MaximumHeight)。并进来的后果不是
    // 「偏宽一点」,是**一个未细化的瓦片把全屏的体高钉死在 10km**:每条路都成
    // 了 10 公里高的幕布。真机实测(GpuPass 逐区间计时,2026-08-09):宽视野下
    // vec:mvt-basemap 135.16ms/帧,把体半高硬钳到 150m 后 9.31ms —— 14.5×,
    // 且当帧日志里 clampH 恰是 -1000/9000。正确性方向确实安全,代价此前没人量过。
    //
    // 分桶策略:优先用**测得真高度**的瓦片汇总;一个都没有(启动/纯 loose 期)
    // 才退回 loose 桶 —— 那种情形下宁可要 10km 的体也不能让路网整片消失
    // (体矮了穿不透地形 = 没有,不是变淡)。
    double clampMinHeight = std::numeric_limits<double>::max();
    double clampMaxHeight = std::numeric_limits<double>::lowest();
    double looseMinHeight = std::numeric_limits<double>::max();
    double looseMaxHeight = std::numeric_limits<double>::lowest();
    // 机制信号:tight/loose 各多少瓦片参与汇总。只看 clampH 那对数分不清
    // 「本帧真没有 loose 瓦片」和「判据没认出 loose」—— 两者读数一模一样。
    int clampTightTiles = 0;
    int clampLooseTiles = 0;
    // 逐瓦片快照:worker 侧按自己那块的矩形取局部范围,而不是全屏一个 union
    // (union 让平原上的路背着山地的相对高差,而体高直接换算成 fill)。
    // 只放 tight 瓦片 —— loose 的那对是占位常量,进来就等于没收窄。
    auto clampCells =
        std::make_shared<FeatureRenderLayer::TerrainHeightRangeCells>();
    if (terrainForClamp) {
        for (TilesetTile* tile :
             terrainForClamp->tilePlan().tilesToRenderThisFrame) {
            if (tile == nullptr || !tile->boundingVolume.has_value()) continue;
            const TileBoundingVolume& bv = *tile->boundingVolume;
            // 只有 Region 的 min/maxHeight 有意义(球/盒的那两个字段是缺省值)。
            if (bv.kind != TileBoundingVolumeKind::Region) continue;
            // 两条判据缺一不可:占位范围有时带 looseFittingHeights 标志,有时
            // 只是数值恰为那对常量(与 TileLoadResultMetadataApplicator 的
            // hasLooseFittingHeights 同款双判)。只判标志会漏掉后者,而漏一个
            // 就等于没修 —— 全屏体高由最宽的那一个决定。
            const bool looseHeights =
                bv.looseFittingHeights ||
                (std::abs(bv.minimumHeight -
                          TileBoundsMetrics::kDefaultTerrainMinimumHeight) <=
                     MathUtils::Epsilon5 &&
                 std::abs(bv.maximumHeight -
                          TileBoundsMetrics::kDefaultTerrainMaximumHeight) <=
                     MathUtils::Epsilon5);
            if (looseHeights) {
                ++clampLooseTiles;
                looseMinHeight = std::min(looseMinHeight, bv.minimumHeight);
                looseMaxHeight = std::max(looseMaxHeight, bv.maximumHeight);
                continue;
            }
            ++clampTightTiles;
            clampMinHeight = std::min(clampMinHeight, bv.minimumHeight);
            clampMaxHeight = std::max(clampMaxHeight, bv.maximumHeight);
            clampCells->push_back(FeatureRenderLayer::TerrainHeightRangeCell{
                bv.region, bv.minimumHeight, bv.maximumHeight});
        }
        if (clampMaxHeight < clampMinHeight && looseMaxHeight >= looseMinHeight) {
            clampMinHeight = looseMinHeight;
            clampMaxHeight = looseMaxHeight;
        }
    }
    lastClampTightTiles_ = clampTightTiles;
    lastClampLooseTiles_ = clampLooseTiles;
    lastClampRangeApplied_ = clampMaxHeight >= clampMinHeight;
    lastClampMinHeight_ = lastClampRangeApplied_ ? clampMinHeight : 0.0;
    lastClampMaxHeight_ = lastClampRangeApplied_ ? clampMaxHeight : 0.0;
    for (auto& fLayer : context.featureRenderLayers) {
        FeatureTerrainSampling sampling;
        if (terrainForClamp) {
            // 统一采样服务:cell 索引让逐点查询 O(档数),区域预筛不再必要
            // (area 参数保留在闭包签名里,矢量层接口不动)。矢量贴地 →
            // 渲染网格一致采样,与上屏地形面同一分段线性面。
            sampling.makeAreaSampler =
                [terrainForClamp](const Rectangle& area)
                -> std::function<std::optional<float>(double, double)> {
                (void)area;
                const TerrainHeightService* heights =
                    &terrainForClamp->heightService();
                return [heights](double lng,
                                 double lat) -> std::optional<float> {
                    const auto sample = heights->sample(
                        lng, lat,
                        TerrainHeightService::Interp::RenderGridConsistent);
                    if (!sample) {
                        return std::nullopt;
                    }
                    return sample->height;
                };
            };
            // 强代次替代 contentBytesUsed 弱代理(字节数恰好不变会漏失效)。
            sampling.revision = []() {
                return TerrainHeightService::heightmapGeneration();
            };
        }
        fLayer->setTerrainSampling(std::move(sampling));
        // 汇总不到(无地形 / 本帧无可见瓦片 / 非 Region 包围体)时传
        // min > max = 「未知」,图层据此退回不贴地,而不是拿一个瞎猜的范围
        // 去建体 —— 后者会把线画在错误高度上,比不贴地更难查。
        fLayer->setWorkerTerrainHeightRange(clampMinHeight, clampMaxHeight);
        // 逐瓦片快照与全局标量一起发布:worker 取不到相交瓦片时退回标量,
        // 两者必须描述同一帧,否则局部范围会比"全局"还宽。
        fLayer->setWorkerTerrainHeightRangeCells(clampCells);
        if (fLayer->visible()) {
            fLayer->buildRenderCommands(
                context.frameState, context.renderer, context.commands);
        }
    }
    vectorCommandsMs = perf::nowMs() - vectorStartMs;
}

void SceneRenderPipeline::assembleTerrainBatches(
    Context& context,
    double& batchMs) const {
    const double startMs = perf::nowMs();
    const TerrainInstanceBatcher::Stats stats = terrainBatcher_.assemble(
        context.commands, context.renderDevice, context.renderer);
    context.diagnostics.batchedTerrainCommands = stats.batchedCommands;
    context.diagnostics.terrainBatches = stats.batches;
    batchMs = perf::nowMs() - startMs;

    // 策略生效率:有资格的命令按定义就是"能合的",看多少真的进了批。
    // 分母为 0(本帧没有可合批的命令)不计 —— 没机会生效不等于没生效。
    policy::observe(policy::Id::BatchFormation,
                    stats.batchedCommands, stats.eligibleCommands);

    // 判因行:batches 长期为 0 时,这一行直接说出卡在三步链路的哪一步 ——
    // shaderReady=0 / eligible=0(看 rejects 谁最大)/ groups>0 但全是单例。
    // 此前 batchedTerrainCommands 与 terrainBatches 是**只写字段**(记了从不
    // 输出),于是"合批到底有没有在工作"完全不可观测。节流 120 帧。
    if (++batchDetFrameCounter_ % 120u == 1u) {
        char rejects[256];
        int offset = 0;
        for (size_t i = 0;
             i < static_cast<size_t>(
                     TerrainInstanceBatcher::RejectReason::Count);
             ++i) {
            if (stats.rejects[i] == 0) continue;  // 只报非零项,免得刷屏
            const int written = std::snprintf(
                rejects + offset, sizeof(rejects) - static_cast<size_t>(offset),
                " %s=%d",
                TerrainInstanceBatcher::rejectReasonName(
                    static_cast<TerrainInstanceBatcher::RejectReason>(i)),
                stats.rejects[i]);
            if (written <= 0 ||
                static_cast<size_t>(offset + written) >= sizeof(rejects)) {
                break;
            }
            offset += written;
        }
        rejects[offset] = '\0';
        platformLog(LogLevel::Info, "BatchDet",
                    "cmds=%d shaderReady=%d eligible=%d groups=%d "
                    "singleton=%d oversize=%d batches=%d batched=%d |%s",
                    stats.totalCommands, stats.shaderReady ? 1 : 0,
                    stats.eligibleCommands, stats.groups,
                    stats.singletonGroups, stats.oversizeGroups,
                    stats.batches, stats.batchedCommands,
                    offset > 0 ? rejects : " (无拒绝项)");
    }
}

void SceneRenderPipeline::applyMvpUniforms(
    Context& context,
    double& mvpUniformsMs) const {
    const double startMs = perf::nowMs();
    SceneRenderCommandUniformUpdater::apply(context.frameState,
                                            context.commands);
    mvpUniformsMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::sortAndValidate(
    Context& context,
    double& sortMs,
    double& surfaceDiagnosticsMs,
    double& validateMs) const {
    const double sortStartMs = perf::nowMs();
    bool needsSort = false;
    bool hasTranslucentGltf = false;
    for (size_t i = 1; i < context.commands.size(); ++i) {
        if (context.commands[i - 1].blend &&
            (context.commands[i - 1].kind == RenderCommandKind::GltfPrimitive ||
             context.commands[i - 1].kind ==
                 RenderCommandKind::GltfPrimitiveInstanced)) {
            hasTranslucentGltf = true;
        }
        if (mvpRenderOrder(context.commands[i - 1].kind) >
            mvpRenderOrder(context.commands[i].kind)) {
            needsSort = true;
            break;
        }
    }
    if (!context.commands.empty()) {
        const RenderCommand& last = context.commands.back();
        if (last.blend &&
            (last.kind == RenderCommandKind::GltfPrimitive ||
             last.kind == RenderCommandKind::GltfPrimitiveInstanced)) {
            hasTranslucentGltf = true;
        }
    }
    if (needsSort || hasTranslucentGltf) {
        sortMvpRenderCommands(context.commands);
    }
    sortMs = perf::nowMs() - sortStartMs;

    const double surfaceStartMs = perf::nowMs();
    SceneRenderDiagnostics::updateSurfaceCommandGeneration(
        context.commands,
        context.frameState.frameId,
        context.diagnostics);
    surfaceDiagnosticsMs = perf::nowMs() - surfaceStartMs;

    const double validateStartMs = perf::nowMs();
    if (auto error = validateMvpRenderCommands(
            context.commands, context.frameState.frameId)) {
        throw std::runtime_error(
            "MVP render command validation failed for '" + error->owner +
            "': " + error->message);
    }
    validateMs = perf::nowMs() - validateStartMs;
}

void SceneRenderPipeline::prepareTerrainOcclusion(Context& context) const {
    Renderer::TerrainOcclusionParams params;
    if (context.renderDevice) {
        if (!terrainDepthPrepassInitAttempted_) {
            terrainDepthPrepassInitAttempted_ = true;
            terrainDepthPrepass_.initialize(context.renderDevice,
                                            &context.renderer);
        }
        if (terrainDepthPrepass_.ready() &&
            terrainDepthPrepass_.ensureFramebuffer(
                context.surfaceWidthPixels, context.surfaceHeightPixels)) {
            params.depthTexture = terrainDepthPrepass_.depthTexture();
        }
    }
    if (const Camera* cam = context.frameState.camera) {
        params.nearPlaneMeters = static_cast<float>(cam->nearPlaneMeters());
        params.farPlaneMeters = static_cast<float>(cam->farPlaneMeters());
    }
    context.renderer.setTerrainOcclusion(params);
}

void SceneRenderPipeline::runTerrainDepthPrepass(Context& context) const {
    if (!context.renderDevice) {
        return;
    }
    if (!terrainDepthPrepassInitAttempted_) {
        terrainDepthPrepassInitAttempted_ = true;
        terrainDepthPrepass_.initialize(context.renderDevice, &context.renderer);
    }
    if (!terrainDepthPrepass_.ready()) {
        return;
    }
    RenderCommandList depthCommands =
        terrainDepthPrepass_.extractTerrainCommands(context.commands);
    if (depthCommands.empty()) {
        // 本帧没有真实地形(纯椭球/加载期 fill 代理)→ 不跑 prepass。此时
        // 深度纹理保持上一帧内容,但符号侧靠 u_terrainDepthEnabled 关掉判定,
        // 不会读到陈旧深度(见 FeatureRenderLayer 绑定处)。
        return;
    }
    Framebuffer* depthTarget = terrainDepthPrepass_.ensureFramebuffer(
        context.surfaceWidthPixels, context.surfaceHeightPixels);
    if (!depthTarget) {
        return;
    }
    // prepass → 再把场景 pass begin 回来。此刻场景 FBO 还没画过任何东西,
    // beginPass 的二次 clear 无害(GLES beginPass = 绑 FBO + 复位状态 + clear)。
    if (!context.renderDevice->beginPass(depthTarget)) {
        context.renderDevice->beginPass(context.sceneTarget);
        return;
    }
    // subdividable=false:prepass 里全是地形命令,若按桶再切分,它会和主 pass 的
    // 地形命令合进同一个 "terrain" 名字 —— 半分辨率 depth-only 与全分辨率带片元
    // 着色是两笔性质完全不同的成本,合成一个数就问不出"prepass 到底值不值"。
    context.renderDevice->beginGpuRegion("pass.terrainDepthPrepass", false);
    context.renderer.submit(depthCommands);
    context.renderDevice->endGpuRegion();
    context.renderDevice->beginPass(context.sceneTarget);

    // 机制信号:通路是否真的在跑。观感 A/B 出现"零变化"时,必须能区分
    // 「场景里没有被遮挡的符号」与「prepass 静默失效」—— 没有这行就只能靠猜。
    static int sPrepassLogCount = 0;
    if ((sPrepassLogCount++ % 120) == 0) {
        platformLog(LogLevel::Info, "EarthPerf",
                    "TerrainDepthPrepass cmds=%zu fbo=%dx%d depthTex=%d",
                    depthCommands.size(), depthTarget->width(),
                    depthTarget->height(),
                    terrainDepthPrepass_.depthTexture() != nullptr ? 1 : 0);
    }
}

void SceneRenderPipeline::aggregateDiagnostics(
    Context& context,
    double& diagnosticsMs) const {
    const double startMs = perf::nowMs();
    // 诊断聚合含全 registry 瓦片状态遍历(TileLoadDiagnosticsCollector::collect
    // 逐瓦片数 load 状态),掠视宽视野(百级可见瓦片/千级 registry)下 ~3-5ms/帧,
    // 且字段纯诊断消费(调试面板/EarthPerf 日志,无功能逻辑读取)。节流到每 15
    // 帧聚合一次,其余帧保留上次值(最多 15 帧陈旧;EarthPerf 120 帧心跳是 15
    // 的倍数,心跳行永远拿到当帧新值)。首帧总是聚合,单帧消费者(启动即读的
    // 调试面板/单帧渲染的测试)不吃 15 帧空窗。
    constexpr uint64_t kAggregateIntervalFrames = 15;
    if (context.frameState.frameId > 1u &&
        context.frameState.frameId % kAggregateIntervalFrames != 0u) {
        diagnosticsMs = perf::nowMs() - startMs;
        return;
    }
    SceneFrameDiagnosticsAggregator::aggregateRenderFrame(
        context.commands,
        context.terrainTileset,
        context.additionalTilesets,
        context.diagnostics);
    applyTerrainRenderEntryDiagnostics(
        context.terrainTileset,
        context.diagnostics);
    context.diagnostics.terrainSurfaceCommandsSubmitted =
        countTerrainSurfaceCommands(context.commands);
    diagnosticsMs = perf::nowMs() - startMs;
}

bool SceneRenderPipeline::shouldHoldPresentationAfterCommandBuild(
    const Context& context) const {
    const Tileset* terrainTileset = context.terrainTileset;
    if (!terrainTileset ||
        !terrainTileset->requiresBaseImageryPresentationSurface()) {
        return false;
    }
    bool hasTerrainSurfaceCommand = false;
    int terrainCommands = 0;
    int noTexCount = 0;
    int noBaseStateCount = 0;
    const RenderCommand* firstOffender = nullptr;
    for (const RenderCommand& command : context.commands) {
        if (!isTerrainSurfaceCommand(command)) {
            continue;
        }
        hasTerrainSurfaceCommand = true;
        ++terrainCommands;
        if (!isTerrainSurfaceCommandWithBaseImagery(command)) {
            if (command.gltfRasterOverlayTextureCount <= 0) {
                ++noTexCount;
            }
            if (command.surfaceBaseRasterState <= 0) {
                ++noBaseStateCount;
            }
            if (!firstOffender) {
                firstOffender = &command;
            }
        }
    }
    const bool hold = !hasTerrainSurfaceCommand || firstOffender != nullptr;
    // hold 会把整帧扣住不 present,而 present 不发生时 frameId 不前进 → 所有
    // %120 的心跳诊断全哑火,现场只剩一块不动的屏幕。所以这里独立限频打印,
    // 不依赖 frameId。
    if (hold) {
        static int sHoldLogCount = 0;
        if ((sHoldLogCount++ % 60) == 0) {
            platformLog(
                LogLevel::Info,
                "EarthPerf",
                "HoldDiag cmds=%zu terrainCmds=%d noTex=%d noBaseState=%d "
                "firstOffender[tex=%d baseState=%d mapped=%d ancDelta=%d "
                "idx=%d skirt=%d]",
                context.commands.size(),
                terrainCommands,
                noTexCount,
                noBaseStateCount,
                firstOffender ? firstOffender->gltfRasterOverlayTextureCount : -1,
                firstOffender ? firstOffender->surfaceBaseRasterState : -1,
                firstOffender ? firstOffender->surfaceBaseIsMappedRasterTile : -1,
                firstOffender ? firstOffender->imageryAncestorLevelDelta : -1,
                firstOffender ? firstOffender->surfaceMeshIndexCount : -1,
                firstOffender ? firstOffender->surfaceSkirtIndexCount : -1);
        }
    }
    return hold;
}

void SceneRenderPipeline::releaseRenderReferences(Context& context,
                                                 bool presentable) const {
    // 契约(消费侧):本帧若提交过命令,释放必须发生在那次 submit **之后**。
    //
    // 渲染命令持有裸 Buffer*/Texture* 加 resourceKeepAlive shared_ptr;先释放会
    // 在 draw 中途放掉 GPU 资源。AI_INDEX §20 把这条列为跨子系统契约,并在小标题
    // 里写明 "enforced by call order, not by types" —— 在此之前它只是一句文档。
    //
    // 未提交帧(presentable=false,hold/跳帧)不参与判定:那种帧没有需要保活的
    // 提交,释放本来就是安全的。硬要求"每帧都必须先 submit"会在 hold 帧误报,
    // 而误报会训练出忽略这条警告的习惯。
    //
    // ⚠️ 弱点如实记:谓词是「内部状态 vs 内部状态」,不满足数据独立那条准入标准
    // (模块内时序不变量)。它挡的是**未来的重排**,不是当下的错误;
    // 活性由 coverage 计数证明,判别力没有反例控制组。
    const bool submittedThisFrame =
        submitDoneFrameId_ == context.frameState.frameId;
    GE_CONTRACT(contracts::Id::SubmitBeforeReleaseRefs,
                !presentable || submittedThisFrame,
                "frame=%llu submitDoneFrame=%llu commands=%zu",
                (unsigned long long)context.frameState.frameId,
                (unsigned long long)submitDoneFrameId_,
                context.commands.size());
    if (context.terrainTileset) {
        context.terrainTileset->releaseRenderReferences();
    }
    if (context.pendingTerrainTileset) {
        context.pendingTerrainTileset->releaseRenderReferences();
    }
    for (auto& tileset : context.additionalTilesets) {
        if (tileset) {
            tileset->releaseRenderReferences();
        }
    }
}

} // namespace earth_engine
