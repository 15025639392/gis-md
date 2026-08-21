#include "RasterOverlayImageCompositing.h"

#include "../debug/PlatformLog.h"
#include "../tiling/RasterOverlayProjection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

struct PixelRectangle {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct CombinedImageMeasurements {
    Rectangle rectangle;
    int width = 0;
    int height = 0;
    int channels = 0;
    int bytesPerChannel = 1;
};

/// Projected-equivalent bounds for a raster source, used by
/// measureCombinedImage after the pipeline entry projection.
struct ProjectedSource {
    Rectangle bounds;
    std::optional<Rectangle> sourceSubset;
};

PixelRectangle computePixelRectangle(
    const DecodedImage& image,
    const Rectangle& totalRectangle,
    const Rectangle& partRectangle) {

    int x = static_cast<int>(MathUtils::roundDown(
        image.width * (partRectangle.west() - totalRectangle.west()) /
            totalRectangle.width(),
        kCompositingPixelTolerance));
    x = std::max(0, x);

    int y = static_cast<int>(MathUtils::roundDown(
        image.height * (totalRectangle.north() - partRectangle.north()) /
            totalRectangle.height(),
        kCompositingPixelTolerance));
    y = std::max(0, y);

    int maxX = static_cast<int>(MathUtils::roundUp(
        image.width * (partRectangle.east() - totalRectangle.west()) /
            totalRectangle.width(),
        kCompositingPixelTolerance));
    maxX = std::min(maxX, image.width);

    int maxY = static_cast<int>(MathUtils::roundUp(
        image.height * (totalRectangle.north() - partRectangle.south()) /
            totalRectangle.height(),
        kCompositingPixelTolerance));
    maxY = std::min(maxY, image.height);

    return PixelRectangle{x, y, std::max(0, maxX - x), std::max(0, maxY - y)};
}

CombinedImageMeasurements measureCombinedImage(
    const Rectangle& targetBounds,
    const std::vector<ProjectedSource>& projectedSources,
    const std::vector<RasterSourceResult>& sources,
    double projectedWidthPerPixel,
    double projectedHeightPerPixel) {
    std::optional<Rectangle> combinedBounds;
    int channels = 0;
    int bytesPerChannel = 1;
    for (size_t i = 0; i < sources.size(); ++i) {
        const RasterSourceResult& source = sources[i];
        if (source.image) {
            channels = std::max(channels, source.image->channels);
            bytesPerChannel =
                std::max(bytesPerChannel, source.image->bytesPerChannel);
        }
        const Rectangle sourceRect = i < projectedSources.size()
            ? projectedSources[i].sourceSubset.value_or(projectedSources[i].bounds)
            : source.bounds;
        // Projection-space intersection — compute directly without
        // antimeridian wrapping (computeIntersection adds kTwoPi for geographic
        // coords, which corrupts projected meters).
        double projWest = std::max(targetBounds.west(), sourceRect.west());
        double projEast = std::min(targetBounds.east(), sourceRect.east());
        double projSouth = std::max(targetBounds.south(), sourceRect.south());
        double projNorth = std::min(targetBounds.north(), sourceRect.north());
        if (projWest >= projEast || projSouth >= projNorth) {
            continue; // no overlap in projection space
        }
        Rectangle intersection(projWest, projSouth, projEast, projNorth);

        const double roundedWest =
            MathUtils::roundDown(
                intersection.west() / projectedWidthPerPixel,
                kCompositingPixelTolerance) *
            projectedWidthPerPixel;
        const double roundedSouth =
            MathUtils::roundDown(
                intersection.south() / projectedHeightPerPixel,
                kCompositingPixelTolerance) *
            projectedHeightPerPixel;
        const double roundedEast =
            MathUtils::roundUp(
                intersection.east() / projectedWidthPerPixel,
                kCompositingPixelTolerance) *
            projectedWidthPerPixel;
        const double roundedNorth =
            MathUtils::roundUp(
                intersection.north() / projectedHeightPerPixel,
                kCompositingPixelTolerance) *
            projectedHeightPerPixel;

        if (roundedWest > roundedEast || roundedSouth > roundedNorth) {
            continue; // degenerate after rounding — skip
        }
        Rectangle expanded(
            roundedWest, roundedSouth, roundedEast, roundedNorth);

        if (expanded.west() == expanded.east()) {
            expanded = Rectangle(
                expanded.west(),
                expanded.south(),
                expanded.east() + projectedWidthPerPixel,
                expanded.north());
        }
        if (expanded.south() == expanded.north()) {
            expanded = Rectangle(
                expanded.west(),
                expanded.south(),
                expanded.east(),
                expanded.north() + projectedHeightPerPixel);
        }

        // Manual projected-space union — computeUnion calls
        // convertLongitudeRange which corrupts WebMercator meter values
        // (mods ~20M by 2π, turning 20M into ~0).
        // Use direct min/max instead, matching cesium-native behavior
        // where expanded rectangles are in projection space.
        if (!combinedBounds) {
            combinedBounds = expanded;
        } else {
            combinedBounds = Rectangle(
                std::min(combinedBounds->west(), expanded.west()),
                std::min(combinedBounds->south(), expanded.south()),
                std::max(combinedBounds->east(), expanded.east()),
                std::max(combinedBounds->north(), expanded.north()));
        }
    }

    if (!combinedBounds) {
        return {};
    }

    int width = static_cast<int>(MathUtils::roundUp(
        combinedBounds->computeWidth() / projectedWidthPerPixel,
        kCompositingPixelTolerance));
    int height = static_cast<int>(MathUtils::roundUp(
        combinedBounds->computeHeight() / projectedHeightPerPixel,
        kCompositingPixelTolerance));
    width = std::max(1, width);
    height = std::max(1, height);
    return CombinedImageMeasurements{
        *combinedBounds,
        width,
        height,
        channels,
        bytesPerChannel};
}

/// Unsafe row-wise memory copy with support for different source/target row
/// strides and channel counts. Matches cesium-native
/// ImageManipulation::unsafeBlitImage. When source has fewer channels than
/// target, the missing channels (alpha) are set to 0xFF.
void unsafeBlitImage(uint8_t* pTarget,
                     size_t targetRowStride,
                     size_t targetChannels,
                     int targetBytesPerChannel,
                     const uint8_t* pSource,
                     size_t sourceRowStride,
                     size_t sourceChannels,
                     int sourceBytesPerChannel,
                     size_t sourceWidth,
                     size_t sourceHeight,
                     size_t bytesPerPixel) {
    if (sourceChannels == targetChannels &&
        sourceBytesPerChannel == targetBytesPerChannel) {
        const size_t bytesToCopyPerRow = bytesPerPixel * sourceWidth;
        if (bytesToCopyPerRow == targetRowStride &&
            targetRowStride == sourceRowStride) {
            std::memcpy(pTarget, pSource,
                        sourceWidth * sourceHeight * bytesPerPixel);
        } else {
            for (size_t j = 0; j < sourceHeight; ++j) {
                std::memcpy(pTarget, pSource, bytesToCopyPerRow);
                pTarget += targetRowStride;
                pSource += sourceRowStride;
            }
        }
    } else {
        // Channel or bytesPerChannel mismatch: copy per-pixel/chan.
        const size_t sourceBytesPerChan = static_cast<size_t>(sourceBytesPerChannel);
        const size_t targetBytesPerChan = static_cast<size_t>(targetBytesPerChannel);
        const size_t sourceBytesPerPixel = sourceChannels * sourceBytesPerChan;
        for (size_t j = 0; j < sourceHeight; ++j) {
            for (size_t i = 0; i < sourceWidth; ++i) {
                size_t c = 0;
                for (; c < sourceChannels; ++c) {
                    size_t cOff = i * bytesPerPixel + c * targetBytesPerChan;
                    for (size_t b = 0; b < sourceBytesPerChan; ++b) {
                        pTarget[cOff + b] =
                            pSource[i * sourceBytesPerPixel +
                                    c * sourceBytesPerChan + b];
                    }
                    for (size_t b = sourceBytesPerChan; b < targetBytesPerChan; ++b) {
                        pTarget[cOff + b] = 0;
                    }
                }
                for (; c < targetChannels; ++c) {
                    std::memset(pTarget + i * bytesPerPixel + c * targetBytesPerChan,
                                0xFF, targetBytesPerChan);
                }
            }
            pTarget += targetRowStride;
            pSource += sourceRowStride;
        }
    }
}

/// Bilinear interpolation for 1-byte-per-channel images (cesium-native pattern).
void unsafeBilinearResize(uint8_t* pTarget,
                          int targetWidth,
                          int targetHeight,
                          size_t targetRowStride,
                          const uint8_t* pSource,
                          int sourceWidth,
                          int sourceHeight,
                          size_t sourceRowStride,
                          int channels) {
    for (int ty = 0; ty < targetHeight; ++ty) {
        const double sy_f = static_cast<double>(ty) *
            static_cast<double>(sourceHeight - 1) /
            static_cast<double>(std::max(targetHeight - 1, 1));
        const int sy0 = std::min(static_cast<int>(sy_f), sourceHeight - 2);
        const int sy1 = sy0 + 1;
        const double vy = sy_f - static_cast<double>(sy0);

        for (int tx = 0; tx < targetWidth; ++tx) {
            const double sx_f = static_cast<double>(tx) *
                static_cast<double>(sourceWidth - 1) /
                static_cast<double>(std::max(targetWidth - 1, 1));
            const int sx0 = std::min(static_cast<int>(sx_f), sourceWidth - 2);
            const int sx1 = sx0 + 1;
            const double vx = sx_f - static_cast<double>(sx0);

            for (int c = 0; c < channels; ++c) {
                const double p00 = pSource[static_cast<size_t>(sy0) * sourceRowStride +
                                           static_cast<size_t>(sx0) * static_cast<size_t>(channels) +
                                           static_cast<size_t>(c)];
                const double p10 = pSource[static_cast<size_t>(sy0) * sourceRowStride +
                                           static_cast<size_t>(sx1) * static_cast<size_t>(channels) +
                                           static_cast<size_t>(c)];
                const double p01 = pSource[static_cast<size_t>(sy1) * sourceRowStride +
                                           static_cast<size_t>(sx0) * static_cast<size_t>(channels) +
                                           static_cast<size_t>(c)];
                const double p11 = pSource[static_cast<size_t>(sy1) * sourceRowStride +
                                           static_cast<size_t>(sx1) * static_cast<size_t>(channels) +
                                           static_cast<size_t>(c)];
                const double top = p00 + (p10 - p00) * vx;
                const double bot = p01 + (p11 - p01) * vx;
                const double val = top + (bot - top) * vy;
                pTarget[static_cast<size_t>(ty) * targetRowStride +
                        static_cast<size_t>(tx) * static_cast<size_t>(channels) +
                        static_cast<size_t>(c)] =
                    static_cast<uint8_t>(std::clamp(val, 0.0, 255.0));
            }
        }
    }
}

}  // namespace

// ============================================================
// 投影工具
// ============================================================

RasterOverlayProjection projectionForSourceScheme(const TileScheme& scheme) {
    return scheme.crsProfile() == "EPSG:3857"
        ? RasterOverlayProjection::WebMercator
        : RasterOverlayProjection::Geographic;
}

RasterOverlayProjection projectionForScheme(
    const TileScheme& scheme,
    RasterOverlayGeoreference georeference) {
    if (georeference != RasterOverlayGeoreference::Gcj02WebMercator) {
        return projectionForSourceScheme(scheme);
    }
    if (scheme.crsProfile() == "EPSG:3857") {
        return RasterOverlayProjection::Gcj02WebMercator;
    }
    platformLog(LogLevel::Warning, "RasterOverlay",
                "GCJ-02 georeference requested but scheme '%s' "
                "(crs=%s) is not plain EPSG:3857; falling back to %s — "
                "imagery over China will stay ~500m offset",
                scheme.id().c_str(),
                scheme.crsProfile().c_str(),
                projectionForSourceScheme(scheme) ==
                        RasterOverlayProjection::WebMercator
                    ? "WebMercator"
                    : "Geographic");
    return projectionForSourceScheme(scheme);
}

Rectangle projectGeographicToProvider(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    return projectRasterSourceRectangle(rectangle, projection);
}

Rectangle unprojectProviderToGeographic(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    return unprojectRasterSourceRectangle(rectangle, projection);
}

double webMercatorY(double latRad) {
    const double lat = std::clamp(
        latRad, -kCompositingMaxWebMercatorLat, kCompositingMaxWebMercatorLat);
    return std::log(std::tan(lat * 0.5 + kCompositingPi * 0.25));
}

bool isWebMercatorScheme(const TileScheme& scheme) {
    const std::string id = scheme.id();
    return id == "XYZ-WebMercator" ||
           id == "TMS-WebMercator" ||
           id == "OpenGlobus-Earth";
}

double projectedSouth(const TileScheme& scheme, const Rectangle& bounds) {
    return isWebMercatorScheme(scheme) ? webMercatorY(bounds.south())
                                       : bounds.south();
}

double projectedNorth(const TileScheme& scheme, const Rectangle& bounds) {
    return isWebMercatorScheme(scheme) ? webMercatorY(bounds.north())
                                       : bounds.north();
}

double projectedHeight(const TileScheme& scheme, const Rectangle& bounds) {
    return std::max(
        1e-12,
        std::abs(projectedNorth(scheme, bounds) -
                 projectedSouth(scheme, bounds)));
}

double projectedVForLatitudeInternal(const TileScheme& scheme,
                                     const Rectangle& bounds,
                                     double lat) {
    const double north = projectedNorth(scheme, bounds);
    const double south = projectedSouth(scheme, bounds);
    const double h = std::max(1e-12, std::abs(north - south));
    const double projected = isWebMercatorScheme(scheme)
        ? webMercatorY(lat)
        : lat;
    return std::clamp((north - projected) / h, 0.0, 1.0);
}

// ============================================================
// 图像合成
// ============================================================

void blitImage(DecodedImage& target,
               const Rectangle& targetRectangle,
               const DecodedImage& source,
               const Rectangle& sourceRectangle,
               const std::optional<Rectangle>& sourceSubset) {
    const Rectangle& srcRect = sourceSubset.value_or(sourceRectangle);
    const double oWest = std::max(targetRectangle.west(), srcRect.west());
    const double oEast = std::min(targetRectangle.east(), srcRect.east());
    const double oSouth = std::max(targetRectangle.south(), srcRect.south());
    const double oNorth = std::min(targetRectangle.north(), srcRect.north());
    if (oWest >= oEast || oSouth >= oNorth) return;
    const Rectangle overlap(oWest, oSouth, oEast, oNorth);

    const PixelRectangle dst =
        computePixelRectangle(target, targetRectangle, overlap);
    const PixelRectangle src =
        computePixelRectangle(source, sourceRectangle, overlap);
    if (dst.width <= 0 || dst.height <= 0 ||
        src.width <= 0 || src.height <= 0) {
        return;
    }

    const size_t targetBytesPerPixel =
        static_cast<size_t>(target.channels) *
        static_cast<size_t>(target.bytesPerChannel);
    const size_t sourceBytesPerPixel =
        static_cast<size_t>(source.channels) *
        static_cast<size_t>(source.bytesPerChannel);
    const size_t sourceRowStride =
        static_cast<size_t>(source.width) * sourceBytesPerPixel;
    const size_t targetRowStride =
        static_cast<size_t>(target.width) * targetBytesPerPixel;
    const size_t bytesPerPixel = targetBytesPerPixel;

    uint8_t* pTargetRow = target.pixels.data() +
        static_cast<size_t>(dst.y) * targetRowStride +
        static_cast<size_t>(dst.x) * targetBytesPerPixel;
    const uint8_t* pSourceRow = source.pixels.data() +
        static_cast<size_t>(src.y) * sourceRowStride +
        static_cast<size_t>(src.x) * sourceBytesPerPixel;

    if (src.width == dst.width && src.height == dst.height) {
        unsafeBlitImage(pTargetRow, targetRowStride,
                        static_cast<size_t>(target.channels),
                        target.bytesPerChannel,
                        pSourceRow, sourceRowStride,
                        static_cast<size_t>(source.channels),
                        source.bytesPerChannel,
                        static_cast<size_t>(dst.width),
                        static_cast<size_t>(dst.height),
                        bytesPerPixel);
    } else {
        if (target.bytesPerChannel != 1 || source.bytesPerChannel != 1) {
            return;
        }
        const int channels = target.channels;
        if (source.channels < channels) {
            for (int y = 0; y < dst.height; ++y) {
                const int sy = std::clamp(
                    src.y + static_cast<int>(
                        (static_cast<int64_t>(y) * src.height) / dst.height),
                    0, source.height - 1);
                const int dy = dst.y + y;
                if (dy < 0 || dy >= target.height) continue;
                uint8_t* pTarget = target.pixels.data() +
                    static_cast<size_t>(dy) * targetRowStride +
                    static_cast<size_t>(dst.x) * bytesPerPixel;
                const uint8_t* pSrc = source.pixels.data() +
                    static_cast<size_t>(sy) * sourceRowStride +
                    static_cast<size_t>(src.x) * bytesPerPixel;
                for (int x = 0; x < dst.width; ++x) {
                    const int sx = std::clamp(
                        src.x + static_cast<int>(
                            (static_cast<int64_t>(x) * src.width) / dst.width),
                        0, source.width - 1);
                    for (int c = 0; c < channels; ++c) {
                        if (c == 3 && source.channels < 4) {
                            pTarget[static_cast<size_t>(x) * bytesPerPixel + 3] = 0xFF;
                        } else {
                            pTarget[static_cast<size_t>(x) * bytesPerPixel + c] =
                                pSrc[static_cast<size_t>(sx - src.x) * bytesPerPixel +
                                     static_cast<size_t>(std::min(c, source.channels - 1))];
                        }
                    }
                }
            }
        } else {
            unsafeBilinearResize(pTargetRow,
                                 dst.width, dst.height, targetRowStride,
                                 pSourceRow,
                                 src.width, src.height, sourceRowStride,
                                 channels);
        }
    }
}

RasterOverlayTileProvider::CompositeImageResult combineQuadtreeSourceImages(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    std::vector<RasterSourceResult>&& sources) {
    std::vector<std::string> diagnostics;
    std::vector<std::string> credits;
    for (RasterSourceResult& source : sources) {
        diagnostics.insert(
            diagnostics.end(),
            std::make_move_iterator(source.diagnostics.begin()),
            std::make_move_iterator(source.diagnostics.end()));
        appendCredits(credits, source.credits);
    }
    sources.erase(
        std::remove_if(sources.begin(), sources.end(),
                       [](const RasterSourceResult& source) {
                           return !source.image ||
                                  !isRasterCompositeSourceImage(*source.image);
                       }),
        sources.end());
    if (sources.empty()) {
        RasterOverlayTileProvider::CompositeImageResult result;
        result.diagnostics = std::move(diagnostics);
        result.credits = std::move(credits);
        return result;
    }

    const RasterOverlayProjection projType =
        projectionForSourceScheme(scheme);
    const Rectangle projectedTarget =
        projectGeographicToProvider(targetBounds, projType);

    double projectedWidthPerPixel = std::numeric_limits<double>::max();
    double projectedHeightPerPixel = std::numeric_limits<double>::max();
    for (const RasterSourceResult& source : sources) {
        const Rectangle projectedSource =
            projectGeographicToProvider(source.bounds, projType);
        projectedWidthPerPixel = std::min(
            projectedWidthPerPixel,
            projectedSource.computeWidth() / static_cast<double>(source.image->width));
        projectedHeightPerPixel = std::min(
            projectedHeightPerPixel,
            projectedSource.computeHeight() / static_cast<double>(source.image->height));
    }
    if (projectedWidthPerPixel <= 0.0 || projectedHeightPerPixel <= 0.0 ||
        !std::isfinite(projectedWidthPerPixel) ||
        !std::isfinite(projectedHeightPerPixel)) {
        RasterOverlayTileProvider::CompositeImageResult result;
        result.image = std::make_unique<DecodedImage>();
        result.rectangle = targetBounds;
        result.diagnostics = std::move(diagnostics);
        result.credits = std::move(credits);
        return result;
    }

    std::vector<ProjectedSource> projectedSources;
    projectedSources.reserve(sources.size());
    for (const RasterSourceResult& src : sources) {
        ProjectedSource ps;
        ps.bounds = projectGeographicToProvider(src.bounds, projType);
        if (src.sourceSubset) {
            ps.sourceSubset = projectGeographicToProvider(*src.sourceSubset, projType);
        }
        projectedSources.push_back(std::move(ps));
    }

    CombinedImageMeasurements measurements = measureCombinedImage(
        projectedTarget,
        projectedSources,
        sources,
        projectedWidthPerPixel,
        projectedHeightPerPixel);

    if (measurements.width <= 0 || measurements.height <= 0 ||
        measurements.channels <= 0) {
        RasterOverlayTileProvider::CompositeImageResult result;
        result.rectangle = targetBounds;
        result.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::Yes;
        result.diagnostics = std::move(diagnostics);
        result.credits = std::move(credits);
        return result;
    }
    auto output = std::make_unique<DecodedImage>();
    output->width = measurements.width;
    output->height = measurements.height;
    output->channels = measurements.channels;
    output->bytesPerChannel = measurements.bytesPerChannel;
    output->pixels.resize(static_cast<size_t>(output->width) *
                          static_cast<size_t>(output->height) *
                          static_cast<size_t>(output->channels) *
                          static_cast<size_t>(output->bytesPerChannel),
                          0);

    for (size_t i = 0; i < sources.size(); ++i) {
        const RasterSourceResult& source = sources[i];
        const Rectangle& projectedSource = projectedSources[i].bounds;
        const std::optional<Rectangle>& projectedSubset =
            projectedSources[i].sourceSubset;
        blitImage(*output,
                  measurements.rectangle,  // already in projection space
                  *source.image,
                  projectedSource,  // projected — symmetric with measurements.rectangle
                  projectedSubset);
    }

    // cesium-native: combineImages returns projection-space rectangle.
    // Ours unprojects back to geographic to match downstream expectations
    // (RasterOverlayTile::getRectangle, UV transform, etc.).
    RasterOverlayTileProvider::CompositeImageResult result;
    result.image = std::move(output);
    result.rectangle = unprojectProviderToGeographic(measurements.rectangle, projType);
    const bool moreDetailAvailable = std::any_of(
        sources.begin(),
        sources.end(),
        [](const RasterSourceResult& source) {
            return !source.sourceSubset.has_value() &&
                   source.moreDetailAvailable ==
                       RasterOverlayTile::MoreDetailAvailable::Yes;
        });
    result.moreDetailAvailable =
        moreDetailAvailable
            ? RasterOverlayTile::MoreDetailAvailable::Yes
            : RasterOverlayTile::MoreDetailAvailable::No;
    result.diagnostics = std::move(diagnostics);
    result.credits = std::move(credits);
    return result;
}

RasterOverlayTileProvider::CompositeImageResult composeMappedSourceImageSet(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    std::vector<RasterSourceResult>&& sources,
    bool emptyWhenOnlyAncestorFallback) {
    const bool haveAnyUsefulImageData =
        !emptyWhenOnlyAncestorFallback ||
        hasNonAncestorRasterSourceImage(sources);
    if (!haveAnyUsefulImageData) {
        RasterOverlayTileProvider::CompositeImageResult result;
        result.image = std::make_unique<DecodedImage>();
        result.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        for (RasterSourceResult& source : sources) {
            result.diagnostics.insert(
                result.diagnostics.end(),
                std::make_move_iterator(source.diagnostics.begin()),
                std::make_move_iterator(source.diagnostics.end()));
            appendCredits(result.credits, source.credits);
        }
        return result;
    }

    return combineQuadtreeSourceImages(
        scheme,
        targetBounds,
        std::move(sources));
}

bool isDecodedImageUploadable(const DecodedImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.channels <= 0 ||
        image.bytesPerChannel <= 0) {
        return false;
    }
    const int64_t requiredBytes =
        static_cast<int64_t>(image.width) *
        static_cast<int64_t>(image.height) *
        static_cast<int64_t>(image.channels) *
        static_cast<int64_t>(image.bytesPerChannel);
    return requiredBytes > 0 &&
           image.pixels.size() >= static_cast<size_t>(requiredBytes);
}

bool isRasterCompositeSourceImage(const DecodedImage& image) {
    return isDecodedImageUploadable(image) &&
           (image.channels == 1 || image.channels == 3 ||
            image.channels == 4);
}

bool hasNonAncestorRasterSourceImage(
    const std::vector<RasterSourceResult>& sources) {
    return std::any_of(
        sources.begin(),
        sources.end(),
        [](const RasterSourceResult& source) {
            return source.image && !source.sourceSubset.has_value();
        });
}

int64_t decodedImageSizeBytes(const DecodedImage& image) {
    return static_cast<int64_t>(image.pixels.size());
}

void appendCredits(std::vector<std::string>& target,
                   const std::vector<std::string>& credits) {
    for (const std::string& credit : credits) {
        if (credit.empty()) {
            continue;
        }
        target.push_back(credit);
    }
}

TileKey parentTileKey(const TileKey& key) {
    return key.parent();
}

std::string sourceCacheKey(const TileKey& key) {
    return key.schemeId.str() + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

std::string sourceCacheKey(uint64_t epoch, const TileKey& key) {
    return "epoch/" + std::to_string(epoch) + "/" + sourceCacheKey(key);
}

void trackPeakBytes(int64_t currentBytes, int64_t& peakBytes) {
    if (currentBytes > peakBytes) {
        peakBytes = currentBytes;
    }
}

void decrementActiveRasterTileLoads(std::atomic<uint32_t>& activeLoads) {
    uint32_t current = activeLoads.load(
        std::memory_order_relaxed);
    while (current > 0 &&
           !activeLoads.compare_exchange_weak(
               current,
               current - 1,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

/// 节流名额唯一释放：完成回调与 abandon/析构可能并发认领同一名额
/// （completed 置位到回调 erase 之间条目仍在 activeMappedSourceSets），
/// 以 exchange 决定唯一递减方，防止双重释放静默偷走其他在途名额。
void releaseRasterThrottleSlotOnce(std::atomic<bool>& released,
                                   std::atomic<uint32_t>& activeLoads) {
    if (!released.exchange(true)) {
        decrementActiveRasterTileLoads(activeLoads);
    }
}

bool rectanglesEqualForDirectRasterTile(const Rectangle& a,
                                        const Rectangle& b) {
    const double span =
        std::max({std::abs(a.width()),
                  std::abs(a.height()),
                  std::abs(b.width()),
                  std::abs(b.height()),
                  1.0});
    return a.equalsEpsilon(b, span * 1e-12);
}

std::unique_ptr<TileScheme> createAsyncSchemeSnapshot(
    const TileScheme& scheme) {
    const std::string id = scheme.id();
    if (id == "XYZ-WebMercator") {
        return TileScheme::createXYZWebMercator();
    }
    if (id == "TMS-WebMercator") {
        return TileScheme::createTMS();
    }
    if (id == "OpenGlobus-Earth") {
        return TileScheme::createOpenGlobusEarth();
    }
    if (id == "Geographic-TMS") {
        return TileScheme::createGeographicTMS();
    }
    return nullptr;
}

bool isResolvedRasterSourceResult(const RasterSourceResult& source) {
    return source.image || source.terminalFailure;
}

}  // namespace earth_engine
