#include "TileRasterOverlayReadinessPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "SurfaceRasterBinding.h"
#include "RasterBindingSet.h"
#include "TileLoadState.h"
#include "TilesetTile.h"

#include "../content/GltfModel.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"

#include <algorithm>

namespace earth_engine {

bool TileRasterOverlayReadinessPolicy::doneTileCannotHoldRasterOverlays(
    const TilesetTile& tile) {
    return tile.content.loadState == TileLoadState::Done &&
           (tile.content.contentKind != TileContentKind::Render ||
            !tile.hasRasterOverlayHostContent());
}

bool TileRasterOverlayReadinessPolicy::requiredOverlaysReady(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible() ||
            !activeOverlay->getOverlay().blocksCompleteRenderable()) {
            continue;
        }
        if (!tile.rasterOverlayState.hasReadyMapping(i)) {
            return false;
        }
    }

    return true;
}

BaseImageryBlockReason
TileRasterOverlayReadinessPolicy::baseImageryBlockReason(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    const RasterBindingSet bindings =
        RasterBindingSet::resolve(tile, rasterOverlays);
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        const RasterOverlay& overlay = activeOverlay->getOverlay();
        if (overlay.role() != RasterOverlayRole::BaseImagery ||
            !overlay.blocksCompleteRenderable()) {
            continue;
        }
        const RasterBinding* resolved = bindings.bindingAtRuntimeSlot(i);
        const RasterMappedToTilesetTile* mapped =
            resolved ? resolved->mapped : nullptr;
        if (!resolved || !resolved->allowedByPolicy) {
            return mapped
                ? BaseImageryBlockReason::NoReadyTexture
                : BaseImageryBlockReason::NoMapping;
        }
        const int32_t textureCoordinateID =
            resolved->textureCoordinateId;
        if (textureCoordinateID < 0 ||
            textureCoordinateID >= static_cast<int32_t>(kGltfMaxTexCoordSets)) {
            return BaseImageryBlockReason::TexcoordInvalid;
        }
    }

    return BaseImageryBlockReason::None;
}

BaseImageryNoTextureProbe
TileRasterOverlayReadinessPolicy::probeNoReadyTexture(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    BaseImageryNoTextureProbe probe;
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        const RasterOverlay& overlay = activeOverlay->getOverlay();
        if (overlay.role() != RasterOverlayRole::BaseImagery ||
            !overlay.blocksCompleteRenderable()) {
            continue;
        }
        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        if (!mapped) {
            continue;  // NoMapping,不是本探针的对象。
        }
        if (rasterOverlayBindingAllowedByPolicy(
                activeOverlay,
                mapped,
                chooseSurfaceRasterBinding(mapped))) {
            continue;
        }

        probe.valid = true;
        probe.zoom = tile.key.z;
        if (const RasterOverlayTile* loading = mapped->getLoadingTile()) {
            probe.loadingState = static_cast<int>(loading->getState());
        }
        if (const RasterOverlayTile* ready = mapped->getReadyTile()) {
            probe.readyState = static_cast<int>(ready->getState());
            probe.readyHasTexture = ready->getTexture() != nullptr;
        }
        probe.mappingState = static_cast<int>(mapped->getState());
        probe.authoritativeUpdates =
            tile.rasterOverlayState.authoritativeUpdateCount();
        probe.tileLoadState = static_cast<int>(tile.content.loadState);
        probe.tileContentKind = static_cast<int>(tile.content.contentKind);
        // 祖先链画像:用 index i 直接查(mapping 槽位与 overlay 列表同序,见
        // ensureMappingSlots),不复刻 findLoadedTileOverlay 的 provider 匹配 ——
        // 这里要回答的是"祖先手上到底有没有可画的底图",不是复现那次查找。
        for (const TilesetTile* ancestor = tile.parent;
             ancestor;
             ancestor = ancestor->parent) {
            ++probe.ancestorDepth;
            const RasterMappedToTilesetTile* ancestorMapped =
                ancestor->rasterOverlayState.mappingAt(i);
            if (!ancestorMapped) {
                continue;
            }
            ++probe.ancestorsWithMapping;
            if (chooseSurfaceRasterBinding(ancestorMapped).kind !=
                SurfaceRasterBindingKind::None) {
                ++probe.ancestorsWithTexture;
            }
        }
        break;
    }
    return probe;
}

bool TileRasterOverlayReadinessPolicy::requiredBaseImageryDrawableReady(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    return baseImageryBlockReason(tile, rasterOverlays) ==
           BaseImageryBlockReason::None;
}

bool TileRasterOverlayReadinessPolicy::terrainSurfaceImageryDrawableReady(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    const TileRenderContentState& renderContent = tile.content.renderContent;
    if (!renderContent.isTerrainRenderContent() &&
        !renderContent.drawIsTerrainContent()) {
        return true;
    }

    return requiredBaseImageryDrawableReady(tile, rasterOverlays);
}

std::vector<size_t> TileRasterOverlayReadinessPolicy::processingOrder(
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    std::vector<size_t> order;
    order.reserve(rasterOverlays.size());
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const ActivatedRasterOverlay* lhs = rasterOverlays[a];
        const ActivatedRasterOverlay* rhs = rasterOverlays[b];
        const int lhsPriority = lhs
            ? static_cast<int>(lhs->getOverlay().priority())
            : static_cast<int>(RasterOverlayPriority::Low);
        const int rhsPriority = rhs
            ? static_cast<int>(rhs->getOverlay().priority())
            : static_cast<int>(RasterOverlayPriority::Low);
        return lhsPriority > rhsPriority;
    });
    return order;
}

} // namespace earth_engine
