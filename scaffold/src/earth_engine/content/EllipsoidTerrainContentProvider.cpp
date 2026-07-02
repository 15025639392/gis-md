#include "EllipsoidTerrainContentProvider.h"

#include "GltfModel.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/geodesy/QuadtreeGeometricError.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "../tiling/TileBoundingVolume.h"
#include "../tiling/TileScheme.h"
#include "../tiling/TileSelectionRootPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

constexpr double kEllipsoidTerrainMinimumHeight = 0.0;
constexpr double kEllipsoidTerrainMaximumHeight = 0.0;

std::unique_ptr<TileScheme> createScheme(const std::string& schemeId) {
    if (schemeId == "Geographic-TMS") {
        return TileScheme::createGeographicTMS();
    }
    return TileScheme::createXYZWebMercator();
}

RasterOverlayProjection projectionForScheme(const std::string& schemeId) {
    return schemeId == "XYZ-WebMercator"
        ? RasterOverlayProjection::WebMercator
        : RasterOverlayProjection::Geographic;
}

Rectangle rootRectangleForScheme(const std::string& schemeId) {
    return schemeId == "XYZ-WebMercator"
        ? WebMercatorProjection::maximumGlobeRectangle()
        : Rectangle::MAXIMUM;
}

std::vector<TileKey> quadtreeChildrenForKey(const TileKey& key) {
    const int z = key.z + 1;
    const int x = key.x * 2;
    const int y = key.y * 2;
    return {
        TileKey{key.schemeId, z, x, y},
        TileKey{key.schemeId, z, x + 1, y},
        TileKey{key.schemeId, z, x, y + 1},
        TileKey{key.schemeId, z, x + 1, y + 1}};
}

std::pair<Vec3, Vec3> splitHighLow(const Vec3& value) {
    constexpr double kSplit = 65536.0;
    const auto split = [](double v) {
        const double high = std::floor(v / kSplit) * kSplit;
        return std::pair<double, double>{high, v - high};
    };
    const auto sx = split(value.x());
    const auto sy = split(value.y());
    const auto sz = split(value.z());
    return {
        Vec3(sx.first, sy.first, sz.first),
        Vec3(sx.second, sy.second, sz.second)};
}

void setLocalPosition(SurfaceVertex& vertex, const Vec3& localPosition) {
    vertex.positionEcef = localPosition;
    const auto split = splitHighLow(localPosition);
    vertex.positionHighEcef = split.first;
    vertex.positionLowEcef = split.second;
}

Rectangle projectRasterRectangle(const Rectangle& geographicRectangle,
                                 RasterOverlayProjection projection) {
    if (projection == RasterOverlayProjection::WebMercator) {
        return projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            geographicRectangle);
    }
    return geographicRectangle;
}

void rewriteProjectionTexCoords(GltfPrimitive& primitive,
                                RasterOverlayProjection projection,
                                const Rectangle& geographicRectangle) {
    primitive.vertexTexCoords[0].clear();
    primitive.vertexTexCoords[0].reserve(primitive.runtime.baseVertices.size());
    const Rectangle projectedRectangle =
        projectRasterRectangle(geographicRectangle, projection);
    const double width = projectedRectangle.width();
    const double height = projectedRectangle.height();
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    WebMercatorProjection webMercator(ellipsoid);
    for (const SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        const std::optional<Cartographic> cartographic =
            ellipsoid.tryCartesianToCartographic(vertex.positionEcef);
        if (!cartographic || width <= 0.0 || height <= 0.0) {
            primitive.vertexTexCoords[0].push_back({0.0f, 0.0f});
            continue;
        }
        const Vec3 projected = projection == RasterOverlayProjection::WebMercator
            ? projectPosition(webMercator, *cartographic)
            : Vec3(
                  cartographic->longitude(),
                  cartographic->latitude(),
                  cartographic->height());
        primitive.vertexTexCoords[0].push_back({
            static_cast<float>(std::clamp(
                (projected.x() - projectedRectangle.west()) / width,
                0.0,
                1.0)),
            static_cast<float>(std::clamp(
                (projected.y() - projectedRectangle.south()) / height,
                0.0,
                1.0))});
    }
}

RasterOverlayDetails makeRasterOverlayDetails(
    const Rectangle& geographicRectangle,
    RasterOverlayProjection projection) {
    RasterOverlayDetails details;
    details.rasterOverlayProjections = {projection};
    details.rasterOverlayRectangles = {
        projectRasterRectangle(geographicRectangle, projection)};
    details.rasterOverlayInvertedVCoordinates = {false};
    details.boundingRegion = {
        geographicRectangle,
        kEllipsoidTerrainMinimumHeight,
        kEllipsoidTerrainMaximumHeight};
    return details;
}

double mixValue(double a, double b, double t) {
    return a + (b - a) * t;
}

// Ported from the former TileSurface::buildEllipsoidMesh / vertexForUnitUv /
// setPosition. Generates a regular ellipsoid-surface grid over the geographic
// rectangle. Vertices carry absolute ECEF positions (with high/low split),
// geodetic surface normals, and unit-UV texture coordinates. Index winding
// matches the original (a, c, b, b, c, d).
void buildEllipsoidGrid(const Rectangle& tileBounds,
                        int gridSize,
                        std::vector<SurfaceVertex>& outVertices,
                        std::vector<uint32_t>& outIndices) {
    const int safeGrid = std::max(1, gridSize);
    const int n = safeGrid + 1;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();

    outVertices.clear();
    outIndices.clear();
    outVertices.reserve(static_cast<size_t>(n * n));
    outIndices.reserve(static_cast<size_t>(safeGrid * safeGrid * 6));

    for (int y = 0; y < n; ++y) {
        const double v = static_cast<double>(y) / static_cast<double>(safeGrid);
        const double clampedV = std::clamp(v, 0.0, 1.0);
        // Linear latitude sampling: north at v=0, south at v=1.
        const double lat = mixValue(tileBounds.north(), tileBounds.south(),
                                    clampedV);
        for (int x = 0; x < n; ++x) {
            const double u =
                static_cast<double>(x) / static_cast<double>(safeGrid);
            const double clampedU = std::clamp(u, 0.0, 1.0);
            const double lng = mixValue(tileBounds.west(), tileBounds.east(),
                                        clampedU);
            const Vec3 ecef = ellipsoid.cartographicToCartesian(
                Cartographic::fromRadians(lng, lat, 0.0));

            SurfaceVertex vertex;
            setLocalPosition(vertex, ecef);
            vertex.normalEcef = ellipsoid.geodeticSurfaceNormal(ecef);
            vertex.uv = {
                static_cast<float>(clampedU),
                static_cast<float>(clampedV)};
            outVertices.push_back(vertex);
        }
    }

    for (int y = 0; y < safeGrid; ++y) {
        for (int x = 0; x < safeGrid; ++x) {
            const uint32_t a = static_cast<uint32_t>(y * n + x);
            const uint32_t b = static_cast<uint32_t>(y * n + x + 1);
            const uint32_t c = static_cast<uint32_t>((y + 1) * n + x);
            const uint32_t d = static_cast<uint32_t>((y + 1) * n + x + 1);
            outIndices.push_back(a);
            outIndices.push_back(c);
            outIndices.push_back(b);
            outIndices.push_back(b);
            outIndices.push_back(c);
            outIndices.push_back(d);
        }
    }
}

std::unique_ptr<GltfModel> makeEllipsoidTerrainModel(
    const Rectangle& geographicRectangle,
    RasterOverlayProjection projection,
    int gridSize) {
    std::vector<SurfaceVertex> gridVertices;
    std::vector<uint32_t> gridIndices;
    buildEllipsoidGrid(
        geographicRectangle,
        gridSize,
        gridVertices,
        gridIndices);
    auto model = std::make_unique<GltfModel>();
    const Cartographic center = Cartographic::fromRadians(
        (geographicRectangle.west() + geographicRectangle.east()) * 0.5,
        (geographicRectangle.south() + geographicRectangle.north()) * 0.5,
        0.0);
    const Vec3 localOrigin = Ellipsoid::WGS84().cartographicToCartesian(center);
    model->preferredLocalOriginEcef = localOrigin;
    model->rasterOverlayDetails =
        makeRasterOverlayDetails(geographicRectangle, projection);

    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(localOrigin);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.baseTranslation = {
        localOrigin.x(),
        localOrigin.y(),
        localOrigin.z()};
    rootNode.translation = rootNode.baseTranslation;
    rootNode.mesh = 0;
    rootNode.hasMatrix = false;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);

    GltfPrimitive primitive;
    primitive.vertices = std::move(gridVertices);
    primitive.indices = std::move(gridIndices);
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.doubleSided = false;
    primitive.metallicFactor = 0.0f;
    primitive.roughnessFactor = 1.0f;
    primitive.unlit = false;
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    rewriteProjectionTexCoords(primitive, projection, geographicRectangle);
    for (SurfaceVertex& vertex : primitive.vertices) {
        setLocalPosition(vertex, vertex.positionEcef - localOrigin);
    }
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    if (!model->rebuildRuntime()) {
        return nullptr;
    }
    return model;
}

} // namespace

EllipsoidTerrainContentProvider::EllipsoidTerrainContentProvider(
    std::string schemeId,
    int maximumLevel,
    int gridSize)
    : schemeId_(std::move(schemeId)),
      maximumLevel_(std::max(0, maximumLevel)),
      gridSize_(std::max(1, gridSize)) {}

std::string EllipsoidTerrainContentProvider::id() const {
    return "ellipsoid-terrain-content";
}

bool EllipsoidTerrainContentProvider::supportsTile(const TileKey& key) const {
    if (key.schemeId != schemeId_) {
        return false;
    }
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(key)) {
        return true;
    }
    return key.z >= 0 && key.z <= maximumLevel_;
}

std::vector<TileKey> EllipsoidTerrainContentProvider::rootTiles() const {
    return {TileSelectionRootPolicy::virtualTerrainRootKey(schemeId_)};
}

std::optional<TilesetContentTileMetadata>
EllipsoidTerrainContentProvider::tileMetadata(const TileKey& key) const {
    if (!supportsTile(key)) {
        return std::nullopt;
    }
    TilesetContentTileMetadata metadata;
    metadata.key = key;
    metadata.bounds = TileSelectionRootPolicy::isVirtualTerrainRoot(key)
        ? rootRectangleForScheme(schemeId_)
        : createScheme(schemeId_)->tileToRectangle(key);
    metadata.hasExplicitBounds = true;
    metadata.boundingVolume = TileBoundingVolume::fromRegion(
        metadata.bounds,
        kEllipsoidTerrainMinimumHeight,
        kEllipsoidTerrainMaximumHeight);
    metadata.geometricError = calcLayerJsonTerrainGeometricError(
        Ellipsoid::WGS84(),
        metadata.bounds);
    metadata.refine = TileRefine::Replace;
    metadata.unconditionallyRefine =
        TileSelectionRootPolicy::isVirtualTerrainRoot(key);
    if (!metadata.unconditionallyRefine) {
        metadata.parentKey = key.z == 0
            ? std::optional<TileKey>(
                  TileSelectionRootPolicy::virtualTerrainRootKey(schemeId_))
            : std::optional<TileKey>(
                  TileKey{schemeId_, key.z - 1, key.x / 2, key.y / 2});
    }
    return metadata;
}

std::vector<TileKey>
EllipsoidTerrainContentProvider::childTiles(const TileKey& key) const {
    if (!supportsTile(key)) {
        return {};
    }
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(key)) {
        return TileSelectionRootPolicy::levelZeroTerrainRoots(schemeId_);
    }
    if (key.z >= maximumLevel_) {
        return {};
    }
    return quadtreeChildrenForKey(key);
}

TileAvailabilityState EllipsoidTerrainContentProvider::availabilityState(
    const TileKey& key) const {
    return supportsTile(key) ? TileAvailabilityState::Available
                             : TileAvailabilityState::NotAvailable;
}

void EllipsoidTerrainContentProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback,
    HttpRequestPriority) {
    if (token.isCancelled()) {
        callback(key, TileContentLoadResult::cancelled());
        return;
    }
    if (!supportsTile(key) ||
        TileSelectionRootPolicy::isVirtualTerrainRoot(key)) {
        callback(key, TileContentLoadResult::failed());
        return;
    }
    const Rectangle bounds = createScheme(schemeId_)->tileToRectangle(key);
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
        bounds,
        kEllipsoidTerrainMinimumHeight,
        kEllipsoidTerrainMaximumHeight);
    metadata.terrainHeightRange = {
        kEllipsoidTerrainMinimumHeight,
        kEllipsoidTerrainMaximumHeight};
    metadata.rasterOverlayDetails =
        makeRasterOverlayDetails(bounds, projectionForScheme(schemeId_));
    callback(
        key,
        TileContentLoadResult::renderTerrain(
            makeEllipsoidTerrainModel(
                bounds,
                projectionForScheme(schemeId_),
                gridSize_),
            std::move(metadata)));
}

TileContentLoadResult EllipsoidTerrainContentProvider::decodeContent(
    const uint8_t*,
    size_t) {
    return TileContentLoadResult::failed();
}

} // namespace earth_engine
