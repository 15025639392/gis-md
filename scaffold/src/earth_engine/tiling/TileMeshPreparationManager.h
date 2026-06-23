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
        bool useHeightmapSurfacePath,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    void prepareRenderableTile(TilesetTile& tile);
    void prepareContentTerrainFrame(TilesetTile& tile);
    void ensureLegacySurfaceMesh(TilesetTile& tile);
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
    bool useHeightmapSurfacePath_ = true;
    RenderDevice* device_ = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays_;
};

} // namespace earth_engine
