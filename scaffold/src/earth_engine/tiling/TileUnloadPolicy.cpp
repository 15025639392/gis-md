#include "TileUnloadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "TileUnloadQueue.h"

namespace earth_engine {

bool TileUnloadPolicy::isEligibleForContentUnloadQueue(
    const TilesetTile& tile) {
    if (tile.content.loadState == TileLoadState::Unloaded ||
        tile.content.loadState == TileLoadState::ContentLoading ||
        tile.content.loadState == TileLoadState::Unloading) {
        return false;
    }
    return true;
}

bool TileUnloadPolicy::hasReferencedDescendant(const TilesetTile& tile) {
    for (const TilesetTile* child : tile.children) {
        if (!child) continue;
        if (child->referenceCount() > 0 ||
            hasReferencedDescendant(*child)) {
            return true;
        }
    }
    return false;
}

bool TileUnloadPolicy::hasContentLoadingUpsampledDescendant(
    const TilesetTile& tile) {
    for (const TilesetTile* child : tile.children) {
        if (!child) continue;
        if (child->content.upsampledFromParent &&
            child->content.loadState == TileLoadState::ContentLoading) {
            return true;
        }
        if (hasContentLoadingUpsampledDescendant(*child)) {
            return true;
        }
    }
    return false;
}

bool TileUnloadPolicy::shouldDeferForReferences(
    const TilesetTile& tile,
    bool externalSubtreeHasActiveWork) {
    return tile.referenceCount() > 0 ||
           hasReferencedDescendant(tile) ||
           (tile.content.contentKind == TileContentKind::External &&
            externalSubtreeHasActiveWork);
}

bool TileUnloadPolicy::hasQueuedTileInState(
    const TileUnloadQueue& queue,
    const std::unordered_map<
        std::string,
        std::unique_ptr<TilesetTile>>& tiles,
    TileLoadState state) {
    for (const std::string& queuedKey : queue.keys()) {
        auto tileIt = tiles.find(queuedKey);
        if (tileIt != tiles.end() &&
            tileIt->second &&
            tileIt->second->content.loadState == state) {
            return true;
        }
    }
    return false;
}

void TileUnloadPolicy::releaseMainThreadRenderResourcesForProtectedUnload(
    TilesetTile& tile) {
    tile.content.renderContent.releaseGpuResources();
    tile.clearFrameRenderability();
}

void TileUnloadPolicy::releaseRasterOverlayReferences(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseReferences(pPrepRenderer);
}

void TileUnloadPolicy::releaseAndClearRasterOverlayReferences(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
}

void TileUnloadPolicy::releaseRenderContentResources(TilesetTile& tile) {
    tile.content.renderContent.clearRenderContent();
}

void TileUnloadPolicy::markContentUnloaded(TilesetTile& tile) {
    tile.markContentUnloaded();
}

} // namespace earth_engine
