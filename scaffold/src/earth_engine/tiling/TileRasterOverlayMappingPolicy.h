#pragma once

#include "TileRasterOverlayDetailsGenerator.h"
#include "TilesetTile.h"

#include <optional>

namespace earth_engine {

struct TileRasterOverlayMappingContext {
    bool hasRenderContentDetails = false;
    bool mapsLoadedRenderContent = false;
    bool waitForContentTerrainDetails = false;
    const RasterOverlayDetails* overlayDetails = nullptr;

    const RasterOverlayDetails& details() const {
        static const RasterOverlayDetails emptyDetails;
        return overlayDetails ? *overlayDetails : emptyDetails;
    }
};

struct TileRasterOverlayMappingPolicy {
    static TileRasterOverlayMappingContext contextFor(
        const TilesetTile& tile) {
        const bool hasRenderContentDetails =
            tile.content.contentKind == TileContentKind::Render &&
            tile.content.renderContent.hasRasterOverlayDetailsContent();
        const bool mapsLoadedRenderContent =
            tile.content.contentKind == TileContentKind::Render &&
            tile.content.renderContent.hasRenderableTerrainContent();

        return TileRasterOverlayMappingContext{
            hasRenderContentDetails,
            mapsLoadedRenderContent,
            tile.waitsForContentTerrainRasterDetails(),
            mapsLoadedRenderContent
                ? &tile.content.renderContent.rasterOverlayDetails()
                : nullptr};
    }

    static const Rectangle* geometryRectangle(
        const TileRasterOverlayMappingContext& context,
        RasterOverlayProjection projection) {
        return context.hasRenderContentDetails
            ? context.details().findRectangleForOverlayProjection(projection)
            : nullptr;
    }

    static std::optional<Rectangle> boundingVolumeRectangle(
        const TilesetTile& tile,
        const TileRasterOverlayMappingContext& context,
        RasterOverlayProjection projection) {
        return context.hasRenderContentDetails ||
               context.waitForContentTerrainDetails
            ? std::nullopt
            : TileRasterOverlayDetailsGenerator::
                  projectEffectiveContentBoundingVolumeRectangle(
                      tile,
                      projection);
    }

    static const Rectangle& targetRectangle(
        const TilesetTile& tile,
        const Rectangle* geometryRectangle,
        const std::optional<Rectangle>& boundingVolumeRectangle) {
        if (geometryRectangle) {
            return *geometryRectangle;
        }
        return boundingVolumeRectangle ? *boundingVolumeRectangle
                                       : tile.bounds;
    }
};

} // namespace earth_engine
