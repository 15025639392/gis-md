#pragma once

#include "TileLoadTypes.h"

namespace earth_engine {

struct TileLoadDomainPolicy {
    static bool shouldFailUploadForDomain(
        TileLoadDomain domain,
        const TileLoadResult& result) {
        if (result.status != TileLoadStatus::Renderable) {
            return false;
        }
        if (domain == TileLoadDomain::TerrainContent) {
            return !result.content.satisfiesContentTerrainPayloadContract();
        }
        return result.content.terrainRenderContent &&
               !result.content.satisfiesContentTerrainPayloadContract();
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
