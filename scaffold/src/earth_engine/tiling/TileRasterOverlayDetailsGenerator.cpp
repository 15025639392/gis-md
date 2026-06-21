#include "TileRasterOverlayDetailsGenerator.h"

#include "TileBoundingVolume.h"
#include "TileRenderContentState.h"

namespace earth_engine {

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
    generated.setGeographicRectangle(
        boundingVolume.region,
        boundingVolume.minimumHeight,
        boundingVolume.maximumHeight);
    details->merge(generated);
    return true;
}

} // namespace earth_engine
