#pragma once

#include "TileContentRuntimeState.h"
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

    /// Determines whether the frame materializer should attempt to create
    /// terrain quadtree children for the given tile. Returns false when the
    /// tile is at max zoom, is itself an upsampled child (cannot refine
    /// further), or the scene has no terrain quadtree. Callers should still
    /// handle the availability-boundary retry-later case separately.
    static bool shouldCreateTerrainChildren(
        int tileZoom,
        int maxZoom,
        bool hasTerrainQuadtree,
        bool isTerrainAvailabilityUpsample) {
        if (tileZoom >= maxZoom) {
            return false;
        }
        if (isTerrainAvailabilityUpsample) {
            return false;
        }
        if (!hasTerrainQuadtree) {
            return false;
        }
        return true;
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
