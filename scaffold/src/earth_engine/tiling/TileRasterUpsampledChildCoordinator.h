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

    void createRasterOverlayUpsampledChildren(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer = nullptr);

private:
    void markResourcesDirty();

    TileContentAccess& contentAccess_;
    TileContentResourceInvalidator& resourceInvalidator_;
};

} // namespace earth_engine
