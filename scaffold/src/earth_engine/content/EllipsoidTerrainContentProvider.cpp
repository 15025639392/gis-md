#include "EllipsoidTerrainContentProvider.h"

#include "EllipsoidTerrainMeshBuilder.h"
#include "AmapClassicTerrainInternal.h"
#include "GltfModel.h"
#include "../style/AmapClassicStyleInternal.h"
#include "../core/geodesy/QuadtreeGeometricError.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "../tiling/TileBoundingVolume.h"
#include "../tiling/TileScheme.h"
#include "../tiling/TileSelectionRootPolicy.h"
#include "../tiling/TerrainRasterOverlayProjectionResolver.h"

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
    HttpRequestPriority priority) {
    requestTileContent(
        key,
        std::move(token),
        std::move(callback),
        priority,
        TileContentRequestOptions{});
}

void EllipsoidTerrainContentProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback,
    HttpRequestPriority,
    TileContentRequestOptions options) {
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
    const RasterOverlayProjection terrainProjection =
        TerrainRasterOverlayProjectionResolver::forSchemeId(schemeId_);
    std::vector<RasterOverlayProjection> projections{terrainProjection};
    for (RasterOverlayProjection projection :
         options.requiredRasterOverlayProjections) {
        if (std::find(
                projections.begin(),
                projections.end(),
                projection) == projections.end()) {
            projections.push_back(projection);
        }
    }
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
        bounds,
        kEllipsoidTerrainMinimumHeight,
        kEllipsoidTerrainMaximumHeight);
    metadata.terrainHeightRange = {
        kEllipsoidTerrainMinimumHeight,
        kEllipsoidTerrainMaximumHeight};
    metadata.rasterOverlayDetails =
        EllipsoidTerrainMeshBuilder::makeRasterOverlayDetails(
            bounds,
            projections);
    callback(
        key,
        TileContentLoadResult::renderTerrain(
            EllipsoidTerrainMeshBuilder::makeModel(
                bounds,
                projections,
                gridSize_,
                {},
                false,
                false,
                false),
            std::move(metadata),
            Mat4::identity(),
            TileTerrainRenderSource::EllipsoidFallback));
}

TileContentLoadResult EllipsoidTerrainContentProvider::decodeContent(
    const uint8_t*,
    size_t) {
    return TileContentLoadResult::failed();
}

namespace {

class AmapClassicTerrainContentProvider final : public TilesetContentProvider {
public:
    explicit AmapClassicTerrainContentProvider(
        std::unique_ptr<TilesetContentProvider> provider)
        : provider_(std::move(provider)) {}

    std::string id() const override {
        return "amap-classic-terrain:" + provider_->id();
    }
    bool supportsTile(const TileKey& key) const override {
        return provider_->supportsTile(key);
    }
    std::vector<TileKey> rootTiles() const override {
        return provider_->rootTiles();
    }
    std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey& key) const override {
        return provider_->tileMetadata(key);
    }
    uint64_t tileMetadataRevision() const override {
        return provider_->tileMetadataRevision();
    }
    std::vector<TileKey> childTiles(const TileKey& key) const override {
        return provider_->childTiles(key);
    }
    bool providesTerrainQuadtree() const override {
        return provider_->providesTerrainQuadtree();
    }
    uint64_t childTopologyRevision() const override {
        return provider_->childTopologyRevision();
    }
    bool isTerrainAvailabilityBoundaryLevel(int level) const override {
        return provider_->isTerrainAvailabilityBoundaryLevel(level);
    }
    int estimatedRequestFanout(const TileKey& key) const override {
        return provider_->estimatedRequestFanout(key);
    }
    TileAvailabilityState availabilityState(
        const TileKey& key) const override {
        return provider_->availabilityState(key);
    }
    void requestTileContent(
        const TileKey& key, CancellationToken token, ContentCallback callback,
        HttpRequestPriority priority = HttpRequestPriority::Normal) override {
        requestTileContent(
            key, std::move(token), std::move(callback), priority,
            TileContentRequestOptions{});
    }
    void requestTileContent(
        const TileKey& key, CancellationToken token, ContentCallback callback,
        HttpRequestPriority priority,
        TileContentRequestOptions options) override {
        provider_->requestTileContent(
            key, std::move(token),
            [callback = std::move(callback)](
                const TileKey& loadedKey, TileContentLoadResult result) mutable {
                AmapClassicTerrainContentProvider::applyOfficialLand(result);
                callback(loadedKey, std::move(result));
            },
            priority, std::move(options));
    }
    TileContentLoadResult decodeContent(
        const uint8_t* data, size_t size) override {
        TileContentLoadResult result = provider_->decodeContent(data, size);
        applyOfficialLand(result);
        return result;
    }
    ProviderRequestDiagnostics requestDiagnostics() const override {
        return provider_->requestDiagnostics();
    }

private:
    static void applyOfficialLand(TileContentLoadResult& result) {
        if (!result.gltfModel) return;
        for (GltfPrimitive& primitive : result.gltfModel->primitives) {
            primitive.baseColorFactor = amapClassicLandBaseColor();
            primitive.unlit = true;
        }
    }
    std::unique_ptr<TilesetContentProvider> provider_;
};

} // namespace

std::unique_ptr<TilesetContentProvider>
decorateAmapClassicTerrainContentProvider(
    std::unique_ptr<TilesetContentProvider> provider) {
    if (!provider) return nullptr;
    return std::make_unique<AmapClassicTerrainContentProvider>(
        std::move(provider));
}

} // namespace earth_engine
