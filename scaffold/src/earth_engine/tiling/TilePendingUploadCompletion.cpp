#include "TilePendingUploadCompletion.h"

#include "TileLoadLifecycle.h"

namespace earth_engine {

void TilePendingUploadCompletion::eraseUpload(
    TileLoadLifecycle& lifecycle,
    const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(lifecycle.mutex());
    lifecycle.pendingLoads().eraseUploadKey(cacheKey);
}

} // namespace earth_engine
