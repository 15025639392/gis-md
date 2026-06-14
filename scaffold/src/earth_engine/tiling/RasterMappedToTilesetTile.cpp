#include "RasterMappedToTilesetTile.h"
#include "../layers/BasemapLayer.h"
#include "../tiling/TileSurface.h"

namespace earth_engine {

RasterMappedToTilesetTile::RasterMappedToTilesetTile() = default;

bool RasterMappedToTilesetTile::update(const TileKey& geometryKey,
                                        const Rectangle& geometryBounds,
                                        BasemapLayer* imageryLayer,
                                        RenderDevice* device) {
    if (!imageryLayer || !device) return false;

    // Try to find imagery at the geometry tile's zoom
    ImageryAttachment attachment;
    int preferredZoom = geometryKey.z;
    bool found = imageryLayer->resolveAttachmentForBounds(
        geometryBounds, preferredZoom, attachment);

    if (found) {
        bool changed = (readyKey_ != attachment.textureKey);
        readyKey_ = attachment.textureKey;
        readyTexture_ = attachment.texture;
        offsetU_ = attachment.uvOffsetU;
        offsetV_ = attachment.uvOffsetV;
        scaleU_ = attachment.uvScaleU;
        scaleV_ = attachment.uvScaleV;
        if (changed) {
            computeTranslationAndScale(geometryBounds);
            state_ = State::Attached;
        } else if (state_ == State::Unattached) {
            state_ = State::TemporarilyAttached;
        }
        return changed;
    }

    // Parent fallback: try lower zoom levels
    TileKey parentKey = geometryKey;
    while (parentKey.z > 0) {
        parentKey = TileKey{parentKey.schemeId, parentKey.z - 1,
                            parentKey.x >> 1, parentKey.y >> 1};
        found = imageryLayer->resolveAttachmentForBounds(
            geometryBounds, parentKey.z, attachment);
        if (found) {
            readyKey_ = attachment.textureKey;
            readyTexture_ = attachment.texture;
            offsetU_ = attachment.uvOffsetU;
            offsetV_ = attachment.uvOffsetV;
            scaleU_ = attachment.uvScaleU;
            scaleV_ = attachment.uvScaleV;
            computeTranslationAndScale(geometryBounds);
            state_ = State::Attached;
            return true;
        }
    }

    originalFailed_ = true;
    return false;
}

void RasterMappedToTilesetTile::computeTranslationAndScale(
    const Rectangle& geometryBounds) {
    if (!readyTexture_) return;

    // The imagery bounds are not directly available here; the
    // translation/scale was already computed in resolveAttachmentForBounds.
    // We store the values directly from the ImageryAttachment.
    // This method is a hook for future recomputation if needed.
    (void)geometryBounds;
}

} // namespace earth_engine
