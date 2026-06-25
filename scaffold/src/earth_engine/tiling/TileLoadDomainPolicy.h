#pragma once

#include "TileLoadTypes.h"

namespace earth_engine {

struct TileAvailabilityUpdateSelection {
    std::vector<QuantizedMeshAvailabilityUpdate>* updates = nullptr;
    bool clearAfterApply = false;
};

struct TileLoadDomainPolicy {
    static TileAvailabilityUpdateSelection availabilityUpdatesForDomain(
        TileLoadDomain domain,
        TileLoadResult& result) {
        if (domain == TileLoadDomain::TerrainContent &&
            result.status == TileLoadStatus::Failed &&
            !result.quantizedMeshAvailabilityUpdates.empty()) {
            return {&result.quantizedMeshAvailabilityUpdates, true};
        }
        if (result.status == TileLoadStatus::Renderable &&
            result.content.satisfiesContentTerrainPayloadContract() &&
            !result.content.quantizedMeshAvailabilityUpdates.empty()) {
            return {&result.content.quantizedMeshAvailabilityUpdates, false};
        }
        return {};
    }

    static bool shouldUploadForDomain(
        TileLoadDomain domain,
        const TileLoadResult& result) {
        if (domain == TileLoadDomain::TerrainContent) {
            return result.isRenderableContentTerrain();
        }
        return result.shouldUpload() &&
               (!result.content.terrainRenderContent ||
                result.content.satisfiesContentTerrainPayloadContract());
    }

    static bool shouldFailUploadForDomain(
        TileLoadDomain domain,
        const TileLoadResult& result) {
        return result.status == TileLoadStatus::Renderable &&
               !shouldUploadForDomain(domain, result);
    }

    static bool shouldApplyTerminalMetadataForDomain(
        TileLoadDomain domain,
        const TileLoadResult& result) {
        if (!result.shouldApplyTerminalMetadata()) {
            return false;
        }
        return domain != TileLoadDomain::TerrainContent ||
               result.status == TileLoadStatus::Empty;
    }

    static TileLoadResult normalizeForDomain(
        TileLoadDomain domain,
        TileLoadResult&& result) {
        if (shouldFailUploadForDomain(domain, result)) {
            return TileLoadResult::createFailedPreservingAvailability(
                std::move(result));
        }
        return std::move(result);
    }
};

} // namespace earth_engine
