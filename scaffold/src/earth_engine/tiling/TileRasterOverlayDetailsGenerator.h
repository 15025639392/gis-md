#pragma once

#include "SurfaceTile.h"

#include <optional>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class TileRenderContentState;
struct TilesetTile;
struct TileBoundingVolume;

class TileRasterOverlayDetailsGenerator {
public:
    static Rectangle projectRegionRectangle(
        const Rectangle& rectangle,
        RasterOverlayProjection projection);

    static std::optional<Rectangle>
    projectEffectiveContentBoundingVolumeRectangle(
        const TilesetTile& tile,
        RasterOverlayProjection projection);

    static bool ensureProjectionDetailsFromRegion(
        TileRenderContentState& renderContent,
        const TileBoundingVolume& boundingVolume,
        RasterOverlayProjection projection);

    static int ensureProjectionDetailsFromActiveOverlays(
        TileRenderContentState& renderContent,
        const TileBoundingVolume* boundingVolume,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        RenderDevice* device);

    static int ensureProjectionDetailsFromActiveOverlays(
        TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        RenderDevice* device);
};

} // namespace earth_engine
