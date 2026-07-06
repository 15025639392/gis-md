#include "TileRenderFrameContext.h"

#include "TileCacheOwnershipManager.h"
#include "TileContentAccess.h"
#include "TileRenderCommandManager.h"

namespace earth_engine {

TilesetTile* TileRenderFrameContext::ensureTile(const TileKey& key) const {
    return contentAccess.ensureTile(key);
}

void TileRenderFrameContext::markIneligibleForUnloading(
    const std::string& cacheKey) const {
    cacheOwnership.markIneligibleForUnloading(cacheKey);
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

void TileRenderFrameContext::markEligibleForUnloading(
    const TilesetTile* tile,
    const std::string& cacheKey) const {
    cacheOwnership.markEligibleForUnloading(tile, cacheKey);
}

void TileRenderFrameContext::updateTotalBytesUsed() const {
    cacheOwnership.updateTotalBytesUsed();
}

void TileRenderFrameContext::unloadCachedBytes(Renderer& renderer) const {
    cacheOwnership.unloadConfiguredCachedBytes(&renderer);
}

} // namespace earth_engine
