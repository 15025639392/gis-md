#pragma once

#include "TileKey.h"
#include "TileLoadPriorityPolicy.h"
#include "TileLoadResultMetadata.h"
#include "../content/GltfContentProvider.h"
#include "../core/math/Mat4.h"

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
    // Per-reason drop counters for the request loop. A stalled load queue
    // (entries present, nothing issued) is diagnosed by which counter eats
    // the requests; surfaced on the debug overlay.
    size_t skippedEmptyCacheKey = 0;
    size_t skippedAlreadyPending = 0;
    size_t skippedEmptyTile = 0;
    size_t skippedClassified = 0;
    size_t skippedUpsampleSourceNotReady = 0;
    size_t skippedUpsampleNoContentSource = 0;
    size_t skippedDispatch = 0;
    size_t skippedNoContentProvider = 0;
    size_t stoppedAtDispatch = 0;
};

struct TileLoadedContent {
    static TileLoadedContent fromContentResult(
        TileContentLoadResult&& result) {
        TileLoadedContent content;
        content.gltfModel = std::move(result.gltfModel);
        content.contentTransform = result.contentTransform;
        content.metadata = std::move(result.metadata);
        content.terrainRenderContent = result.terrainRenderContent;
        content.quantizedMeshAvailabilityUpdates =
            std::move(result.quantizedMeshAvailabilityUpdates);
        return content;
    }

    std::unique_ptr<GltfModel> gltfModel;
    bool terrainRenderContent = false;
    Mat4 contentTransform = Mat4::identity();
    TileLoadResultMetadata metadata;
    std::vector<QuantizedMeshAvailabilityUpdate>
        quantizedMeshAvailabilityUpdates;

    bool satisfiesContentTerrainPayloadContract() const {
        if (!terrainRenderContent || gltfModel == nullptr) {
            return false;
        }
        const bool modelHasDetails = !gltfModel->rasterOverlayDetails.empty();
        const bool metadataHasDetails =
            metadata.rasterOverlayDetails.has_value() &&
            !metadata.rasterOverlayDetails->empty();
        if (!modelHasDetails && !metadataHasDetails) {
            return true;
        }
        return modelHasDetails && metadataHasDetails &&
               metadata.rasterOverlayDetails->equalsExact(
                   gltfModel->rasterOverlayDetails);
    }

    bool hasGltfTerrainPayload() const {
        return terrainRenderContent && gltfModel != nullptr;
    }

    bool hasRenderablePayload() const {
        return gltfModel != nullptr;
    }
};

enum class TileLoadDomain {
    TerrainContent,
    Content
};

inline bool isContentLoadDomain(TileLoadDomain domain) {
    return domain == TileLoadDomain::Content ||
           domain == TileLoadDomain::TerrainContent;
}

struct TileLoadResult {
    static TileLoadResult createTerminal(TileLoadStatus status) {
        TileLoadResult loadResult;
        loadResult.status = status;
        return loadResult;
    }

    static TileLoadResult createTerminal(
        TileLoadStatus status,
        TileLoadResultMetadata metadata) {
        TileLoadResult loadResult = createTerminal(status);
        if (isSuccessfulTileLoadStatus(status)) {
            loadResult.content.metadata = std::move(metadata);
        }
        return loadResult;
    }

    static TileLoadResult createFailedPreservingAvailability(
        TileLoadResult&& result) {
        TileLoadResult failed = createTerminal(TileLoadStatus::Failed);
        if (!result.quantizedMeshAvailabilityUpdates.empty()) {
            failed.quantizedMeshAvailabilityUpdates =
                std::move(result.quantizedMeshAvailabilityUpdates);
        } else {
            failed.quantizedMeshAvailabilityUpdates =
                std::move(result.content.quantizedMeshAvailabilityUpdates);
        }
        return failed;
    }

    static TileLoadResult createRenderableGltfTerrain(
        std::unique_ptr<GltfModel> model,
        TileLoadResultMetadata metadata = {},
        Mat4 contentTransform = Mat4::identity()) {
        return fromContentResult(TileContentLoadResult::renderTerrain(
            std::move(model),
            std::move(metadata),
            contentTransform));
    }

    static TileLoadResult fromContentResult(TileContentLoadResult&& result) {
        TileLoadResult loadResult;
        loadResult.status = result.status;
        if (result.status == TileLoadStatus::Renderable &&
            result.gltfModel == nullptr) {
            loadResult.status = TileLoadStatus::Failed;
            loadResult.quantizedMeshAvailabilityUpdates =
                std::move(result.quantizedMeshAvailabilityUpdates);
            return loadResult;
        }
        if (isSuccessfulTileLoadStatus(result.status)) {
            loadResult.content = TileLoadedContent::fromContentResult(
                std::move(result));
            if (loadResult.content.terrainRenderContent &&
                !loadResult.content.satisfiesContentTerrainPayloadContract()) {
                auto availabilityUpdates = std::move(
                    loadResult.content.quantizedMeshAvailabilityUpdates);
                loadResult.status = TileLoadStatus::Failed;
                loadResult.content = {};
                loadResult.quantizedMeshAvailabilityUpdates =
                    std::move(availabilityUpdates);
                return loadResult;
            }
            loadResult.quantizedMeshAvailabilityUpdates.clear();
            return loadResult;
        }
        loadResult.quantizedMeshAvailabilityUpdates =
            std::move(result.quantizedMeshAvailabilityUpdates);
        return loadResult;
    }

    bool hasRenderableContent() const {
        return content.hasRenderablePayload();
    }

    bool isRenderableContentTerrain() const {
        return status == TileLoadStatus::Renderable &&
               content.satisfiesContentTerrainPayloadContract();
    }

    bool shouldUpload() const {
        return status == TileLoadStatus::Renderable &&
               hasRenderableContent();
    }

    bool shouldApplyTerminalMetadata() const {
        return isSuccessfulTileLoadStatus(status) &&
               (status == TileLoadStatus::Empty ||
                status == TileLoadStatus::External);
    }

    TileLoadStatus status = TileLoadStatus::Failed;
    TileLoadedContent content;
    std::vector<QuantizedMeshAvailabilityUpdate>
        quantizedMeshAvailabilityUpdates;
};

struct PendingTileLoad {
    PendingTileLoad() = default;
    PendingTileLoad(TileLoadDomain domain_,
                    TileKey key_,
                    std::string cacheKey_,
                    TileLoadPriorityGroup group_,
                    double priority_,
                    TileLoadStatus status_)
        : PendingTileLoad(
              domain_,
              std::move(key_),
              std::move(cacheKey_),
              group_,
              priority_,
              TileLoadResult::createTerminal(status_)) {}
    PendingTileLoad(TileLoadDomain domain_,
                    TileKey key_,
                    std::string cacheKey_,
                    TileLoadPriorityGroup group_,
                    double priority_,
                    TileLoadResult result_)
        : domain(domain_),
          key(std::move(key_)),
          cacheKey(std::move(cacheKey_)),
          group(group_),
          priority(priority_),
          result(std::move(result_)) {}

    TileLoadedContent& content() {
        return result.content;
    }

    const TileLoadedContent& content() const {
        return result.content;
    }

    TileLoadDomain domain = TileLoadDomain::Content;
    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TileLoadResult result = TileLoadResult::createTerminal(
        TileLoadStatus::Failed);
};

} // namespace earth_engine
