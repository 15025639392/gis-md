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
    /// 渲染面不是全分辨率 heightmap:网格节点 = min(tileSize-1, renderGridSize)
    /// 格上 sampleBilinear(节点uv),节点间线性插值(GPU 位移与 CPU baked 同)。
    /// 全分辨率采样与该面的格内起伏差是**结构性**的(重钳不消除),贴地
    /// 几何必须按同一分段线性面取高,offset 只需覆盖数值误差。
    ///
    /// renderGridSize = 该瓦片**本帧实际使用的位移模板档位**(自适应密度后
    /// 不再恒 64;调用方从瓦片常驻 draw 命令的 terrainHeightGridSize 读真值)。
    /// 0 = 未知 → 退回 coarse 档。
    static float sampleHeightRenderGrid(const DecodedHeightmap& heightmap,
                                        const Rectangle& sourceBounds,
                                        double longitudeRadians,
                                        double latitudeRadians,
                                        int renderGridSize);
};

} // namespace earth_engine
