#include "TilePendingUploadCompletion.h"

#include "TileLoadLifecycle.h"

namespace earth_engine {

void TilePendingUploadCompletion::eraseTerrainUpload(
    TileLoadLifecycle& lifecycle,
    const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(lifecycle.mutex());
    lifecycle.pendingLoads().eraseTerrainUploadKey(cacheKey);
}

void TilePendingUploadCompletion::eraseContentUpload(
    TileLoadLifecycle& lifecycle,
    const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(lifecycle.mutex());
    lifecycle.pendingLoads().eraseContentUploadKey(cacheKey);
}

} // namespace earth_engine
