#pragma once

#include "TerrainDisplacementTemplatePool.h"

namespace earth_engine {

/// 「这块地形瓦片走不走共享位移模板」的**唯一判据**。
///
/// 摘 glTF 之前这个判据被抄了三份：内容层(造不造网格)、prepare 侧(建不建
/// per-tile VBO)、draw 侧(换不换模板 VBO)。三份必须完全一致，任何一处漂移
/// 都会落进两个静默故障之一：
///   造了不画 = 白烧解码 CPU + 白占内存(没有报错，只有 CpuAcct 读数变胖)；
///   不造要画 = 该瓦片空白(没有报错，只有地上一块洞)。
/// 两种失效都不会抛异常、不会红测试 —— 正因如此判据必须收在一处，而不是
/// 靠三处注释互相叮嘱「必须与 XXX 同源」。
///
/// 拆成两级是因为三个调用点掌握的信息不同：
///   - 内容层只有 (模板池活跃, z)，此时瓦片还没有 renderContent；
///   - prepare/draw 还额外知道内容种类与高度图是否到位。
/// 后者恒蕴含前者：`forLoadedTile` 为真 ⇒ `byZoomAndPool` 为真。
struct TerrainTemplateEligibility {
    /// 内容层判据:模板池活跃 + 该 LOD 有起伏(fade>0)。
    ///
    /// fade≈0 的粗瓦片**故意**留在 baked 路径:纯把高度缩放到 0 仍然绑的是
    /// 粗模板，地平线极掠视下会留下 limb faceting 微纹(实测过)。这不是遗漏，
    /// 是取舍 —— 要统一到模板路，得先解决粗模板在 limb 上的 facet，不能只是
    /// 把这里的阈值删掉。
    static bool byZoomAndPool(bool sharedTemplatePoolActive, int z) {
        return sharedTemplatePoolActive && terrainReliefFade(z) > kFadeEpsilon;
    }

    /// prepare/draw 判据:在上面之外，还要求这瓦片是**真实地形**且有自有高度图。
    /// 上采样瓦片(无自有高度图)的顶点里已经含父级真实高度，绝不能绑平模板 ——
    /// 那会让深近景塌回平椭球。
    static bool forLoadedTile(bool sharedTemplatePoolActive,
                              int z,
                              bool isRealTerrainContent,
                              bool hasValidOwnHeightmap) {
        return byZoomAndPool(sharedTemplatePoolActive, z) &&
               isRealTerrainContent && hasValidOwnHeightmap;
    }

    static constexpr float kFadeEpsilon = 0.001f;
};

} // namespace earth_engine
