#pragma once

#include "RasterMappedToTilesetTile.h"
#include "TileChildMaterializer.h"
#include "TilesetTile.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <vector>

namespace earth_engine {

class TileRasterUpsampledChildMaterializer {
public:
    template <typename EnsureTileFn>
    static bool materialize(
        TilesetTile& tile,
        double defaultGeometricError,
        EnsureTileFn&& ensureTile) {
        if (!tile.content.renderContent.hasRenderableTerrainContent() ||
            tile.children.size() >= 4) {
            return false;
        }

        const RasterOverlayDetails& details =
            tile.content.renderContent.rasterOverlayDetails();
        Rectangle subdivisionRectangle = tile.bounds;
        if (details.boundingRegion.minimumHeight <=
                details.boundingRegion.maximumHeight &&
            !details.boundingRegion.rectangle.isEmpty()) {
            subdivisionRectangle = details.boundingRegion.rectangle;
        }
        bool hasMoreRasterDetail = false;
        tile.rasterOverlayState.forEachMapping([&](const auto* mapped) {
            if (hasMoreRasterDetail ||
                !mapped ||
                !mapped->isMoreDetailAvailable()) {
                return;
            }
            const RasterOverlayTile* readyTile = mapped->getReadyTile();
            if (!readyTile) {
                return;
            }
            const RasterOverlayProjection projection =
                readyTile->getTileProvider().getProjection();
            const Rectangle* projectionRectangle =
                details.findRectangleForOverlayProjection(projection);
            if (!projectionRectangle) {
                return;
            }
            subdivisionRectangle =
                unprojectSubdivisionRectangle(*projectionRectangle, projection);
            hasMoreRasterDetail = true;
        });

        if (!hasMoreRasterDetail) {
            return false;
        }

        return TileChildMaterializer::materializeRasterUpsampledChildren(
            tile,
            subdivisionRectangle,
            defaultGeometricError,
            ensureTile);
    }

private:
    static Rectangle unprojectSubdivisionRectangle(
        const Rectangle& projectionRectangle,
        RasterOverlayProjection projection) {
        switch (projection) {
            case RasterOverlayProjection::Geographic:
                return projectionRectangle;
            case RasterOverlayProjection::WebMercator:
                return unprojectRectangleSimple(
                    WebMercatorProjection(Ellipsoid::WGS84()),
                    projectionRectangle);
        }
        return projectionRectangle;
    }
};

} // namespace earth_engine
