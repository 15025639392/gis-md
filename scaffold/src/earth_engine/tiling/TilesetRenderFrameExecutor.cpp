#include "TilesetRenderFrameExecutor.h"

#include "TerrainDisplacementTemplatePool.h"
#include "TileRenderCommandManager.h"
#include "../debug/PerfTimer.h"
#include "../renderer/Renderer.h"

#include <array>
#include <cstdio>

namespace earth_engine {

void TilesetRenderFrameExecutor::buildRenderCommands(
    TileRenderFrameContext context,
    Renderer& renderer,
    RenderCommandList& commands) {
    TileRenderFrameCoordinator::run(
        context.input,
        renderer,
        commands,
        [&context](const std::string& cacheKey) {
            context.markIneligibleForUnloading(cacheKey);
        },
        [&context](TilesetTile* tile,
                   std::string cacheKey,
                   bool countedReference) {
            context.trackRenderReference(
                tile,
                std::move(cacheKey),
                countedReference);
        },
        [&context](Renderer& renderer,
                   TilesetTile& tile,
                   RenderCommandList& commands,
                   float transitionOpacity,
                   bool allowSynchronousMeshPrep,
                   const std::optional<std::array<float, 4>>& surfaceClipUv,
                   const TilesetTile* surfaceClipDescendant) {
            context.buildTileDrawCommand(
                renderer,
                tile,
                commands,
                transitionOpacity,
                allowSynchronousMeshPrep,
                surfaceClipUv,
                surfaceClipDescendant);
        },
        [&context](bool cachePressure) {
            context.trimRasterCaches(cachePressure);
        },
        [&context]() {
            return context.shouldUnloadCachedBytes();
        },
        [&context, &renderer]() {
            context.unloadCachedBytes(renderer);
        });

    const TileRenderCommandPerformanceTimings& timings =
        context.renderCommandTimings();
    // 破洞诊断:零命令成因分桶从 draw 侧回填到 tilePlan,与既有 renderEntry*
    // 计数一起被 SceneRenderPipeline 抄进 Diagnostics。
    context.input.tilePlan.renderEntryZeroDrawNoContentNoFillCount =
        timings.zeroDraw.noContentNoFill;
    context.input.tilePlan.renderEntryZeroDrawFillNoCommandsCount =
        timings.zeroDraw.fillNoCommands;
    context.input.tilePlan.renderEntryZeroDrawContentNoCommandsCount =
        timings.zeroDraw.contentNoCommands;
    const GltfDrawCommandBuildTimings& draw = timings.drawCommand;
    const int firstBuildDeferred =
        context.input.tilePlan.renderEntryFirstBuildDeferredCount;
    int heightEpochMiss = 0;
    int heightGridMiss = 0;
    int heightEvicted = 0;
    if (TerrainDisplacementTemplatePool* pool =
            renderer.terrainDisplacementPool()) {
        const auto& hs = pool->heightFrameStats();
        heightEpochMiss = hs.epochMiss;
        heightGridMiss = hs.gridMiss;
        heightEvicted = hs.evicted;
    }
    std::array<char, 512> detail{};
    std::snprintf(
        detail.data(),
        detail.size(),
        "tiles=%d mesh=%.2f resources=%.2f draw=%.2f eligible=%.2f rebuild=%.2f rebuilds=%d fbDef=%d epoch=%d grid=%d evict=%d copy=%.2f frameState=%.2f rasterBind=%.2f cmds=%d",
        timings.tileCount,
        timings.ensureMeshMs,
        timings.resourcePrepareMs,
        timings.drawBuildMs,
        draw.eligibilityMs,
        draw.cacheRebuildMs,
        draw.cacheRebuildCount,
        firstBuildDeferred,
        heightEpochMiss,
        heightGridMiss,
        heightEvicted,
        draw.commandCopyMs,
        draw.perFrameStateMs,
        draw.rasterBindingMs,
        draw.commandCount);
    perf::logTimingAtLeast(
        context.input.frameNumber,
        "Tileset.buildTileDrawCommands",
        timings.totalMs,
        2.0,
        detail.data());
}

} // namespace earth_engine
