#pragma once

#include "TileLoadTypes.h"

#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class IPrepareRendererResources;
class RenderDevice;
class TilesetContentProvider;
struct TilesetTile;

struct TileContentUploadCommitAction {
    bool ensureChildren = false;
    bool resourcesDirty = false;
};

struct TileContentUploadCommitter {
    static void prepareRenderContent(
        TilesetTile& tile,
        TileLoadedContent&& content,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays = {},
        RenderDevice* device = nullptr,
        IPrepareRendererResources* pPrepRenderer = nullptr,
        TilesetContentProvider* contentProvider = nullptr);
    static TileContentUploadCommitAction finishRenderResourcePreparation(
        TilesetTile& tile,
        bool resourcesReady,
        IPrepareRendererResources* pPrepRenderer = nullptr);
};

} // namespace earth_engine
