#pragma once

#include "TileLoadTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class FrameResourceBudget;
class ActivatedRasterOverlay;
class IPrepareRendererResources;
class RenderDevice;
class TileContentAccess;
class TileContentLifecycleManager;
class TileContentResourceInvalidator;
class TileMeshPreparationManager;
class TilesetContentProvider;
struct TilesetTile;

struct TileContentRuntimeRequestFrame {
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>&
        tiles;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    uint64_t frameNumber = 0;
    uint32_t maximumSimultaneousTileLoads = 0;
    double mainThreadLoadingTimeLimit = 0.0;
    double currentFrameTimeSeconds = 0.0;
    uint32_t smoothedMainThreadUploadLimit = 0;
};

struct TileContentRuntimeUploadFrame {
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    IPrepareRendererResources* pPrepRenderer = nullptr;
    uint64_t frameNumber = 0;
    uint32_t maximumSimultaneousTileLoads = 0;
    double mainThreadLoadingTimeLimit = 0.0;
    double currentFrameTimeSeconds = 0.0;
    uint32_t smoothedMainThreadUploadLimit = 0;
};

class TileContentRuntime {
public:
    TileContentRuntime(
        TileContentLifecycleManager& lifecycle,
        TileContentAccess& contentAccess,
        TileMeshPreparationManager& meshPreparation,
        TileContentResourceInvalidator& resourceInvalidator);

    TileLoadRequestOutcome requestMissingTiles(
        const std::vector<TileLoadRequest>& loadRequests,
        const TileContentRuntimeRequestFrame& frame,
        FrameResourceBudget* budget);
    bool processPendingUploads(
        const TileContentRuntimeUploadFrame& frame,
        bool interactionActive,
        bool resourceSmoothingActive,
        FrameResourceBudget* budget);
    void markResourcesDirty();

private:
    TileContentLifecycleManager& lifecycle_;
    TileContentAccess& contentAccess_;
    TileMeshPreparationManager& meshPreparation_;
    TileContentResourceInvalidator& resourceInvalidator_;
};

} // namespace earth_engine
