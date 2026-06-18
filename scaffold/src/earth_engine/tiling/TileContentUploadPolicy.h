#pragma once

#include "../content/GltfContentProvider.h"

namespace earth_engine {

struct TilesetTile;

struct TileContentUploadPolicy {
    static void prepareGltfRenderContent(
        TilesetTile& tile,
        TileContentLoadResult&& result);
    static void markGltfRenderResourcesFailed(TilesetTile& tile);
};

} // namespace earth_engine
