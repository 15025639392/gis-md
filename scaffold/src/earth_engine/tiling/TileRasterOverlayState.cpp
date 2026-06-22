#include "TileRasterOverlayState.h"

#include "RasterMappedToTilesetTile.h"
#include "SurfaceRasterBinding.h"

namespace earth_engine {

RasterMappedToTilesetTile* TileRasterOverlayState::mappingAt(size_t index) {
    if (index >= mappings_.size()) {
        return nullptr;
    }
    return mappings_[index].get();
}

const RasterMappedToTilesetTile* TileRasterOverlayState::mappingAt(
    size_t index) const {
    if (index >= mappings_.size()) {
        return nullptr;
    }
    return mappings_[index].get();
}

RasterMappedToTilesetTile& TileRasterOverlayState::ensureMapping(
    size_t index) {
    ensureMappingSlots(index + 1);
    if (!mappings_[index]) {
        mappings_[index] = std::make_unique<RasterMappedToTilesetTile>();
    }
    return *mappings_[index];
}

void TileRasterOverlayState::releaseMapping(
    size_t index,
    IPrepareRendererResources* pPrepRenderer) {
    if (index >= mappings_.size() || !mappings_[index]) {
        return;
    }
    mappings_[index]->releaseTileReferences(pPrepRenderer);
    mappings_[index].reset();
}

void TileRasterOverlayState::resizeMappingSlots(
    size_t count,
    IPrepareRendererResources* pPrepRenderer) {
    if (count >= mappings_.size()) {
        ensureMappingSlots(count);
        return;
    }

    for (size_t i = count; i < mappings_.size(); ++i) {
        releaseMapping(i, pPrepRenderer);
    }
    mappings_.resize(count);
}

bool TileRasterOverlayState::hasReadyMapping(size_t index) const {
    const RasterMappedToTilesetTile* mapping = mappingAt(index);
    return mapping && mapping->getReadyTile() != nullptr;
}

bool TileRasterOverlayState::hasDrawableReadyMapping(size_t index) const {
    const RasterMappedToTilesetTile* mapping = mappingAt(index);
    return chooseSurfaceRasterBinding(mapping).kind !=
           SurfaceRasterBindingKind::None;
}

void TileRasterOverlayState::releaseReferences(
    IPrepareRendererResources* pPrepRenderer) {
    for (auto& overlay : mappings_) {
        if (overlay) {
            overlay->releaseTileReferences(pPrepRenderer);
        }
    }
}

void TileRasterOverlayState::releaseAndClearReferences(
    IPrepareRendererResources* pPrepRenderer) {
    releaseReferences(pPrepRenderer);
    mappings_.clear();
    missingProjections_.clear();
}

} // namespace earth_engine
