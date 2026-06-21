#pragma once

#include "TileKey.h"

#include <string>
#include <vector>

namespace earth_engine {

struct TileSelectionRootPolicy {
    static TileKey virtualTerrainRootKey(const std::string& schemeId);
    static bool isVirtualTerrainRoot(const TileKey& key);
    static bool usesVirtualTerrainRoot(const std::string& schemeId);
    static std::vector<TileKey> levelZeroTerrainRoots(
        const std::string& schemeId);

    static std::vector<TileKey> chooseRoots(
        const std::string& schemeId,
        const std::vector<TileKey>& explicitRoots,
        bool useVirtualTerrainRoot = false);
};

} // namespace earth_engine
