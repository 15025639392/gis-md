#include "TileRasterOverlayDetailsGenerator.h"

#include "TileBoundingVolume.h"
#include "TileRenderContentState.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"

namespace earth_engine {
namespace {

Rectangle projectRegionRectangle(const Rectangle& rectangle,
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

} // namespace

bool TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
    TileRenderContentState& renderContent,
    const TileBoundingVolume& boundingVolume,
    RasterOverlayProjection projection) {
    RasterOverlayDetails* details =
        renderContent.mutableRasterOverlayDetails();
    if (!details ||
        details->findRectangleForOverlayProjection(projection) != nullptr) {
        return false;
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

} // namespace earth_engine
