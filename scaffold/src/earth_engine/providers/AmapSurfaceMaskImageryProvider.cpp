#include "AmapSurfaceMaskImageryProvider.h"

#include "../data/AmapSurfaceMaskRasterizer.h"
#include "../style/AmapClassicStyleInternal.h"
#include "../style/AmapClassicZoom.h"
#include "../tiling/TileScheme.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace earth_engine {
namespace {

struct PreparedFeature {
    const Feature* feature = nullptr;
    int drawOrder = 0;
    int classCode = 0;
    int subKey = 0;
    std::array<float, 4> color{};
};

std::optional<int> parseIntegerProperty(
    const std::unordered_map<std::string, std::string>& properties,
    const char* name) {
    const auto it = properties.find(name);
    if (it == properties.end() || it->second.empty()) return std::nullopt;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(it->second.c_str(), &end, 10);
    if (errno == ERANGE || end == it->second.c_str() || *end != '\0' ||
        value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::unique_ptr<DecodedImage> makeAmapSurfaceMaskImageImpl(
    const AmapSurfaceMaskImageryProvider::FeatureSet& featureSet,
    const Rectangle& tileBounds,
    double displayZoom,
    AmapSurfaceMaskRasterizerOptions::Projection projection) {
    auto image = std::make_unique<DecodedImage>();
    image->width = 256;
    image->height = 256;
    image->channels = 4;
    image->bytesPerChannel = 1;
    image->pixels.assign(static_cast<size_t>(image->width) *
                             static_cast<size_t>(image->height) * 4,
                         0);

    if (!featureSet || featureSet->empty()) return image;

    // FeatureRenderLayer paints lower official draw orders first.  Keep the
    // source order stable for ties; this is important for overlapping records
    // with equal draw order and mirrors stable vector-source ingestion.
    std::vector<PreparedFeature> prepared;
    prepared.reserve(featureSet->size());
    for (const Feature& feature : *featureSet) {
        if (feature.type != GeometryType::Polygon ||
            feature.properties.count("amap_height") != 0) {
            continue;
        }
        const auto classCode =
            parseIntegerProperty(feature.properties, "amap_class");
        const auto subKey =
            parseIntegerProperty(feature.properties, "amap_subkey");
        if (!classCode || !subKey) continue;
        const auto color = amapClassicSurfaceColorForDisplayZoom(
            *classCode, *subKey, displayZoom);
        if (!color || (*color)[3] <= 0.0f) continue;
        const auto drawOrder =
            parseIntegerProperty(feature.properties, "amap_draworder")
                .value_or(0);
        prepared.push_back(PreparedFeature{&feature, drawOrder, *classCode,
                                           *subKey, *color});
    }
    std::stable_sort(
        prepared.begin(), prepared.end(),
        [](const PreparedFeature& lhs, const PreparedFeature& rhs) {
            return lhs.drawOrder < rhs.drawOrder;
        });

    // Keep premultiplied RGB and alpha while composing.  This is the standard
    // source-over operation and avoids precision loss at antialiased edges.
    const size_t pixelCount = static_cast<size_t>(image->width) *
                              static_cast<size_t>(image->height);
    std::vector<float> premultipliedRgb(pixelCount * 3, 0.0f);
    std::vector<float> alpha(pixelCount, 0.0f);

    size_t begin = 0;
    while (begin < prepared.size()) {
        size_t end = begin + 1;
        while (end < prepared.size() &&
               prepared[end].drawOrder == prepared[begin].drawOrder &&
               prepared[end].classCode == prepared[begin].classCode &&
               prepared[end].subKey == prepared[begin].subKey) {
            ++end;
        }

        std::vector<const Feature*> group;
        group.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            group.push_back(prepared[i].feature);
        }
        AmapSurfaceMaskRasterizerOptions rasterOptions;
        rasterOptions.projection = projection;
        const AmapSurfaceMask mask = rasterizeAmapSurfaceMask(
            group, tileBounds, rasterOptions);
        const std::array<float, 4>& color = prepared[begin].color;
        for (int y = 0; y < image->height; ++y) {
            for (int x = 0; x < image->width; ++x) {
                const uint8_t coverage = mask.sample(x, y);
                if (coverage == 0) continue;
                const size_t pixel = static_cast<size_t>(y) *
                                         static_cast<size_t>(image->width) +
                                     static_cast<size_t>(x);
                const float sourceAlpha =
                    color[3] * static_cast<float>(coverage) / 255.0f;
                if (sourceAlpha <= 0.0f) continue;
                const float inverse = 1.0f - sourceAlpha;
                const float destinationAlpha = alpha[pixel];
                const size_t rgb = pixel * 3;
                premultipliedRgb[rgb + 0] =
                    color[0] * sourceAlpha + premultipliedRgb[rgb + 0] * inverse;
                premultipliedRgb[rgb + 1] =
                    color[1] * sourceAlpha + premultipliedRgb[rgb + 1] * inverse;
                premultipliedRgb[rgb + 2] =
                    color[2] * sourceAlpha + premultipliedRgb[rgb + 2] * inverse;
                alpha[pixel] = sourceAlpha + destinationAlpha * inverse;
            }
        }
        begin = end;
    }

    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const float a = std::clamp(alpha[pixel], 0.0f, 1.0f);
        const size_t dst = pixel * 4;
        image->pixels[dst + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
        if (a <= 0.0f) continue;
        const size_t rgb = pixel * 3;
        image->pixels[dst + 0] = static_cast<uint8_t>(std::lround(
            std::clamp(premultipliedRgb[rgb + 0] / a, 0.0f, 1.0f) * 255.0f));
        image->pixels[dst + 1] = static_cast<uint8_t>(std::lround(
            std::clamp(premultipliedRgb[rgb + 1] / a, 0.0f, 1.0f) * 255.0f));
        image->pixels[dst + 2] = static_cast<uint8_t>(std::lround(
            std::clamp(premultipliedRgb[rgb + 2] / a, 0.0f, 1.0f) * 255.0f));
    }
    return image;
}

} // namespace

std::unique_ptr<DecodedImage> makeAmapSurfaceMaskImage(
    const std::shared_ptr<const std::vector<Feature>>& featureSet,
    const Rectangle& tileBounds, double displayZoom,
    AmapSurfaceMaskRasterizerOptions::Projection projection) {
    return makeAmapSurfaceMaskImageImpl(
        featureSet, tileBounds, displayZoom, projection);
}

SurfaceFillResolver amapSurfaceFillResolver() {
    return [](const Feature& feature, double displayZoom)
        -> std::optional<SurfaceFillRecord> {
        if (feature.type != GeometryType::Polygon ||
            feature.properties.count("amap_height") != 0) {
            return std::nullopt;
        }
        const auto classCode =
            parseIntegerProperty(feature.properties, "amap_class");
        const auto subKey =
            parseIntegerProperty(feature.properties, "amap_subkey");
        if (!classCode || !subKey) return std::nullopt;
        const auto color = amapClassicSurfaceColorForDisplayZoom(
            *classCode, *subKey, displayZoom);
        if (!color || (*color)[3] <= 0.0f) return std::nullopt;
        SurfaceFillRecord record;
        record.color = *color;
        record.drawOrder =
            parseIntegerProperty(feature.properties, "amap_draworder")
                .value_or(0);
        record.identity =
            (static_cast<uint64_t>(*classCode) << 32) |
            static_cast<uint64_t>(*subKey);
        return record;
    };
}

AmapSurfaceMaskStyleState::AmapSurfaceMaskStyleState(double displayZoom) {
    const double discrete = amapClassicDiscreteZoomValue(displayZoom);
    const uint64_t zoom = std::isfinite(discrete)
        ? static_cast<uint64_t>(std::clamp(static_cast<int>(discrete), 0, 255))
        : 0;
    packedState_.store(zoom, std::memory_order_release);
}

void AmapSurfaceMaskStyleState::setDisplayZoom(double displayZoom) {
    const double discrete = amapClassicDiscreteZoomValue(displayZoom);
    if (!std::isfinite(discrete)) return;
    const uint64_t nextZoom = static_cast<uint64_t>(
        std::clamp(static_cast<int>(discrete), 0, 255));
    uint64_t current = packedState_.load(std::memory_order_acquire);
    for (;;) {
        if ((current & 0xFFu) == nextZoom) return;
        const uint64_t next = (((current >> 8u) + 1u) << 8u) | nextZoom;
        if (packedState_.compare_exchange_weak(
                current, next,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
}

double AmapSurfaceMaskStyleState::displayZoom() const {
    return snapshot().displayZoom;
}

uint64_t AmapSurfaceMaskStyleState::revision() const {
    return snapshot().revision;
}

AmapSurfaceMaskStyleState::Snapshot
AmapSurfaceMaskStyleState::snapshot() const {
    const uint64_t packed = packedState_.load(std::memory_order_acquire);
    return Snapshot{
        static_cast<double>(packed & 0xFFu),
        packed >> 8u};
}

AmapSurfaceMaskImageryProvider::AmapSurfaceMaskImageryProvider(
    SurfaceFeatureFetch fetch, double displayZoom)
    : AmapSurfaceMaskImageryProvider(
          std::move(fetch),
          std::make_shared<AmapSurfaceMaskStyleState>(displayZoom)) {}

AmapSurfaceMaskImageryProvider::AmapSurfaceMaskImageryProvider(
    SurfaceFeatureFetch fetch,
    std::shared_ptr<AmapSurfaceMaskStyleState> styleState)
    : fetch_(std::move(fetch)), styleState_(std::move(styleState)) {
    if (!styleState_) {
        styleState_ = std::make_shared<AmapSurfaceMaskStyleState>();
    }
}

std::string AmapSurfaceMaskImageryProvider::buildUrl(const TileKey& key) const {
    return "amap-surface-mask://" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

bool AmapSurfaceMaskImageryProvider::supportsTile(const TileKey& key) const {
    if (key.schemeId != schemeId() || key.z < minZoom() ||
        key.z > maxZoom() || key.z >= 31 || key.x < 0 || key.y < 0) {
        return false;
    }
    const int tiles = 1 << key.z;
    return key.x < tiles * 2 && key.y < tiles;
}

void AmapSurfaceMaskImageryProvider::requestTile(
    const TileKey& key, CancellationToken token, TileCallback callback,
    HttpRequestPriority priority) {
    if (!callback) return;
    if (!supportsTile(key) || !fetch_ || token.isCancelled()) {
        callback(key, nullptr);
        return;
    }

    auto fetch = fetch_;
    const double displayZoom = this->displayZoom();
    auto targetScheme = TileScheme::createGeographicTMS();
    const Rectangle tileBounds = targetScheme->tileToRectangle(key);
    fetch(key, token,
          [key, token, callback = std::move(callback), tileBounds,
           displayZoom](FeatureSet features) mutable {
              if (token.isCancelled() || !features) {
                  callback(key, nullptr);
                  return;
              }
              callback(key,
                       makeAmapSurfaceMaskImageImpl(
                           features, tileBounds, displayZoom,
                           AmapSurfaceMaskRasterizerOptions::Projection::Geographic));
          },
          priority);
}

std::unique_ptr<DecodedImage> AmapSurfaceMaskImageryProvider::decodeTile(
    const uint8_t* /*data*/, size_t /*len*/) {
    // This provider is generated from decoded vector features, not an image
    // transport.  There is intentionally no byte-stream decode path.
    return nullptr;
}

} // namespace earth_engine
