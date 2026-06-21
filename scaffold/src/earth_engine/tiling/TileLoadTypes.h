#pragma once

#include "TileKey.h"
#include "TileLoadPriorityPolicy.h"
#include "TileLoadResultMetadata.h"
#include "../content/GltfContentProvider.h"
#include "../core/math/Mat4.h"
#include "../providers/TerrainProvider.h"

#include <limits>
#include <memory>
#include <optional>
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

struct TileLoadedContent {
    static TileLoadedContent fromContentResult(
        TileContentLoadResult&& result) {
        TileLoadedContent content;
        content.gltfModel = std::move(result.gltfModel);
        content.contentTransform = result.contentTransform;
        content.metadata = std::move(result.metadata);
        return content;
    }

    std::unique_ptr<DecodedHeightmap> heightmap;
    std::unique_ptr<SurfaceTileMesh> surfaceMesh;
    std::unique_ptr<GltfModel> gltfModel;
    Mat4 contentTransform = Mat4::identity();
    TileLoadResultMetadata metadata;
    std::vector<QuantizedMeshAvailabilityUpdate>
        quantizedMeshAvailabilityUpdates;
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
                             availabilityUpdates_ = {},
                         TileLoadResultMetadata metadata_ = {})
        : key(std::move(key_)),
          cacheKey(std::move(cacheKey_)),
          group(group_),
          priority(priority_) {
        content.heightmap = std::move(heightmap_);
        content.surfaceMesh = std::move(surfaceMesh_);
        content.metadata = std::move(metadata_);
        content.quantizedMeshAvailabilityUpdates =
            std::move(availabilityUpdates_);
    }

    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TileLoadedContent content;
};

struct PendingTerrainTerminalResult {
    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TileLoadStatus status = TileLoadStatus::Failed;
};

struct PendingContentUpload {
    PendingContentUpload() = default;
    PendingContentUpload(TileKey key_,
                         std::string cacheKey_,
                         TileLoadPriorityGroup group_,
                         double priority_,
                         TileContentLoadResult result_)
        : key(std::move(key_)),
          cacheKey(std::move(cacheKey_)),
          group(group_),
          priority(priority_) {
        content = TileLoadedContent::fromContentResult(std::move(result_));
    }

    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TileLoadedContent content;
};

struct PendingContentTerminalResult {
    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TileLoadStatus status = TileLoadStatus::Failed;
};

} // namespace earth_engine
