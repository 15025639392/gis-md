#pragma once

#include "TileKey.h"
#include "TileLoadPriorityPolicy.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

#include <limits>
#include <memory>
#include <string>
#include <utility>

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
    PendingTerrainUpload() = default;
    PendingTerrainUpload(TileKey key_,
                         std::string cacheKey_,
                         TileLoadPriorityGroup group_,
                         double priority_,
                         std::unique_ptr<DecodedHeightmap> heightmap_,
                         std::unique_ptr<SurfaceTileMesh> surfaceMesh_ = nullptr,
                         std::vector<QuantizedMeshAvailabilityUpdate>
                             availabilityUpdates_ = {})
        : key(std::move(key_)),
          cacheKey(std::move(cacheKey_)),
          group(group_),
          priority(priority_),
          heightmap(std::move(heightmap_)),
          surfaceMesh(std::move(surfaceMesh_)),
          quantizedMeshAvailabilityUpdates(
              std::move(availabilityUpdates_)) {}

    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    std::unique_ptr<DecodedHeightmap> heightmap;
    std::unique_ptr<SurfaceTileMesh> surfaceMesh;
    std::vector<QuantizedMeshAvailabilityUpdate>
        quantizedMeshAvailabilityUpdates;
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
