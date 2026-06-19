#pragma once

namespace earth_engine {

class TileContentAccess;
class TileContentResourceInvalidator;
struct TilesetTile;

class TileRasterUpsampledChildCoordinator {
public:
    TileRasterUpsampledChildCoordinator(
        TileContentAccess& contentAccess,
        TileContentResourceInvalidator& resourceInvalidator);

    void createRasterOverlayUpsampledChildren(TilesetTile& tile);

private:
    void markResourcesDirty();

    TileContentAccess& contentAccess_;
    TileContentResourceInvalidator& resourceInvalidator_;
};

} // namespace earth_engine
