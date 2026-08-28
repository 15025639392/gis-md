#include "RasterBindingSet.h"

#include "TilesetTile.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "RasterOverlayRuntime.h"

namespace earth_engine {

RasterBindingSet RasterBindingSet::resolve(
    const TilesetTile& tile,
    const RasterOverlayFrameContext& frame) {
    RasterBindingSet result;
    result.bindings_.reserve(frame.slots().size());
    for (size_t i = 0; i < frame.slots().size(); ++i) {
        const RasterOverlayFrameSlot& slot = frame.slots()[i];
        const DirectRasterMapping* mapped =
            tile.rasterOverlayState.mappingAt(i);
        DirectRasterBindingResolution direct =
            resolveDirectRasterBinding(mapped);
        RasterResolution& resolution = direct.resolution;
        resolution.opacity = slot.opacity;
        resolution.visible =
            slot.directProvider != nullptr && slot.visible;
        resolution.role = slot.role;
        resolution.priority = slot.priority;
        resolution.fallbackPolicy = slot.fallbackPolicy;
        resolution.generation = slot.generation;
        resolution.blocksCompleteRenderable = slot.blocksCompleteRenderable;
        resolution.allowedByPolicy =
            rasterResolutionAllowedByPolicy(resolution);
        result.bindings_.push_back(RasterBinding{
            i,
            std::move(direct.resolution),
            std::move(direct.sample)});
    }
    return result;
}

} // namespace earth_engine
