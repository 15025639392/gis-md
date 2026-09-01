#include "earth_engine/providers/VectorSurfaceFillImageryProvider.h"

#include "earth_engine/platform/bridge/PlatformBridge.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

int discreteZoom(double displayZoom) {
    if (!std::isfinite(displayZoom)) return 0;
    return static_cast<int>(std::clamp(displayZoom, 0.0, 255.0));
}

} // namespace

VectorSurfaceFillImageryProvider::VectorSurfaceFillImageryProvider(
    std::unique_ptr<TileScheme> scheme, FeatureFetch fetch,
    SurfaceFillResolver resolver, double displayZoom)
    : scheme_(std::move(scheme)), fetch_(std::move(fetch)),
      resolver_(std::move(resolver)) {
    packedState_.store(static_cast<uint64_t>(discreteZoom(displayZoom)),
                       std::memory_order_release);
}

std::string VectorSurfaceFillImageryProvider::schemeId() const {
    return scheme_ ? scheme_->id() : std::string();
}

uint64_t VectorSurfaceFillImageryProvider::contentRevision() const {
    return packedState_.load(std::memory_order_acquire) >> 8u;
}

std::string VectorSurfaceFillImageryProvider::buildUrl(const TileKey& key) const {
    return "vector-surface-fill://" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

bool VectorSurfaceFillImageryProvider::supportsTile(const TileKey& key) const {
    if (!scheme_ || key.schemeId != schemeId() || key.z < minZoom() ||
        key.z > maxZoom() || key.z >= 31 || key.x < 0 || key.y < 0) {
        return false;
    }
    const int tiles = 1 << key.z;
    return key.x < tiles * 2 && key.y < tiles;
}

void VectorSurfaceFillImageryProvider::requestTile(
    const TileKey& key, CancellationToken token, TileCallback callback,
    HttpRequestPriority priority) {
    if (!callback) return;
    if (!supportsTile(key) || !fetch_ || !resolver_ || token.isCancelled()) {
        callback(key, nullptr);
        return;
    }
    const double displayZoom = static_cast<double>(packedState_.load(
        std::memory_order_acquire) & 0xFFu);
    if (!scheme_) {
        callback(key, nullptr);
        return;
    }
    const Rectangle tileBounds = scheme_->tileToRectangle(key);
    const bool geographic = key.schemeId == "Geographic-TMS";
    const AmapSurfaceMaskRasterizerOptions::Projection projection =
        geographic
            ? AmapSurfaceMaskRasterizerOptions::Projection::Geographic
            : AmapSurfaceMaskRasterizerOptions::Projection::WebMercator;
    auto fetch = fetch_;
    auto resolver = resolver_;
    fetch(key, token,
          [key, token, callback = std::move(callback), tileBounds, displayZoom,
           resolver, projection](FeatureSet features) mutable {
              if (token.isCancelled() || !features) {
                  callback(key, nullptr);
                  return;
              }
              callback(key,
                       rasterizeSurfaceFill(
                           features, tileBounds, displayZoom, resolver,
                           projection));
          },
          priority);
}

std::unique_ptr<DecodedImage> VectorSurfaceFillImageryProvider::decodeTile(
    const uint8_t* /*data*/, size_t /*len*/) {
    return nullptr;
}

void VectorSurfaceFillImageryProvider::setDisplayZoom(double displayZoom) {
    const int next = discreteZoom(displayZoom);
    uint64_t current = packedState_.load(std::memory_order_acquire);
    for (;;) {
        if ((current & 0xFFu) == static_cast<uint64_t>(next)) return;
        const uint64_t nextPacked = (((current >> 8u) + 1u) << 8u) |
                                    static_cast<uint64_t>(next);
        if (packedState_.compare_exchange_weak(
                current, nextPacked,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
}

} // namespace earth_engine
