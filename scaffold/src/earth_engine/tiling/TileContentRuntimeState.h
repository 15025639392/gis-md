#pragma once

#include "TileLoadState.h"
#include "TileRenderContentState.h"
#include "SurfaceTile.h"

#include <optional>

namespace earth_engine {

/// Cross-frame content lifecycle state for a tile.
///
/// This groups the terrain/glTF render resources with the load/content
/// classification that governs upload, unload and renderability decisions.
struct TileContentRuntimeState {
    TileRenderContentState renderContent;
    TileLoadState loadState = TileLoadState::Unloaded;
    TileContentKind contentKind = TileContentKind::Unknown;

    /// cesium-native UpsampledQuadtreeNode equivalent. The tile is not
    /// requestable; its render content is derived from an ancestor tile.
    bool upsampledFromParent = false;

    /// This upsampled tile exists to subdivide the surface for higher-detail
    /// raster imagery. It still derives terrain from its parent, but may keep
    /// refining while raster overlays report more detail.
    bool rasterUpsampledForMoreDetail = false;
    std::optional<RasterOverlayProjection> rasterDetailSourceProjection;

    bool derivesTerrainFromParent() const {
        return upsampledFromParent;
    }

    bool isTerrainAvailabilityUpsample() const {
        return upsampledFromParent && !rasterUpsampledForMoreDetail;
    }

    bool isRasterDetailUpsample() const {
        return upsampledFromParent && rasterUpsampledForMoreDetail;
    }

    void markTerrainAvailabilityUpsample() {
        upsampledFromParent = true;
        rasterUpsampledForMoreDetail = false;
        rasterDetailSourceProjection.reset();
    }

    void markRasterDetailUpsample() {
        upsampledFromParent = true;
        rasterUpsampledForMoreDetail = true;
        rasterDetailSourceProjection.reset();
    }

    void markRasterDetailUpsample(RasterOverlayProjection sourceProjection) {
        markRasterDetailUpsample();
        rasterDetailSourceProjection = sourceProjection;
    }

    void clearUpsampleKind() {
        upsampledFromParent = false;
        rasterUpsampledForMoreDetail = false;
        rasterDetailSourceProjection.reset();
    }
};

} // namespace earth_engine
