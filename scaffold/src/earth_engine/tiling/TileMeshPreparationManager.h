#pragma once

#include "TileKey.h"
#include "TileLoadTypes.h"
#include "TileMeshFrameEnsurer.h"

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
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
        bool hasTerrainQuadtree,
        TileMeshLegacyHeightmapMode legacyHeightmapMode,
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
    bool hasTerrainQuadtree_ = false;
    TileMeshLegacyHeightmapMode legacyHeightmapMode_ =
        TileMeshLegacyHeightmapMode::Include;
    RenderDevice* device_ = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays_;
};

} // namespace earth_engine
