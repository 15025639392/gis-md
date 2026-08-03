#pragma once

#include "TerrainDisplacementTemplatePool.h"
#include "TilePlan.h"
#include "TilesetTile.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace earth_engine {

/// 无缝北极星机制 B:从**本帧实际渲染集**为每个选中瓦片解析 4 条边的
/// 邻居八度差,打包进 selectionFrameState.edgeSnapPacked(消费方:
/// applyPerFrameCommandState → u_terrainLayers.z → 顶点 shader 边吸附)。
///
/// 八度 = z + (dense 档 ? 2 : 0):dense(256)比 coarse(64)细 log2(256/64)=2
/// 个二分级,同 z 异档边界与异 z 边界是同一种 T-junction,统一处理。
/// 祖先回退(remap)条目:数据来自祖先 → 按 renderKey.z(数据八度)参与
/// 邻居索引,但自身不吸附(其边界本就是祖先平滑场,由邻居侧向它吸附)。
///
/// 已知近似(接受,残余由裙墙覆盖):dense 档判定用与 draw 侧同一纯函数
/// terrainGridSizeForSse,但 draw 侧层池触顶时可能实际回落 coarse,此时
/// 本瓦片八度被高估 2 → 邻居少吸 2 级,退化为改动前的状态,不会更差。
struct TileEdgeSnapResolver {
    static void resolve(TilePlan& plan) {
        // 渲染集索引:占屏 cell(selectedKey)→ 八度。仅本帧选中条目
        // (fading 条目画在过渡层,不参与边界几何契约)。
        std::unordered_map<uint64_t, int> octaves;
        octaves.reserve(plan.renderEntries.size() * 2);
        for (const TileRenderEntry& e : plan.renderEntries) {
            if (!e.selectedThisFrame || !e.selectedTile) {
                continue;
            }
            octaves[cellKey(e.selectedKey)] = entryOctave(e);
        }
        for (TileRenderEntry& e : plan.renderEntries) {
            if (!e.selectedTile) {
                continue;
            }
            TileSelectionFrameState& state =
                e.selectedTile->selectionFrameState;
            if (!e.selectedThisFrame || e.usesAncestorFallback) {
                state.edgeSnapPacked = 0.0f;
                continue;
            }
            const int own = entryOctave(e);
            const TileKey& k = e.selectedKey;
            // 边序与 shader 解码约定一致:W + 8·E + 64·N + 512·S。
            // y 轴:v=0 为北 → N 边 = y-1 cell,S 边 = y+1 cell。
            const int w = edgeSnapLog2(octaves, k, -1, 0, own);
            const int east = edgeSnapLog2(octaves, k, +1, 0, own);
            const int n = edgeSnapLog2(octaves, k, 0, -1, own);
            const int south = edgeSnapLog2(octaves, k, 0, +1, own);
            state.edgeSnapPacked =
                static_cast<float>(w + 8 * east + 64 * n + 512 * south);
        }
    }

    static int entryOctave(const TileRenderEntry& e) {
        if (e.usesAncestorFallback) {
            return e.renderKey.z;  // remap:数据八度 = 祖先层级(恒 coarse 档)
        }
        const int grid = terrainGridSizeForSse(
            e.selectedTile->selectionFrameState.screenSpaceError);
        return e.selectedKey.z +
               (grid >= kTerrainDenseGridSize ? 2 : 0);
    }

private:
    // cell 键:schemeId interned 句柄哈希掺 4bit + z/x/y 打包。z≤27 层内
    // x,y < 2^27,本引擎 z≤18 富余。
    static uint64_t cellKey(const TileKey& k) {
        const uint64_t scheme =
            std::hash<SchemeId>{}(k.schemeId) & 0xFull;
        return (scheme << 60) |
               (static_cast<uint64_t>(static_cast<uint32_t>(k.z) & 0x3F)
                << 54) |
               (static_cast<uint64_t>(static_cast<uint32_t>(k.x) &
                                      0x7FFFFFF)
                << 27) |
               static_cast<uint64_t>(static_cast<uint32_t>(k.y) & 0x7FFFFFF);
    }

    /// 邻居 cell 自本级向上探(邻居更细时本级即 miss → 0,不吸附:细侧
    /// 负责向粗侧吸)。返回 clamp 到 [0,6] 的 log2 步长(coarse 档 64 格,
    /// 步长上限 2^6)。
    static int edgeSnapLog2(
        const std::unordered_map<uint64_t, int>& octaves,
        const TileKey& k, int dx, int dy, int ownOctave) {
        const int tilesAtLevel = 1 << k.z;
        int nx = k.x + dx;
        const int ny = k.y + dy;
        if (ny < 0 || ny >= tilesAtLevel) {
            return 0;  // 极侧无邻居
        }
        nx = ((nx % tilesAtLevel) + tilesAtLevel) % tilesAtLevel;  // 经向环绕
        TileKey probe{k.schemeId, k.z, nx, ny};
        for (int up = 0; up <= 8 && probe.z >= 0; ++up) {
            auto it = octaves.find(cellKey(probe));
            if (it != octaves.end()) {
                return std::clamp(ownOctave - it->second, 0, 6);
            }
            if (probe.z == 0) {
                break;
            }
            probe = TileKey{probe.schemeId, probe.z - 1, probe.x >> 1,
                            probe.y >> 1};
        }
        return 0;
    }
};

} // namespace earth_engine
