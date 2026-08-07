#pragma once

#include "DecodedHeightmapSampler.h"
#include "TerrainDisplacementTemplatePool.h"
#include "TilePlan.h"
#include "TilesetTile.h"
#include "../core/math/Rectangle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace earth_engine {

/// 跨瓦接边**几何错位**的直接测量(无缝北极星 ①-1 的判据)。
///
/// 为什么另起一把尺子:2026-08-08 修好接缝采集台后,漏天像素 steady/trans
/// 实测基本恒 0;但同日 `tools/seam_metric/pyramid_eps.py` 在**线上源**仍量到
/// 金字塔层间 ε 边界 p95 4.7~12.8m。两件事不矛盾 —— ε 被裙墙盖住了,没漏成天。
/// 于是"漏天=0"这把尺子再也回答不了"边吸附改取邻居真值还值不值得做"。
/// 本探针不看像素,直接比两侧顶点高度。
///
/// 采样点取机制 B 吸附边上的 2^k 间距节点(**shader 真正取值的那些位置**):
///   - 细侧(吸附方)在节点处渲染的就是自纹理该纹素 —— 吸附分支
///     `hOut = mix(hA, hB, t)` 整个覆盖了 morph 混合,而节点处 t=0/1;
///   - 粗侧不吸附(细侧负责),渲染 mix(hCoarse, hFine, morph_邻居)。
/// 二者之差 = 该节点的几何错位,单位米。金字塔严格嵌套时应恒 0。
///
/// ⚠️ 两侧高度都必须走 `DecodedHeightmapSampler::sampleHeightRenderGrid`
/// ——它是 draw 侧与 CPU 侧的单一事实源。在这里另写一份采样,量出来的就是
/// 我自己两份代码的差,不是引擎的错位(测试台自带被测逻辑复制品,踩过)。
struct TileEdgeMismatchProbe {
    /// 视为"对齐"的容差。0.1m 是 Terrain-RGB 的编码步长 —— 比它小的差
    /// 数据本身就表达不出来,不该记成错位。
    static constexpr float kToleranceMeters = 0.1f;

    /// 每边最多取多少节点。满档(dense 邻 dense,step=2)一边有 129 个节点;
    /// 按 stride 抽稀 —— 错位是沿边连续的场,抽稀不改变分布形状。
    /// 33 是逐帧测量下的实测折中:掠视稳态约 10 条吸附边 × 33 节点 × 3 次
    /// 双线性 ≈ 1000 次/帧,可忽略。
    static constexpr int kMaxNodesPerEdge = 33;

    /// 直方图桶上界(米)。用直方图而非存全部样本:免掉逐帧几万个 float 的
    /// 分配,且天然可跨帧累积成窗口分布。
    static constexpr int kBucketCount = 8;
    static constexpr float kBucketUpper[kBucketCount] = {
        0.1f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 25.0f, 1e30f};

    struct Stats {
        int samples = 0;          // 参与比较的节点数
        int within = 0;           // |Δh| ≤ kToleranceMeters 的节点数
        int edges = 0;            // 参与的吸附边数
        double sumAbs = 0.0;
        float maxAbs = 0.0f;
        int buckets[kBucketCount] = {};

        double meanAbs() const {
            return samples > 0 ? sumAbs / samples : 0.0;
        }
        /// 分母为 0 = 本帧没有吸附边(没机会),**不参与判定** —— 与 Contracts
        /// 的 disabled、Policies 的空窗口同理:"没机会"与"不达标"混在一起,
        /// 冷启动/俯视场景会疯狂误报。
        bool hasSamples() const { return samples > 0; }
        double agreementRatio() const {
            return samples > 0 ? static_cast<double>(within) / samples : 0.0;
        }
        /// 由直方图取分位上界(保守:返回该分位落入的桶的上界)。
        float percentileUpper(double q) const {
            if (samples <= 0) return 0.0f;
            const int target = static_cast<int>(std::ceil(q * samples));
            int acc = 0;
            for (int b = 0; b < kBucketCount; ++b) {
                acc += buckets[b];
                if (acc >= target) return kBucketUpper[b];
            }
            return kBucketUpper[kBucketCount - 1];
        }

        void add(float absDelta) {
            ++samples;
            sumAbs += absDelta;
            maxAbs = std::max(maxAbs, absDelta);
            if (absDelta <= kToleranceMeters) ++within;
            for (int b = 0; b < kBucketCount; ++b) {
                if (absDelta <= kBucketUpper[b]) { ++buckets[b]; break; }
            }
        }

        void merge(const Stats& o) {
            samples += o.samples; within += o.within; edges += o.edges;
            sumAbs += o.sumAbs; maxAbs = std::max(maxAbs, o.maxAbs);
            for (int b = 0; b < kBucketCount; ++b) buckets[b] += o.buckets[b];
        }
    };

    /// 两个群体**必须分开统计**,不能混进一个分布:
    ///   fadeUniform 两侧 fade 相同 → 差值就是金字塔层间 ε,这是 ①-1 的
    ///               目标群体,A/B 只看它;
    ///   fadeDiffer  两侧 fade 不同(跨 z<9 档)→ 粗侧被刻意压平,几十~几百米
    ///               的台阶是设计使然,①-1 不会也不该改变它。
    /// 混在一起会让 ①-1 的效果被一个它管不着的、且量级大一个数量级的population
    /// 稀释掉 —— 那正是"比率不对时先怀疑分母/口径"的同一个坑。
    struct Result {
        Stats fadeUniform;
        Stats fadeDiffer;
        int skippedEdges = 0;  // 取不到两侧 heightmap,无法比较

        void merge(const Result& o) {
            fadeUniform.merge(o.fadeUniform);
            fadeDiffer.merge(o.fadeDiffer);
            skippedEdges += o.skippedEdges;
        }
    };

    /// 一个渲染条目在本帧**实际用于位移的**高度来源。remap 条目画的是祖先的
    /// 数据(morph 钉 1,模板恒 coarse 档),常规条目画自己的。
    struct HeightSource {
        const DecodedHeightmap* heightmap = nullptr;
        const Rectangle* bounds = nullptr;
        int gridSize = 0;
        float morph = 1.0f;
        // 起伏淡入系数。高度纹理是按 {minH·fade, range·fade} 上传的,shader
        // 解码出来的就是 fade×真高。⚠️ 不乘它,量出的既不是这一侧也不是那一侧
        // 真正渲染的高度 —— 首版漏了,z6~8 段读出 max=428m 的假错位。
        float fade = 1.0f;
        bool valid() const { return heightmap && bounds && gridSize > 0; }
    };

    static HeightSource sourceOf(const TileRenderEntry& e) {
        HeightSource s;
        const TilesetTile* tile =
            e.usesAncestorFallback ? e.renderTile : e.selectedTile;
        if (!tile) return s;
        s.heightmap = tile->content.renderContent.retainedHeightmap();
        s.bounds = &tile->bounds;
        // fade 恒按**占屏瓦片**(selectedKey)的 z 取:remap 时位移幅度跟后代走
        // (见 GltfDrawCommandBuilder 的 `terrainReliefFade(desc.key.z)`)。
        s.fade = terrainReliefFade(e.selectedKey.z);
        if (e.usesAncestorFallback) {
            // remap:模板恒 coarse 档,且 fine/coarse 同源同值 → morph 钉 1
            // (见 GltfDrawCommandBuilder 的 remap 分支)。
            s.gridSize = kTerrainDisplacementGridSize;
            s.morph = 1.0f;
        } else {
            s.gridSize = terrainGridSizeForSse(
                tile->selectionFrameState.screenSpaceError);
            s.morph = tile->selectionFrameState.terrainMorphFactor;
        }
        return s;
    }

    /// 该来源在给定经纬处**渲染出来**的高度 = mix(coarse, fine, morph)。
    /// coarse 档 = 偶数格点晶格,恰好等于半密度栅格上的同一采样(节点 i=2j 在
    /// gridN 栅格上的 u = j/(gridN/2)),故复用同一个函数换档位传参即可。
    static float renderedHeight(const HeightSource& s, double lonRad,
                                double latRad) {
        const float fine = DecodedHeightmapSampler::sampleHeightRenderGrid(
            *s.heightmap, *s.bounds, lonRad, latRad, s.gridSize);
        if (s.morph >= 0.999f) return fine * s.fade;
        const float coarse = DecodedHeightmapSampler::sampleHeightRenderGrid(
            *s.heightmap, *s.bounds, lonRad, latRad,
            std::max(1, s.gridSize / 2));
        return (coarse + (fine - coarse) * s.morph) * s.fade;
    }

    /// 边 index 与 shader 打包序一致:0=W 1=E 2=N 3=S;v=0 为北。
    /// 返回该边第 t∈[0,1] 处的经纬(UV 在瓦片 Rectangle 内线性,与
    /// sampleHeightRenderGrid 的 u/v 反算约定同一套)。
    static void edgePoint(const Rectangle& b, int edge, double t,
                          double& lonRad, double& latRad) {
        switch (edge) {
            case 0: lonRad = b.west();  latRad = b.north() - t * b.height(); break;
            case 1: lonRad = b.east();  latRad = b.north() - t * b.height(); break;
            case 2: latRad = b.north(); lonRad = b.west() + t * b.width();  break;
            default: latRad = b.south(); lonRad = b.west() + t * b.width();  break;
        }
    }

    static Result measure(const TilePlan& plan) {
        Result out;
        for (const TileEdgeSnapRecord& rec : plan.edgeSnapRecords) {
            if (!rec.tile) continue;
            const DecodedHeightmap* ownHm =
                rec.tile->content.renderContent.retainedHeightmap();
            const int ownGrid = terrainGridSizeForSse(
                rec.tile->selectionFrameState.screenSpaceError);
            const float ownFade = terrainReliefFade(rec.tile->key.z);
            for (int edge = 0; edge < TileEdgeSnapRecord::kEdgeCount; ++edge) {
                const int lg = rec.edgeLog2[edge];
                const TileRenderEntry* nbr = rec.neighbor[edge];
                if (lg <= 0 || !nbr) continue;
                const HeightSource ns = sourceOf(*nbr);
                if (!ownHm || !ownHm->valid() || !ns.valid()) {
                    ++out.skippedEdges;
                    continue;
                }
                Stats& st = (std::fabs(ownFade - ns.fade) > 1e-3f)
                                ? out.fadeDiffer
                                : out.fadeUniform;
                ++st.edges;
                // 吸附节点:自栅格上每 2^lg 一个 → 共 gridN/2^lg + 1 个。
                const int step = 1 << lg;
                const int nodes = std::max(1, ownGrid / step) + 1;
                const int stride = std::max(1, (nodes - 1) / kMaxNodesPerEdge + 1);
                for (int j = 0; j < nodes; j += stride) {
                    const double t =
                        static_cast<double>(j * step) / ownGrid;
                    double lon = 0.0, lat = 0.0;
                    edgePoint(rec.tile->bounds, edge, std::min(t, 1.0), lon, lat);
                    // 细侧:吸附覆盖 morph,节点处取的就是自纹理该纹素。
                    const float own =
                        DecodedHeightmapSampler::sampleHeightRenderGrid(
                            *ownHm, rec.tile->bounds, lon, lat, ownGrid) *
                        ownFade;
                    const float other = renderedHeight(ns, lon, lat);
                    st.add(std::fabs(own - other));
                }
            }
        }
        return out;
    }
};

} // namespace earth_engine
