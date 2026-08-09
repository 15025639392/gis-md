#pragma once

#include "GpuUploadQueue.h"
#include "TileLoadQueue.h"
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
struct TilesetTestAccess;
struct TilesetTile;

struct TileContentRuntimeRequestFrame {
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>&
        tiles;
    TilesetContentProvider* contentProvider = nullptr;
    RenderDevice* device = nullptr;
    IPrepareRendererResources* pPrepRenderer = nullptr;
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
    GpuUploadQueue* gpuUploadQueue = nullptr;  // async CPU→GPU pipeline
    uint64_t frameNumber = 0;
    uint32_t maximumSimultaneousTileLoads = 0;
    double mainThreadLoadingTimeLimit = 0.0;
    double currentFrameTimeSeconds = 0.0;
    uint32_t smoothedMainThreadUploadLimit = 0;
    /// 幽灵网格摘除许可(见 TileRenderContentState::releaseGhostTerrainGeometry)。
    /// = decoupleImageryFromGeometry —— 关掉它时 GltfTerrainUpsampler 要读**父
    /// 瓦片**的顶点造 z13+ 子瓦片,而父瓦片(z12,自有高度图、fade=1)恰好是摘除
    /// 的目标,摘了就产出空网格。生产默认 true,该分支不跑。
    bool allowGhostGeometryRelease = false;
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
    TileLoadRequestOutcome requestMissingTiles(
        TileLoadQueue& loadQueue,
        const TileContentRuntimeRequestFrame& frame,
        FrameResourceBudget* budget);
    bool processPendingUploads(
        const TileContentRuntimeUploadFrame& frame,
        bool interactionActive,
        bool resourceSmoothingActive,
        FrameResourceBudget* budget);
    bool drainGpuUploadQueue(
        const TileContentRuntimeUploadFrame& frame,
        FrameResourceBudget* budget,
        uint32_t maxUploadsPerFrame);
    void markResourcesDirty();
    void markTileResourcesDirty(TilesetTile& tile);

private:
    friend struct TilesetTestAccess;

    TileContentLifecycleManager& lifecycle_;
    TileContentAccess& contentAccess_;
    TileMeshPreparationManager& meshPreparation_;
    TileContentResourceInvalidator& resourceInvalidator_;
};

} // namespace earth_engine
