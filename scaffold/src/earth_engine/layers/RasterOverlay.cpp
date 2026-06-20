#include "RasterOverlay.h"
#include "../providers/ImageryProvider.h"
#include "../tiling/TileScheme.h"

#include <algorithm>

namespace earth_engine {

RasterOverlay::RasterOverlay(std::unique_ptr<ImageryProvider> provider,
                             std::unique_ptr<TileScheme> scheme,
                             Options options)
    : provider_(std::move(provider))
    , scheme_(std::move(scheme))
    , options_(std::move(options))
    , id_(provider_->id()) {
    if (options_.maximumSimultaneousTileLoads <= 0) {
        options_.maximumSimultaneousTileLoads = 20;  // cesium-native default
    }
    if (options_.maximumScreenSpaceError <= 0.0) {
        options_.maximumScreenSpaceError = 2.0;
    }
    if (options_.maximumTextureSize <= 0) {
        options_.maximumTextureSize = 2048;
    }
    options_.opacity = std::clamp(options_.opacity, 0.0f, 1.0f);
}

RasterOverlay::~RasterOverlay() = default;

void RasterOverlay::setOpacity(float opacity) {
    options_.opacity = std::clamp(opacity, 0.0f, 1.0f);
}

} // namespace earth_engine
