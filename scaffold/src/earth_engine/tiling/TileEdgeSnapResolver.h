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
    /// 邻居探测结果:八度差(clamp 后的 log2 步长)+ 命中的邻居 entry。
    /// entry 指针供 ①-1 的边高度 LUT 取邻居真实渲染高度用;lg=0 时无意义。
    struct EdgeNeighbor {
        int log2Step = 0;
        const TileRenderEntry* entry = nullptr;
    };

    /// 渲染集索引的值:该 cell 的八度与产生它的 entry。与 EdgeNeighbor 分开
    /// 定义是因为两者的 int 含义不同(八度 vs log2 步长),共用一个类型必然
    /// 有人把八度当步长用。
    struct CellEntry {
        int octave = 0;
        const TileRenderEntry* entry = nullptr;
    };

    static void resolve(TilePlan& plan) {
        plan.edgeSnapRecords.clear();
        // 渲染集索引:占屏 cell(selectedKey)→ (八度, entry)。仅本帧选中条目
        // (fading 条目画在过渡层,不参与边界几何契约)。
        std::unordered_map<uint64_t, CellEntry> octaves;
        octaves.reserve(plan.renderEntries.size() * 2);
        for (const TileRenderEntry& e : plan.renderEntries) {
            if (!e.selectedThisFrame || !e.selectedTile) {
                continue;
            }
            octaves[cellKey(e.selectedKey)] = CellEntry{entryOctave(e), &e};
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
            const EdgeNeighbor edges[TileEdgeSnapRecord::kEdgeCount] = {
                edgeSnapNeighbor(octaves, k, -1, 0, own),
                edgeSnapNeighbor(octaves, k, +1, 0, own),
                edgeSnapNeighbor(octaves, k, 0, -1, own),
                edgeSnapNeighbor(octaves, k, 0, +1, own),
            };
            state.edgeSnapPacked = static_cast<float>(
                edges[0].log2Step + 8 * edges[1].log2Step +
                64 * edges[2].log2Step + 512 * edges[3].log2Step);
            TileEdgeSnapRecord rec;
            rec.tile = e.selectedTile;
            for (int i = 0; i < TileEdgeSnapRecord::kEdgeCount; ++i) {
                rec.edgeLog2[i] = edges[i].log2Step;
                rec.neighbor[i] = edges[i].entry;
            }
            if (rec.hasAnySnap()) {
                plan.edgeSnapRecords.push_back(rec);
            }
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
    /// 步长上限 2^6)与命中的邻居 entry。
    ///
    /// ⚠️ log2Step 被 clamp 而 entry 不被 clamp:step 触顶(>6)时几何吸附
    /// 退化,但 LUT 仍按**未 clamp 的真实间距**去取邻居高度会与 shader 采样
    /// 位置错位 —— 故 clamp 命中时连 entry 一起丢弃,整条边退回自吸附。
    static EdgeNeighbor edgeSnapNeighbor(
        const std::unordered_map<uint64_t, CellEntry>& octaves,
        const TileKey& k, int dx, int dy, int ownOctave) {
        const int tilesAtLevel = 1 << k.z;
        int nx = k.x + dx;
        const int ny = k.y + dy;
        if (ny < 0 || ny >= tilesAtLevel) {
            return {};  // 极侧无邻居
        }
        nx = ((nx % tilesAtLevel) + tilesAtLevel) % tilesAtLevel;  // 经向环绕
        TileKey probe{k.schemeId, k.z, nx, ny};
        for (int up = 0; up <= 8 && probe.z >= 0; ++up) {
            auto it = octaves.find(cellKey(probe));
            if (it != octaves.end()) {
                const int raw = ownOctave - it->second.octave;
                if (raw <= 0) {
                    return {};  // 邻居同级或更细:细侧不吸
                }
                if (raw > 6) {
                    // 触顶:几何仍按 2^6 吸附(与改前一致),但不给 LUT 邻居
                    // (采样位置对不上,给了反而引入新错位)。
                    return EdgeNeighbor{6, nullptr};
                }
                return EdgeNeighbor{raw, it->second.entry};
            }
            if (probe.z == 0) {
                break;
            }
            probe = TileKey{probe.schemeId, probe.z - 1, probe.x >> 1,
                            probe.y >> 1};
        }
        return {};
    }
};

} // namespace earth_engine
