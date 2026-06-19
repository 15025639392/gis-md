#pragma once

#include "TileKey.h"
#include "TileLoadTypes.h"

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class TerrainProvider;
class TileContentCacheManager;
class TileContentLifecycleManager;
class TileLoadQueue;
struct TileSelectionReuseState;
struct TilesetTile;

class TileMeshPreparationManager {
public:
    TileMeshPreparationManager(
        TileContentLifecycleManager& contentLifecycle,
        TileContentCacheManager& contentCache,
        TileSelectionReuseState& selectionReuseState,
        TileLoadQueue& loadQueue,
        TerrainProvider* terrainProvider,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    void ensureTileMesh(TilesetTile& tile);
    bool prepareUpsampleSourceTile(
        TilesetTile& tile,
        double priority);

private:
    void markResourcesDirty();
    void queueTileLoad(const TileKey& key,
                       TileLoadPriorityGroup group,
                       double priority);

    TileContentLifecycleManager& contentLifecycle_;
    TileContentCacheManager& contentCache_;
    TileSelectionReuseState& selectionReuseState_;
    TileLoadQueue& loadQueue_;
    TerrainProvider* terrainProvider_ = nullptr;
    RenderDevice* device_ = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays_;
};

} // namespace earth_engine
