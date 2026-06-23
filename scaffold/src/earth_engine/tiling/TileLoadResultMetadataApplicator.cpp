#include "TileLoadResultMetadataApplicator.h"

#include "RasterMappedToTilesetTile.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TileRenderContentState.h"
#include "TilesetTile.h"

namespace earth_engine {
namespace {

constexpr double kLooseMinimumHeight = -1000.0;
constexpr double kLooseMaximumHeight = 9000.0;

bool isDefaultLooseRegion(const TilesetTile& tile) {
    return tile.boundingVolume &&
           tile.boundingVolume->kind == TileBoundingVolumeKind::Region &&
           tile.boundingVolume->minimumHeight == kLooseMinimumHeight &&
           tile.boundingVolume->maximumHeight == kLooseMaximumHeight;
}

bool hasValidBoundingRegion(const RasterOverlayDetails& details) {
    const auto& region = details.boundingRegion;
    return !region.rectangle.isEmpty() &&
           region.minimumHeight <= region.maximumHeight;
}

bool hasValidBoundingRegion(
    const BoundingRegionBuilder::BoundingRegion& region) {
    return !region.rectangle.isEmpty() &&
           region.minimumHeight <= region.maximumHeight;
}

} // namespace

void TileLoadResultMetadataApplicator::apply(
    TilesetTile& tile,
    TileLoadResultMetadata&& metadata) {
    if (metadata.initialBoundingVolume) {
        tile.initialBoundingVolume = std::move(metadata.initialBoundingVolume);
    } else if (!tile.initialBoundingVolume && metadata.updatedBoundingVolume &&
               tile.boundingVolume) {
        tile.initialBoundingVolume = *tile.boundingVolume;
    }

    if (metadata.initialContentBoundingVolume) {
        tile.initialContentBoundingVolume =
            std::move(metadata.initialContentBoundingVolume);
    } else if (!tile.initialContentBoundingVolume &&
               metadata.updatedContentBoundingVolume &&
               tile.contentBoundingVolume) {
        tile.initialContentBoundingVolume = *tile.contentBoundingVolume;
    }

    if (!metadata.updatedBoundingVolume &&
        metadata.rasterOverlayDetails &&
        isDefaultLooseRegion(tile) &&
        hasValidBoundingRegion(*metadata.rasterOverlayDetails)) {
        if (!tile.initialBoundingVolume && tile.boundingVolume) {
            tile.initialBoundingVolume = *tile.boundingVolume;
        }
        const auto& region = metadata.rasterOverlayDetails->boundingRegion;
        metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
            region.rectangle,
            region.minimumHeight,
            region.maximumHeight);
    } else if (!metadata.updatedBoundingVolume && isDefaultLooseRegion(tile)) {
        const std::optional<BoundingRegionBuilder::BoundingRegion>
            tightRegion = TileRasterOverlayDetailsGenerator::
                computeTightModelBoundingRegion(tile.content.renderContent);
        if (tightRegion && hasValidBoundingRegion(*tightRegion)) {
            if (!tile.initialBoundingVolume && tile.boundingVolume) {
                tile.initialBoundingVolume = *tile.boundingVolume;
            }
            metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
                tightRegion->rectangle,
                tightRegion->minimumHeight,
                tightRegion->maximumHeight);
        }
    }

    if (metadata.updatedBoundingVolume &&
        !metadata.updatedContentBoundingVolume &&
        tile.contentBoundingVolume) {
        if (!tile.initialContentBoundingVolume) {
            tile.initialContentBoundingVolume = *tile.contentBoundingVolume;
        }
        tile.contentBoundingVolume.reset();
    }

    if (metadata.updatedBoundingVolume) {
        tile.boundingVolume = std::move(metadata.updatedBoundingVolume);
        if (!metadata.terrainHeightRange) {
            metadata.terrainHeightRange = {
                tile.boundingVolume->minimumHeight,
                tile.boundingVolume->maximumHeight};
        }
    }
    if (metadata.updatedContentBoundingVolume) {
        tile.contentBoundingVolume =
            std::move(metadata.updatedContentBoundingVolume);
    }
    if (metadata.rasterOverlayDetails) {
        if (RasterOverlayDetails* details =
                tile.content.renderContent.mutableRasterOverlayDetails()) {
            details->merge(*metadata.rasterOverlayDetails);
        }
    }
    if (metadata.terrainHeightRange) {
        tile.content.renderContent.setTerrainHeightRange(
            metadata.terrainHeightRange->first,
            metadata.terrainHeightRange->second);
    }
    if (metadata.horizonOcclusionPoint) {
        tile.content.renderContent.setHorizonOcclusionPoint(
            *metadata.horizonOcclusionPoint);
    }
}

} // namespace earth_engine
