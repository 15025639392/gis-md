#pragma once

#include "TerrainEdgeNeighborHeight.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace earth_engine {

/// 无缝北极星 ①-1:边吸附改取**邻居真值**的 CPU 侧数据。
///
/// 机制 B 的边吸附把细瓦片边顶点吸到「自纹理 2^k 间距的线性插值」。那只保证
/// **细瓦片自洽**,不保证与粗邻居显示的一致 —— 粗邻居显示的是它自己那份数据,
/// 两者之差就是金字塔层间重采样差 ε(线上源实测边界 p95 4.7~12.8m,引擎侧
/// `TileEdgeMismatchProbe` 稳态读到 mean 3~6.5m / agree 0.13~0.37)。
///
/// 本表把「粗邻居在吸附节点处实际渲染出来的高度」逐节点算好,交给 shader 取代
/// 自纹理采样。两侧于是在共享边上求值同一个函数 → 恒等,而不是"逼近到很小"。
///
/// **为什么是 CPU 算好而不是 GPU 采邻居纹理**(方案 A 被否的两条):
///   ① 四条边各面向不同邻居,每边都要独立的 (layer, mr, gridN, 子矩形偏移)
///      ≈ 6 float,实例流 96→192B 翻倍;
///   ② coarse(65²)与 dense(257²)是**两个 texture array 对象**
///      (TerrainDisplacementTemplatePool.h:207),而吸附恰在档边界最常触发,
///      shader 只绑一个 sampler 采不到跨档邻居。
/// CPU 侧直接读 `DecodedHeightmap`,两条都不存在;代价是每边 ≤129 个节点的
/// 一张小表。
///
/// ⚠️ 节点位置必须与 shader 的取值位置逐一对应:shader 用
/// `a0 = floor(a/snapStep)*snapStep`、`a1 = min(a0+snapStep, gridN)`,
/// 节点序号 j = a/snapStep。对不上就是把错位换个地方而已。
struct TerrainEdgeHeightLut {
    static constexpr int kEdges = TileEdgeSnapRecord::kEdgeCount;
    /// 最大节点数。最密情形 = dense 档(gridN=256)邻居只粗一个八度(step=2)
    /// → 256/2+1 = 129。
    static constexpr int kMaxNodes = 129;
    struct Data {
        /// 存**差值**:邻居在该节点渲染出的高度 − 本瓦片该纹素渲染出的高度。
        /// shader 侧 `hA/hB` 照旧取自纹理,再加上这里的差 → 两侧于是在共享边上
        /// 求值同一个函数。差值形式让失效路径天然安全:delta=0 = 改前行为。
        float delta[kEdges][kMaxNodes] = {};
        /// 0 = 该边不吸附,或邻居数据取不到 → shader 退回自纹理吸附(改前行为,
        /// 不会更差)。
        int nodeCount[kEdges] = {};
        bool hasAny() const {
            for (int e = 0; e < kEdges; ++e) {
                if (nodeCount[e] > 0) return true;
            }
            return false;
        }
        int filledEdges() const {
            int n = 0;
            for (int e = 0; e < kEdges; ++e) n += nodeCount[e] > 0 ? 1 : 0;
            return n;
        }
    };

    /// 为一个吸附瓦片构建四条边的差值表。ownGridSize = 该瓦片本帧的位移模板
    /// 档位(从 draw 命令的 terrainHeightGridSize 读真值,不要另猜)。
    ///
    /// ⚠️ 「自己这一侧」必须与 shader 取的是同一个数:shader 的吸附分支取的是
    /// 自纹理该纹素(吸附覆盖了 morph 混合),所以这里也取 fine 档 × fade,
    /// **不带 morph** —— 带上就等于给差值预置了一个随相机漂移的偏置。
    static Data build(const TileEdgeSnapRecord& rec, int ownGridSize) {
        Data d;
        if (!rec.tile || ownGridSize <= 0) return d;
        const DecodedHeightmap* ownHm =
            rec.tile->content.renderContent.retainedHeightmap();
        if (!ownHm || !ownHm->valid()) return d;
        const float ownFade = terrainReliefFade(rec.tile->key.z);
        for (int edge = 0; edge < kEdges; ++edge) {
            const int lg = rec.edgeLog2[edge];
            const TileRenderEntry* nbr = rec.neighbor[edge];
            if (lg <= 0 || !nbr) continue;
            const terrain_edge::HeightSource ns = terrain_edge::sourceOf(*nbr);
            if (!ns.valid() || !ns.heightmap->valid()) continue;
            const int nodes = terrain_edge::edgeNodeCount(ownGridSize, lg);
            if (nodes <= 1 || nodes > kMaxNodes) continue;
            const int step = 1 << lg;
            for (int j = 0; j < nodes; ++j) {
                const double t = std::min(
                    static_cast<double>(j * step) / ownGridSize, 1.0);
                double lon = 0.0, lat = 0.0;
                terrain_edge::edgePoint(rec.tile->bounds, edge, t, lon, lat);
                const float own =
                    DecodedHeightmapSampler::sampleHeightRenderGrid(
                        *ownHm, rec.tile->bounds, lon, lat, ownGridSize) *
                    ownFade;
                d.delta[edge][j] =
                    terrain_edge::renderedHeight(ns, lon, lat) - own;
            }
            d.nodeCount[edge] = nodes;
        }
        return d;
    }

    /// shader 会取的两个节点序号。抽成函数是为了让「表的索引约定」与
    /// 「shader 的取值约定」有一处可被单独证伪的共同定义 —— 两边各写一遍
    /// 必然有一遍会写错(定位阶段已在别处踩过位置错位的坑)。
    static void shaderNodePair(float a, int log2Step, int ownGridSize,
                               int& j0, int& j1, float& frac) {
        const float snapStep = static_cast<float>(1 << log2Step);
        const float a0 = std::floor(a / snapStep) * snapStep;
        const float a1 = std::min(a0 + snapStep, static_cast<float>(ownGridSize));
        j0 = static_cast<int>(a0 / snapStep);
        j1 = static_cast<int>(a1 / snapStep);
        const float denom = std::max(a1 - a0, 1e-6f);
        frac = std::clamp((a - a0) / denom, 0.0f, 1.0f);
    }
};

} // namespace earth_engine
