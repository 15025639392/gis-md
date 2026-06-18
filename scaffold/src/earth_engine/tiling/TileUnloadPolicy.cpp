#include "TileUnloadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "TileUnloadQueue.h"

namespace earth_engine {

bool TileUnloadPolicy::isEligibleForContentUnloadQueue(
    const TilesetTile& tile) {
    if (tile.contentKind == TileContentKind::Unknown) {
        return false;
    }
    if (tile.loadState == TileLoadState::Unloaded ||
        tile.loadState == TileLoadState::ContentLoading ||
        tile.loadState == TileLoadState::Unloading) {
        return false;
    }
    return true;
}

bool TileUnloadPolicy::hasReferencedDescendant(const TilesetTile& tile) {
    for (const TilesetTile* child : tile.children) {
        if (!child) continue;
        if (child->referenceCount() > 0 ||
            hasReferencedDescendant(*child)) {
            return true;
        }
    }
    return false;
}

bool TileUnloadPolicy::hasContentLoadingUpsampledDescendant(
    const TilesetTile& tile) {
    for (const TilesetTile* child : tile.children) {
        if (!child) continue;
        if (child->upsampledFromParent &&
            child->loadState == TileLoadState::ContentLoading) {
            return true;
        }
        if (hasContentLoadingUpsampledDescendant(*child)) {
            return true;
        }
    }
    return false;
}

bool TileUnloadPolicy::shouldDeferForReferences(
    const TilesetTile& tile,
    bool externalSubtreeHasActiveWork) {
    return tile.referenceCount() > 0 ||
           hasReferencedDescendant(tile) ||
           (tile.contentKind == TileContentKind::External &&
            externalSubtreeHasActiveWork);
}

bool TileUnloadPolicy::hasQueuedTileInState(
    const TileUnloadQueue& queue,
    const std::unordered_map<
        std::string,
        std::unique_ptr<TilesetTile>>& tiles,
    TileLoadState state) {
    for (const std::string& queuedKey : queue.keys()) {
        auto tileIt = tiles.find(queuedKey);
        if (tileIt != tiles.end() &&
            tileIt->second &&
            tileIt->second->loadState == state) {
            return true;
        }
    }
    return false;
}

void TileUnloadPolicy::releaseMainThreadRenderResourcesForProtectedUnload(
    TilesetTile& tile) {
    tile.gpuVertexBuffer.reset();
    tile.gpuIndexBuffer.reset();
    tile.gltfTextureResources.clear();
    tile.gltfPrimitiveResources.clear();
    tile.surfaceDrawable = false;
    tile.surfaceSource = SurfaceDrawableSource::None;
    tile.completeRenderable = false;
    tile.renderable = false;
}

void TileUnloadPolicy::releaseRasterOverlayReferences(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    for (auto& overlay : tile.rasterOverlays) {
        if (overlay) {
            overlay->releaseTileReferences(pPrepRenderer);
        }
    }
}

void TileUnloadPolicy::releaseAndClearRasterOverlayReferences(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    releaseRasterOverlayReferences(tile, pPrepRenderer);
    tile.rasterOverlays.clear();
}

void TileUnloadPolicy::releaseRenderContentResources(TilesetTile& tile) {
    tile.mesh.reset();
    tile.gltfModel.reset();
    tile.gltfContentTransform = Mat4::identity();
    tile.gltfTextureResources.clear();
    tile.gltfPrimitiveResources.clear();
    tile.gpuVertexBuffer.reset();
    tile.gpuIndexBuffer.reset();
    tile.meshReady = false;
    tile.surfaceDrawable = false;
    tile.surfaceSource = SurfaceDrawableSource::None;
}

void TileUnloadPolicy::markContentUnloaded(TilesetTile& tile) {
    tile.contentKind = TileContentKind::Unknown;
    tile.loadState = TileLoadState::Unloaded;
    tile.completeRenderable = false;
    tile.renderable = false;
}

} // namespace earth_engine
