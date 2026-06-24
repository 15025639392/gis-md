#pragma once

#include "TileKey.h"
#include "TileLoadTypes.h"
#include "TileLegacyHeightmapSurfacePreparer.h"
#include "TileMeshFrameEnsurer.h"

#include <memory>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class TileContentLifecycleManager;
class TileContentResourceInvalidator;
class TileLoadQueue;
struct TilesetTile;

enum class TileMeshPreparationMode {
    ContentTerrain,
    LegacyHeightmapSurface
};

class TileMeshPreparationManager {
public:
    TileMeshPreparationManager(
        TileContentLifecycleManager& contentLifecycle,
        TileContentResourceInvalidator& resourceInvalidator,
        TileLoadQueue& loadQueue,
        bool hasTerrainQuadtree,
        TileMeshPreparationMode mode,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    void prepareRenderableTile(TilesetTile& tile);
    bool prepareUpsampleSourceTile(
        TilesetTile& tile,
        double priority);

private:
    void prepareContentTerrainFrame(TilesetTile& tile);
    bool usesLegacyHeightmapSurfacePath() const;
    void markResourcesDirty();
    void queueTileLoad(const TileKey& key,
                       TileLoadPriorityGroup group,
                       double priority);

    TileContentLifecycleManager& contentLifecycle_;
    TileContentResourceInvalidator& resourceInvalidator_;
    TileLoadQueue& loadQueue_;
    TileMeshPreparationMode mode_ = TileMeshPreparationMode::ContentTerrain;
    std::unique_ptr<TileLegacyHeightmapSurfacePreparer>
        legacyHeightmapSurfacePreparer_;
};

} // namespace earth_engine
