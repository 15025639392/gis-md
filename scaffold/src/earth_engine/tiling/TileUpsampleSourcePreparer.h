#pragma once

#include "TileLoadTypes.h"
#include "TilesetTile.h"

#include <utility>

namespace earth_engine {

class TileUpsampleSourcePreparer {
public:
    static const TilesetTile* findSourceTile(
        const TilesetTile& tile,
        bool allowUnloadingSource = false) {
        const TilesetTile* ancestor = tile.parent;
        while (ancestor) {
            const bool sourceStateReady =
                ancestor->loadState == TileLoadState::Done ||
                (allowUnloadingSource &&
                 ancestor->loadState == TileLoadState::Unloading);
            if (sourceStateReady &&
                ancestor->contentKind == TileContentKind::Render &&
                ancestor->meshReady &&
                ancestor->mesh) {
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
        if (findSourceTile(tile)) {
            return true;
        }

        for (TilesetTile* ancestor = tile.parent;
             ancestor;
             ancestor = ancestor->parent) {
            if ((ancestor->loadState == TileLoadState::ContentLoaded ||
                 ancestor->loadState == TileLoadState::Done) &&
                ancestor->contentKind == TileContentKind::Render) {
                ensureTileMesh(*ancestor);
                if (findSourceTile(tile)) {
                    return true;
                }
            }

            if (ancestor->loadState == TileLoadState::Unloaded ||
                ancestor->loadState == TileLoadState::FailedTemporarily) {
                queueTileLoad(
                    ancestor->key,
                    TileLoadPriorityGroup::Urgent,
                    priority);
                return false;
            }

            if (ancestor->loadState == TileLoadState::ContentLoading ||
                ancestor->loadState == TileLoadState::Unloading) {
                return false;
            }
        }

        return false;
    }
};

} // namespace earth_engine
