#pragma once

#include "TileLoadTypes.h"

namespace earth_engine {

struct TileLoadDomainPolicy {
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
