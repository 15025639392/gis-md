#include "earth_engine/providers/VectorSurfaceFillRasterizer.h"

#include "earth_engine/platform/bridge/PlatformBridge.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

struct Prepared {
    const Feature* feature = nullptr;
    int drawOrder = 0;
    uint64_t identity = 0;
    std::array<float, 4> color{};
};

} // namespace

std::unique_ptr<DecodedImage> rasterizeSurfaceFill(
    const std::shared_ptr<const std::vector<Feature>>& featureSet,
    const Rectangle& tileBounds, double displayZoom,
    const SurfaceFillResolver& resolver,
    AmapSurfaceMaskRasterizerOptions::Projection projection) {
    auto image = std::make_unique<DecodedImage>();
    image->width = 256;
    image->height = 256;
    image->channels = 4;
    image->bytesPerChannel = 1;
    image->pixels.assign(static_cast<size_t>(image->width) *
                             static_cast<size_t>(image->height) * 4,
                         0);
    if (!featureSet || featureSet->empty() || !resolver) return image;

    // The resolver decides identity + color (source schema/style); the
    // rasterizer only composites.  Lower draw orders paint first.
    std::vector<Prepared> prepared;
    prepared.reserve(featureSet->size());
    for (const Feature& feature : *featureSet) {
        if (feature.type != GeometryType::Polygon) continue;
        const auto record = resolver(feature, displayZoom);
        if (!record || (*record).color[3] <= 0.0f) continue;
        prepared.push_back(Prepared{&feature, record->drawOrder,
                                    record->identity, record->color});
    }
    if (prepared.empty()) return image;
    std::stable_sort(prepared.begin(), prepared.end(),
                     [](const Prepared& lhs, const Prepared& rhs) {
                         return lhs.drawOrder < rhs.drawOrder;
                     });

    const size_t pixelCount = static_cast<size_t>(image->width) *
                              static_cast<size_t>(image->height);
    std::vector<float> premultipliedRgb(pixelCount * 3, 0.0f);
    std::vector<float> alpha(pixelCount, 0.0f);

    size_t begin = 0;
    while (begin < prepared.size()) {
        size_t end = begin + 1;
        while (end < prepared.size() &&
               prepared[end].drawOrder == prepared[begin].drawOrder &&
               prepared[end].identity == prepared[begin].identity) {
            ++end;
        }

        std::vector<const Feature*> group;
        group.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) group.push_back(prepared[i].feature);
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

} // namespace earth_engine
