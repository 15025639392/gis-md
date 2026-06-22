#include "TileRasterOverlayDetailsGenerator.h"

#include "TileBoundingVolume.h"
#include "TileRenderContentState.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"

namespace earth_engine {

Rectangle TileRasterOverlayDetailsGenerator::projectRegionRectangle(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    switch (projection) {
        case RasterOverlayProjection::Geographic:
            return rectangle;
        case RasterOverlayProjection::WebMercator:
            return projectRectangleSimple(
                WebMercatorProjection(Ellipsoid::WGS84()),
                rectangle);
    }
    return rectangle;
}

bool TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
    TileRenderContentState& renderContent,
    const TileBoundingVolume& boundingVolume,
    RasterOverlayProjection projection) {
    RasterOverlayDetails* details =
        renderContent.mutableRasterOverlayDetails();
    if (!details) {
        return false;
    }
    for (RasterOverlayProjection existingProjection :
         details->rasterOverlayProjections) {
        if (existingProjection == projection) {
            return false;
        }
    }
    if (boundingVolume.kind != TileBoundingVolumeKind::Region) {
        return false;
    }

    RasterOverlayDetails generated;
    generated.rasterOverlayProjections = {projection};
    generated.rasterOverlayRectangles = {
        projectRegionRectangle(boundingVolume.region, projection)};
    generated.boundingRegion = {
        boundingVolume.region,
        boundingVolume.minimumHeight,
        boundingVolume.maximumHeight};
    details->merge(generated);
    return true;
}

int TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromActiveOverlays(
    TileRenderContentState& renderContent,
    const TileBoundingVolume* boundingVolume,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device) {
    if (!boundingVolume ||
        boundingVolume->kind != TileBoundingVolumeKind::Region) {
        return 0;
    }

    int generated = 0;
    for (ActivatedRasterOverlay* activeOverlay : rasterOverlays) {
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        RasterOverlayTileProvider* provider =
            activeOverlay->ensureTileProvider(device);
        if (!provider || !provider->isReady()) {
            continue;
        }
        if (ensureProjectionDetailsFromRegion(
                renderContent,
                *boundingVolume,
                provider->getProjection())) {
            ++generated;
        }
    }
    return generated;
}

} // namespace earth_engine
