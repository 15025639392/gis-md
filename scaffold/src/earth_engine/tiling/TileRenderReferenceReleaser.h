#pragma once

#include "TilesetTile.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct TileRenderReferenceReleaser {
    static void release(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles) {
        for (const auto& entry : tiles) {
            if (entry.second) {
                entry.second->clearReferences();
            }
        }
    }
};

} // namespace earth_engine
