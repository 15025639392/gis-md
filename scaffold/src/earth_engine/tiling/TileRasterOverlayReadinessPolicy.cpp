#include "TileRasterOverlayReadinessPolicy.h"

#include "DirectRasterMapping.h"
#include "SurfaceRasterBinding.h"
#include "RasterBindingSet.h"
#include "TileLoadState.h"
#include "TilesetTile.h"
#include "RasterOverlayRuntime.h"

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
    const RasterOverlayFrameContext& frame) {
    const auto& rasterOverlays = frame.directOverlays();
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        const RasterOverlayFrameSlot& slot = frame.slots()[i];
        const bool visible = slot.directProvider != nullptr && slot.visible;
        const bool blocks = slot.blocksCompleteRenderable;
        if (!activeOverlay || !visible || !blocks) {
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
    const RasterOverlayFrameContext& frame) {
    const auto& rasterOverlays = frame.directOverlays();
    const RasterBindingSet bindings = RasterBindingSet::resolve(tile, frame);
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        const RasterOverlayFrameSlot& slot = frame.slots()[i];
        const bool visible = slot.directProvider != nullptr && slot.visible;
        const RasterOverlayRole role = slot.role;
        const bool blocks = slot.blocksCompleteRenderable;
        if (!activeOverlay || !visible) {
            continue;
        }
        if (role != RasterOverlayRole::BaseImagery || !blocks) {
            continue;
        }
        const RasterBinding* resolved = bindings.bindingAtRuntimeSlot(i);
        if (!resolved ||
            resolved->resolution.requestState == RasterRequestState::NoMapping) {
            return BaseImageryBlockReason::NoMapping;
        }
        if (!resolved->resolution.allowedByPolicy) {
            return BaseImageryBlockReason::NoReadyTexture;
        }
        const int32_t textureCoordinateID =
            resolved->directSample.textureCoordinateId;
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
    const RasterOverlayFrameContext& frame) {
    BaseImageryNoTextureProbe probe;
    const auto& rasterOverlays = frame.directOverlays();
    const RasterBindingSet bindings = RasterBindingSet::resolve(tile, frame);
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        const RasterOverlayFrameSlot& slot = frame.slots()[i];
        const bool visible = slot.directProvider != nullptr && slot.visible;
        const RasterOverlayRole role = slot.role;
        const bool blocks = slot.blocksCompleteRenderable;
        if (!activeOverlay || !visible) {
            continue;
        }
        if (role != RasterOverlayRole::BaseImagery || !blocks) {
            continue;
        }
        const DirectRasterMapping* mapped =
            tile.rasterOverlayState.mappingAt(i);
        if (!mapped) {
            continue;  // NoMapping,不是本探针的对象。
        }
        const RasterBinding* resolved = bindings.bindingAtRuntimeSlot(i);
        if (resolved && resolved->resolution.allowedByPolicy) {
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
            const DirectRasterMapping* ancestorMapped =
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
    const RasterOverlayFrameContext& frame) {
    return baseImageryBlockReason(tile, frame) ==
           BaseImageryBlockReason::None;
}

bool TileRasterOverlayReadinessPolicy::terrainSurfaceImageryDrawableReady(
    const TilesetTile& tile,
    const RasterOverlayFrameContext& frame) {
    const TileRenderContentState& renderContent = tile.content.renderContent;
    if (!renderContent.isTerrainRenderContent() &&
        !renderContent.drawIsTerrainContent()) {
        return true;
    }

    return requiredBaseImageryDrawableReady(tile, frame);
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
            ? static_cast<int>(lhs->priority())
            : static_cast<int>(RasterOverlayPriority::Low);
        const int rhsPriority = rhs
            ? static_cast<int>(rhs->priority())
            : static_cast<int>(RasterOverlayPriority::Low);
        return lhsPriority > rhsPriority;
    });
    return order;
}

} // namespace earth_engine
