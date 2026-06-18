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
        tile.bounds = metadata.bounds;
        tile.geometricError = metadata.geometricError;
        tile.refine = metadata.refine;
        tile.unconditionallyRefine = metadata.unconditionallyRefine;
        tile.boundingVolume = metadata.boundingVolume;
        tile.viewerRequestVolume = metadata.viewerRequestVolume;
        tile.contentBoundingVolume = metadata.contentBoundingVolume;
        applyMetadataHeightRange(tile, metadata);
    }

    static void initializeNewTile(
        TilesetTile& tile,
        const std::optional<TilesetContentTileMetadata>& metadata,
        const TilesetTile* parent,
        double defaultGeometricError,
        size_t rasterOverlayCount) {
        tile.geometricError = metadata
            ? metadata->geometricError
            : defaultGeometricError;
        if (metadata) {
            tile.refine = metadata->refine;
            tile.unconditionallyRefine = metadata->unconditionallyRefine;
            tile.boundingVolume = metadata->boundingVolume;
            tile.viewerRequestVolume = metadata->viewerRequestVolume;
            tile.contentBoundingVolume = metadata->contentBoundingVolume;
        }
        tile.rasterOverlays.resize(rasterOverlayCount);
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
