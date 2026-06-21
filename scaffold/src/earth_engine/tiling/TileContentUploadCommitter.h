#pragma once

#include "TileLoadTypes.h"

namespace earth_engine {

struct TilesetTile;

struct TileContentUploadCommitAction {
    bool resourcesDirty = false;
};

struct TileContentUploadCommitter {
    static void prepareRenderContent(
        TilesetTile& tile,
        TileLoadedContent&& content);
    static TileContentUploadCommitAction finishRenderResourcePreparation(
        TilesetTile& tile,
        bool resourcesReady);
};

} // namespace earth_engine
