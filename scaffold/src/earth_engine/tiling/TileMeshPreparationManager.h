#pragma once

#include "TileKey.h"
#include "TileLoadTypes.h"
#include "TileMeshFrameEnsurer.h"

#include <memory>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class IPrepareRendererResources;
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
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    void prepareRenderableTile(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer = nullptr);
    bool prepareUpsampleSourceTile(
        TilesetTile& tile,
        double priority,
        IPrepareRendererResources* pPrepRenderer = nullptr);

private:
    void prepareContentTerrainFrame(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer);
    void markResourcesDirty();
    void queueTileLoad(const TileKey& key,
                       TileLoadPriorityGroup group,
                       double priority);

    TileContentLifecycleManager& contentLifecycle_;
    TileContentResourceInvalidator& resourceInvalidator_;
    TileLoadQueue& loadQueue_;
};

} // namespace earth_engine
