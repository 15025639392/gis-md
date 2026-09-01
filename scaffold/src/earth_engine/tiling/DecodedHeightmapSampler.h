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
    /// 格上 sampleBilinear(节点uv),格内严格按 GPU 固定的 a-c-b / b-c-d
    /// 两三角形分段线性插值。四节点双线性 patch 不是同一个表面。
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

    /// Same grid interpolation after each node has passed through the RG16
    /// height-texture encode/decode used by TerrainDisplacementTemplatePool.
    static float sampleHeightRenderGridQuantized(
        const DecodedHeightmap& heightmap,
        const Rectangle& sourceBounds,
        double longitudeRadians,
        double latitudeRadians,
        int renderGridSize,
        float minHeight,
        float heightRange);

    /// GPU-exact variant when RG16 encoding and shader decode use different
    /// meter ranges. Terrain textures are encoded with the unfaded DEM range,
    /// while `u_heightDisplace.xy` decodes the same code through an
    /// already-faded range.
    static float sampleHeightRenderGridQuantizedDecoded(
        const DecodedHeightmap& heightmap,
        const Rectangle& sourceBounds,
        double longitudeRadians,
        double latitudeRadians,
        int renderGridSize,
        float encodeMinHeight,
        float encodeHeightRange,
        float decodeMinHeight,
        float decodeHeightRange);

    /// GPU/baked geomorph surface. The shader first computes a bilinear
    /// coarse height at every fine-grid vertex, mixes coarse/fine at that
    /// vertex, then rasterizes the fixed fine-grid triangles. Sampling a
    /// separate half-density triangle surface is not equivalent.
    static float sampleHeightRenderGridMorphed(
        const DecodedHeightmap& heightmap,
        const Rectangle& sourceBounds,
        double longitudeRadians,
        double latitudeRadians,
        int renderGridSize,
        float morph);

    /// Baked XYZ-WebMercator mesh contract: geometry rows are distributed
    /// linearly in geodetic latitude, but each vertex samples the DEM at that
    /// row's projected WebMercator v before triangle rasterization.
    static float sampleHeightRenderGridMorphedWebMercatorV(
        const DecodedHeightmap& heightmap,
        const Rectangle& sourceBounds,
        double longitudeRadians,
        double latitudeRadians,
        int renderGridSize,
        float morph);

    static float sampleHeightRenderGridQuantizedDecodedMorphed(
        const DecodedHeightmap& heightmap,
        const Rectangle& sourceBounds,
        double longitudeRadians,
        double latitudeRadians,
        int renderGridSize,
        float morph,
        float encodeMinHeight,
        float encodeHeightRange,
        float decodeMinHeight,
        float decodeHeightRange);
};

} // namespace earth_engine
