#include "RasterOverlayTile.h"
#include "RasterOverlayTileProvider.h"

#include <utility>

namespace earth_engine {

RasterOverlayTile::RasterOverlayTile(RasterOverlayTileProvider& provider,
                                     const TileKey& key,
                                     const Rectangle& bounds,
                                     std::string cacheKey)
    : provider_(provider)
    , key_(key)
    , cacheKey_(std::move(cacheKey))
    , bounds_(bounds)
    , state_(LoadState::Unloaded) {}

RasterOverlayTile::RasterOverlayTile(RasterOverlayTileProvider& provider)
    : provider_(provider)
    , state_(LoadState::Placeholder) {}

RasterOverlayTile::~RasterOverlayTile() = default;

void RasterOverlayTile::setTexture(std::unique_ptr<Texture> tex) {
    rendererResources_ = static_cast<void*>(tex.get());  // opaque handle
    texture_ = std::move(tex);
    if (texture_) {
        state_ = LoadState::Loaded;
    }
}

void RasterOverlayTile::loadInMainThread() {
    // cesium-native: transitions Loaded → Done.
    // In cesium-native, this is where GPU resources are created from image data.
    // In our architecture, the texture may already be uploaded by the Provider.
    // The rendererResources_ pointer already points to the Texture.
    if (state_ == LoadState::Loaded) {
        state_ = LoadState::Done;
    }
}

RasterOverlayTile::MoreDetailAvailable
RasterOverlayTile::isMoreDetailAvailable() const {
    return moreDetailAvailable_;
}

} // namespace earth_engine
