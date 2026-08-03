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
    template <typename CacheKeyFn,
              typename ProtectTileFn,
              typename BuildTileDrawCommandFn>
    static TileRenderEntryCommandStats build(
        const TilePlan& plan,
        TileRenderEntryPass pass,
        uint64_t frameNumber,
        Renderer& renderer,
        RenderCommandList& commands,
        CacheKeyFn&& cacheKey,
        ProtectTileFn&& protectTile,
        BuildTileDrawCommandFn&& buildTileDrawCommand) {
        return buildEntries(
            plan.renderEntries,
            pass,
            frameNumber,
            renderer,
            commands,
            std::forward<CacheKeyFn>(cacheKey),
            std::forward<ProtectTileFn>(protectTile),
            std::forward<BuildTileDrawCommandFn>(buildTileDrawCommand));
    }

    template <typename CacheKeyFn,
              typename ProtectTileFn,
              typename BuildTileDrawCommandFn>
    static TileRenderEntryCommandStats buildEntries(
        const std::vector<TileRenderEntry>& entries,
        TileRenderEntryPass pass,
        uint64_t frameNumber,
        Renderer& renderer,
        RenderCommandList& commands,
        CacheKeyFn&& cacheKey,
        ProtectTileFn&& protectTile,
        BuildTileDrawCommandFn&& buildTileDrawCommand) {
        TileRenderEntryCommandStats stats;
        for (const TileRenderEntry& entry : entries) {
            if (entry.renderPass() != pass) {
                continue;
            }
            ++stats.plannedEntries;

            TilesetTile* selectedTile = entry.selectedTile;
            if (!selectedTile) {
                ++stats.missingSelectedTiles;
                continue;
            }
            ++stats.ensuredTiles;

            protectTile(selectedTile, frameNumber);

            TilesetTile* commandTile = entry.renderTile;
            if (!commandTile) {
                ++stats.missingRenderTiles;
                continue;
            }

            protectTile(commandTile, frameNumber);

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
                    surfaceClipUv,
                    // 机制 A:clip 上下文携带后代瓦片,盖章期换成后代模板
                    // 几何 + 祖先高度子矩形采样(资源未就绪回落 discard)。
                    surfaceClipUv ? entry.selectedTile : nullptr);
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
