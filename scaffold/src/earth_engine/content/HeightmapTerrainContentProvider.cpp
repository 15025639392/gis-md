#include "HeightmapTerrainContentProvider.h"

#include "EllipsoidTerrainMeshBuilder.h"
#include "GltfModel.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/geodesy/QuadtreeGeometricError.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "../providers/TerrainProvider.h"
#include "../tiling/TerrainDisplacementTemplatePool.h"
#include "../tiling/TileBoundingVolume.h"
#include "../tiling/TileScheme.h"
#include "../tiling/TileSelectionRootPolicy.h"
#include "../tiling/TerrainRasterOverlayProjectionResolver.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace earth_engine {
namespace {

// Pre-decode bounding-volume height range: generous enough not to cull real
// terrain before its tile loads. Corrected to the tile's real min/max height via
// updatedBoundingVolume once the heightmap is decoded.
constexpr double kHeightmapDefaultMinHeight = -1000.0;
constexpr double kHeightmapDefaultMaxHeight = 9000.0;

std::unique_ptr<TileScheme> createScheme(const std::string& schemeId) {
    if (schemeId == "Geographic-TMS") {
        return TileScheme::createGeographicTMS();
    }
    return TileScheme::createXYZWebMercator();
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

// Per-vertex height source for the mesh builder: project (lon,lat) into the same
// web-mercator space the tile's samples are laid out in, normalise to the tile's
// projected rectangle, and bilinearly sample the decoded grid. Height does not
// affect lon/lat, so overlay UVs (a pure function of lon/lat) are unaffected.
EllipsoidProxyHeightSampler makeHeightSampler(const DecodedHeightmap& heightmap,
                                              const Rectangle& tileBounds) {
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const WebMercatorProjection webMercator(ellipsoid);
    const Rectangle projectedRectangle =
        projectRectangleSimple(webMercator, tileBounds);
    const double width = projectedRectangle.width();
    const double height = projectedRectangle.height();
    return [&heightmap, webMercator, projectedRectangle, width, height](
               double longitudeRadians,
               double latitudeRadians) -> std::optional<float> {
        if (width <= 0.0 || height <= 0.0) {
            return std::nullopt;
        }
        const Vec3 projected = projectPosition(
            webMercator,
            Cartographic::fromRadians(longitudeRadians, latitudeRadians, 0.0));
        const double u = std::clamp(
            (projected.x() - projectedRectangle.west()) / width, 0.0, 1.0);
        // NW convention: v=0 at the north edge (matches sample row order N→S).
        const double v = std::clamp(
            (projectedRectangle.north() - projected.y()) / height, 0.0, 1.0);
        const float sampled = heightmap.sampleBilinear(
            static_cast<float>(u), static_cast<float>(v));
        if (heightmap.isNoData(sampled)) {
            return std::nullopt;
        }
        return sampled;
    };
}

} // namespace

HeightmapTerrainContentProvider::HeightmapTerrainContentProvider(
    std::unique_ptr<HeightmapTerrainProvider> provider,
    int maximumLevel)
    : provider_(std::move(provider)),
      schemeId_(provider_ ? provider_->schemeId() : "XYZ-WebMercator"),
      maximumLevel_(std::max(0, maximumLevel)) {}

std::string HeightmapTerrainContentProvider::id() const {
    return "heightmap-terrain-content";
}

bool HeightmapTerrainContentProvider::supportsTile(const TileKey& key) const {
    if (key.schemeId != schemeId_) {
        return false;
    }
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(key)) {
        return true;
    }
    return key.z >= 0 && key.z <= maximumLevel_;
}

std::vector<TileKey> HeightmapTerrainContentProvider::rootTiles() const {
    return {TileSelectionRootPolicy::virtualTerrainRootKey(schemeId_)};
}

std::optional<TilesetContentTileMetadata>
HeightmapTerrainContentProvider::tileMetadata(const TileKey& key) const {
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
        kHeightmapDefaultMinHeight,
        kHeightmapDefaultMaxHeight);
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
HeightmapTerrainContentProvider::childTiles(const TileKey& key) const {
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

TileAvailabilityState HeightmapTerrainContentProvider::availabilityState(
    const TileKey& key) const {
    return supportsTile(key) ? TileAvailabilityState::Available
                             : TileAvailabilityState::NotAvailable;
}

void HeightmapTerrainContentProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback,
    HttpRequestPriority priority) {
    requestTileContent(
        key,
        std::move(token),
        std::move(callback),
        priority,
        TileContentRequestOptions{});
}

void HeightmapTerrainContentProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback,
    HttpRequestPriority priority,
    TileContentRequestOptions options) {
    if (token.isCancelled()) {
        callback(key, TileContentLoadResult::cancelled());
        return;
    }
    if (!supportsTile(key) ||
        TileSelectionRootPolicy::isVirtualTerrainRoot(key) ||
        !provider_) {
        callback(key, TileContentLoadResult::failed());
        return;
    }
    // Async fetch + decode via the composed provider; build the mesh on the
    // completion thread (CPU-only; GPU upload happens later on the main thread).
    provider_->requestTile(
        key,
        std::move(token),
        [this, options, callback](const TileKey& completedKey,
                                  TerrainTileLoadResult result) {
            if (result.status == TileLoadStatus::Cancelled) {
                callback(completedKey, TileContentLoadResult::cancelled());
                return;
            }
            if (result.status != TileLoadStatus::Renderable ||
                !result.heightmap || !result.heightmap->valid()) {
                callback(completedKey, TileContentLoadResult::failed());
                return;
            }
            TileContentLoadResult contentResult =
                buildContent(completedKey, *result.heightmap, options);
            // Phase 2c Stage B:保留原始高度图给 GL 线程建 per-tile 高度纹理。
            if (contentResult.status == TileLoadStatus::Renderable) {
                contentResult.retainedHeightmap = std::move(result.heightmap);
            }
            callback(completedKey, std::move(contentResult));
        },
        priority);
}

TileContentLoadResult HeightmapTerrainContentProvider::buildContent(
    const TileKey& key,
    const DecodedHeightmap& heightmap,
    const TileContentRequestOptions& options) const {
    const Rectangle bounds = createScheme(schemeId_)->tileToRectangle(key);

    const RasterOverlayProjection terrainProjection =
        TerrainRasterOverlayProjectionResolver::forSchemeId(schemeId_);
    std::vector<RasterOverlayProjection> projections{terrainProjection};
    for (RasterOverlayProjection projection :
         options.requiredRasterOverlayProjections) {
        if (std::find(projections.begin(), projections.end(), projection) ==
            projections.end()) {
            projections.push_back(projection);
        }
    }

    // 网格镶嵌密度 = min(源采样数-1, 共享位移模板格数)。**与纹理分辨率解耦**:
    // GPU 位移(P5b)激活时渲染换用共享 kTerrainDisplacementGridSize(64) 模板 +
    // 高度纹理,这个 per-tile 网格的顶点几乎全被丢弃(仅在模板 swap 失败时作为
    // 同分辨率回退渲染)。旧代码 gridSize=tileSize-1 让 514 源建 513×513=26 万
    // 顶点/片,decode 主线程 O(N²) prep 涨到 44-110ms(FABDEM 65→~4ms 的 64×)——
    // 纯浪费,因为渲染只吃 64 格模板。cap 到 64 后 decode prep 与源分辨率脱钩
    // (mapbox/cesium 同理:镶嵌密度不跟纹理走)。retainedHeightmap 仍存全分辨率
    // 514²,高度纹理与 CPU 高度查询精度不受影响。
    const int gridSize =
        std::min(std::max(1, heightmap.tileSize - 1), kTerrainDisplacementGridSize);
    const EllipsoidProxyHeightSampler heightSampler =
        makeHeightSampler(heightmap, bounds);
    std::unique_ptr<GltfModel> model = EllipsoidTerrainMeshBuilder::makeModel(
        bounds,
        projections,
        gridSize,
        heightSampler,
        /*computeGridNormals=*/true,
        /*computeGeomorphDelta=*/true,
        /*buildSkirt=*/true);
    if (!model) {
        return TileContentLoadResult::failed();
    }

    // Real height range from the decoded grid (heightFactor already applied
    // during decode) tightens the bounding volume for culling / SSE. Lower the
    // min by the skirt height so the downward skirt wall stays inside the tile's
    // bounding volume (otherwise its far-edge triangles could be frustum-culled).
    const double minHeight = static_cast<double>(heightmap.minHeight);
    const double maxHeight = static_cast<double>(heightmap.maxHeight);
    const double skirtHeight =
        calcQuadtreeSkirtHeight(Ellipsoid::WGS84(), bounds);
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
        bounds,
        std::min(minHeight, maxHeight) - skirtHeight,
        std::max(minHeight, maxHeight));
    metadata.terrainHeightRange = {
        std::min(minHeight, maxHeight),
        std::max(minHeight, maxHeight)};
    metadata.rasterOverlayDetails =
        EllipsoidTerrainMeshBuilder::makeRasterOverlayDetails(
            bounds,
            projections);
    return TileContentLoadResult::renderTerrain(
        std::move(model),
        std::move(metadata),
        Mat4::identity(),
        TileTerrainRenderSource::Generic);
}

TileContentLoadResult HeightmapTerrainContentProvider::decodeContent(
    const uint8_t*,
    size_t) {
    // Synchronous byte→content decode is not supported (the tile key — hence
    // geographic bounds — is required to place the mesh). The async
    // requestTileContent path is authoritative.
    return TileContentLoadResult::failed();
}

ProviderRequestDiagnostics
HeightmapTerrainContentProvider::requestDiagnostics() const {
    return provider_ ? provider_->requestDiagnostics()
                     : ProviderRequestDiagnostics{};
}

} // namespace earth_engine
