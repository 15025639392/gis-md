#pragma once

#include "DirectRasterMapping.h"
#include "SurfaceTile.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace earth_engine {

class IPrepareRendererResources;

struct TileRasterOverlayUpdateAction {
    bool unloadTileContent = false;
    bool createRasterOverlayUpsampledChildren = false;
};

class TileRasterOverlayState {
public:
    std::vector<std::unique_ptr<DirectRasterMapping>>& mappings() {
        invalidateFrameUpdateCache();
        return mappings_;
    }
    const std::vector<std::unique_ptr<DirectRasterMapping>>& mappings()
        const {
        return mappings_;
    }

    std::vector<RasterOverlayProjection>& missingProjections() {
        invalidateFrameUpdateCache();
        return missingProjections_;
    }
    const std::vector<RasterOverlayProjection>& missingProjections() const {
        return missingProjections_;
    }

    void ensureMappingSlots(size_t count) {
        if (mappings_.size() < count) {
            invalidateFrameUpdateCache();
            mappings_.resize(count);
        }
    }
    void resizeMappingSlots(size_t count,
                            IPrepareRendererResources* pPrepRenderer);
    size_t mappingCount() const { return mappings_.size(); }
    DirectRasterMapping* mappingAt(size_t index);
    const DirectRasterMapping* mappingAt(size_t index) const;
    DirectRasterMapping& ensureMapping(size_t index);
    void releaseMapping(size_t index,
                        IPrepareRendererResources* pPrepRenderer);
    /// Cover-ready for selection/renderability: a failed ready raster still
    /// counts here so required imagery does not permanently block geometry.
    bool hasReadyMapping(size_t index) const;
    /// Drawable-ready for render command binding: requires a real Loaded/Done
    /// raster tile with a texture, matching SurfaceRasterBinding.
    bool hasDrawableReadyMapping(size_t index) const;
    template <typename Fn>
    void forEachMapping(Fn&& fn) const {
        for (const auto& mapping : mappings_) {
            fn(mapping.get());
        }
    }

    void clearMissingProjections() {
        if (!missingProjections_.empty()) {
            invalidateFrameUpdateCache();
            missingProjections_.clear();
        }
    }
    bool hasMissingProjections() const { return !missingProjections_.empty(); }
    int missingProjectionCount() const {
        return static_cast<int>(missingProjections_.size());
    }
    void synchronizeMappingIdentity(uint64_t mappingIdentity,
                                    IPrepareRendererResources* pPrepRenderer);

    void releaseReferences(IPrepareRendererResources* pPrepRenderer);
    void releaseAndClearReferences(IPrepareRendererResources* pPrepRenderer);

    bool tryReuseFrameUpdate(
        uint64_t frameNumber,
        uint64_t mappingIdentity,
        uint64_t configuration,
        uint64_t providerMappingRevision,
        uint64_t contentRevision,
        uint64_t runtimeStateSignature,
        bool stableAcrossFrames,
        TileRasterOverlayUpdateAction& action,
        bool& rendererMaterialized);
    void recordFrameUpdate(
        uint64_t frameNumber,
        uint64_t mappingIdentity,
        uint64_t configuration,
        uint64_t providerMappingRevision,
        uint64_t contentRevision,
        uint64_t runtimeStateSignature,
        bool stableAcrossFrames,
        TileRasterOverlayUpdateAction action,
        bool rendererMaterialized);
    void attachReadyMappingsInMainThread(
        IPrepareRendererResources* pPrepRenderer);
    void invalidateFrameUpdateCache();
    uint64_t runtimeStateSignature() const;

    uint64_t authoritativeUpdateCount() const {
        return authoritativeUpdateCount_;
    }
    void countAuthoritativeUpdate() {
        ++authoritativeUpdateCount_;
    }

private:
    std::vector<std::unique_ptr<DirectRasterMapping>> mappings_;
    std::vector<RasterOverlayProjection> missingProjections_;
    bool hasMappingIdentity_ = false;
    uint64_t mappingIdentity_ = 0;
    bool frameUpdateCacheValid_ = false;
    bool frameUpdateRendererMaterialized_ = false;
    uint64_t frameUpdateNumber_ = 0;
    uint64_t frameUpdateMappingIdentity_ = 0;
    uint64_t frameUpdateConfiguration_ = 0;
    uint64_t frameUpdateProviderMappingRevision_ = 0;
    uint64_t frameUpdateContentRevision_ = 0;
    uint64_t frameUpdateRuntimeStateSignature_ = 0;
    bool frameUpdateStableAcrossFrames_ = false;
    TileRasterOverlayUpdateAction frameUpdateAction_;
    uint64_t authoritativeUpdateCount_ = 0;
};

} // namespace earth_engine
