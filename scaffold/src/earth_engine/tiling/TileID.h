#pragma once

#include "OctreeTilingScheme.h"
#include "TileKey.h"

#include <string>
#include <variant>

namespace earth_engine {

struct UpsampledQuadtreeNode {
    TileKey tileID;

    bool operator==(const UpsampledQuadtreeNode& rhs) const {
        return tileID == rhs.tileID;
    }

    bool operator!=(const UpsampledQuadtreeNode& rhs) const {
        return !(*this == rhs);
    }
};

using TileID = std::variant<
    std::string,
    TileKey,
    OctreeTileID,
    UpsampledQuadtreeNode>;

struct TileIdUtilities {
    static std::string createTileIdString(const TileID& tileID);
    static bool isLoadable(const TileID& tileID);
};

} // namespace earth_engine
