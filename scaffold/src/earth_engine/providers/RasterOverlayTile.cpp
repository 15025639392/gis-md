#include "RasterOverlayTile.h"
#include "RasterOverlayTileProvider.h"
#include "../renderer/RenderDevice.h"

#include <algorithm>
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
    , state_(LoadState::Unloaded)
    , textureByteLedger_(provider.textureByteLedgerForTiles()) {
}

RasterOverlayTile::RasterOverlayTile(RasterOverlayTileProvider& provider)
    : provider_(provider)
    , state_(LoadState::Placeholder)
    , textureByteLedger_(provider.textureByteLedgerForTiles()) {
}

RasterOverlayTile::~RasterOverlayTile() {
    updateTextureByteAccounting(0);
}

void RasterOverlayTile::setTexture(std::unique_ptr<Texture> tex) {
    rendererResources_ = static_cast<void*>(tex.get());  // opaque handle
    texture_ = std::move(tex);
    const int64_t textureBytes =
        texture_ ? static_cast<int64_t>(texture_->sizeBytes()) : 0;
    updateTextureByteAccounting(textureBytes);
    if (texture_) {
        emptyComposition_ = false;
        state_ = LoadState::Loaded;
    }
}

void RasterOverlayTile::markLoadedWithoutTexture() {
    texture_.reset();
    updateTextureByteAccounting(0);
    rendererResources_ = nullptr;
    emptyComposition_ = true;
    state_ = LoadState::Loaded;
}

void RasterOverlayTile::updateTextureByteAccounting(int64_t nextBytes) {
    nextBytes = std::max<int64_t>(0, nextBytes);
    if (!textureByteLedger_ || nextBytes == accountedTextureBytes_) {
        accountedTextureBytes_ = nextBytes;
        return;
    }
    textureByteLedger_->bytes.fetch_add(
        nextBytes - accountedTextureBytes_,
        std::memory_order_relaxed);
    accountedTextureBytes_ = nextBytes;
}

void RasterOverlayTile::loadInMainThread() {
    // cesium-native: transitions Loaded → Done.
    // In cesium-native, this is where GPU resources are created from image data.
    // In our architecture, the texture may already be uploaded by the Provider.
    // The rendererResources_ pointer already points to the Texture.
    if (texture_ && rendererResources_ == nullptr) {
        rendererResources_ = static_cast<void*>(texture_.get());
    }
    if (state_ == LoadState::Loaded) {
        state_ = LoadState::Done;
    }
}

RasterOverlayTile::MoreDetailAvailable
RasterOverlayTile::isMoreDetailAvailable() const {
    return moreDetailAvailable_;
}

} // namespace earth_engine
