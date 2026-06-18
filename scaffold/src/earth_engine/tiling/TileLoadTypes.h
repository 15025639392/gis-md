#pragma once

#include "TileKey.h"
#include "TileLoadPriorityPolicy.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

#include <limits>
#include <memory>
#include <string>

namespace earth_engine {

struct TileLoadRequest {
    TileKey key;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = std::numeric_limits<double>::max();
};

struct TileLoadRequestOutcome {
    size_t issued = 0;
    bool blockedByInflight = false;
};

struct PendingTerrainUpload {
    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    std::unique_ptr<DecodedHeightmap> heightmap;
};

struct PendingTerrainTerminalResult {
    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TerrainTileLoadStatus status = TerrainTileLoadStatus::Failed;
};

struct PendingContentUpload {
    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TileContentLoadResult result;
};

struct PendingContentTerminalResult {
    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TileContentLoadStatus status = TileContentLoadStatus::Failed;
};

} // namespace earth_engine
