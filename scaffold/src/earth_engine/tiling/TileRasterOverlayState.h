#pragma once

#include "SurfaceTile.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace earth_engine {

class IPrepareRendererResources;
class RasterMappedToTilesetTile;

class TileRasterOverlayState {
public:
    std::vector<std::unique_ptr<RasterMappedToTilesetTile>>& mappings() {
        return mappings_;
    }
    const std::vector<std::unique_ptr<RasterMappedToTilesetTile>>& mappings()
        const {
        return mappings_;
    }

    std::vector<RasterOverlayProjection>& missingProjections() {
        return missingProjections_;
    }
    const std::vector<RasterOverlayProjection>& missingProjections() const {
        return missingProjections_;
    }

    void ensureMappingSlots(size_t count) {
        if (mappings_.size() < count) {
            mappings_.resize(count);
        }
    }
    void resizeMappingSlots(size_t count,
                            IPrepareRendererResources* pPrepRenderer);
    size_t mappingCount() const { return mappings_.size(); }
    RasterMappedToTilesetTile* mappingAt(size_t index);
    const RasterMappedToTilesetTile* mappingAt(size_t index) const;
    RasterMappedToTilesetTile& ensureMapping(size_t index);
    void releaseMapping(size_t index,
                        IPrepareRendererResources* pPrepRenderer);
    bool hasReadyMapping(size_t index) const;
    template <typename Fn>
    void forEachMapping(Fn&& fn) const {
        for (const auto& mapping : mappings_) {
            fn(mapping.get());
        }
    }

    void clearMissingProjections() { missingProjections_.clear(); }
    bool hasMissingProjections() const { return !missingProjections_.empty(); }
    int missingProjectionCount() const {
        return static_cast<int>(missingProjections_.size());
    }

    void releaseReferences(IPrepareRendererResources* pPrepRenderer);
    void releaseAndClearReferences(IPrepareRendererResources* pPrepRenderer);

private:
    std::vector<std::unique_ptr<RasterMappedToTilesetTile>> mappings_;
    std::vector<RasterOverlayProjection> missingProjections_;
};

} // namespace earth_engine
