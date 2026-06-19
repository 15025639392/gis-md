#include "TilesetContentLifecycleFacade.h"

#include "TileContentRuntime.h"
#include "Tileset.h"

namespace earth_engine {

TileLoadRequestOutcome TilesetContentLifecycleFacade::requestMissingTiles(
    Tileset& tileset,
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget) {
    return tileset.contentRuntime_.requestMissingTiles(
        loadRequests,
        tileset.makeContentRuntimeFrame(),
        budget);
}

bool TilesetContentLifecycleFacade::processPendingUploads(
    Tileset& tileset,
    bool interactionActive,
    bool resourceSmoothingActive,
    FrameResourceBudget* budget) {
    return tileset.contentRuntime_.processPendingUploads(
        tileset.makeContentRuntimeFrame(),
        interactionActive,
        resourceSmoothingActive,
        budget);
}

void TilesetContentLifecycleFacade::markTileResourcesDirty(Tileset& tileset) {
    tileset.contentRuntime_.markResourcesDirty();
}

} // namespace earth_engine
