#pragma once

#include "RasterMappedToTilesetTile.h"
#include "TilePlan.h"
#include "TilesetTile.h"

#include "../renderer/RenderCommand.h"

#include <array>
#include <cstddef>
#include <optional>

namespace earth_engine {

class Renderer;

struct TileRenderEntryCommandStats {
    int ensuredTiles = 0;
    int meshReadyTiles = 0;
    int drawAttempts = 0;
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

            TilesetTile* selectedTile = ensureTile(entry.selectedKey);
            if (!selectedTile) continue;
            ++stats.ensuredTiles;

            selectedTile->lastUsedFrame = frameNumber;
            markIneligible(cacheKey(entry.selectedKey));

            TilesetTile* commandTile = ensureTile(entry.renderKey);
            if (!commandTile) continue;

            commandTile->lastUsedFrame = frameNumber;
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
            }
            if (commandTile->meshReady && commandTile->gpuVertexBuffer) {
                ++stats.meshReadyTiles;
            }
            if (commands.size() > before) {
                ++stats.drawAttempts;
            }
        }
        return stats;
    }
};

} // namespace earth_engine
