#pragma once

#include "TileLoadTypes.h"
#include "TilesetTile.h"

#include <utility>

namespace earth_engine {

class TileUpsampleSourcePreparer {
public:
    static const TilesetTile* findSourceTile(
        const TilesetTile& tile,
        bool allowUnloadingSource = false,
        bool allowGltfTerrainSource = false) {
        const TilesetTile* ancestor = tile.parent;
        while (ancestor) {
            const bool sourceStateReady =
                ancestor->content.loadState == TileLoadState::Done ||
                (allowUnloadingSource &&
                 ancestor->content.loadState == TileLoadState::Unloading);
            if (sourceStateReady &&
                ancestor->content.contentKind == TileContentKind::Render &&
                ((ancestor->content.renderContent.hasTerrainMesh() &&
                  ancestor->content.renderContent.isSurfaceMeshReady()) ||
                 (allowGltfTerrainSource &&
                  ancestor->content.renderContent.isTerrainRenderContent() &&
                  ancestor->content.renderContent.hasGltfContent()))) {
                return ancestor;
            }
            ancestor = ancestor->parent;
        }
        return nullptr;
    }

    template <typename EnsureTileMeshFn, typename QueueTileLoadFn>
    static bool prepareSourceTile(
        TilesetTile& tile,
        double priority,
        EnsureTileMeshFn&& ensureTileMesh,
        QueueTileLoadFn&& queueTileLoad) {
        if (findSourceTile(tile, false, true)) {
            return true;
        }

        for (TilesetTile* ancestor = tile.parent;
             ancestor;
             ancestor = ancestor->parent) {
            if ((ancestor->content.loadState == TileLoadState::ContentLoaded ||
                 ancestor->content.loadState == TileLoadState::Done) &&
                ancestor->content.contentKind == TileContentKind::Render) {
                ensureTileMesh(*ancestor);
                if (findSourceTile(tile, false, true)) {
                    return true;
                }
            }

            if (ancestor->content.loadState == TileLoadState::Unloaded ||
                ancestor->content.loadState == TileLoadState::FailedTemporarily) {
                queueTileLoad(
                    ancestor->key,
                    TileLoadPriorityGroup::Urgent,
                    priority);
                return false;
            }

            if (ancestor->content.loadState == TileLoadState::ContentLoading ||
                ancestor->content.loadState == TileLoadState::Unloading) {
                return false;
            }
        }

        return false;
    }
};

} // namespace earth_engine
