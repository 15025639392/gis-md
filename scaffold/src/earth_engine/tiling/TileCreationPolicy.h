#pragma once

#include "TileTerrainHeightRangePolicy.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"

#include <cstddef>
#include <optional>

namespace earth_engine {

struct TileCreationPolicy {
    static void applyContentMetadata(
        TilesetTile& tile,
        const TilesetContentTileMetadata& metadata) {
        tile.setBounds(metadata.bounds);
        tile.setGeometricError(metadata.geometricError);
        tile.setRefine(metadata.refine);
        tile.setUnconditionallyRefine(metadata.unconditionallyRefine);
        tile.setBoundingVolume(metadata.boundingVolume);
        tile.viewerRequestVolume = metadata.viewerRequestVolume;
        tile.setContentBoundingVolume(metadata.contentBoundingVolume);
        applyMetadataHeightRange(tile, metadata);
    }

    static void initializeNewTile(
        TilesetTile& tile,
        const std::optional<TilesetContentTileMetadata>& metadata,
        const TilesetTile* parent,
        double defaultGeometricError) {
        tile.setGeometricError(
            metadata ? metadata->geometricError : defaultGeometricError);
        if (metadata) {
            tile.setRefine(metadata->refine);
            tile.setUnconditionallyRefine(metadata->unconditionallyRefine);
            tile.setBoundingVolume(metadata->boundingVolume);
            tile.viewerRequestVolume = metadata->viewerRequestVolume;
            tile.setContentBoundingVolume(metadata->contentBoundingVolume);
        }
        initializeTerrainHeightRange(tile, metadata, parent);
    }

private:
    static void applyMetadataHeightRange(
        TilesetTile& tile,
        const TilesetContentTileMetadata& metadata) {
        if (metadata.boundingVolume &&
            metadata.boundingVolume->kind == TileBoundingVolumeKind::Region) {
            TileTerrainHeightRangePolicy::setTerrainHeightRange(
                tile,
                metadata.boundingVolume->minimumHeight,
                metadata.boundingVolume->maximumHeight);
        }
    }

    static void initializeTerrainHeightRange(
        TilesetTile& tile,
        const std::optional<TilesetContentTileMetadata>& metadata,
        const TilesetTile* parent) {
        if (metadata) {
            applyMetadataHeightRange(tile, *metadata);
        } else if (parent) {
            TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                tile,
                *parent);
        } else {
            TileTerrainHeightRangePolicy::setDefaultTerrainHeightRange(tile);
        }
    }
};

} // namespace earth_engine
