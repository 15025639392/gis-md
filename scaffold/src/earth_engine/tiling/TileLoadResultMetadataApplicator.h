#pragma once

#include "TileLoadResultMetadata.h"

namespace earth_engine {

struct TilesetTile;

class TileLoadResultMetadataApplicator {
public:
    static void apply(TilesetTile& tile, TileLoadResultMetadata&& metadata);
};

} // namespace earth_engine
