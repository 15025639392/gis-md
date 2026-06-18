#pragma once

#include <string>

namespace earth_engine {

class TileLoadLifecycle;

struct TilePendingUploadCompletion {
    static void eraseTerrainUpload(
        TileLoadLifecycle& lifecycle,
        const std::string& cacheKey);
    static void eraseContentUpload(
        TileLoadLifecycle& lifecycle,
        const std::string& cacheKey);
};

} // namespace earth_engine
