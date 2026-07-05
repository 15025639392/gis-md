#include "TileSelectionShadowTree.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

#include <memory>
#include <utility>

namespace earth_engine {

void TileSelectionShadowTree::mirrorReadSurface(
    const TilesetTile& live,
    TilesetTile& shadow) {
    // key + bounds are established at construction; mirror the remaining
    // selection read-surface here (see class doc for the field inventory and
    // the deliberately-omitted glTF/raster render-content surface).
    shadow.bounds = live.bounds;
    shadow.geometricError = live.geometricError;
    shadow.refine = live.refine;
    shadow.unconditionallyRefine = live.unconditionallyRefine;
    shadow.boundingVolume = live.boundingVolume;
    shadow.viewerRequestVolume = live.viewerRequestVolume;
    shadow.contentBoundingVolume = live.contentBoundingVolume;
    shadow.initialBoundingVolume = live.initialBoundingVolume;
    shadow.initialContentBoundingVolume = live.initialContentBoundingVolume;

    // Load / content classification (plain enums + upsample kind). The glTF
    // render resources and raster mappings are intentionally left empty.
    shadow.content.loadState = live.content.loadState;
    shadow.content.contentKind = live.content.contentKind;
    shadow.content.contentUpsampleKind = live.content.contentUpsampleKind;
    shadow.content.rasterDetailSourceProjection =
        live.content.rasterDetailSourceProjection;

    // Cross-frame selection history the traversal reads (previousSelectionState
    // for refine-flow / kick decisions; selectionState is carried so the
    // shadow's own frame-start reset shifts it exactly as the sync path does).
    shadow.selectionFrameState.previousSelectionState =
        live.selectionFrameState.previousSelectionState;
    shadow.selectionFrameState.selectionState =
        live.selectionFrameState.selectionState;
}

void TileSelectionShadowTree::build(const TilesetTileRegistry& liveRegistry) {
    TilesetTileRegistry::TileMap& shadowTiles = shadowRegistry_.tiles();
    shadowTiles.clear();

    const TilesetTileRegistry::TileMap& liveTiles = liveRegistry.tiles();
    shadowTiles.reserve(liveTiles.size());

    // Pass 1: create every shadow tile and mirror its read-surface.
    for (const auto& [cacheKey, liveTile] : liveTiles) {
        if (!liveTile) {
            continue;
        }
        auto shadow = std::make_unique<TilesetTile>(
            liveTile->key,
            liveTile->bounds);
        mirrorReadSurface(*liveTile, *shadow);
        shadowTiles[cacheKey] = std::move(shadow);
    }

    // Pass 2: wire parent/child topology by key (order preserved), so the
    // shadow tree is a faithful structural mirror the traversal can walk.
    for (const auto& [cacheKey, liveTile] : liveTiles) {
        if (!liveTile) {
            continue;
        }
        TilesetTile* shadow =
            shadowTiles[cacheKey].get();
        if (!shadow) {
            continue;
        }
        if (liveTile->parent) {
            shadow->parent = shadowRegistry_.findTile(liveTile->parent->key);
        }
        shadow->children.clear();
        shadow->children.reserve(liveTile->children.size());
        for (const TilesetTile* liveChild : liveTile->children) {
            if (!liveChild) {
                continue;
            }
            if (TilesetTile* shadowChild =
                    shadowRegistry_.findTile(liveChild->key)) {
                shadow->children.push_back(shadowChild);
            }
        }
    }
}

} // namespace earth_engine
