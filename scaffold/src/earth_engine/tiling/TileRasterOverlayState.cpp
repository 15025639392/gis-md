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
        invalidateFrameUpdateCache();
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
    invalidateFrameUpdateCache();
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

    invalidateFrameUpdateCache();
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

void TileRasterOverlayState::synchronizeMappingIdentity(
    uint64_t mappingIdentity,
    IPrepareRendererResources* pPrepRenderer) {
    if (!hasMappingIdentity_) {
        invalidateFrameUpdateCache();
        hasMappingIdentity_ = true;
        mappingIdentity_ = mappingIdentity;
        return;
    }
    if (mappingIdentity_ == mappingIdentity) {
        return;
    }

    invalidateFrameUpdateCache();
    releaseAndClearReferences(pPrepRenderer);
    hasMappingIdentity_ = true;
    mappingIdentity_ = mappingIdentity;
}

void TileRasterOverlayState::releaseReferences(
    IPrepareRendererResources* pPrepRenderer) {
    invalidateFrameUpdateCache();
    for (auto& overlay : mappings_) {
        if (overlay) {
            overlay->releaseTileReferences(pPrepRenderer);
        }
    }
}

void TileRasterOverlayState::releaseAndClearReferences(
    IPrepareRendererResources* pPrepRenderer) {
    invalidateFrameUpdateCache();
    releaseReferences(pPrepRenderer);
    mappings_.clear();
    missingProjections_.clear();
    hasMappingIdentity_ = false;
    mappingIdentity_ = 0;
}

bool TileRasterOverlayState::tryReuseFrameUpdate(
    uint64_t frameNumber,
    uint64_t mappingIdentity,
    uint64_t configuration,
    uint64_t providerMappingRevision,
    uint64_t contentRevision,
    uint64_t runtimeStateSignature,
    bool stableAcrossFrames,
    TileRasterOverlayUpdateAction& action,
    bool& rendererMaterialized) {
    if (frameNumber == 0 || !frameUpdateCacheValid_ ||
        frameUpdateMappingIdentity_ != mappingIdentity ||
        frameUpdateConfiguration_ != configuration ||
        frameUpdateProviderMappingRevision_ != providerMappingRevision ||
        frameUpdateContentRevision_ != contentRevision ||
        frameUpdateRuntimeStateSignature_ != runtimeStateSignature) {
        return false;
    }

    const bool sameFrame = frameUpdateNumber_ == frameNumber;
    if (!sameFrame &&
        (!frameUpdateStableAcrossFrames_ || !stableAcrossFrames)) {
        return false;
    }

    frameUpdateNumber_ = frameNumber;
    action = frameUpdateAction_;
    rendererMaterialized = frameUpdateRendererMaterialized_;
    return true;
}

void TileRasterOverlayState::recordFrameUpdate(
    uint64_t frameNumber,
    uint64_t mappingIdentity,
    uint64_t configuration,
    uint64_t providerMappingRevision,
    uint64_t contentRevision,
    uint64_t runtimeStateSignature,
    bool stableAcrossFrames,
    TileRasterOverlayUpdateAction action,
    bool rendererMaterialized) {
    if (frameNumber == 0) {
        invalidateFrameUpdateCache();
        return;
    }
    frameUpdateCacheValid_ = true;
    frameUpdateRendererMaterialized_ = rendererMaterialized;
    frameUpdateNumber_ = frameNumber;
    frameUpdateMappingIdentity_ = mappingIdentity;
    frameUpdateConfiguration_ = configuration;
    frameUpdateProviderMappingRevision_ = providerMappingRevision;
    frameUpdateContentRevision_ = contentRevision;
    frameUpdateRuntimeStateSignature_ = runtimeStateSignature;
    frameUpdateStableAcrossFrames_ = stableAcrossFrames;
    frameUpdateAction_ = action;
}

void TileRasterOverlayState::attachReadyMappingsInMainThread(
    IPrepareRendererResources* pPrepRenderer) {
    if (!pPrepRenderer) {
        return;
    }
    for (auto& mapping : mappings_) {
        if (mapping) {
            mapping->attachReadyTileInMainThread(pPrepRenderer);
        }
    }
}

void TileRasterOverlayState::invalidateFrameUpdateCache() {
    frameUpdateCacheValid_ = false;
    frameUpdateRendererMaterialized_ = false;
    frameUpdateStableAcrossFrames_ = false;
}

uint64_t TileRasterOverlayState::runtimeStateSignature() const {
    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t signature = kOffset;
    for (const auto& mapping : mappings_) {
        signature ^= reinterpret_cast<uintptr_t>(mapping.get());
        signature *= kPrime;
        if (mapping) {
            signature ^= mapping->runtimeStateSignature();
            signature *= kPrime;
        }
    }
    return signature;
}

} // namespace earth_engine
