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
    size_t classifiedContent = 0;
    size_t classifiedTerrainAvailabilityUpsample = 0;
    size_t classifiedRasterDetailUpsample = 0;
    size_t issuedContent = 0;
    size_t issuedTerrainAvailabilityUpsample = 0;
    size_t issuedRasterDetailUpsample = 0;
    bool blockedByInflight = false;
    /// 差集回收真取消的陈旧在飞请求数(连续 N 个处理帧无人再要)。
    size_t cancelledStaleInflight = 0;
    // Per-reason drop counters for the request loop. A stalled load queue
    // (entries present, nothing issued) is diagnosed by which counter eats
    // the requests; surfaced on the debug overlay.
    size_t skippedEmptyCacheKey = 0;
    size_t skippedAlreadyPending = 0;
    size_t skippedEmptyTile = 0;
    size_t skippedClassified = 0;
    size_t skippedUpsampleSourceNotReady = 0;
    size_t skippedUpsampleNoContentSource = 0;
    size_t skippedUpsampleWorkerCapacity = 0;
    size_t skippedDispatch = 0;
    size_t skippedNoContentProvider = 0;
    size_t stoppedAtDispatch = 0;
    // cullRequestsWhileMoving:本帧因相机运动过快(相对瓦片尺寸)被延迟的请求数。
    size_t skippedMotionCull = 0;
};

struct TileLoadedContent {
    static TileLoadedContent fromContentResult(
        TileContentLoadResult&& result) {
        TileLoadedContent content;
        content.gltfModel = std::move(result.gltfModel);
        content.contentTransform = result.contentTransform;
        content.metadata = std::move(result.metadata);
        content.terrainRenderContent = result.terrainRenderContent;
        content.terrainRenderSource = result.terrainRenderSource;
        content.retainedHeightmap = std::move(result.retainedHeightmap);
        return content;
    }

    std::unique_ptr<GltfModel> gltfModel;
    bool terrainRenderContent = false;
    TileTerrainRenderSource terrainRenderSource =
        TileTerrainRenderSource::Generic;
    Mat4 contentTransform = Mat4::identity();
    TileLoadResultMetadata metadata;
    // Phase 2c Stage B:worker 解码的原始高度图,GL 线程建高度纹理后消费。
    std::unique_ptr<DecodedHeightmap> retainedHeightmap;

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
            return loadResult;
        }
        if (isSuccessfulTileLoadStatus(result.status)) {
            loadResult.content = TileLoadedContent::fromContentResult(
                std::move(result));
            if (loadResult.content.terrainRenderContent &&
                !loadResult.content.satisfiesContentTerrainPayloadContract()) {
                loadResult.status = TileLoadStatus::Failed;
                loadResult.content = {};
                return loadResult;
            }
            return loadResult;
        }
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
