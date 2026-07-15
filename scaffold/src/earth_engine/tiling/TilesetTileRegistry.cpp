#include "TilesetTileRegistry.h"

#include "TileCacheKey.h"
#include "TileCreationPolicy.h"
#include "TileLoadStatePredicates.h"
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

void initializeVirtualTerrainRoot(TilesetTile& tile) {
    constexpr double kLooseMinimumHeight = -1000.0;
    constexpr double kLooseMaximumHeight = 9000.0;

    tile.setBounds(Rectangle::MAXIMUM);
    tile.setGeometricError(
        calcLayerJsonTerrainGeometricError(Ellipsoid::WGS84(), tile.bounds));
    tile.setRefine(TileRefine::Replace);
    tile.setUnconditionallyRefine(true);
    tile.setBoundingVolume(TileBoundingVolume::fromLooseRegion(
        tile.bounds, kLooseMinimumHeight, kLooseMaximumHeight));
    tile.resetContentBoundingVolume();
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
    const TilesetContentProvider* contentProvider) {
    const std::string ck = TileCacheKey::forTile(key);
    auto it = tiles_.find(ck);
    if (it != tiles_.end() && it->second) {
        TilesetTile& tile = *it->second;
        const uint64_t metadataRevision =
            contentProvider ? contentProvider->tileMetadataRevision() : 0;
        if (metadataRevision != 0 &&
            metadataRevision != tile.appliedContentMetadataRevision &&
            TileLoadStatePredicates::
                canRefreshContentMetadataBeforeContentAccepted(
                    tile.content.loadState)) {
            const std::optional<TilesetContentTileMetadata> contentMetadata =
                contentProvider->tileMetadata(key);
            if (contentMetadata) {
                TileCreationPolicy::applyContentMetadata(
                    tile,
                    *contentMetadata);
            }
            tile.appliedContentMetadataRevision = metadataRevision;
        }
        return &tile;
    }

    const uint64_t metadataRevision =
        contentProvider ? contentProvider->tileMetadataRevision() : 0;
    const std::optional<TilesetContentTileMetadata> contentMetadata =
        contentProvider ? contentProvider->tileMetadata(key) : std::nullopt;

    if (TileSelectionRootPolicy::isVirtualTerrainRoot(key)) {
        auto tile = std::make_unique<TilesetTile>(key, Rectangle::MAXIMUM);
        initializeVirtualTerrainRoot(*tile);
        if (contentMetadata) {
            TileCreationPolicy::applyContentMetadata(*tile, *contentMetadata);
        }
        tile->appliedContentMetadataRevision = metadataRevision;
        TilesetTile* raw = tile.get();
        tiles_[ck] = std::move(tile);
        return raw;
    }

    TilesetTile* parent = nullptr;
    if (contentMetadata && contentMetadata->parentKey) {
        parent = ensureTile(
            *contentMetadata->parentKey,
            tileScheme,
            contentProvider);
    } else if (!contentMetadata && key.z > 0) {
        parent = ensureTile(
            TilePlanBuilder::parentKey(key),
            tileScheme,
            contentProvider);
    }

    const Rectangle bounds = contentMetadata && contentMetadata->hasExplicitBounds
        ? contentMetadata->bounds
        : tileScheme.tileToRectangle(key);
    auto tile = std::make_unique<TilesetTile>(key, bounds, parent);
    TileCreationPolicy::initializeNewTile(
        *tile,
        contentMetadata,
        parent,
        calcLayerJsonTerrainGeometricError(Ellipsoid::WGS84(), tile->bounds));
    tile->appliedContentMetadataRevision = metadataRevision;

    TilesetTile* raw = tile.get();
    tiles_[ck] = std::move(tile);

    if (parent) {
        parent->attachChild(*raw);
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
