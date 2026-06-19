#include "TileID.h"

namespace earth_engine {

std::string TileIdUtilities::createTileIdString(const TileID& tileID) {
    struct Operation {
        std::string operator()(const std::string& url) const {
            return url;
        }

        std::string operator()(const TileKey& quadtreeTileID) const {
            return std::string("L") + std::to_string(quadtreeTileID.z) + "-" +
                   "X" + std::to_string(quadtreeTileID.x) + "-" +
                   "Y" + std::to_string(quadtreeTileID.y);
        }

        std::string operator()(const OctreeTileID& octreeTileID) const {
            return std::string("L") + std::to_string(octreeTileID.level) + "-" +
                   "X" + std::to_string(octreeTileID.x) + "-" +
                   "Y" + std::to_string(octreeTileID.y) + "-" +
                   "Z" + std::to_string(octreeTileID.z);
        }

        std::string operator()(const UpsampledQuadtreeNode& node) const {
            return std::string("upsampled-L") + std::to_string(node.tileID.z) +
                   "-" + "X" + std::to_string(node.tileID.x) + "-" +
                   "Y" + std::to_string(node.tileID.y);
        }
    };

    return std::visit(Operation{}, tileID);
}

bool TileIdUtilities::isLoadable(const TileID& tileID) {
    const std::string* url = std::get_if<std::string>(&tileID);
    return url ? !url->empty() : true;
}

} // namespace earth_engine
