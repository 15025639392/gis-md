#include "TileSelectionHistory.h"

#include "DirectRasterMapping.h"
#include "TilesetTile.h"

namespace earth_engine {

bool TileSelectionHistory::wasRenderedLastFrame(const TilesetTile& tile) {
    return tile.selectionFrameState.previousSelectionState ==
           TileSelectionState::Rendered;
}

bool TileSelectionHistory::childWasRefinedLastFrame(const TilesetTile& tile) {
    for (const TilesetTile* child : tile.children) {
        if (!child) {
            continue;
        }
        if (child->selectionFrameState.previousSelectionState ==
            TileSelectionState::Refined) {
            return true;
        }
    }
    return false;
}

bool TileSelectionHistory::anyDescendantWasRenderedLastFrame(
    const TilesetTile& tile) {
    for (const TilesetTile* child : tile.children) {
        if (!child) {
            continue;
        }
        if (child->selectionFrameState.previousSelectionState ==
                TileSelectionState::Rendered ||
            anyDescendantWasRenderedLastFrame(*child)) {
            return true;
        }
    }
    return false;
}

} // namespace earth_engine
