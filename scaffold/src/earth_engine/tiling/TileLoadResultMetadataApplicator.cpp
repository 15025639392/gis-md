#include "TileLoadResultMetadataApplicator.h"

#include "DirectRasterMapping.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TileRenderContentState.h"
#include "TilesetTile.h"
#include "../core/math/MathUtils.h"

#include <cmath>

namespace earth_engine {
namespace {

constexpr double kLooseMinimumHeight = -1000.0;
constexpr double kLooseMaximumHeight = 9000.0;

bool isDefaultLooseRegion(const TilesetTile& tile) {
    return tile.boundingVolume &&
           tile.boundingVolume->kind == TileBoundingVolumeKind::Region &&
           std::abs(tile.boundingVolume->minimumHeight -
                    kLooseMinimumHeight) <= MathUtils::Epsilon5 &&
           std::abs(tile.boundingVolume->maximumHeight -
                    kLooseMaximumHeight) <= MathUtils::Epsilon5;
}

bool hasLooseFittingHeights(const TilesetTile& tile) {
    return tile.boundingVolume &&
           tile.boundingVolume->kind == TileBoundingVolumeKind::Region &&
           (tile.boundingVolume->looseFittingHeights ||
            isDefaultLooseRegion(tile));
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
        hasLooseFittingHeights(tile) &&
        hasValidBoundingRegion(*metadata.rasterOverlayDetails)) {
        if (!tile.initialBoundingVolume && tile.boundingVolume) {
            tile.initialBoundingVolume = *tile.boundingVolume;
        }
        const auto& region = metadata.rasterOverlayDetails->boundingRegion;
        metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
            region.rectangle,
            region.minimumHeight,
            region.maximumHeight);
    } else if (!metadata.updatedBoundingVolume &&
               hasLooseFittingHeights(tile)) {
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

    if (metadata.updatedBoundingVolume) {
        tile.setBoundingVolume(std::move(metadata.updatedBoundingVolume));
        if (!metadata.terrainHeightRange) {
            metadata.terrainHeightRange = {
                tile.boundingVolume->minimumHeight,
                tile.boundingVolume->maximumHeight};
        }
    }
    if (metadata.updatedContentBoundingVolume) {
        tile.setContentBoundingVolume(
            std::move(metadata.updatedContentBoundingVolume));
    }
    if (metadata.rasterOverlayDetails) {
        if (RasterOverlayDetails* details =
                tile.content.renderContent.mutableRasterOverlayDetails()) {
            if (tile.content.renderContent.isTerrainRenderContent()) {
                *details = *metadata.rasterOverlayDetails;
            } else if (!details->equalsExact(*metadata.rasterOverlayDetails)) {
                details->merge(*metadata.rasterOverlayDetails);
            }
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
