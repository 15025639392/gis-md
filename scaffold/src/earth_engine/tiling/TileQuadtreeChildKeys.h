#pragma once

#include "TileKey.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

struct TileQuadtreeChildKeys {
    static std::vector<TileKey> terrainChildren(const TileKey& parent) {
        const int64_t childZ = static_cast<int64_t>(parent.z) + 1;
        const int64_t childX = static_cast<int64_t>(parent.x) * 2;
        const int64_t childY = static_cast<int64_t>(parent.y) * 2;
        if (!canRepresentTileCoordinate(childZ) ||
            !canRepresentTileCoordinate(childX + 1) ||
            !canRepresentTileCoordinate(childY + 1)) {
            return {};
        }

        std::vector<TileKey> children;
        children.reserve(4);
        const bool yDown = terrainSchemeYDirectionDown(parent.schemeId);
        for (int row = 0; row < 2; ++row) {
            const int dy = yDown ? 1 - row : row;
            for (int dx = 0; dx < 2; ++dx) {
                TileKey child{
                    parent.schemeId,
                    static_cast<int>(childZ),
                    static_cast<int>(childX + dx),
                    static_cast<int>(childY + dy)};
                if (parent.schemeId == "Geographic-TMS" &&
                    isOutOfRangeGeographicTmsChild(child)) {
                    continue;
                }
                children.push_back(child);
            }
        }
        return children;
    }

    static bool terrainSchemeYDirectionDown(const std::string& schemeId) {
        return schemeId == "XYZ-WebMercator" ||
               schemeId == "OpenGlobus-Earth";
    }

    static std::optional<int64_t> geographicTmsXCount64(int z) {
        if (z < 0 || z >= 62) {
            return std::nullopt;
        }
        return int64_t{2} << z;
    }

    static std::optional<int64_t> geographicTmsYCount64(int z) {
        if (z < 0 || z >= 63) {
            return std::nullopt;
        }
        return int64_t{1} << z;
    }

    static bool isOutOfRangeGeographicTmsChild(const TileKey& key) {
        if (key.x < 0 || key.y < 0) {
            return true;
        }
        const std::optional<int64_t> xCount =
            geographicTmsXCount64(key.z);
        const std::optional<int64_t> yCount =
            geographicTmsYCount64(key.z);
        if (!xCount || !yCount) {
            return true;
        }
        return static_cast<int64_t>(key.x) >= *xCount ||
               static_cast<int64_t>(key.y) >= *yCount;
    }

    static bool canRepresentTileCoordinate(int64_t value) {
        return value >= 0 &&
               value <=
                   static_cast<int64_t>(std::numeric_limits<int>::max());
    }
};

} // namespace earth_engine
