#pragma once

#include "GltfDrawCommandBuilder.h"
#include "GltfRenderResourcePreparer.h"
#include "SurfaceRasterOverlayStateUpdater.h"
#include "SurfaceTileDrawCommandBuilder.h"
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
        typename HasSurfaceDrawableFn,
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
        HasSurfaceDrawableFn&& hasSurfaceDrawable,
        UnloadTileContentFn&& unloadTileContent,
        CreateRasterOverlayUpsampledChildrenFn&&
            createRasterOverlayUpsampledChildren) {
        if (tile.gltfModel) {
            GltfRenderResourcePreparer::prepare(
                tile,
                device,
                context.currentFrameTimeSeconds);
            GltfDrawCommandBuilder::build(
                renderer,
                tile,
                commands,
                GltfDrawCommandBuildContext{
                    context.frameNumber,
                    context.generation,
                    context.transitionOpacity});
            return;
        }

        if (!tile.meshReady) {
            if (!context.allowSynchronousMeshPrep) {
                return;
            }
            ensureTileMesh(tile);
        }
        if (!hasSurfaceDrawable(tile)) {
            return;
        }

        const std::vector<size_t> overlayOrder =
            TileSelectionRasterOverlayPreparer::processingOrder(
                rasterOverlays);
        const SurfaceRasterOverlayUpdateAction overlayAction =
            SurfaceRasterOverlayStateUpdater::update(
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

        tile.surfaceDrawable = hasSurfaceDrawable(tile);
        tile.completeRenderable =
            TileSelectionRasterOverlayPreparer::isCompleteRenderable(
                tile,
                rasterOverlays);
        tile.renderable = tile.completeRenderable;

        if (overlayAction.createRasterOverlayUpsampledChildren &&
            tile.children.empty()) {
            createRasterOverlayUpsampledChildren(tile);
        }

        SurfaceTileDrawCommandBuilder::build(
            renderer,
            tile,
            rasterOverlays,
            commands,
            SurfaceTileDrawCommandBuildContext{
                context.frameNumber,
                context.generation,
                context.transitionOpacity,
                context.surfaceClipUv});
    }
};

} // namespace earth_engine
