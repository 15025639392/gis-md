#pragma once

#include "DecodedHeightmapSampler.h"
#include "TerrainDisplacementTemplatePool.h"
#include "TilePlan.h"
#include "TilesetTile.h"
#include "../core/math/Rectangle.h"

#include <algorithm>

namespace earth_engine {

/// 「一个渲染条目在本帧**实际渲染出来的**地形高度」—— 接边相关两个消费者的
/// 共用真值源:
///   - `TileEdgeMismatchProbe`  量接边错位有多大(判据);
///   - `TerrainEdgeHeightLut`   把邻居真值喂给边吸附(①-1 的修法)。
///
/// 抽出来是因为这两者**必须逐位一致**:量的和修的若各写一份,修完指标归零
/// 也只能说明两份代码互相同意,证不了引擎的接边对齐了。
namespace terrain_edge {

/// 一个渲染条目在本帧实际用于位移的高度来源。remap 条目画的是祖先的数据
/// (morph 钉 1,模板恒 coarse 档),常规条目画自己的。
struct HeightSource {
    const DecodedHeightmap* heightmap = nullptr;
    const Rectangle* bounds = nullptr;
    int gridSize = 0;
    float morph = 1.0f;
    /// 起伏淡入系数。高度纹理是按 {minH·fade, range·fade} 上传的,shader
    /// 解码出来的就是 fade×真高。⚠️ 不乘它,算出的既不是这一侧也不是那一侧
    /// 真正渲染的高度 —— 探针首版漏了,z6~8 段读出 max=428m 的假错位。
    float fade = 1.0f;
    bool quantizedTexture = false;
    float quantizationMinHeight = 0.0f;
    float quantizationHeightRange = 1.0f;
    /// Exact shader `mr` values. They already include relief fade because the
    /// command uploads `{minHeight * fade, heightRange * fade}`. Recovering
    /// the unfaded pair by division and multiplying decoded heights again is
    /// algebraically similar but not float32-equivalent to the GPU.
    float textureMinHeight = 0.0f;
    float textureHeightRange = 1.0f;
    /// HeightmapTerrainContentProvider bakes legacy mesh vertices by mapping
    /// geodetic latitude through WebMercator before sampling DEM rows. The
    /// displacement texture path intentionally uses its own linear tile UV
    /// and leaves this false.
    bool webMercatorHeightV = false;
    bool valid() const { return heightmap && bounds && gridSize > 0; }
};

inline HeightSource sourceOf(const TileRenderEntry& e) {
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
        // remap:模板恒 coarse 档,且 fine/coarse 同源同值 → morph 钉 1。
        s.gridSize = kTerrainDisplacementGridSize;
        s.morph = 1.0f;
    } else {
        // 档位读每帧唯一决策(refresher 盖章),与 draw 实际渲染档同一份
        // —— 无迟滞重推会在迟滞带内量到/喂进"没在画的那一档"的高度。
        s.gridSize = decidedOrPredictGridSize(
            tile->selectionFrameState.displacementGridSize,
            tile->selectionFrameState.screenSpaceError);
        s.morph = tile->selectionFrameState.terrainMorphFactor;
    }
    return s;
}

/// 该来源在给定经纬处渲染出来的高度 = fade · mix(coarse, fine, morph)。
/// coarse 档 = 偶数格点晶格,恰好等于半密度栅格上的同一采样(节点 i=2j 在
/// gridN 栅格上的 u = j/(gridN/2)),故复用同一个函数换档位传参即可。
inline float renderedHeight(const HeightSource& s, double lonRad,
                            double latRad) {
    if (s.quantizedTexture) {
        return DecodedHeightmapSampler::
            sampleHeightRenderGridQuantizedDecodedMorphed(
                *s.heightmap, *s.bounds, lonRad, latRad, s.gridSize,
                s.morph, s.quantizationMinHeight,
                s.quantizationHeightRange, s.textureMinHeight,
                s.textureHeightRange);
    }
    const float height = s.webMercatorHeightV
        ? DecodedHeightmapSampler::sampleHeightRenderGridMorphedWebMercatorV(
              *s.heightmap, *s.bounds, lonRad, latRad, s.gridSize, s.morph)
        : DecodedHeightmapSampler::sampleHeightRenderGridMorphed(
              *s.heightmap, *s.bounds, lonRad, latRad, s.gridSize, s.morph);
    return height * s.fade;
}

/// 边 index 与 shader 打包序一致:0=W 1=E 2=N 3=S;v=0 为北。
/// 返回该边第 t∈[0,1] 处的经纬(UV 在瓦片 Rectangle 内线性,与
/// sampleHeightRenderGrid 的 u/v 反算约定同一套)。
inline void edgePoint(const Rectangle& b, int edge, double t, double& lonRad,
                      double& latRad) {
    switch (edge) {
        case 0: lonRad = b.west();  latRad = b.north() - t * b.height(); break;
        case 1: lonRad = b.east();  latRad = b.north() - t * b.height(); break;
        case 2: latRad = b.north(); lonRad = b.west() + t * b.width();  break;
        default: latRad = b.south(); lonRad = b.west() + t * b.width();  break;
    }
}

/// 吸附边上的节点数:自栅格每 2^log2Step 一个 → gridN/2^log2Step + 1 个。
/// shader 的 `a0 = floor(a/snapStep)*snapStep`、`a1 = min(a0+snapStep, gridN)`
/// 取的正是这些位置,节点序号 j = a/snapStep。
inline int edgeNodeCount(int ownGridSize, int log2Step) {
    if (log2Step <= 0 || ownGridSize <= 0) return 0;
    return std::max(1, ownGridSize >> log2Step) + 1;
}

} // namespace terrain_edge
} // namespace earth_engine
