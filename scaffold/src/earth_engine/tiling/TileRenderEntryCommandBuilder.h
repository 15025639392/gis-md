#pragma once

#include "RasterMappedToTilesetTile.h"
#include "TilePlan.h"
#include "TilesetTile.h"

#include "../renderer/RenderCommand.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace earth_engine {

class Renderer;

struct TileRenderEntryCommandStats {
    int plannedEntries = 0;
    int ensuredTiles = 0;
    int meshReadyTiles = 0;
    int drawAttempts = 0;
    int missingSelectedTiles = 0;
    int missingRenderTiles = 0;
    int deferredEntries = 0;
    int missedDrawEntries = 0;
};

class TileRenderEntryCommandBuilder {
public:
    template <typename EnsureTileFn,
              typename CacheKeyFn,
              typename MarkIneligibleFn,
              typename BuildTileDrawCommandFn>
    static TileRenderEntryCommandStats build(
        const TilePlan& plan,
        bool selectedThisFrame,
        uint64_t frameNumber,
        Renderer& renderer,
        RenderCommandList& commands,
        EnsureTileFn&& ensureTile,
        CacheKeyFn&& cacheKey,
        MarkIneligibleFn&& markIneligible,
        BuildTileDrawCommandFn&& buildTileDrawCommand) {
        TileRenderEntryCommandStats stats;
        for (const TileRenderEntry& entry : plan.renderEntries) {
            if (entry.selectedThisFrame != selectedThisFrame) {
                continue;
            }
            ++stats.plannedEntries;

            TilesetTile* selectedTile = ensureTile(entry.selectedKey);
            if (!selectedTile) {
                ++stats.missingSelectedTiles;
                continue;
            }
            ++stats.ensuredTiles;

            selectedTile->markUsedForRenderFrame(frameNumber);
            markIneligible(cacheKey(entry.selectedKey));

            TilesetTile* commandTile = ensureTile(entry.renderKey);
            if (!commandTile) {
                ++stats.missingRenderTiles;
                continue;
            }

            commandTile->markUsedForRenderFrame(frameNumber);
            commandTile->addReference();
            markIneligible(cacheKey(commandTile->key));

            const size_t before = commands.size();
            if (entry.allowSynchronousMeshPrep) {
                const std::optional<std::array<float, 4>> surfaceClipUv =
                    entry.surfaceClipEnabled
                        ? std::optional<std::array<float, 4>>(
                              entry.surfaceClipUv)
                        : std::nullopt;
                buildTileDrawCommand(
                    renderer,
                    *commandTile,
                    commands,
                    entry.opacity,
                    entry.allowSynchronousMeshPrep,
                    surfaceClipUv);
            } else {
                ++stats.deferredEntries;
            }
            if (commandTile->content.renderContent.hasGpuSurfaceGeometry()) {
                ++stats.meshReadyTiles;
            }
            if (commands.size() > before) {
                size_t stableIndex = 0;
                std::string baseStableKey = cacheKey(commandTile->key);
                if (entry.surfaceClipEnabled) {
                    baseStableKey += "|clip:";
                    baseStableKey += cacheKey(entry.selectedKey);
                }
                for (size_t i = before; i < commands.size(); ++i) {
                    commands[i].stableKey =
                        baseStableKey + "#" + std::to_string(stableIndex++);
                }
                ++stats.drawAttempts;
            } else if (entry.allowSynchronousMeshPrep) {
                ++stats.missedDrawEntries;
            }
        }
        return stats;
    }
};

} // namespace earth_engine
