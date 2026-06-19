#pragma once

#include "TileCacheUnloadCoordinator.h"
#include "TileContentCacheManager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

class IPrepareRendererResources;
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
        bool& resourceSmoothingActiveForFrame,
        int64_t& maximumCachedBytes,
        double& tileCacheUnloadTimeLimit);

    void updateTotalBytesUsed();
    void markEligibleForUnloading(const std::string& key);
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
    bool& resourceSmoothingActiveForFrame_;
    int64_t& maximumCachedBytes_;
    double& tileCacheUnloadTimeLimit_;
};

} // namespace earth_engine
