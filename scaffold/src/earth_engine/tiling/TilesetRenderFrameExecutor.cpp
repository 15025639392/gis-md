#include "TilesetRenderFrameExecutor.h"

#include "../renderer/Renderer.h"

namespace earth_engine {

void TilesetRenderFrameExecutor::buildRenderCommands(
    TileRenderFrameContext context,
    Renderer& renderer,
    RenderCommandList& commands) {
    TileRenderFrameCoordinator::run(
        context.input,
        renderer,
        commands,
        [&context](const TileKey& key) {
            return context.ensureTile(key);
        },
        [&context](const std::string& cacheKey) {
            context.markIneligibleForUnloading(cacheKey);
        },
        [&context](Renderer& renderer,
                   TilesetTile& tile,
                   RenderCommandList& commands,
                   float transitionOpacity,
                   bool allowSynchronousMeshPrep,
                   const std::optional<std::array<float, 4>>& surfaceClipUv) {
            context.buildTileDrawCommand(
                renderer,
                tile,
                commands,
                transitionOpacity,
                allowSynchronousMeshPrep,
                surfaceClipUv);
        },
        [&context](const std::string& cacheKey) {
            context.markEligibleForUnloading(cacheKey);
        },
        [&context]() {
            context.updateTotalBytesUsed();
        },
        [&context, &renderer]() {
            context.unloadCachedBytes(renderer);
        });
}

} // namespace earth_engine
