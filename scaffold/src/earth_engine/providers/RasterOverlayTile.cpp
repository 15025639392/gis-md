#include "RasterOverlayTile.h"
#include "RasterOverlayTileProvider.h"
#include "../renderer/RenderDevice.h"

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
    , state_(LoadState::Unloaded) {
#ifdef __ANDROID__
    // Android lifetime guard for retained raw RasterOverlayTile pointers.
    RasterOverlayTileProvider::registerLiveTileForLifetimeGuard(this);
#endif
}

RasterOverlayTile::RasterOverlayTile(RasterOverlayTileProvider& provider)
    : provider_(provider)
    , state_(LoadState::Placeholder) {
#ifdef __ANDROID__
    // Android lifetime guard for retained placeholder tile pointers.
    RasterOverlayTileProvider::registerLiveTileForLifetimeGuard(this);
#endif
}

RasterOverlayTile::~RasterOverlayTile() {
#ifdef __ANDROID__
    // Android lifetime guard: keep retained raw pointers in sync with the
    // provider-owned texture lifetime.
    RasterOverlayTileProvider::unregisterLiveTextureForLifetimeGuard(texture_.get());
    RasterOverlayTileProvider::unregisterLiveTileForLifetimeGuard(this);
#endif
}

void RasterOverlayTile::setTexture(std::unique_ptr<Texture> tex) {
#ifdef __ANDROID__
    // Android lifetime guard: texture raw pointers are retained by
    // renderer/mapping paths, so register only currently owned textures.
    RasterOverlayTileProvider::unregisterLiveTextureForLifetimeGuard(texture_.get());
#endif
    rendererResources_ = static_cast<void*>(tex.get());  // opaque handle
    texture_ = std::move(tex);
#ifdef __ANDROID__
    RasterOverlayTileProvider::registerLiveTextureForLifetimeGuard(texture_.get());
#endif
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
