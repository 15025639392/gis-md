#pragma once

namespace earth_engine {

struct TilesetTile;

struct TileTerrainUploadCommitAction {
    bool resourcesDirty = false;
};

struct TileTerrainUploadCommitter {
    static void prepareTerrainRenderContent(TilesetTile& tile);
    static TileTerrainUploadCommitAction finishMeshResourcePreparation(
        TilesetTile& tile,
        bool resourcesReady);
};

} // namespace earth_engine
