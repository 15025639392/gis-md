#include "RasterMappedToTilesetTile.h"
#include "../layers/BasemapLayer.h"
#include "../tiling/TileSurface.h"

namespace earth_engine {

RasterMappedToTilesetTile::RasterMappedToTilesetTile() = default;

bool RasterMappedToTilesetTile::update(const TileKey& geometryKey,
                                        const Rectangle& geometryBounds,
                                        BasemapLayer* imageryLayer,
                                        RenderDevice* /*device*/) {
    if (!imageryLayer) return false;

    // cesium-native RasterMappedTo3DTile::update:
    // 1. If already attached with ready tile, check if more detail available
    if (state_ == State::Attached && readyKey_.z >= geometryKey.z) {
        // Already have imagery at sufficient resolution
        return false;
    }

    // 2. Try to load a NEW tile at the desired zoom
    ImageryAttachment attachment;
    int desiredZoom = geometryKey.z;
    bool found = imageryLayer->resolveAttachmentForBounds(
        geometryBounds, desiredZoom, attachment);

    if (found) {
        // New tile is different from current ready tile
        if (readyKey_ != attachment.textureKey) {
            // Store OLD ready tile as fallback, set NEW tile as ready
            loadingKey_ = attachment.textureKey;
            // Immediately promote to ready (simplified: no async loading state)
            readyKey_ = attachment.textureKey;
            readyTexture_ = attachment.texture;
            offsetU_ = attachment.uvOffsetU;
            offsetV_ = attachment.uvOffsetV;
            scaleU_ = attachment.uvScaleU;
            scaleV_ = attachment.uvScaleV;
            state_ = State::Attached;
            originalFailed_ = false;
            return true;
        }
        // Same tile, update state if needed
        if (state_ == State::Unattached) {
            state_ = State::TemporarilyAttached;
        }
        return false;
    }

    // 3. Parent fallback: try progressively lower zoom levels
    if (!found || originalFailed_) {
        TileKey parentKey = geometryKey;
        while (parentKey.z > 0) {
            parentKey = TileKey{parentKey.schemeId, parentKey.z - 1,
                                parentKey.x >> 1, parentKey.y >> 1};
            found = imageryLayer->resolveAttachmentForBounds(
                geometryBounds, parentKey.z, attachment);
            if (found) {
                if (readyKey_ != attachment.textureKey) {
                    readyKey_ = attachment.textureKey;
                    readyTexture_ = attachment.texture;
                    offsetU_ = attachment.uvOffsetU;
                    offsetV_ = attachment.uvOffsetV;
                    scaleU_ = attachment.uvScaleU;
                    scaleV_ = attachment.uvScaleV;
                    state_ = State::Attached;
                }
                return true;
            }
        }
    }

    // 4. Nothing found
    if (!found) {
        originalFailed_ = true;
    }
    return false;
}

bool RasterMappedToTilesetTile::isMoreDetailAvailable() const {
    // cesium-native: check if a higher-zoom tile could be loaded
    // Simplified: if we're using parent fallback, more detail may be available
    return state_ == State::Attached && !originalFailed_;
}

void RasterMappedToTilesetTile::computeTranslationAndScale(
    const Rectangle& geometryBounds) {
    // Values are already stored from resolveAttachmentForBounds.
    // Recompute would need the imagery tile's rectangle, which isn't
    // directly available here. The values from ImageryAttachment
    // are already correct (computed via computeTranslationAndScale
    // inside resolveAttachmentForBounds).
    (void)geometryBounds;
}

} // namespace earth_engine
