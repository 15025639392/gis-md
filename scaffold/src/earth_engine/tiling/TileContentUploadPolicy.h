#pragma once

#include "TileLoadTypes.h"

namespace earth_engine {

struct TilesetTile;

struct TileContentUploadPolicy {
    static void prepareGltfRenderContent(
        TilesetTile& tile,
        TileLoadedContent&& content);
    static void markGltfRenderResourcesFailed(TilesetTile& tile);
};

} // namespace earth_engine
