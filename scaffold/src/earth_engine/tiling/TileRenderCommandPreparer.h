#pragma once

#include "GltfDrawCommandBuilder.h"
#include "GltfRenderResourcePreparer.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TilesetTile.h"
#include "../debug/PerfTimer.h"
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
    float transitionOpacity = 1.0f;
    bool allowSynchronousMeshPrep = true;
    std::optional<std::array<float, 4>> surfaceClipUv;
};

struct TileRenderCommandPerformanceTimings {
    double totalMs = 0.0;
    double ensureMeshMs = 0.0;
    double resourcePrepareMs = 0.0;
    double drawBuildMs = 0.0;
    int tileCount = 0;
    GltfDrawCommandBuildTimings drawCommand;
};

class TileRenderCommandPreparer {
public:
    template <typename EnsureTileMeshFn>
    static bool build(
        Renderer& renderer,
        TilesetTile& tile,
        RenderCommandList& commands,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        RenderDevice* device,
        const TileRenderCommandPrepareContext& context,
        EnsureTileMeshFn&& ensureTileMesh,
        TileRenderCommandPerformanceTimings* timings = nullptr) {
        const double totalStartMs = timings ? perf::nowMs() : 0.0;
        if (timings) {
            ++timings->tileCount;
        }
        auto finish = [&](bool resourcesChanged) {
            if (timings) {
                timings->totalMs += perf::nowMs() - totalStartMs;
            }
            return resourcesChanged;
        };

        if (!tile.content.renderContent.hasGltfContent() &&
            context.allowSynchronousMeshPrep) {
            const double ensureMeshStartMs =
                timings ? perf::nowMs() : 0.0;
            ensureTileMesh(tile);
            if (timings) {
                timings->ensureMeshMs +=
                    perf::nowMs() - ensureMeshStartMs;
            }
        }

        if (tile.content.renderContent.hasGltfContent()) {
            const uint64_t retainedRevisionBefore =
                tile.content.renderContent.retainedResourcesRevision();
            const TileLoadState loadStateBefore = tile.content.loadState;
            const TileContentKind contentKindBefore =
                tile.content.contentKind;
            const bool renderReadyBefore =
                tile.content.renderContent.isRenderContentReady();
            const double resourcePrepareStartMs =
                timings ? perf::nowMs() : 0.0;
            const GltfModel* model =
                tile.content.renderContent.gltfModelForRead();
            // Static resource creation belongs to the update/upload lifecycle.
            // Draw may only advance an already-resident animated model.
            if (tile.content.renderContent.isGltfRenderReady() &&
                model &&
                model->hasRuntimeAnimation()) {
                GltfRenderResourcePreparer::prepare(
                    tile,
                    device,
                    context.currentFrameTimeSeconds);
            }
            if (timings) {
                timings->resourcePrepareMs +=
                    perf::nowMs() - resourcePrepareStartMs;
            }
            const bool resourcesChanged =
                retainedRevisionBefore !=
                    tile.content.renderContent.retainedResourcesRevision() ||
                loadStateBefore != tile.content.loadState ||
                contentKindBefore != tile.content.contentKind ||
                renderReadyBefore !=
                    tile.content.renderContent.isRenderContentReady();
            tile.updateFrameRenderability(
                tile.content.renderContent.isGltfRenderReady(),
                TileSelectionRasterOverlayPreparer::isCompleteRenderable(
                    tile,
                    rasterOverlays));
            const double drawBuildStartMs =
                timings ? perf::nowMs() : 0.0;
            GltfDrawCommandBuilder::build(
                renderer,
                tile,
                rasterOverlays,
                commands,
                GltfDrawCommandBuildContext{
                    context.frameNumber,
                    context.generation,
                    context.transitionOpacity,
                    context.surfaceClipUv},
                timings ? &timings->drawCommand : nullptr);
            if (timings) {
                timings->drawBuildMs +=
                    perf::nowMs() - drawBuildStartMs;
            }
            return finish(resourcesChanged);
        }

        // Terrain fill: real content is not present yet, but a drape-ready
        // ellipsoid proxy is. Draw it so imagery appears on the smooth globe
        // immediately. Its raster mappings are advanced by update preparation
        // before this draw-only stage. The
        // tile is DRAWABLE but NOT complete-renderable — LOD refinement keeps
        // pursuing the real terrain, and the proxy is dropped the frame real
        // terrain becomes ready (markRenderContentReady).
        if (tile.content.renderContent.isFillReady()) {
            tile.updateFrameRenderability(
                /*drawable=*/true,
                /*complete=*/false);
            const double drawBuildStartMs =
                timings ? perf::nowMs() : 0.0;
            GltfDrawCommandBuilder::build(
                renderer,
                tile,
                rasterOverlays,
                commands,
                GltfDrawCommandBuildContext{
                    context.frameNumber,
                    context.generation,
                    context.transitionOpacity,
                    context.surfaceClipUv},
                timings ? &timings->drawCommand : nullptr);
            if (timings) {
                timings->drawBuildMs +=
                    perf::nowMs() - drawBuildStartMs;
            }
            return finish(false);
        }

        // No glTF content - nothing to render
        tile.clearFrameRenderability();
        return finish(false);
    }
};

} // namespace earth_engine
