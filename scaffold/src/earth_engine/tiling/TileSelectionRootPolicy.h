#pragma once

#include "TileKey.h"

#include <string>
#include <vector>

namespace earth_engine {

struct TileSelectionRootPolicy {
    static std::vector<TileKey> chooseRoots(
        const std::string& schemeId,
        const std::vector<TileKey>& explicitRoots);
};

} // namespace earth_engine
