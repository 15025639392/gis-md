#pragma once

#include "TileLoadTypes.h"

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
struct TilesetTile;

struct TileContentUploadCommitAction {
    bool resourcesDirty = false;
};

struct TileContentUploadCommitter {
    static void prepareRenderContent(
        TilesetTile& tile,
        TileLoadedContent&& content,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays = {},
        RenderDevice* device = nullptr);
    static TileContentUploadCommitAction finishRenderResourcePreparation(
        TilesetTile& tile,
        bool resourcesReady);
};

} // namespace earth_engine
