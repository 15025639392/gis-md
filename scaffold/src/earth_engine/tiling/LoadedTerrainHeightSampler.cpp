#include "LoadedTerrainHeightSampler.h"

#include "DecodedHeightmapSampler.h"
#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

#include "../content/GltfModel.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../providers/TerrainProvider.h"

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace earth_engine {

namespace {

struct LoadedTerrainSample {
    float height = 0.0f;
    int zoom = -1;
};

struct CartographicVertex {
    double longitude = 0.0;
    double latitude = 0.0;
    double height = 0.0;
};

std::optional<CartographicVertex> cartographicVertex(
    const SurfaceVertex& vertex,
    const Mat4& transform) {
    std::optional<Cartographic> cartographic =
        Ellipsoid::WGS84().tryCartesianToCartographic(
            transform * vertex.positionEcef);
    if (!cartographic) {
        return std::nullopt;
    }
    return CartographicVertex{
        cartographic->longitude(),
        cartographic->latitude(),
        cartographic->height()};
}

std::optional<float> sampleTriangleHeight(
    const CartographicVertex& a,
    const CartographicVertex& b,
    const CartographicVertex& c,
    double longitudeRadians,
    double latitudeRadians) {
    const double v0x = b.longitude - a.longitude;
    const double v0y = b.latitude - a.latitude;
    const double v1x = c.longitude - a.longitude;
    const double v1y = c.latitude - a.latitude;
    const double v2x = longitudeRadians - a.longitude;
    const double v2y = latitudeRadians - a.latitude;
    const double denom = v0x * v1y - v1x * v0y;
    if (std::abs(denom) <= std::numeric_limits<double>::epsilon()) {
        return std::nullopt;
    }

    const double invDenom = 1.0 / denom;
    const double u = (v2x * v1y - v1x * v2y) * invDenom;
    const double v = (v0x * v2y - v2x * v0y) * invDenom;
    constexpr double kBarycentricEpsilon = 1e-10;
    if (u < -kBarycentricEpsilon || v < -kBarycentricEpsilon ||
        u + v > 1.0 + kBarycentricEpsilon) {
        return std::nullopt;
    }

    const double w = 1.0 - u - v;
    return static_cast<float>(
        a.height * w +
        b.height * u +
        c.height * v);
}

std::optional<float> sampleGltfTerrainHeight(
    const GltfModel& model,
    const Mat4& contentTransform,
    double longitudeRadians,
    double latitudeRadians) {
    std::optional<float> highestHeight;
    for (const GltfPrimitive& primitive : model.primitives) {
        const std::vector<SurfaceVertex>& vertices = primitive.vertices;
        if (vertices.empty()) {
            continue;
        }

        auto sampleIndexedTriangle =
            [&](uint32_t ia,
                uint32_t ib,
                uint32_t ic,
                const Mat4& transform) -> std::optional<float> {
            if (ia >= vertices.size() || ib >= vertices.size() ||
                ic >= vertices.size()) {
                return std::nullopt;
            }
            std::optional<CartographicVertex> a =
                cartographicVertex(vertices[ia], transform);
            std::optional<CartographicVertex> b =
                cartographicVertex(vertices[ib], transform);
            std::optional<CartographicVertex> c =
                cartographicVertex(vertices[ic], transform);
            if (!a || !b || !c) {
                return std::nullopt;
            }
            return sampleTriangleHeight(
                *a,
                *b,
                *c,
                longitudeRadians,
                latitudeRadians);
        };

        auto sourceIndex = [&](size_t i) -> std::optional<uint32_t> {
            const uint32_t index = primitive.indices.empty()
                ? static_cast<uint32_t>(i)
                : primitive.indices[i];
            return index < vertices.size()
                ? std::optional<uint32_t>(index)
                : std::nullopt;
        };

        auto samplePrimitiveTriangle =
            [&](size_t a,
                size_t b,
                size_t c,
                const Mat4& transform) -> std::optional<float> {
            std::optional<uint32_t> ia = sourceIndex(a);
            std::optional<uint32_t> ib = sourceIndex(b);
            std::optional<uint32_t> ic = sourceIndex(c);
            if (!ia || !ib || !ic) {
                return std::nullopt;
            }
            return sampleIndexedTriangle(*ia, *ib, *ic, transform);
        };

        std::vector<Mat4> transforms;
        if (primitive.instances.empty()) {
            transforms.push_back(contentTransform);
        } else {
            transforms.reserve(primitive.instances.size());
            for (const GltfInstance& instance : primitive.instances) {
                transforms.push_back(contentTransform * instance.transform);
            }
        }

        const size_t indexCount =
            primitive.indices.empty() ? vertices.size()
                                      : primitive.indices.size();
        for (const Mat4& transform : transforms) {
            if (primitive.primitiveMode == GltfPrimitiveMode::Triangles) {
                for (size_t i = 0; i + 2 < indexCount; i += 3) {
                    std::optional<float> height =
                        samplePrimitiveTriangle(i, i + 1, i + 2, transform);
                    if (height && (!highestHeight ||
                                   *height > *highestHeight)) {
                        highestHeight = height;
                    }
                }
            } else if (primitive.primitiveMode ==
                       GltfPrimitiveMode::TriangleStrip) {
                for (size_t i = 0; i + 2 < indexCount; ++i) {
                    const bool odd = (i % 2u) == 1u;
                    std::optional<float> height = odd
                        ? samplePrimitiveTriangle(
                              i + 1,
                              i,
                              i + 2,
                              transform)
                        : samplePrimitiveTriangle(
                              i,
                              i + 1,
                              i + 2,
                              transform);
                    if (height && (!highestHeight ||
                                   *height > *highestHeight)) {
                        highestHeight = height;
                    }
                }
            } else if (primitive.primitiveMode ==
                       GltfPrimitiveMode::TriangleFan) {
                for (size_t i = 1; i + 1 < indexCount; ++i) {
                    std::optional<float> height =
                        samplePrimitiveTriangle(0, i, i + 1, transform);
                    if (height && (!highestHeight ||
                                   *height > *highestHeight)) {
                        highestHeight = height;
                    }
                }
            }
        }
    }

    return highestHeight;
}

void commitBestSample(std::optional<LoadedTerrainSample>& bestSample,
                      LoadedTerrainSample candidate) {
    if (!bestSample ||
        candidate.zoom > bestSample->zoom ||
        (candidate.zoom == bestSample->zoom &&
         candidate.height > bestSample->height)) {
        bestSample = candidate;
    }
}

} // namespace

float LoadedTerrainHeightSampler::sampleHeight(
    const std::unordered_map<
        std::string,
        std::unique_ptr<TilesetTile>>& tiles,
        const std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        double longitudeRadians,
        double latitudeRadians,
        bool includeLegacyHeightmapTerrainCache) {
    std::optional<LoadedTerrainSample> bestSample;

    for (const auto& [cacheKey, tile] : tiles) {
        if (!tile ||
            (bestSample && tile->key.z < bestSample->zoom) ||
            !tile->bounds.contains(longitudeRadians, latitudeRadians)) {
            continue;
        }

        const TileRenderContentState& renderContent =
            tile->content.renderContent;
        if (renderContent.isTerrainRenderContent()) {
            const GltfModel* gltf = renderContent.gltfModelForRead();
            if (gltf) {
                std::optional<float> gltfHeight = sampleGltfTerrainHeight(
                    *gltf,
                    renderContent.gltfTransform(),
                    longitudeRadians,
                    latitudeRadians);
                if (gltfHeight) {
                    commitBestSample(
                        bestSample,
                        LoadedTerrainSample{*gltfHeight, tile->key.z});
                    continue;
                }
            }
        }

        auto terrainIt = includeLegacyHeightmapTerrainCache
            ? terrainCache.find(cacheKey)
            : terrainCache.end();
        if (includeLegacyHeightmapTerrainCache &&
            terrainIt != terrainCache.end() &&
            terrainIt->second && terrainIt->second->valid()) {
            commitBestSample(bestSample, LoadedTerrainSample{
                DecodedHeightmapSampler::sampleHeight(
                    *terrainIt->second,
                    tile->bounds,
                    longitudeRadians,
                    latitudeRadians),
                tile->key.z});
        }
    }

    return bestSample ? bestSample->height : 0.0f;
}

} // namespace earth_engine
