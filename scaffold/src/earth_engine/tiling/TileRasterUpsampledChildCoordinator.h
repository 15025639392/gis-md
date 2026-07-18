#pragma once

namespace earth_engine {

class TileContentAccess;
class TileContentResourceInvalidator;
class IPrepareRendererResources;
struct TilesetTile;

class TileRasterUpsampledChildCoordinator {
public:
    TileRasterUpsampledChildCoordinator(
        TileContentAccess& contentAccess,
        TileContentResourceInvalidator& resourceInvalidator);

    // decoupleImageryFromGeometry=true(北极星 Phase 2a)时直接早退:影像细节不再
    // 捏造上采样地形子瓦片,几何 cap 在 DEM native max LOD。默认 false = 忠实 cesium。
    void createRasterOverlayUpsampledChildren(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer = nullptr,
        bool decoupleImageryFromGeometry = false);

private:
    void markResourcesDirty();

    TileContentAccess& contentAccess_;
    TileContentResourceInvalidator& resourceInvalidator_;
};

} // namespace earth_engine
