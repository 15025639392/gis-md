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
    const SurfaceVertex& vertex) {
    std::optional<Cartographic> cartographic =
        Ellipsoid::WGS84().tryCartesianToCartographic(vertex.positionEcef);
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
    double longitudeRadians,
    double latitudeRadians) {
    for (const GltfPrimitive& primitive : model.primitives) {
        if (primitive.primitiveMode != GltfPrimitiveMode::Triangles) {
            continue;
        }
        const std::vector<SurfaceVertex>& vertices = primitive.vertices;
        if (vertices.empty()) {
            continue;
        }

        auto sampleIndexedTriangle =
            [&](uint32_t ia,
                uint32_t ib,
                uint32_t ic) -> std::optional<float> {
            if (ia >= vertices.size() || ib >= vertices.size() ||
                ic >= vertices.size()) {
                return std::nullopt;
            }
            std::optional<CartographicVertex> a =
                cartographicVertex(vertices[ia]);
            std::optional<CartographicVertex> b =
                cartographicVertex(vertices[ib]);
            std::optional<CartographicVertex> c =
                cartographicVertex(vertices[ic]);
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

        if (!primitive.indices.empty()) {
            for (size_t i = 0; i + 2 < primitive.indices.size(); i += 3) {
                std::optional<float> height = sampleIndexedTriangle(
                    primitive.indices[i],
                    primitive.indices[i + 1],
                    primitive.indices[i + 2]);
                if (height) {
                    return height;
                }
            }
        } else {
            for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
                std::optional<float> height = sampleIndexedTriangle(
                    static_cast<uint32_t>(i),
                    static_cast<uint32_t>(i + 1),
                    static_cast<uint32_t>(i + 2));
                if (height) {
                    return height;
                }
            }
        }
    }

    return std::nullopt;
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
        bool useTerrainCache) {
    std::optional<LoadedTerrainSample> bestSample;

    for (const auto& [cacheKey, tile] : tiles) {
        if (!tile ||
            (bestSample && tile->key.z < bestSample->zoom) ||
            !tile->bounds.contains(longitudeRadians, latitudeRadians)) {
            continue;
        }

        auto terrainIt = useTerrainCache
            ? terrainCache.find(cacheKey)
            : terrainCache.end();
        if (useTerrainCache && terrainIt != terrainCache.end() &&
            terrainIt->second && terrainIt->second->valid()) {
            bestSample = LoadedTerrainSample{
                DecodedHeightmapSampler::sampleHeight(
                    *terrainIt->second,
                    tile->bounds,
                    longitudeRadians,
                    latitudeRadians),
                tile->key.z};
            continue;
        }

        const TileRenderContentState& renderContent =
            tile->content.renderContent;
        if (renderContent.isTerrainRenderContent()) {
            const GltfModel* gltf = renderContent.gltfModelForRead();
            if (gltf) {
                std::optional<float> gltfHeight = sampleGltfTerrainHeight(
                    *gltf,
                    longitudeRadians,
                    latitudeRadians);
                if (gltfHeight) {
                    bestSample = LoadedTerrainSample{*gltfHeight, tile->key.z};
                }
            }
        }
    }

    return bestSample ? bestSample->height : 0.0f;
}

} // namespace earth_engine
