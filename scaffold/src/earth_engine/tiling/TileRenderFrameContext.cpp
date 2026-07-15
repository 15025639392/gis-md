#include "TileRenderFrameContext.h"

#include "TileCacheOwnershipManager.h"
#include "TileRenderCommandManager.h"

namespace earth_engine {

void TileRenderFrameContext::markIneligibleForUnloading(
    const std::string& cacheKey) const {
    cacheOwnership.markIneligibleForUnloading(cacheKey);
}

void TileRenderFrameContext::trackRenderReference(
    TilesetTile* tile,
    std::string cacheKey,
    bool countedReference) const {
    renderReferences.push_back(
        TileRenderReference{
            tile,
            std::move(cacheKey),
            countedReference});
}

void TileRenderFrameContext::buildTileDrawCommand(
    Renderer& renderer,
    TilesetTile& tile,
    RenderCommandList& commands,
    float transitionOpacity,
    bool allowSynchronousMeshPrep,
    const std::optional<std::array<float, 4>>& surfaceClipUv) const {
    renderCommands.buildTileDrawCommand(renderer,
                                        tile,
                                        commands,
                                        transitionOpacity,
                                        allowSynchronousMeshPrep,
                                        surfaceClipUv);
}

const TileRenderCommandPerformanceTimings&
TileRenderFrameContext::renderCommandTimings() const {
    return renderCommands.frameTimings();
}

void TileRenderFrameContext::trimRasterCaches(bool cachePressure) const {
    cacheOwnership.trimRasterCaches(cachePressure);
}

bool TileRenderFrameContext::shouldUnloadCachedBytes() const {
    return cacheOwnership.shouldUnloadCachedBytes();
}

void TileRenderFrameContext::unloadCachedBytes(Renderer& renderer) const {
    cacheOwnership.unloadConfiguredCachedBytes(&renderer);
}

} // namespace earth_engine
