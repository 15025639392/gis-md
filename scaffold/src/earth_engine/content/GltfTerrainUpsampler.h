#pragma once

#include "GltfModel.h"
#include "../tiling/TileID.h"

#include <array>
#include <memory>

namespace earth_engine {

// 存活条件(2026-08-05 复核,见 contracts::Gate::ImageryDrivenUpsample):
// 本上采样器只有唯一消费者 TileGltfTerrainUpsampledChildMaterializer,后者只在
// 瓦片被标为 upsample 时才跑,而标记只有两个来源:
//   ① RasterDetail(TileChildMaterializer::materializeRasterUpsampledChildren)
//      —— 影像 isMoreDetailAvailable 驱动几何细分。仅
//      decoupleImageryFromGeometry=false 时存在(TileRasterUpsampledChild-
//      Coordinator 在 true 时直接早退)。生产默认 true,故这条路默认关闭。
//   ② TerrainAvailability(TileChildMaterializer::materializeTerrainChildren)
//      —— 覆盖边缘上「兄弟中有人 Available、自己 NotAvailable」的洞瓦片,从已
//      覆盖的父级裁剪出真地形而非拍平成椭球(见 CompositeTerrainProvider::
//      isPureHoleQuad 与 test_composite_terrain_provider 的 CoverageEdge 用例)。
//      这条与 decouple 无关,但要求内容 provider 的 availability 是**空间性**的
//      (同层兄弟可以不同)。当前唯一的地形 provider(Heightmap[+Ellipsoid 兜底])
//      的 availability 只看 key.z,四个兄弟恒同答案,故运行时不可达。
// 结论:接一个带真实覆盖索引的 partial 数据源(layer.json availability /
// 稀疏 DEM 覆盖掩码),或把 decoupleImageryFromGeometry 关回 false,本文件立刻
// 复活。改动上述任一条件时请同步更新此注释与 contracts 的 Gate 登记。

// Child window inside the parent's texcoord space ([0,1] x [0,1]). Values are
// raw UV coordinates: for inverted-V texcoord sets the caller must flip the
// projected window into UV space before passing it here (v0 < v1 always).
struct GltfUpsampleUvWindow {
    double u0 = 0.0;
    double v0 = 0.0;
    double u1 = 1.0;
    double v1 = 1.0;
};

// Where the upsampled child sits inside each of the parent's texcoord spaces.
// texCoordSets is index-aligned with rasterOverlayDetails projections; sets at
// or beyond texCoordSetCount keep their parent values untouched. nativeUv
// covers SurfaceVertex::uv (linear geographic, south-up, never inverted).
struct GltfUpsampleWindows {
    std::array<GltfUpsampleUvWindow, kGltfMaxTexCoordSets> texCoordSets{};
    size_t texCoordSetCount = 0;
    GltfUpsampleUvWindow nativeUv{};
};

class GltfTerrainUpsampler {
public:
    // Clips the parent mesh to the child window of the textureCoordinateIndex
    // set and renormalizes every windowed texcoord set (plus the native uv) to
    // span [0,1] over the child. This matches cesium-native semantics, which
    // removes overlay texcoords during upsampling and regenerates them against
    // the child's own rectangle (RasterOverlayUtilities.cpp: "it will be
    // generated later"): because texcoords are linear in their projected
    // space, renormalizing the kept subrange is mathematically equivalent to
    // regenerating per vertex. Callers must pair the windows with child
    // rasterOverlayDetails rectangles derived by the same mapping so that the
    // engine invariant "UV [0,1] spans details.rasterOverlayRectangles[set]"
    // keeps holding for upsampled content.
    //
    // When windows is null, exact half-split quadrant windows are assumed
    // (TMS parity: odd x -> east, odd y -> north), which is correct for
    // linear-projection texcoords on TMS schemes only.
    static std::unique_ptr<GltfModel> upsampleForRasterOverlay(
        const GltfModel& parentModel,
        const UpsampledQuadtreeNode& childID,
        int textureCoordinateIndex = 0,
        bool hasInvertedVCoordinate = false,
        const GltfUpsampleWindows* windows = nullptr);

    // Exact half-split window for a quadrant child assuming TMS parity
    // (odd x -> east, odd y -> north). Used as fallback when no projected
    // rectangles are available to derive the real window.
    static GltfUpsampleUvWindow quadrantUvWindow(
        const UpsampledQuadtreeNode& childID,
        bool hasInvertedVCoordinate);
};

} // namespace earth_engine
