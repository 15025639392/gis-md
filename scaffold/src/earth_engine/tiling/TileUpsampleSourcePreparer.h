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
        bool allowGltfTerrainSource = false,
        bool useHeightmapSurfacePath = true) {
        if (tile.content.isRasterDetailUpsample()) {
            return findDirectGltfTerrainParent(tile, allowUnloadingSource);
        }

        const TilesetTile* parent = tile.parent;
        if (allowGltfTerrainSource && parent) {
            const bool parentStateReady =
                parent->content.loadState == TileLoadState::Done ||
                (allowUnloadingSource &&
                 parent->content.loadState == TileLoadState::Unloading);
            if (parentStateReady &&
                parent->content.contentKind == TileContentKind::Render &&
                parent->content.renderContent.isTerrainRenderContent() &&
                parent->content.renderContent.hasGltfContent()) {
                return parent;
            }
        }

        if (!useHeightmapSurfacePath) {
            return nullptr;
        }

        const TilesetTile* ancestor = tile.parent;
        while (ancestor) {
            const bool sourceStateReady =
                ancestor->content.loadState == TileLoadState::Done ||
                (allowUnloadingSource &&
                 ancestor->content.loadState == TileLoadState::Unloading);
            if (sourceStateReady &&
                ancestor->content.contentKind == TileContentKind::Render &&
                ancestor->content.renderContent.hasTerrainMesh() &&
                ancestor->content.renderContent.isSurfaceMeshReady()) {
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
        bool useHeightmapSurfacePath,
        EnsureTileMeshFn&& ensureTileMesh,
        QueueTileLoadFn&& queueTileLoad) {
        if (tile.content.isRasterDetailUpsample()) {
            return prepareRasterDetailSourceTile(
                tile,
                priority,
                std::forward<EnsureTileMeshFn>(ensureTileMesh),
                std::forward<QueueTileLoadFn>(queueTileLoad));
        }

        if (findSourceTile(tile,
                           false,
                           true,
                           useHeightmapSurfacePath)) {
            return true;
        }

        for (TilesetTile* ancestor = tile.parent;
             ancestor;
             ancestor = ancestor->parent) {
            if ((ancestor->content.loadState == TileLoadState::ContentLoaded ||
                 ancestor->content.loadState == TileLoadState::Done) &&
                ancestor->content.contentKind == TileContentKind::Render) {
                ensureTileMesh(*ancestor);
                if (findSourceTile(tile,
                                   false,
                                   true,
                                   useHeightmapSurfacePath)) {
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

private:
    static const TilesetTile* findDirectGltfTerrainParent(
        const TilesetTile& tile,
        bool allowUnloadingSource) {
        const TilesetTile* parent = tile.parent;
        if (!parent) {
            return nullptr;
        }
        const bool parentStateReady =
            parent->content.loadState == TileLoadState::Done ||
            (allowUnloadingSource &&
             parent->content.loadState == TileLoadState::Unloading);
        return parentStateReady &&
                parent->content.contentKind == TileContentKind::Render &&
                parent->content.renderContent.isTerrainRenderContent() &&
                parent->content.renderContent.hasGltfContent()
            ? parent
            : nullptr;
    }

    template <typename EnsureTileMeshFn, typename QueueTileLoadFn>
    static bool prepareRasterDetailSourceTile(
        TilesetTile& tile,
        double priority,
        EnsureTileMeshFn&& ensureTileMesh,
        QueueTileLoadFn&& queueTileLoad) {
        TilesetTile* parent = tile.parent;
        if (!parent) {
            return false;
        }

        if (findDirectGltfTerrainParent(tile, false)) {
            return true;
        }

        if ((parent->content.loadState == TileLoadState::ContentLoaded ||
             parent->content.loadState == TileLoadState::Done) &&
            parent->content.contentKind == TileContentKind::Render &&
            parent->content.renderContent.isTerrainRenderContent() &&
            parent->content.renderContent.hasGltfContent()) {
            ensureTileMesh(*parent);
            return findDirectGltfTerrainParent(tile, false) != nullptr;
        }

        if (parent->content.loadState == TileLoadState::Unloaded ||
            parent->content.loadState == TileLoadState::FailedTemporarily) {
            queueTileLoad(
                parent->key,
                TileLoadPriorityGroup::Urgent,
                priority);
            return false;
        }

        return false;
    }
};

} // namespace earth_engine
