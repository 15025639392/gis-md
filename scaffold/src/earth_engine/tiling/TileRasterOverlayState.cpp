#include "TileRasterOverlayState.h"

#include "RasterMappedToTilesetTile.h"

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

void TileRasterOverlayState::resizeMappingSlots(
    size_t count,
    IPrepareRendererResources* pPrepRenderer) {
    if (count >= mappings_.size()) {
        ensureMappingSlots(count);
        return;
    }

    for (size_t i = count; i < mappings_.size(); ++i) {
        if (mappings_[i]) {
            mappings_[i]->releaseTileReferences(pPrepRenderer);
        }
    }
    mappings_.resize(count);
}

bool TileRasterOverlayState::hasReadyMapping(size_t index) const {
    const RasterMappedToTilesetTile* mapping = mappingAt(index);
    return mapping && mapping->getReadyTile() != nullptr;
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
}

} // namespace earth_engine
