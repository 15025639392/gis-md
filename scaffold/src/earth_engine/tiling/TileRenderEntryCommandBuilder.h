#pragma once

#include "RasterMappedToTilesetTile.h"
#include "TilePlan.h"
#include "TilesetTile.h"

#include "../renderer/RenderCommand.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

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
                        // stableKey 是非拥有 view。clip key 每帧现算(不进 tile
                        // 缓存,否则瞬态键会污染常驻缓存),存进帧级 arena,view
                        // 活到本帧命令消费完(见 internTransientStableKey)。
                        commands[i].stableKey = internTransientStableKey(
                            frameNumber,
                            baseStableKey + "#" + std::to_string(stableIndex++));
                    }
                }
                ++stats.drawAttempts;
            } else if (entry.allowSynchronousMeshPrep) {
                ++stats.missedDrawEntries;
            }
        }
        return stats;
    }

private:
    // clip 命令 stableKey(纯诊断 view)的帧级真源持有存储。
    //
    // 生命周期契约:clip key 逐帧现算,view 须活到本帧命令被消费(submit +
    // PresentationTrace 构建)完。arena 在 frameNumber 变化时(即每帧首个 clip
    // 写入前)整体清空 → 上一帧的 view 到此才失效,而它们早已在上一帧消费完。
    // 同一 frameNumber 内多趟 pass / 多 tileset 的 clip key 累积共存,互不失效。
    // deque 保证 push_back 不迁移既有元素 → 已发出的 view 帧内稳定。
    //
    // 渲染单线程,thread_local 兼作并发兜底(任一线程各自独立 arena)。此存储
    // 只承载诊断字符串,最坏故障是 trace 里 clip key 陈旧,永不触及渲染正确性。
    static std::string_view internTransientStableKey(uint64_t frameNumber,
                                                     std::string key) {
        static thread_local std::deque<std::string> arena;
        static thread_local uint64_t arenaFrame = ~static_cast<uint64_t>(0);
        if (frameNumber != arenaFrame) {
            arena.clear();
            arenaFrame = frameNumber;
        }
        arena.push_back(std::move(key));
        return arena.back();
    }
};

} // namespace earth_engine
