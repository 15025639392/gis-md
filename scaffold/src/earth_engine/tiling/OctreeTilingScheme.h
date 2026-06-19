#pragma once

#include "../core/math/AxisAlignedBox.h"
#include "../core/math/Vec3.h"

#include <optional>

namespace earth_engine {

struct OctreeTileID {
    int level = 0;
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const OctreeTileID& rhs) const {
        return level == rhs.level && x == rhs.x && y == rhs.y && z == rhs.z;
    }

    bool operator!=(const OctreeTileID& rhs) const { return !(*this == rhs); }
};

class OctreeTilingScheme {
public:
    OctreeTilingScheme(AxisAlignedBox box,
                       int rootTilesX,
                       int rootTilesY,
                       int rootTilesZ);

    const AxisAlignedBox& box() const { return box_; }
    int rootTilesX() const { return rootTilesX_; }
    int rootTilesY() const { return rootTilesY_; }
    int rootTilesZ() const { return rootTilesZ_; }

    int tileCountX(int level) const;
    int tileCountY(int level) const;
    int tileCountZ(int level) const;

    std::optional<OctreeTileID> positionToTile(const Vec3& position,
                                               int level) const;
    AxisAlignedBox tileToBox(const OctreeTileID& tile) const;

private:
    AxisAlignedBox box_;
    int rootTilesX_ = 1;
    int rootTilesY_ = 1;
    int rootTilesZ_ = 1;
};

} // namespace earth_engine
