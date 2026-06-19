#include "TilesetRenderFrameFacade.h"

#include "TileCacheOwnershipManager.h"
#include "TileRenderCommandManager.h"
#include "TileRenderFrameContext.h"
#include "Tileset.h"
#include "TilesetRenderFrameExecutor.h"

namespace earth_engine {

void TilesetRenderFrameFacade::buildRenderCommands(
    Tileset& tileset,
    Renderer& renderer,
    RenderCommandList& commands) {
    ++tileset.frameNumber_;
    tileset.renderCommands_.beginFrame(
        tileset.frameNumber_,
        tileset.generation_,
        tileset.currentFrameTimeSeconds_,
        tileset.options_.maximumScreenSpaceError);
    TilesetRenderFrameExecutor::buildRenderCommands(
        TileRenderFrameContext{
            TileRenderFrameCoordinatorInput{
                tileset.tilePlan_,
                tileset.tileRegistry_.tiles(),
                tileset.contentCache_.unloadQueue(),
                tileset.rasterOverlays_,
                tileset.contentCache_.cacheBytesDirty(),
                tileset.frameNumber_,
                tileset.lastCameraPosition_,
                tileset.options_.fogDensityTable,
                tileset.selectionCounters_.fogCulled,
                tileset.resourceSmoothingActiveForFrame_,
                tileset.interactionActiveForFrame_,
                tileset.contentCache_.totalBytesUsed(),
                tileset.options_.maximumCachedBytes},
            tileset.contentAccess_,
            tileset.renderCommands_,
            tileset.cacheOwnership_},
        renderer,
        commands);
}

} // namespace earth_engine
