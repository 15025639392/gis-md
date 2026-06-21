#pragma once

#include <string>

namespace earth_engine {

class TileLoadLifecycle;

struct TilePendingUploadCompletion {
    static void eraseUpload(
        TileLoadLifecycle& lifecycle,
        const std::string& cacheKey);
};

} // namespace earth_engine
