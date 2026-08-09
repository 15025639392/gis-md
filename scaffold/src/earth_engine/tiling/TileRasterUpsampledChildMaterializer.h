#pragma once

#include "RasterMappedToTilesetTile.h"
#include "TileChildMaterializer.h"
#include "TilesetTile.h"

#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <optional>

namespace earth_engine {

class IPrepareRendererResources;

class TileRasterUpsampledChildMaterializer {
public:
    static std::optional<RasterOverlayProjection>
    geometryProjectionForRasterDetail(
        const RasterOverlayDetails& details,
        RasterOverlayProjection imageryProjection) {
        if (imageryProjection !=
            RasterOverlayProjection::Gcj02WebMercator) {
            return imageryProjection;
        }
        if (details.findRectangleForOverlayProjection(
                RasterOverlayProjection::Geographic)) {
            return RasterOverlayProjection::Geographic;
        }
        if (details.findRectangleForOverlayProjection(
                RasterOverlayProjection::WebMercator)) {
            return RasterOverlayProjection::WebMercator;
        }
        return std::nullopt;
    }

    template <typename EnsureTileFn>
    static bool materialize(
        TilesetTile& tile,
        double defaultGeometricError,
        EnsureTileFn&& ensureTile,
        IPrepareRendererResources* pPrepRenderer = nullptr) {
        if (!tile.content.renderContent.hasRasterOverlayDetailsContent() ||
            tile.children.size() >= 4) {
            return false;
        }
        if (tile.content.renderContent.isTerrainRenderContent() &&
            !tile.content.renderContent.hasGltfContent()) {
            return false;
        }

        const RasterOverlayDetails& details =
            tile.content.renderContent.rasterOverlayDetails();
        // Don't upsample from a tile without geometry (cesium
        // createQuadtreeSubdividedChildren guards the same way).
        if (details.boundingRegion.maximumHeight <
            details.boundingRegion.minimumHeight) {
            return false;
        }
        bool hasMoreRasterDetail = false;
        RasterOverlayProjection moreDetailProjection =
            RasterOverlayProjection::Geographic;
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
            if (!details.findRectangleForOverlayProjection(projection)) {
                return;
            }
            hasMoreRasterDetail = true;
            moreDetailProjection = projection;
        });

        if (!hasMoreRasterDetail) {
            return false;
        }

        const std::optional<RasterOverlayProjection> geometryProjection =
            geometryProjectionForRasterDetail(
                details,
                moreDetailProjection);
        if (!geometryProjection) {
            return false;
        }

        // Child bounds come from the registry/scheme rectangle, never from
        // the content-derived details rectangle — see
        // TileChildMaterializer::materializeRasterUpsampledChildren.
        return TileChildMaterializer::materializeRasterUpsampledChildren(
            tile,
            defaultGeometricError,
            ensureTile,
            pPrepRenderer,
            *geometryProjection);
    }
};

} // namespace earth_engine
