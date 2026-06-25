#pragma once

#include "TileContentUpsampleKind.h"
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
    /// requestable; its render content is derived from an ancestor tile. The
    /// kind distinguishes terrain availability fallback from raster-detail
    /// subdivision so request, unload and mesh preparation paths can keep their
    /// lifecycle rules separate.
    TileContentUpsampleKind contentUpsampleKind =
        TileContentUpsampleKind::None;
    std::optional<RasterOverlayProjection> rasterDetailSourceProjection;

    TileContentUpsampleKind upsampleKind() const {
        return contentUpsampleKind;
    }

    bool derivesTerrainFromParent() const {
        return upsampleKind() != TileContentUpsampleKind::None;
    }

    bool isTerrainAvailabilityUpsample() const {
        return upsampleKind() ==
               TileContentUpsampleKind::TerrainAvailability;
    }

    bool isRasterDetailUpsample() const {
        return upsampleKind() == TileContentUpsampleKind::RasterDetail;
    }

    void markTerrainAvailabilityUpsample() {
        contentUpsampleKind = TileContentUpsampleKind::TerrainAvailability;
        rasterDetailSourceProjection.reset();
    }

    void markRasterDetailUpsample() {
        contentUpsampleKind = TileContentUpsampleKind::RasterDetail;
        rasterDetailSourceProjection.reset();
    }

    void markRasterDetailUpsample(RasterOverlayProjection sourceProjection) {
        markRasterDetailUpsample();
        rasterDetailSourceProjection = sourceProjection;
    }

    void clearUpsampleKind() {
        contentUpsampleKind = TileContentUpsampleKind::None;
        rasterDetailSourceProjection.reset();
    }
};

} // namespace earth_engine
