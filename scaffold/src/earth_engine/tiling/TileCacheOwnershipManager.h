#pragma once

#include "TileCacheUnloadCoordinator.h"
#include "TileContentCacheManager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class IPrepareRendererResources;
class ActivatedRasterOverlay;
class GpuUploadQueue;
class TileContentLifecycleManager;
class TileLoadQueue;
struct TilesetTile;

class TileCacheOwnershipManager {
public:
    TileCacheOwnershipManager(
        TileContentCacheManager& contentCache,
        TileContentLifecycleManager& contentLifecycle,
        TileLoadQueue& loadQueue,
        std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        bool& resourceSmoothingActiveForFrame,
        int64_t& maximumCachedBytes,
        double& tileCacheUnloadTimeLimit,
        GpuUploadQueue* gpuUploadQueue = nullptr);

    void updateTotalBytesUsed();
    int64_t totalBytesUsed() const;
    bool shouldUnloadCachedBytes() const;
    void trimRasterCaches(bool cachePressure);
    void markEligibleForUnloading(const TilesetTile* tile, const std::string& key);
    void markIneligibleForUnloading(const std::string& key);
    void eraseTileIndexState(const std::string& key);
    void clearChildrenRecursively(TilesetTile* tile,
                                  IPrepareRendererResources* pPrepRenderer);
    TileCacheUnloadContentResult unloadTileContent(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer);
    void unloadCachedBytes(int64_t maximumCachedBytes,
                           IPrepareRendererResources* pPrepRenderer);
    void unloadConfiguredCachedBytes(IPrepareRendererResources* pPrepRenderer);

private:
    TileContentCacheManager& contentCache_;
    TileContentLifecycleManager& contentLifecycle_;
    TileLoadQueue& loadQueue_;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles_;
    std::vector<ActivatedRasterOverlay*>& rasterOverlays_;
    bool& resourceSmoothingActiveForFrame_;
    int64_t& maximumCachedBytes_;
    double& tileCacheUnloadTimeLimit_;
    GpuUploadQueue* gpuUploadQueue_ = nullptr;
};

} // namespace earth_engine
