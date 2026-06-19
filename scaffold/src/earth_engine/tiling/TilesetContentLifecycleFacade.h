#pragma once

#include "TileLoadTypes.h"

#include <vector>

namespace earth_engine {

class FrameResourceBudget;
class Tileset;

class TilesetContentLifecycleFacade {
public:
    static TileLoadRequestOutcome requestMissingTiles(
        Tileset& tileset,
        const std::vector<TileLoadRequest>& loadRequests,
        FrameResourceBudget* budget = nullptr);
    static bool processPendingUploads(Tileset& tileset,
                                      bool interactionActive,
                                      bool resourceSmoothingActive,
                                      FrameResourceBudget* budget = nullptr);
    static void markTileResourcesDirty(Tileset& tileset);
};

} // namespace earth_engine
