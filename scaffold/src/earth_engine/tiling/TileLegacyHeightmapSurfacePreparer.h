#pragma once

#include "TileLoadTypes.h"

#include <functional>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class TileContentLifecycleManager;
struct TileKey;
struct TilesetTile;

class TileLegacyHeightmapSurfacePreparer {
public:
    using MarkDirty = std::function<void()>;
    using QueueTileLoad = std::function<void(
        const TileKey&,
        TileLoadPriorityGroup,
        double)>;
    using PrepareRenderableTile = std::function<void(TilesetTile&)>;

    TileLegacyHeightmapSurfacePreparer(
        TileContentLifecycleManager& contentLifecycle,
        RenderDevice* device,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        bool hasTerrainQuadtree,
        MarkDirty markDirty,
        QueueTileLoad queueTileLoad,
        PrepareRenderableTile prepareRenderableTile);

    void prepareRenderableTile(TilesetTile& tile);
    bool prepareUpsampleSourceTile(TilesetTile& tile, double priority);

private:
    TileContentLifecycleManager& contentLifecycle_;
    RenderDevice* device_ = nullptr;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays_;
    bool hasTerrainQuadtree_ = false;
    MarkDirty markDirty_;
    QueueTileLoad queueTileLoad_;
    PrepareRenderableTile prepareRenderableTile_;
};

} // namespace earth_engine
