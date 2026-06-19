#include "TilesetQueryFacade.h"

#include "TileContentCacheManager.h"
#include "TileContentLifecycleManager.h"
#include "TileLoadQueue.h"
#include "Tileset.h"
#include "TilesetProviderDiagnosticsCollector.h"
#include "TilesetTileRegistry.h"
#include "TileFrameResourceBudgetPlanner.h"

namespace earth_engine {

uint32_t TilesetQueryFacade::maximumTransportActiveRequests(
    const Tileset& tileset) {
    return TilesetProviderDiagnosticsCollector::collect(
        tileset.terrainProvider_.get(),
        tileset.contentProvider_.get(),
        tileset.rasterOverlays_)
        .maximumTransportActiveRequests(
            TileFrameResourceBudgetPlanInput::
                kDefaultMaximumTransportActiveRequests);
}

TilesetLoadDiagnostics TilesetQueryFacade::loadDiagnostics(
    const Tileset& tileset) {
    TilesetLoadDiagnostics diagnostics = TileLoadDiagnosticsCollector::collect(
        tileset.loadQueue_,
        tileset.contentLifecycle_.loadLifecycle(),
        tileset.frameResourceBudget_,
        tileset.contentCache_.unloadQueue(),
        tileset.tileRegistry_.tiles());
    TilesetProviderDiagnosticsCollector::collect(
        tileset.terrainProvider_.get(),
        tileset.contentProvider_.get(),
        tileset.rasterOverlays_)
        .applyTo(diagnostics);
    return diagnostics;
}

} // namespace earth_engine
