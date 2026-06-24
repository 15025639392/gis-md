#pragma once

#include "TileLoadState.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

class IPrepareRendererResources;
class TileUnloadQueue;
struct TilesetTile;

struct TileUnloadPolicy {
    static bool isEligibleForContentUnloadQueue(const TilesetTile& tile);
    static bool hasReferencedDescendant(const TilesetTile& tile);
    static bool hasContentLoadingUpsampledDirectChild(const TilesetTile& tile);
    static bool shouldDeferForReferences(
        const TilesetTile& tile,
        bool externalSubtreeHasActiveWork);
    static bool hasQueuedTileInState(
        const TileUnloadQueue& queue,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        TileLoadState state);
    static void releaseMainThreadRenderResourcesForProtectedUnload(
        TilesetTile& tile);
    static void releaseRasterOverlayReferences(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer);
    static void releaseAndClearRasterOverlayReferences(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer);
    static void releaseRenderContentResources(TilesetTile& tile);
    static void markContentUnloaded(TilesetTile& tile);
};

} // namespace earth_engine
