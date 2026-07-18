#pragma once

#include "../core/geodesy/BoundingRegionBuilder.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <vector>

namespace earth_engine {

using TileAvailabilityRect = std::array<uint32_t, 4>;
using QuantizedMeshAvailabilityRange = std::array<uint32_t, 5>;

enum class RasterOverlayProjection {
    Geographic = 0,
    WebMercator = 1
};

struct SurfaceVertex {
    Vec3 positionEcef;
    Vec3 positionHighEcef;
    Vec3 positionLowEcef;
    Vec3 normalEcef;
    std::array<float, 2> uv = {0.0f, 0.0f};
    // geomorph 距离连续 morph 起点偏移（米，沿椭球法线）:= 本瓦片 2×自降采样
    // 高度（osgEarth 邻居平均：偶数格点=self、奇数格点=相邻偶数格双线性）− 真实
    // 高度。regular-grid worker 侧一次算好，复制进 TerrainGpuVertex.heightDelta，
    // shader 做 pos += up·heightDelta·(1−morph)。0 = 无 morph（椭球代理/非规则栅格）。
    float geomorphHeightDelta = 0.0f;
};

struct SurfaceGpuVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
};

/// cesium-native style skirt metadata (see CesiumGltfContent/SkirtMeshMetadata.h).
/// Tracks the vertex/index ranges that belong to the real terrain surface
/// (not the skirt), so downstream operations (e.g. normal map, UV generation)
/// can skip skirt geometry.
struct SkirtMetadata {
    uint32_t noSkirtIndicesBegin = 0;
    uint32_t noSkirtIndicesCount = 0;
    uint32_t noSkirtVerticesBegin = 0;
    uint32_t noSkirtVerticesCount = 0;
    Vec3 meshCenter = Vec3::zero();
    double skirtWestHeight = 0.0;
    double skirtSouthHeight = 0.0;
    double skirtEastHeight = 0.0;
    double skirtNorthHeight = 0.0;
};

/// Water mask from QuantizedMesh extension ID=2.
/// If both allLand and allWater are false, data is a 256x256 R8 bitmap.
/// Older legacy surface fixtures may still provide RGBA8 data.
struct WaterMask {
    std::vector<uint8_t> data;  // 256x256 R8 preferred, empty = no mask
    bool allLand = true;
    bool allWater = false;
    double translationX = 0.0;
    double translationY = 0.0;
    double scale = 1.0;
    bool valid() const { return !data.empty() || allWater; }
};

/// cesium-native RasterOverlayDetails equivalent.
/// Stores the set of projections and their corresponding texture coordinate
/// rectangles for a geometry tile. The current rectangle overlay provider
/// exposes Geographic projection details.
struct RasterOverlayDetails {
    /// Projections for which texture coordinates exist.
    std::vector<RasterOverlayProjection> rasterOverlayProjections;

    /// Texture coordinate rectangles for each projection.
    /// Index-aligned with rasterOverlayProjections.
    std::vector<Rectangle> rasterOverlayRectangles;

    /// Whether each projection's V texture coordinate is inverted.
    /// Index-aligned with rasterOverlayProjections.
    /// 约定：false = NW-based（v=0 在矩形北缘，同 glTF 图像行序 row0=北；
    /// QM 原生 uv 亦已在 QuantizedMeshParser 解码时翻成 NW）；
    /// true = south-based（v=0 在南缘）。窗口换算
    /// （TileSurface::textureWindowForNorthWestUv）、上采样切分与
    /// 祖先裁剪窗全部依赖该约定，勿单独改任一侧。
    std::vector<bool> rasterOverlayInvertedVCoordinates;

    /// cesium-native RasterOverlayDetails::boundingRegion equivalent.
    /// This is the precise content region covered by the generated overlay
    /// texture coordinates, in geographic radians/meters.
    BoundingRegionBuilder::BoundingRegion boundingRegion{
        Rectangle::EMPTY,
        1.0,
        -1.0};

    bool empty() const { return rasterOverlayRectangles.empty(); }

    bool equalsExact(const RasterOverlayDetails& other) const {
        return rasterOverlayProjections == other.rasterOverlayProjections &&
               rasterOverlayRectangles == other.rasterOverlayRectangles &&
               rasterOverlayInvertedVCoordinates ==
                   other.rasterOverlayInvertedVCoordinates &&
               boundingRegion.rectangle == other.boundingRegion.rectangle &&
               boundingRegion.minimumHeight ==
                   other.boundingRegion.minimumHeight &&
               boundingRegion.maximumHeight ==
                   other.boundingRegion.maximumHeight;
    }

    const Rectangle* findRectangleForOverlayProjection(
        RasterOverlayProjection projection) const {
        for (size_t i = 0; i < rasterOverlayProjections.size(); ++i) {
            if (rasterOverlayProjections[i] == projection &&
                i < rasterOverlayRectangles.size()) {
                return &rasterOverlayRectangles[i];
            }
        }
        return nullptr;
    }

    int32_t textureCoordinateIDForProjection(
        RasterOverlayProjection projection) const {
        for (size_t i = 0; i < rasterOverlayProjections.size(); ++i) {
            if (rasterOverlayProjections[i] == projection &&
                i < rasterOverlayRectangles.size()) {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    }

    bool hasInvertedVCoordinateForProjection(
        RasterOverlayProjection projection) const {
        const int32_t textureCoordinateID =
            textureCoordinateIDForProjection(projection);
        return textureCoordinateID >= 0 &&
               static_cast<size_t>(textureCoordinateID) <
                   rasterOverlayInvertedVCoordinates.size() &&
               rasterOverlayInvertedVCoordinates[
                   static_cast<size_t>(textureCoordinateID)];
    }

    void setGeographicRectangle(const Rectangle& rectangle,
                                double minimumHeight = 0.0,
                                double maximumHeight = 0.0) {
        rasterOverlayProjections = {RasterOverlayProjection::Geographic};
        rasterOverlayRectangles = {rectangle};
        rasterOverlayInvertedVCoordinates = {false};
        boundingRegion = {rectangle, minimumHeight, maximumHeight};
    }

    void merge(const RasterOverlayDetails& other) {
        while (rasterOverlayRectangles.size() <
               rasterOverlayProjections.size()) {
            rasterOverlayRectangles.push_back(Rectangle::EMPTY);
        }
        while (rasterOverlayInvertedVCoordinates.size() <
               rasterOverlayProjections.size()) {
            rasterOverlayInvertedVCoordinates.push_back(false);
        }
        for (size_t i = 0; i < other.rasterOverlayProjections.size(); ++i) {
            rasterOverlayProjections.push_back(
                other.rasterOverlayProjections[i]);
            rasterOverlayRectangles.push_back(
                i < other.rasterOverlayRectangles.size()
                    ? other.rasterOverlayRectangles[i]
                    : Rectangle::EMPTY);
            rasterOverlayInvertedVCoordinates.push_back(
                i < other.rasterOverlayInvertedVCoordinates.size()
                    ? other.rasterOverlayInvertedVCoordinates[i]
                    : false);
        }
        const bool hasRegion = boundingRegion.minimumHeight <=
                               boundingRegion.maximumHeight;
        const bool otherHasRegion = other.boundingRegion.minimumHeight <=
                                    other.boundingRegion.maximumHeight;
        if (hasRegion && otherHasRegion) {
            BoundingRegionBuilder builder;
            builder.expandToIncludeRegion(boundingRegion);
            builder.expandToIncludeRegion(other.boundingRegion);
            boundingRegion = builder.toRegion();
        } else if (otherHasRegion) {
            boundingRegion = other.boundingRegion;
        }
    }
};

} // namespace earth_engine
