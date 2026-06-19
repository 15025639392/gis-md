#pragma once

#include "TileSelectionTraversalContext.h"
#include "TileTraversalDetails.h"

#include <cstdint>

namespace earth_engine {

struct SelectorFrame;
struct TilesetTile;

class TileSelectionTraversalExecutor {
public:
    static TileTraversalDetails visitTileIfNeeded(
        TileSelectionTraversalContext& context,
        TilesetTile& tile,
        const SelectorFrame& selectorFrame,
        uint32_t depth,
        bool ancestorMeetsSse);

private:
    static TileTraversalDetails visitTile(
        TileSelectionTraversalContext& context,
        TilesetTile& tile,
        const SelectorFrame& selectorFrame,
        uint32_t depth,
        bool meetsSse,
        bool ancestorMeetsSse,
        double tilePriority,
        double tileSse);
};

} // namespace earth_engine
