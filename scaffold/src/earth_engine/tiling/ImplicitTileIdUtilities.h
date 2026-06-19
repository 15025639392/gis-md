#pragma once

#include "earth_engine/core/math/BoundingCylinderRegion.h"
#include "earth_engine/core/math/OrientedBoundingBox.h"
#include "OctreeTilingScheme.h"
#include "TileKey.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

class ImplicitTileIdUtilities {
public:
    ImplicitTileIdUtilities() = delete;

    static std::vector<TileKey> children(const TileKey& parent);
    static std::vector<OctreeTileID> children(const OctreeTileID& parent);

    static std::string resolveUrl(const std::string& baseUrl,
                                  const std::string& urlTemplate,
                                  const TileKey& tileID);
    static std::string resolveUrl(const std::string& baseUrl,
                                  const std::string& urlTemplate,
                                  const OctreeTileID& tileID);

    static OrientedBoundingBox computeBoundingVolume(
        const OrientedBoundingBox& rootBoundingVolume,
        const TileKey& tileID);
    static OrientedBoundingBox computeBoundingVolume(
        const OrientedBoundingBox& rootBoundingVolume,
        const OctreeTileID& tileID);
    static BoundingCylinderRegion computeBoundingVolume(
        const BoundingCylinderRegion& rootBoundingVolume,
        const TileKey& tileID);
    static BoundingCylinderRegion computeBoundingVolume(
        const BoundingCylinderRegion& rootBoundingVolume,
        const OctreeTileID& tileID);

    static std::optional<TileKey> parentId(const TileKey& tileID);
    static std::optional<OctreeTileID> parentId(const OctreeTileID& tileID);

    static TileKey subtreeRootId(uint32_t subtreeLevels,
                                 const TileKey& tileID);
    static OctreeTileID subtreeRootId(uint32_t subtreeLevels,
                                      const OctreeTileID& tileID);

    static TileKey absoluteTileIdToRelative(const TileKey& rootID,
                                            const TileKey& tileID);
    static OctreeTileID absoluteTileIdToRelative(
        const OctreeTileID& rootID,
        const OctreeTileID& tileID);

    static uint64_t mortonIndex(const TileKey& tileID);
    static uint64_t mortonIndex(const OctreeTileID& tileID);

    static uint64_t relativeMortonIndex(const TileKey& subtreeID,
                                        const TileKey& tileID);
    static uint64_t relativeMortonIndex(const OctreeTileID& subtreeID,
                                        const OctreeTileID& tileID);

    static double levelDenominator(uint32_t level);
};

} // namespace earth_engine
