#pragma once

namespace earth_engine {

class TileContentAccess;
class TileContentCacheManager;
struct TileSelectionReuseState;
struct TilesetTile;

class TileRasterUpsampledChildCoordinator {
public:
    TileRasterUpsampledChildCoordinator(
        TileContentAccess& contentAccess,
        TileContentCacheManager& contentCache,
        TileSelectionReuseState& selectionReuseState);

    void createRasterOverlayUpsampledChildren(TilesetTile& tile);

private:
    void markResourcesDirty();

    TileContentAccess& contentAccess_;
    TileContentCacheManager& contentCache_;
    TileSelectionReuseState& selectionReuseState_;
};

} // namespace earth_engine
