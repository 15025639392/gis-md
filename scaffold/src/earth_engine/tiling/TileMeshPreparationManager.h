#pragma once

#include "TileKey.h"
#include "TileLoadTypes.h"

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class TerrainProvider;
class TileContentLifecycleManager;
class TileContentResourceInvalidator;
class TileLoadQueue;
struct TilesetTile;

class TileMeshPreparationManager {
public:
    TileMeshPreparationManager(
        TileContentLifecycleManager& contentLifecycle,
        TileContentResourceInvalidator& resourceInvalidator,
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
    TileContentResourceInvalidator& resourceInvalidator_;
    TileLoadQueue& loadQueue_;
    TerrainProvider* terrainProvider_ = nullptr;
    RenderDevice* device_ = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays_;
};

} // namespace earth_engine
