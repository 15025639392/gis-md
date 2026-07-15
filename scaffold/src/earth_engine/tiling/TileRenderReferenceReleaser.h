#pragma once

#include "TilesetTile.h"

#include <string>
#include <vector>

namespace earth_engine {

struct TileRenderReference {
    TilesetTile* tile = nullptr;
    std::string cacheKey;
    bool countedReference = false;
};

struct TileRenderReferenceReleaser {
    template <typename MarkEligibleFn>
    static void release(std::vector<TileRenderReference>& references,
                        MarkEligibleFn&& markEligible) {
        for (TileRenderReference& reference : references) {
            if (!reference.tile) {
                continue;
            }
            if (reference.countedReference) {
                reference.tile->removeReference();
            }
            markEligible(reference.tile, reference.cacheKey);
        }
        references.clear();
    }
};

} // namespace earth_engine
