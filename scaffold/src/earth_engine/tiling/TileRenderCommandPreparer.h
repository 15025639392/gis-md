#pragma once

#include "GltfDrawCommandBuilder.h"
#include "GltfRenderResourcePreparer.h"
#include "RenderContentRasterOverlayStateUpdater.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TilesetTile.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../renderer/RenderCommand.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class Renderer;

struct TileRenderCommandPrepareContext {
    uint64_t frameNumber = 0;
    uint64_t generation = 0;
    double currentFrameTimeSeconds = 0.0;
    double maximumScreenSpaceError = 16.0;
    float transitionOpacity = 1.0f;
    bool allowSynchronousMeshPrep = true;
    std::optional<std::array<float, 4>> surfaceClipUv;
};

class TileRenderCommandPreparer {
public:
    template <
        typename EnsureTileMeshFn,
        typename UnloadTileContentFn,
        typename CreateRasterOverlayUpsampledChildrenFn>
    static void build(
        Renderer& renderer,
        TilesetTile& tile,
        RenderCommandList& commands,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        RenderDevice* device,
        FrameResourceBudget& frameResourceBudget,
        const TileRenderCommandPrepareContext& context,
        EnsureTileMeshFn&& ensureTileMesh,
        UnloadTileContentFn&& unloadTileContent,
        CreateRasterOverlayUpsampledChildrenFn&&
            createRasterOverlayUpsampledChildren) {
        if (tile.content.renderContent.hasGltfContent()) {
            const std::vector<size_t> overlayOrder =
                TileSelectionRasterOverlayPreparer::processingOrder(
                    rasterOverlays);
            const RenderContentRasterOverlayUpdateAction overlayAction =
                RenderContentRasterOverlayStateUpdater::update(
                    renderer,
                    tile,
                    rasterOverlays,
                    overlayOrder,
                    device,
                    context.maximumScreenSpaceError,
                    frameResourceBudget);
            if (overlayAction.unloadTileContent) {
                unloadTileContent(tile);
                return;
            }
            if (overlayAction.createRasterOverlayUpsampledChildren &&
                tile.children.empty()) {
                createRasterOverlayUpsampledChildren(tile);
            }

            GltfRenderResourcePreparer::prepare(
                tile,
                device,
                context.currentFrameTimeSeconds);
            tile.updateFrameRenderability(
                tile.content.renderContent.isGltfRenderReady(),
                TileSelectionRasterOverlayPreparer::isCompleteRenderable(
                    tile,
                    rasterOverlays));
            GltfDrawCommandBuilder::build(
                renderer,
                tile,
                rasterOverlays,
                commands,
                GltfDrawCommandBuildContext{
                    context.frameNumber,
                    context.generation,
                    context.transitionOpacity,
                    context.surfaceClipUv});
            return;
        }

        // No glTF content - nothing to render
        tile.clearFrameRenderability();
    }
};

} // namespace earth_engine
