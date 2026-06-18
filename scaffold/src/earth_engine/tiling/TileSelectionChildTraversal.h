#pragma once

#include "TileTraversalDetails.h"

#include <utility>
#include <vector>

namespace earth_engine {

struct TilesetTile;

struct TileSelectionChildTraversal {
    template <typename VisitChild>
    static TileTraversalDetails visitChildren(
        const std::vector<TilesetTile*>& children,
        VisitChild&& visitChild) {
        TileTraversalDetails traversalDetails;
        for (TilesetTile* child : children) {
            if (!child) continue;
            const TileTraversalDetails childDetails = visitChild(*child);
            TileTraversalDetailsPolicy::mergeChild(
                traversalDetails,
                childDetails);
        }
        return traversalDetails;
    }
};

} // namespace earth_engine
