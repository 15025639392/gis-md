#pragma once

#include "TileLoadTypes.h"

#include <utility>
#include <vector>

namespace earth_engine {

struct TileAvailabilityUpdateCommitter {
    static std::vector<QuantizedMeshAvailabilityUpdate>
    extractTerrainAvailabilityUpdates(
        TileLoadDomain domain,
        TileLoadResult& result,
        TilesetContentProvider* contentProvider) {
        if (domain == TileLoadDomain::TerrainContent &&
            contentProvider &&
            contentProvider->providesTerrainQuadtree() &&
            result.status == TileLoadStatus::Failed &&
            !result.quantizedMeshAvailabilityUpdates.empty()) {
            return std::move(result.quantizedMeshAvailabilityUpdates);
        }
        return {};
    }

    static void applyTerrainAvailabilityUpdates(
        const TileLoadedContent& content,
        TilesetContentProvider* contentProvider) {
        if (contentProvider &&
            contentProvider->providesTerrainQuadtree() &&
            content.satisfiesContentTerrainPayloadContract() &&
            !content.quantizedMeshAvailabilityUpdates.empty()) {
            contentProvider->applyTerrainAvailabilityUpdates(
                content.quantizedMeshAvailabilityUpdates);
        }
    }
};

} // namespace earth_engine
