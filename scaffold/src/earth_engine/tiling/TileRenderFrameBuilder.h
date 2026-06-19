#pragma once

#include "TileFrameDebugLogFormatter.h"
#include "TilePlan.h"
#include "TileRenderEntryCommandBuilder.h"
#include "TileRenderFrameMaintenance.h"
#include "TileSelectionMetrics.h"
#include "TileUnloadPolicy.h"
#include "TileUnloadQueue.h"
#include "TilesetTile.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Vec3.h"
#include "../debug/PerfTimer.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../renderer/RenderCommand.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class Renderer;
struct FogDensityAtHeight;

struct TileRenderFrameBuildInput {
    TilePlan& tilePlan;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles;
    TileUnloadQueue& unloadQueue;
    std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    bool& cacheBytesDirty;
    uint64_t frameNumber = 0;
    Vec3 lastCameraPosition;
    const std::vector<FogDensityAtHeight>& fogDensityTable;
    int fogCulled = 0;
    bool resourceSmoothingActive = false;
    bool interactionActive = false;
    int64_t totalBytesUsed = 0;
    int64_t maximumCachedBytes = 0;
};

class TileRenderFrameBuilder {
public:
    template <typename EnsureTileFn,
              typename TerrainCacheKeyFn,
              typename MarkIneligibleFn,
              typename BuildTileDrawCommandFn,
              typename DetachInactiveFn,
              typename MarkEligibleFn,
              typename UpdateTotalBytesFn,
              typename UnloadCachedBytesFn>
    static void build(
        TileRenderFrameBuildInput input,
        Renderer& renderer,
        RenderCommandList& commands,
        EnsureTileFn&& ensureTile,
        TerrainCacheKeyFn&& terrainCacheKey,
        MarkIneligibleFn&& markIneligible,
        BuildTileDrawCommandFn&& buildTileDrawCommand,
        DetachInactiveFn&& detachInactive,
        MarkEligibleFn&& markEligible,
        UpdateTotalBytesFn&& updateTotalBytes,
        UnloadCachedBytesFn&& unloadCachedBytes) {
        const double buildCommandsStartMs = perf::nowMs();
        const size_t commandsBeforeTileset = commands.size();
        input.tilePlan.renderEntryPlannedCommandCount = 0;
        input.tilePlan.renderEntrySelectedPlannedCommandCount = 0;
        input.tilePlan.renderEntryFadingPlannedCommandCount = 0;
        input.tilePlan.renderEntryCommandDrawCount = 0;
        input.tilePlan.renderEntrySelectedCommandDrawCount = 0;
        input.tilePlan.renderEntryFadingCommandDrawCount = 0;
        input.tilePlan.renderEntryCommandMissedDrawCount = 0;
        input.tilePlan.renderEntryCommandMissingSelectedCount = 0;
        input.tilePlan.renderEntryCommandMissingRenderCount = 0;
        input.tilePlan.renderEntryCommandDeferredCount = 0;

        // Raster providers stamp getTile() calls with the current frame.
        for (auto* overlay : input.rasterOverlays) {
            if (overlay) {
                overlay->setFrameNumber(input.frameNumber);
            }
        }

        double cameraHeight = Ellipsoid::WGS84().cartesianToCartographic(
            input.lastCameraPosition).height();
        cameraHeight = std::max(0.0, cameraHeight);
        const double fogDensity =
            TileSelectionMetrics::computeFogDensity(
                input.fogDensityTable,
                cameraHeight);

        TileRenderEntryCommandStats renderStats;
        double selectedBuildMs = 0.0;
        double fadeBuildMs = 0.0;
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
                TileRenderEntryCommandBuilder::build(
                    input.tilePlan,
                    pass,
                    input.frameNumber,
                    renderer,
                    commands,
                    ensureTile,
                    terrainCacheKey,
                    markIneligible,
                    buildTileDrawCommand);
            mergeRenderStats(stats);
            return stats;
        };

        const double selectedBuildStartMs = perf::nowMs();
        const TileRenderEntryCommandStats selectedStats =
            renderEntriesFor(TileRenderEntryPass::Selected);
        selectedBuildMs = perf::nowMs() - selectedBuildStartMs;

        const double fadeBuildStartMs = perf::nowMs();
        const TileRenderEntryCommandStats fadingStats =
            renderEntriesFor(TileRenderEntryPass::Fading);
        fadeBuildMs = perf::nowMs() - fadeBuildStartMs;

        input.tilePlan.renderEntryPlannedCommandCount =
            renderStats.plannedEntries;
        input.tilePlan.renderEntrySelectedPlannedCommandCount =
            selectedStats.plannedEntries;
        input.tilePlan.renderEntryFadingPlannedCommandCount =
            fadingStats.plannedEntries;
        input.tilePlan.renderEntryCommandDrawCount =
            renderStats.drawAttempts;
        input.tilePlan.renderEntrySelectedCommandDrawCount =
            selectedStats.drawAttempts;
        input.tilePlan.renderEntryFadingCommandDrawCount =
            fadingStats.drawAttempts;
        input.tilePlan.renderEntryCommandMissedDrawCount =
            renderStats.missedDrawEntries;
        input.tilePlan.renderEntryCommandMissingSelectedCount =
            renderStats.missingSelectedTiles;
        input.tilePlan.renderEntryCommandMissingRenderCount =
            renderStats.missingRenderTiles;
        input.tilePlan.renderEntryCommandDeferredCount =
            renderStats.deferredEntries;

        const bool hasQueuedUnloadingTile =
            TileUnloadPolicy::hasQueuedTileInState(
                input.unloadQueue,
                input.tiles,
                TileLoadState::Unloading);
        const TileRenderFrameMaintenanceTimings maintenanceTimings =
            TileRenderFrameMaintenance::run(
                input.tiles,
                input.frameNumber,
                input.rasterOverlays,
                input.cacheBytesDirty,
                input.resourceSmoothingActive,
                input.totalBytesUsed > input.maximumCachedBytes ||
                    hasQueuedUnloadingTile,
                detachInactive,
                markEligible,
                updateTotalBytes,
                unloadCachedBytes);

        const std::array<char, 512> buildBreakdown =
            TileFrameDebugLogFormatter::renderBuildDetail(
                TileRenderDebugLogInput{
                    selectedBuildMs,
                    fadeBuildMs,
                    maintenanceTimings,
                    input.tilePlan.visibleTiles.size(),
                    input.tilePlan.tilesFadingOut.size(),
                    renderStats,
                    selectedStats,
                    fadingStats,
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

#ifndef __ANDROID__
        (void)commandsBeforeTileset;
        (void)fogDensity;
        (void)input.fogCulled;
        (void)renderStats;
#endif
    }
};

} // namespace earth_engine
