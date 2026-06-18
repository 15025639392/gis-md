#pragma once

#include "../content/GltfContentProvider.h"

namespace earth_engine {

struct TilesetTile;

struct TileContentUploadCommitAction {
    bool resourcesDirty = false;
};

struct TileContentUploadCommitter {
    static void prepareRenderContent(
        TilesetTile& tile,
        TileContentLoadResult&& result);
    static TileContentUploadCommitAction finishRenderResourcePreparation(
        TilesetTile& tile,
        bool resourcesReady);
};

} // namespace earth_engine
