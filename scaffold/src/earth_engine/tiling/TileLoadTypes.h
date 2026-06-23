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
    static TileLoadedContent fromTerrainResult(
        TerrainTileLoadResult&& result) {
        TileLoadedContent content;
        content.terrainPayloadKind = result.payloadKind;
        switch (result.payloadKind) {
            case TerrainTilePayloadKind::Heightmap:
                content.heightmap = std::move(result.heightmap);
                content.terrainRenderContent = content.heightmap != nullptr;
                break;
            case TerrainTilePayloadKind::None:
                break;
        }
        return content;
    }

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

    std::unique_ptr<DecodedHeightmap> heightmap;
    std::unique_ptr<GltfModel> gltfModel;
    TerrainTilePayloadKind terrainPayloadKind = TerrainTilePayloadKind::None;
    bool terrainRenderContent = false;
    Mat4 contentTransform = Mat4::identity();
    TileLoadResultMetadata metadata;
    std::vector<QuantizedMeshAvailabilityUpdate>
        quantizedMeshAvailabilityUpdates;

    bool hasTerrainPayload() const {
        return terrainPayloadKind != TerrainTilePayloadKind::None ||
               terrainRenderContent;
    }

    bool hasGltfTerrainPayload() const {
        return terrainRenderContent && gltfModel != nullptr;
    }

    bool hasRenderablePayload() const {
        switch (terrainPayloadKind) {
            case TerrainTilePayloadKind::Heightmap:
                return heightmap != nullptr;
            case TerrainTilePayloadKind::None:
                return gltfModel != nullptr;
        }
        return false;
    }
};

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

    static TileLoadResult createRenderable() {
        TileLoadResult loadResult;
        loadResult.status = TileLoadStatus::Renderable;
        return loadResult;
    }

    static TileLoadResult createRenderableTerrain(
        TileLoadResultMetadata metadata = {}) {
        TileLoadResult loadResult;
        loadResult.status = TileLoadStatus::Renderable;
        loadResult.content.metadata = std::move(metadata);
        return loadResult;
    }

    static TileLoadResult createRenderableHeightmapTerrain(
        std::unique_ptr<DecodedHeightmap> heightmap,
        TileLoadResultMetadata metadata = {}) {
        TileLoadResult loadResult = createRenderableTerrain(
            std::move(metadata));
        loadResult.content.heightmap = std::move(heightmap);
        if (loadResult.content.heightmap) {
            loadResult.content.terrainPayloadKind =
                TerrainTilePayloadKind::Heightmap;
        }
        return loadResult;
    }

    static TileLoadResult createRenderableGltfTerrain(
        std::unique_ptr<GltfModel> model,
        TileLoadResultMetadata metadata = {},
        Mat4 contentTransform = Mat4::identity()) {
        TileLoadResult loadResult = createRenderable();
        if (!metadata.rasterOverlayDetails && model) {
            metadata.rasterOverlayDetails = model->rasterOverlayDetails;
        }
        loadResult.content.metadata = std::move(metadata);
        loadResult.content.gltfModel = std::move(model);
        loadResult.content.terrainRenderContent =
            loadResult.content.gltfModel != nullptr;
        loadResult.content.contentTransform = contentTransform;
        return loadResult;
    }

    static TileLoadResult fromTerrainResult(TerrainTileLoadResult&& result) {
        TileLoadResult loadResult;
        loadResult.status = result.status;
        if (isSuccessfulTileLoadStatus(result.status)) {
            loadResult.content = TileLoadedContent::fromTerrainResult(
                std::move(result));
        }
        return loadResult;
    }

    static TileLoadResult fromContentResult(TileContentLoadResult&& result) {
        TileLoadResult loadResult;
        loadResult.status = result.status;
        if (isSuccessfulTileLoadStatus(result.status)) {
            loadResult.content = TileLoadedContent::fromContentResult(
                std::move(result));
        }
        return loadResult;
    }

    bool hasRenderableContent() const {
        return content.hasRenderablePayload();
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

enum class TileLoadDomain {
    Terrain,
    Content
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

    TileLoadDomain domain = TileLoadDomain::Terrain;
    TileKey key;
    std::string cacheKey;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
    double priority = 0.0;
    TileLoadResult result = TileLoadResult::createTerminal(
        TileLoadStatus::Failed);
};

} // namespace earth_engine
