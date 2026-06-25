#pragma once

#include "TileLoadTypes.h"

namespace earth_engine {

struct TileAvailabilityUpdateCommitter {
    static void applyTerrainAvailabilityUpdates(
        TileLoadDomain domain,
        TileLoadResult& result,
        TilesetContentProvider* contentProvider) {
        if (!contentProvider || !contentProvider->providesTerrainQuadtree()) {
            return;
        }

        if (domain == TileLoadDomain::TerrainContent &&
            result.status == TileLoadStatus::Failed &&
            !result.quantizedMeshAvailabilityUpdates.empty()) {
            contentProvider->applyTerrainAvailabilityUpdates(
                result.quantizedMeshAvailabilityUpdates);
            result.quantizedMeshAvailabilityUpdates.clear();
            return;
        }

        if (result.status == TileLoadStatus::Renderable &&
            result.content.satisfiesContentTerrainPayloadContract() &&
            !result.content.quantizedMeshAvailabilityUpdates.empty()) {
            contentProvider->applyTerrainAvailabilityUpdates(
                result.content.quantizedMeshAvailabilityUpdates);
        }
    }
};

} // namespace earth_engine
