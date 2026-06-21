#include "TilesetTileRegistry.h"

#include "TileCacheKey.h"
#include "TileCreationPolicy.h"
#include "TilePlan.h"
#include "TileSelectionRootPolicy.h"
#include "TileScheme.h"
#include "TileTerrainHeightRangePolicy.h"
#include "RasterMappedToTilesetTile.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/QuadtreeGeometricError.h"

#include <algorithm>
#include <optional>

namespace earth_engine {

namespace {

void initializeVirtualTerrainRoot(TilesetTile& tile,
                                  size_t rasterOverlayCount) {
    constexpr double kLooseMinimumHeight = -1000.0;
    constexpr double kLooseMaximumHeight = 9000.0;

    tile.bounds = Rectangle::MAXIMUM;
    tile.geometricError =
        calcLayerJsonTerrainGeometricError(Ellipsoid::WGS84(), tile.bounds);
    tile.refine = TileRefine::Replace;
    tile.unconditionallyRefine = true;
    tile.boundingVolume = TileBoundingVolume::fromRegion(
        tile.bounds,
        kLooseMinimumHeight,
        kLooseMaximumHeight);
    tile.contentBoundingVolume = tile.boundingVolume;
    tile.rasterOverlayState.ensureMappingSlots(rasterOverlayCount);
    TileTerrainHeightRangePolicy::setTerrainHeightRange(
        tile,
        kLooseMinimumHeight,
        kLooseMaximumHeight);
    tile.markEmptyContentDone();
}

} // namespace

TilesetTile* TilesetTileRegistry::ensureTile(
    const TileKey& key,
    const TileScheme& tileScheme,
    const TilesetContentProvider* contentProvider,
    size_t rasterOverlayCount) {
    const std::string ck = TileCacheKey::forTile(key);
    const std::optional<TilesetContentTileMetadata> contentMetadata =
        contentProvider ? contentProvider->tileMetadata(key) : std::nullopt;
    auto it = tiles_.find(ck);
    if (it != tiles_.end() && it->second) {
        if (contentMetadata) {
            TileCreationPolicy::applyContentMetadata(
                *it->second,
                *contentMetadata);
        }
        return it->second.get();
    }

    if (TileSelectionRootPolicy::isVirtualTerrainRoot(key)) {
        auto tile = std::make_unique<TilesetTile>(key, Rectangle::MAXIMUM);
        initializeVirtualTerrainRoot(*tile, rasterOverlayCount);
        TilesetTile* raw = tile.get();
        tiles_[ck] = std::move(tile);
        return raw;
    }

    TilesetTile* parent = nullptr;
    if (contentMetadata && contentMetadata->parentKey) {
        parent = ensureTile(
            *contentMetadata->parentKey,
            tileScheme,
            contentProvider,
            rasterOverlayCount);
    } else if (!contentMetadata && key.z > 0) {
        parent = ensureTile(
            TilePlanBuilder::parentKey(key),
            tileScheme,
            contentProvider,
            rasterOverlayCount);
    }

    const Rectangle bounds = contentMetadata && contentMetadata->hasExplicitBounds
        ? contentMetadata->bounds
        : tileScheme.tileToRectangle(key);
    auto tile = std::make_unique<TilesetTile>(key, bounds, parent);
    TileCreationPolicy::initializeNewTile(
        *tile,
        contentMetadata,
        parent,
        calcLayerJsonTerrainGeometricError(Ellipsoid::WGS84(), tile->bounds),
        rasterOverlayCount);

    TilesetTile* raw = tile.get();
    tiles_[ck] = std::move(tile);

    if (parent) {
        auto& children = parent->children;
        if (std::find(children.begin(), children.end(), raw) == children.end()) {
            children.push_back(raw);
        }
    }

    return raw;
}

TilesetTile* TilesetTileRegistry::findTile(const TileKey& key) {
    auto it = tiles_.find(TileCacheKey::forTile(key));
    if (it == tiles_.end()) {
        return nullptr;
    }
    return it->second.get();
}

const TilesetTile* TilesetTileRegistry::findTile(const TileKey& key) const {
    auto it = tiles_.find(TileCacheKey::forTile(key));
    if (it == tiles_.end()) {
        return nullptr;
    }
    return it->second.get();
}

} // namespace earth_engine
