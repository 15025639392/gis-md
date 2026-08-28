#pragma once

#include "TileFrameDebugLogFormatter.h"
#include "TilePlan.h"
#include "TileRenderEntryCommandBuilder.h"
#include "TileRenderFrameMaintenance.h"
#include "TerrainDisplacementTemplatePool.h"
#include "TileSelectionMetrics.h"
#include "TilesetTile.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Vec3.h"
#include "../debug/PerfTimer.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../renderer/RenderCommand.h"
#include "../renderer/Renderer.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class Renderer;
struct FogDensityAtHeight;

struct TileRenderFrameBuildInput {
    TilePlan& tilePlan;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    uint64_t frameNumber = 0;
    Vec3 lastCameraPosition;
    const std::vector<FogDensityAtHeight>& fogDensityTable;
    int fogCulled = 0;
    bool resourceSmoothingActive = false;
    bool interactionActive = false;
    const std::vector<TileRenderEntry>* renderEntriesOverride = nullptr;
};

class TileRenderFrameBuilder {
public:
    template <typename TerrainCacheKeyFn,
              typename MarkIneligibleFn,
              typename TrackReferenceFn,
              typename BuildTileDrawCommandFn,
              typename TrimRasterCachesFn,
              typename ShouldUnloadCachedBytesFn,
              typename UnloadCachedBytesFn>
    static void build(
        TileRenderFrameBuildInput input,
        Renderer& renderer,
        RenderCommandList& commands,
        TerrainCacheKeyFn&& terrainCacheKey,
        MarkIneligibleFn&& markIneligible,
        TrackReferenceFn&& trackReference,
        BuildTileDrawCommandFn&& buildTileDrawCommand,
        TrimRasterCachesFn&& trimRasterCaches,
        ShouldUnloadCachedBytesFn&& shouldUnloadCachedBytes,
        UnloadCachedBytesFn&& unloadCachedBytes) {
        const double buildCommandsStartMs = perf::nowMs();
        const size_t commandsBeforeTileset = commands.size();
        input.tilePlan.renderEntryPlannedCommandCount = 0;
        input.tilePlan.renderEntrySelectedPlannedCommandCount = 0;
        input.tilePlan.renderEntryCommandDrawCount = 0;
        input.tilePlan.renderEntrySelectedCommandDrawCount = 0;
        input.tilePlan.renderEntryCommandMissedDrawCount = 0;
        input.tilePlan.renderEntrySelectedCommandMissedDrawCount = 0;
        input.tilePlan.renderEntryCommandMissingSelectedCount = 0;
        input.tilePlan.renderEntryCommandMissingRenderCount = 0;
        input.tilePlan.renderEntryCommandDeferredCount = 0;
        input.tilePlan.renderEntrySelectedCommandDeferredCount = 0;

        // Raster providers stamp getTile() calls with the current frame.
        for (auto* overlay : input.rasterOverlays) {
            if (overlay) {
                overlay->setFrameNumber(input.frameNumber);
            }
        }

        const double cameraHeight = Ellipsoid::WGS84().cartesianToCartographic(
            input.lastCameraPosition).height();
        [[maybe_unused]] const double fogDensity =
            TileSelectionMetrics::computeFogDensity(
                input.fogDensityTable,
                cameraHeight);

        std::unordered_set<TilesetTile*> activeTiles;
        activeTiles.reserve(input.tilePlan.visibleTiles.size());
        auto protectTile = [&](TilesetTile* tile, uint64_t frameNumber) {
            if (!tile) {
                return;
            }
            tile->markUsedForRenderFrame(frameNumber);
            if (!activeTiles.insert(tile).second) {
                return;
            }
            tile->addReference();
            std::string cacheKey = terrainCacheKey(tile->key);
            markIneligible(cacheKey);
            trackReference(tile, std::move(cacheKey), true);
        };

        // Cesium Native keeps ViewUpdateResult::tilesToRenderThisFrame as
        // intrusive Tile pointers. Mirror that ownership before cache
        // maintenance, including selected tiles whose raster is not drawable
        // yet and therefore have no render entry.
        for (TilesetTile* tile : input.tilePlan.tilesToRenderThisFrame) {
            protectTile(tile, input.frameNumber);
        }

        // H-S2:准入顺序保护。layerPool 只把「本帧已 touch」的层视为不可淘汰,
        // 而可见瓦的 touch 发生在它自己的 build 里;扫掠期新瓦 acquire 若先于
        // 某可见瓦 build,可能淘汰该瓦的驻留层 → 当帧 epoch 失配 → invalidate →
        // rebuild → 再淘汰,连锁成每帧多次 ~2ms 的重建 burst。这里在 build 前把
        // 本帧所有要画瓦片的驻留层统一 touch 到当帧,新准入的 LRU 淘汰只能落在
        // 「本帧不画」的层上——不设预算、不拒绝准入、不丢命令,保不露底三不变量。
        if (TerrainDisplacementTemplatePool* pool =
                renderer.terrainDisplacementPool()) {
            for (TilesetTile* tile :
                 input.tilePlan.tilesToRenderThisFrame) {
                const TileRenderContentState& rc =
                    tile->content.renderContent;
                if (!rc.hasCachedDrawCommands()) {
                    continue;
                }
                for (const RenderCommand& cached :
                     rc.cachedDrawCommands()) {
                    if (cached.terrainHeightLayer < 0) {
                        continue;
                    }
                    pool->touchHeightTexture(
                        tile->key, cached.terrainHeightGridSize,
                        input.frameNumber);
                    break;
                }
            }
        }

        TileRenderEntryCommandStats renderStats;
        double selectedBuildMs = 0.0;
        const int synchronousRenderPrepCount =
            input.tilePlan.renderEntrySynchronousPrepCount;
        const int deferredRenderPrepCount =
            input.tilePlan.renderEntryDeferredPrepCount;
        const int ancestorFallbackDrawCount =
            input.tilePlan.renderEntryAncestorFallbackCount;

        auto mergeRenderStats =
            [&](const TileRenderEntryCommandStats& stats) {
                renderStats.plannedEntries += stats.plannedEntries;
                renderStats.ensuredTiles += stats.ensuredTiles;
                renderStats.meshReadyTiles += stats.meshReadyTiles;
                renderStats.drawAttempts += stats.drawAttempts;
                renderStats.missingSelectedTiles +=
                    stats.missingSelectedTiles;
                renderStats.missingRenderTiles += stats.missingRenderTiles;
                renderStats.deferredEntries += stats.deferredEntries;
                renderStats.missedDrawEntries += stats.missedDrawEntries;
            };
        auto renderEntriesFor = [&](TileRenderEntryPass pass) {
            TileRenderEntryCommandStats stats =
                TileRenderEntryCommandBuilder::buildEntries(
                    input.renderEntriesOverride
                        ? *input.renderEntriesOverride
                        : input.tilePlan.renderEntries,
                    pass,
                    input.frameNumber,
                    renderer,
                    commands,
                    terrainCacheKey,
                    protectTile,
                    buildTileDrawCommand);
            mergeRenderStats(stats);
            return stats;
        };

        const double selectedBuildStartMs = perf::nowMs();
        const TileRenderEntryCommandStats selectedStats =
            renderEntriesFor(TileRenderEntryPass::Selected);
        selectedBuildMs = perf::nowMs() - selectedBuildStartMs;

        input.tilePlan.renderEntryPlannedCommandCount =
            renderStats.plannedEntries;
        input.tilePlan.renderEntrySelectedPlannedCommandCount =
            selectedStats.plannedEntries;
        input.tilePlan.renderEntryCommandDrawCount =
            renderStats.drawAttempts;
        input.tilePlan.renderEntrySelectedCommandDrawCount =
            selectedStats.drawAttempts;
        input.tilePlan.renderEntryCommandMissedDrawCount =
            renderStats.missedDrawEntries;
        input.tilePlan.renderEntrySelectedCommandMissedDrawCount =
            selectedStats.missedDrawEntries;
        input.tilePlan.renderEntryCommandMissingSelectedCount =
            renderStats.missingSelectedTiles;
        input.tilePlan.renderEntryCommandMissingRenderCount =
            renderStats.missingRenderTiles;
        input.tilePlan.renderEntryCommandDeferredCount =
            renderStats.deferredEntries;
        input.tilePlan.renderEntrySelectedCommandDeferredCount =
            selectedStats.deferredEntries;

        const TileRenderFrameMaintenanceTimings maintenanceTimings =
            TileRenderFrameMaintenance::run(
                trimRasterCaches,
                shouldUnloadCachedBytes,
                unloadCachedBytes);

        const std::array<char, 1024> buildBreakdown =
            TileFrameDebugLogFormatter::renderBuildDetail(
                TileRenderDebugLogInput{
                    selectedBuildMs,
                    maintenanceTimings,
                    input.tilePlan.visibleTiles.size(),
                    renderStats,
                    selectedStats,
                    commands.size() - commandsBeforeTileset,
                    input.interactionActive,
                    input.resourceSmoothingActive,
                    synchronousRenderPrepCount,
                    deferredRenderPrepCount,
                    ancestorFallbackDrawCount});
        perf::logTimingAtLeast(
            input.frameNumber,
            "Tileset.buildRenderCommands",
            perf::nowMs() - buildCommandsStartMs,
            20.0,
            buildBreakdown.data());
    }
};

} // namespace earth_engine
