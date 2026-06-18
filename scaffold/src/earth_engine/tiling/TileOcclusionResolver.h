#pragma once

#include "TileOcclusionState.h"
#include "TilesetTile.h"

namespace earth_engine {

class TileOcclusionResolver {
public:
    template <typename CheckSingleTileOcclusionFn>
    static TileOcclusionState check(
        const TilesetTile& tile,
        CheckSingleTileOcclusionFn&& checkSingleTileOcclusion) {
        const TileOcclusionState tileOcclusion =
            checkSingleTileOcclusion(tile);
        if (tileOcclusion == TileOcclusionState::Occluded ||
            tileOcclusion == TileOcclusionState::OcclusionUnavailable ||
            tile.children.empty()) {
            return tileOcclusion;
        }

        // If the tile is visible, children can provide a tighter finite union.
        // Unconditional-refine children do not form a reliable finite union, so
        // keep the parent visible in that case.
        for (const TilesetTile* child : tile.children) {
            if (!child || child->unconditionallyRefine) {
                return TileOcclusionState::NotOccluded;
            }
        }

        bool anyUnavailable = false;
        for (const TilesetTile* child : tile.children) {
            const TileOcclusionState childOcclusion =
                checkSingleTileOcclusion(*child);
            if (childOcclusion == TileOcclusionState::NotOccluded) {
                return TileOcclusionState::NotOccluded;
            }
            if (childOcclusion == TileOcclusionState::OcclusionUnavailable) {
                anyUnavailable = true;
            }
        }

        return anyUnavailable
            ? TileOcclusionState::OcclusionUnavailable
            : TileOcclusionState::Occluded;
    }
};

} // namespace earth_engine
