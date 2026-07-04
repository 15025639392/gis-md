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
        TileRenderEntryPass pass,
        uint64_t frameNumber,
        Renderer& renderer,
        RenderCommandList& commands,
        EnsureTileFn&& ensureTile,
        CacheKeyFn&& cacheKey,
        MarkIneligibleFn&& markIneligible,
        BuildTileDrawCommandFn&& buildTileDrawCommand) {
        TileRenderEntryCommandStats stats;
        for (const TileRenderEntry& entry : plan.renderEntries) {
            if (entry.renderPass() != pass) {
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
                    entry.hasSurfaceClip()
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
                // 非 clip 命令的 stableKey 由 tile 常驻缓存一次性生成
                // (cacheKey + "#i",见 GltfDrawCommandBuilder)。同一渲染瓦片
                // 的多个 clip 实例可在一帧内共存,必须在 key 里保留被选中
                // 子片的身份,这里每帧改写(罕见路径)。
                if (entry.hasSurfaceClip()) {
                    size_t stableIndex = 0;
                    std::string baseStableKey = cacheKey(commandTile->key);
                    baseStableKey += "|clip:";
                    baseStableKey += cacheKey(entry.selectedKey);
                    for (size_t i = before; i < commands.size(); ++i) {
                        commands[i].stableKey =
                            baseStableKey + "#" + std::to_string(stableIndex++);
                    }
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
