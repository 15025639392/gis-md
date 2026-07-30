#pragma once

namespace earth_engine {

class Rectangle;
struct DecodedHeightmap;

class DecodedHeightmapSampler {
public:
    static float sampleHeight(const DecodedHeightmap& heightmap,
                              const Rectangle& sourceBounds,
                              double longitudeRadians,
                              double latitudeRadians);

    /// 与渲染网格一致的分段线性采样(矢量贴地 P3)。
    ///
    /// 渲染面不是全分辨率 heightmap:网格节点 = min(tileSize-1, 64) 格上
    /// sampleBilinear(节点uv),节点间线性插值(GPU 位移与 CPU baked 同)。
    /// 全分辨率采样与该面的格内起伏差是**结构性**的(重钳不消除),贴地
    /// 几何必须按同一分段线性面取高,offset 只需覆盖数值误差。
    static float sampleHeightRenderGrid(const DecodedHeightmap& heightmap,
                                        const Rectangle& sourceBounds,
                                        double longitudeRadians,
                                        double latitudeRadians);
};

} // namespace earth_engine
