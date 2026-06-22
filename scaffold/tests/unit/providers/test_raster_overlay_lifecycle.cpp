#include <gtest/gtest.h>

#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/ImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/providers/RasterTextureUploader.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/renderer/IPrepareRendererResources.h"
#include "earth_engine/renderer/RenderCommand.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/SurfaceRasterBinding.h"
#include "earth_engine/tiling/SurfaceTileDrawCommandBuilder.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "earth_engine/tiling/TileScheme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <deque>
#include <thread>
#include <vector>

using namespace earth_engine;

namespace {

class TestTexture final : public Texture {
public:
    TestTexture(int width, int height) : width_(width), height_(height) {}

    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
};

std::unique_ptr<DecodedImage> makeImage(int width,
                                        int height,
                                        uint8_t r,
                                        uint8_t g = 0,
                                        uint8_t b = 0,
                                        uint8_t a = 255) {
    auto image = std::make_unique<DecodedImage>();
    image->width = width;
    image->height = height;
    image->channels = 4;
    image->pixels.resize(static_cast<size_t>(width) *
                         static_cast<size_t>(height) * 4u);
    for (size_t i = 0; i < image->pixels.size(); i += 4) {
        image->pixels[i + 0] = r;
        image->pixels[i + 1] = g;
        image->pixels[i + 2] = b;
        image->pixels[i + 3] = a;
    }
    return image;
}

class NullImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "null"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 18; }
    int tileWidth() const override { return 2; }
    int tileHeight() const override { return 2; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
        callback(key, nullptr);
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    int requestCount = 0;
};

class ConfigurableImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "configurable"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return minZoomValue; }
    int maxZoom() const override { return maxZoomValue; }
    int tileWidth() const override { return tileWidthValue; }
    int tileHeight() const override { return tileHeightValue; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, nullptr);
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    int minZoomValue = 0;
    int maxZoomValue = 18;
    int tileWidthValue = 256;
    int tileHeightValue = 256;
};

class ImmediateImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "immediate"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 18; }
    int tileWidth() const override { return 2; }
    int tileHeight() const override { return 2; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
        callback(key, makeImage(2, 2, 64));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    int requestCount = 0;
};

class RgbImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "rgb"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 10; }
    int tileWidth() const override { return 4; }
    int tileHeight() const override { return 4; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        requestedKeys.push_back(key);
        auto image = std::make_unique<DecodedImage>();
        image->width = tileWidth();
        image->height = tileHeight();
        image->channels = 3;
        image->pixels.resize(
            static_cast<size_t>(image->width) *
            static_cast<size_t>(image->height) * 3u,
            static_cast<uint8_t>(key.z));
        callback(key, std::move(image));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    std::vector<TileKey> requestedKeys;
};

class MalformedImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "malformed"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 18; }
    int tileWidth() const override { return 2; }
    int tileHeight() const override { return 2; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        auto image = std::make_unique<DecodedImage>();
        image->width = 2;
        image->height = 2;
        image->channels = 4;
        image->pixels.resize(4);
        callback(key, std::move(image));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }
};

class ParentFallbackImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "parent-fallback"; }
    std::string schemeId() const override { return schemeIdValue; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 10; }
    int tileWidth() const override { return tileWidthValue; }
    int tileHeight() const override { return tileHeightValue; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        requestedKeys.push_back(key);
        if (key == failingKey ||
            std::find(failingKeys.begin(), failingKeys.end(), key) !=
                failingKeys.end()) {
            callback(key, nullptr);
            return;
        }
        callback(key, makeImage(256, 256, static_cast<uint8_t>(key.z)));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    TileKey failingKey{"XYZ-WebMercator", -1, -1, -1};
    std::vector<TileKey> failingKeys;
    std::string schemeIdValue = "XYZ-WebMercator";
    int tileWidthValue = 256;
    int tileHeightValue = 256;
    std::vector<TileKey> requestedKeys;
};

class DeferredImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "deferred"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 10; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        requestedKeys.push_back(key);
        pending.push_back(Pending{key, std::move(callback)});
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    void completeNext() {
        ASSERT_FALSE(pending.empty());
        Pending item = std::move(pending.front());
        pending.pop_front();
        item.callback(
            item.key,
            makeImage(256, 256, static_cast<uint8_t>(item.key.z)));
    }

    struct Pending {
        TileKey key;
        TileCallback callback;
    };
    std::vector<TileKey> requestedKeys;
    std::deque<Pending> pending;
};

class DeferredParentFallbackImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "deferred-parent-fallback"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 10; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        requestedKeys.push_back(key);
        pending.push_back(Pending{key, std::move(callback)});
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    void completeNext() {
        ASSERT_FALSE(pending.empty());
        Pending item = std::move(pending.front());
        pending.pop_front();
        const bool fails =
            std::find(failingKeys.begin(), failingKeys.end(), item.key) !=
            failingKeys.end();
        item.callback(
            item.key,
            fails
                ? nullptr
                : makeImage(256, 256, static_cast<uint8_t>(item.key.z)));
    }

    struct Pending {
        TileKey key;
        TileCallback callback;
    };
    std::vector<TileKey> failingKeys;
    std::vector<TileKey> requestedKeys;
    std::deque<Pending> pending;
};

class CountingRasterUploader final : public RasterTextureUploader {
public:
    int maxTextureSize() const override { return maxTextureSizeValue; }

    std::unique_ptr<Texture> uploadRasterTexture(
        const DecodedImage& image,
        const RasterTextureUploadOptions&) override {
        ++uploadCount;
        lastUpload = image;
        return std::make_unique<TestTexture>(image.width, image.height);
    }

    int uploadCount = 0;
    int maxTextureSizeValue = 2048;
    DecodedImage lastUpload;
};

class SlowRasterUploader final : public RasterTextureUploader {
public:
    int maxTextureSize() const override { return 2048; }

    std::unique_ptr<Texture> uploadRasterTexture(
        const DecodedImage& image,
        const RasterTextureUploadOptions&) override {
        ++uploadCount;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return std::make_unique<TestTexture>(image.width, image.height);
    }

    int uploadCount = 0;
};

class RecordingPrepareRendererResources final
    : public IPrepareRendererResources {
public:
    void attachRasterInMainThread(
        const TileKey&,
        int32_t,
        std::shared_ptr<const RasterOverlayTile> rasterTile,
        Texture* texture,
        float,
        float,
        float,
        float) override {
        ++attachCount;
        lastRasterTile = std::move(rasterTile);
        lastTexture = texture;
    }

    void detachRasterInMainThread(
        const TileKey&,
        int32_t) noexcept override {
        ++detachCount;
    }

    int attachCount = 0;
    int detachCount = 0;
    std::shared_ptr<const RasterOverlayTile> lastRasterTile;
    Texture* lastTexture = nullptr;
};

Rectangle projectForProvider(const TileScheme& scheme,
                             const Rectangle& geographicRectangle) {
    if (scheme.crsProfile() == "EPSG:3857") {
        return projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            geographicRectangle);
    }
    return geographicRectangle;
}

Rectangle projectForProvider(const RasterOverlayTileProvider& provider,
                             const Rectangle& geographicRectangle) {
    return projectForProvider(provider.getTileScheme(), geographicRectangle);
}

RasterOverlayDetails makeProviderDetails(const TileScheme& scheme,
                                         const Rectangle& geographicRectangle) {
    RasterOverlayDetails details;
    if (scheme.crsProfile() == "EPSG:3857") {
        details.rasterOverlayProjections = {RasterOverlayProjection::WebMercator};
        details.rasterOverlayRectangles = {
            projectForProvider(scheme, geographicRectangle)};
        details.boundingRegion = {geographicRectangle, 0.0, 0.0};
    } else {
        details.setGeographicRectangle(geographicRectangle);
    }
    return details;
}

} // namespace

TEST(RasterOverlayLifecycleTest, RectangleSourceZoomFollowsCesiumTargetScreenPixels) {
    ConfigurableImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle z3Bounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 3, 2, 3});

    auto matchingTile = provider.getTile(projectForProvider(provider, z3Bounds), 512.0, 512.0);
    ASSERT_NE(nullptr, matchingTile);
    EXPECT_FALSE(matchingTile->isRectangleTile());
    EXPECT_EQ((TileKey{scheme->id(), 3, 2, 3}), matchingTile->getTileID());

    auto widerTile = provider.getTile(projectForProvider(provider, z3Bounds), 1024.0, 256.0);
    ASSERT_NE(nullptr, widerTile);
    EXPECT_TRUE(widerTile->isRectangleTile());
    EXPECT_EQ(4, widerTile->getSourceZoom());

    imagery.minZoomValue = 5;
    RasterOverlayTileProvider minClampedProvider(imagery, *scheme, nullptr);
    auto minClampedTile = minClampedProvider.getTile(projectForProvider(minClampedProvider, z3Bounds), 512.0, 512.0);
    ASSERT_NE(nullptr, minClampedTile);
    EXPECT_EQ(5, minClampedTile->getSourceZoom());

    imagery.minZoomValue = 0;
    imagery.maxZoomValue = 3;
    RasterOverlayTileProvider maxClampedProvider(imagery, *scheme, nullptr);
    auto maxClampedTile = maxClampedProvider.getTile(projectForProvider(maxClampedProvider, z3Bounds), 1024.0, 256.0);
    ASSERT_NE(nullptr, maxClampedTile);
    EXPECT_FALSE(maxClampedTile->isRectangleTile());
    EXPECT_EQ((TileKey{scheme->id(), 3, 2, 3}), maxClampedTile->getTileID());
}

TEST(RasterOverlayLifecycleTest, RectangleSourceZoomRespectsOverlayLevelRange) {
    ConfigurableImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle z3Bounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 3, 2, 3});

    RasterOverlay::Options options;
    options.minimumZoom = 5;
    auto minZoomImagery = std::make_unique<ConfigurableImageryProvider>();
    RasterOverlay minOverlay(
        std::move(minZoomImagery),
        TileScheme::createXYZWebMercator(),
        options);
    RasterOverlayTileProvider minProvider(
        minOverlay.getProvider(),
        minOverlay.getTileScheme(),
        nullptr);
    minProvider.setOwner(&minOverlay);
    auto minTile = minProvider.getTile(projectForProvider(minProvider, z3Bounds), 512.0, 512.0);
    ASSERT_NE(nullptr, minTile);
    EXPECT_EQ(5, minTile->getSourceZoom());

    options.minimumZoom = 0;
    options.maximumZoom = 3;
    auto maxZoomImagery = std::make_unique<ConfigurableImageryProvider>();
    RasterOverlay maxOverlay(
        std::move(maxZoomImagery),
        TileScheme::createXYZWebMercator(),
        options);
    RasterOverlayTileProvider maxProvider(
        maxOverlay.getProvider(),
        maxOverlay.getTileScheme(),
        nullptr);
    maxProvider.setOwner(&maxOverlay);
    auto maxTile = maxProvider.getTile(projectForProvider(maxProvider, z3Bounds), 1024.0, 256.0);
    ASSERT_NE(nullptr, maxTile);
    EXPECT_FALSE(maxTile->isRectangleTile());
    EXPECT_EQ((TileKey{scheme->id(), 3, 2, 3}), maxTile->getTileID());
}

TEST(RasterOverlayLifecycleTest, DirectTileCreationRejectsUnsupportedProviderTiles) {
    ConfigurableImageryProvider imagery;
    imagery.minZoomValue = 2;
    imagery.maxZoomValue = 4;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    EXPECT_EQ(nullptr, provider.getTile(TileKey{scheme->id(), 1, 0, 0}));
    EXPECT_EQ(nullptr, provider.getTile(TileKey{scheme->id(), 5, 0, 0}));
    EXPECT_EQ(nullptr, provider.getTile(TileKey{"Geographic-TMS", 2, 0, 0}));
    EXPECT_EQ(0, provider.getCachedTileCount());

    auto supported = provider.getTile(TileKey{scheme->id(), 2, 0, 0});
    ASSERT_NE(nullptr, supported);
    EXPECT_EQ(1, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, DirectTileCreationRejectsUnsupportedOverlayLevels) {
    auto imagery = std::make_unique<ConfigurableImageryProvider>();
    RasterOverlay::Options options;
    options.minimumZoom = 2;
    options.maximumZoom = 4;
    RasterOverlay overlay(
        std::move(imagery),
        TileScheme::createXYZWebMercator(),
        options);
    RasterOverlayTileProvider provider(
        overlay.getProvider(),
        overlay.getTileScheme(),
        nullptr);
    provider.setOwner(&overlay);
    auto scheme = TileScheme::createXYZWebMercator();

    EXPECT_EQ(nullptr, provider.getTile(TileKey{scheme->id(), 1, 0, 0}));
    EXPECT_EQ(nullptr, provider.getTile(TileKey{scheme->id(), 5, 0, 0}));
    EXPECT_EQ(0, provider.getCachedTileCount());

    auto supported = provider.getTile(TileKey{scheme->id(), 2, 0, 0});
    ASSERT_NE(nullptr, supported);
    EXPECT_EQ(1, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, DirectTileCreationRejectsOutsideCoverageLikeCesiumNative) {
    ConfigurableImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey coveredKey{scheme->id(), 2, 1, 1};
    const TileKey outsideKey{scheme->id(), 2, 3, 3};
    provider.setCoverageRectangle(scheme->tileToRectangle(coveredKey));

    EXPECT_EQ(nullptr, provider.getTile(outsideKey));
    EXPECT_EQ(0, provider.getCachedTileCount());

    auto covered = provider.getTile(coveredKey);
    ASSERT_NE(nullptr, covered);
    EXPECT_EQ(coveredKey, covered->getTileID());
    EXPECT_EQ(1, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, RectangleCoverageClampsOutsideAndClipsSourcePlan) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    RasterOverlay::Options options;
    options.coverageRectangle =
        scheme->tileToRectangle(TileKey{scheme->id(), 1, 0, 0});
    provider.setCoverageRectangle(options.coverageRectangle);

    Rectangle outside =
        scheme->tileToRectangle(TileKey{scheme->id(), 1, 1, 1});
    auto outsideTile =
        provider.getTile(projectForProvider(provider, outside), 512.0, 512.0);
    ASSERT_NE(nullptr, outsideTile);
    EXPECT_TRUE(outsideTile->isRectangleTile());
    EXPECT_TRUE(provider.loadTileThrottled(*outsideTile, nullptr));

    ASSERT_FALSE(imagery.requestedKeys.empty());
    for (const TileKey& requested : imagery.requestedKeys) {
        Rectangle requestedBounds = scheme->tileToRectangle(requested);
        EXPECT_TRUE(requestedBounds.intersects(options.coverageRectangle));
    }

    ParentFallbackImageryProvider overlappingImagery;
    RasterOverlayTileProvider overlappingProvider(
        overlappingImagery,
        *scheme,
        nullptr);
    overlappingProvider.setCoverageRectangle(options.coverageRectangle);
    Rectangle overlapping(
        options.coverageRectangle.east() - options.coverageRectangle.width() * 0.5,
        options.coverageRectangle.south(),
        options.coverageRectangle.east() + options.coverageRectangle.width() * 0.5,
        options.coverageRectangle.north());
    auto tile = overlappingProvider.getTile(
        projectForProvider(overlappingProvider, overlapping),
        512.0,
        512.0);
    ASSERT_NE(nullptr, tile);
    EXPECT_TRUE(tile->isRectangleTile());
    EXPECT_TRUE(overlappingProvider.loadTileThrottled(*tile, nullptr));

    ASSERT_FALSE(overlappingImagery.requestedKeys.empty());
    for (const TileKey& requested : overlappingImagery.requestedKeys) {
        Rectangle requestedBounds = scheme->tileToRectangle(requested);
        EXPECT_TRUE(requestedBounds.intersects(options.coverageRectangle));
    }
}

TEST(RasterOverlayLifecycleTest,
     RectangleCoverageClipDoesNotRequestEdgeTouchingSourceTiles) {
    RgbImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(1, 1);

    const TileKey coveredKey{scheme->id(), 1, 0, 0};
    const Rectangle coverage = scheme->tileToRectangle(coveredKey);
    const Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    provider.setCoverageRectangle(coverage);

    RasterOverlayTileProvider::TilePtr tile =
        provider.getTile(projectForProvider(provider, rootBounds),
                         1024.0,
                         1024.0);
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(tile->isRectangleTile());
    EXPECT_TRUE(provider.loadTile(*tile));

    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(coveredKey, imagery.requestedKeys.front());
}

TEST(RasterOverlayLifecycleTest, RectangleCoverageMissClampsToNearestCoverageEdge) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle coverage = scheme->tileToRectangle(
        TileKey{scheme->id(), 3, 2, 3});
    const Rectangle outsideCoverage = scheme->tileToRectangle(
        TileKey{scheme->id(), 3, 2, 5});
    provider.setCoverageRectangle(coverage);

    auto tile =
        provider.getTile(projectForProvider(provider, outsideCoverage), 512.0, 512.0);
    ASSERT_NE(nullptr, tile);
    EXPECT_TRUE(tile->isRectangleTile());
    EXPECT_TRUE(provider.loadTile(*tile));
    ASSERT_FALSE(imagery.requestedKeys.empty());
    for (const TileKey& requested : imagery.requestedKeys) {
        EXPECT_TRUE(scheme->tileToRectangle(requested).intersects(coverage));
    }
}

TEST(RasterOverlayLifecycleTest, DirectAlignedSingleSourceUploadsWithoutResampling) {
    RgbImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    auto rectangleMappedTile = provider.getTile(projectForProvider(provider, sourceBounds), 8.0, 8.0);

    ASSERT_NE(nullptr, rectangleMappedTile);
    EXPECT_FALSE(rectangleMappedTile->isRectangleTile());
    EXPECT_EQ(sourceKey, rectangleMappedTile->getTileID());
    EXPECT_EQ(projectForProvider(provider, sourceBounds),
              rectangleMappedTile->getRectangle());
    EXPECT_EQ(1, provider.getCachedTileCount());

    ASSERT_TRUE(provider.loadTile(*rectangleMappedTile));
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    EXPECT_EQ(1, provider.processPendingUploads(false));
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(4, uploaderPtr->lastUpload.width);
    EXPECT_EQ(4, uploaderPtr->lastUpload.height);
    EXPECT_EQ(3, uploaderPtr->lastUpload.channels);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              rectangleMappedTile->getState());
}

TEST(RasterOverlayLifecycleTest, DirectFastPathDoesNotRunForPartialRectangle) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());

    auto rectangleTile = provider.getTile(projectForProvider(provider, westHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, rectangleTile);
    EXPECT_TRUE(rectangleTile->isRectangleTile());
    EXPECT_EQ(sourceKey.z, rectangleTile->getSourceZoom());
}

TEST(RasterOverlayLifecycleTest, RectangleSourceRangeTrimsTileEdgeTouchesLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    imagery.tileWidthValue = 64;
    imagery.tileHeightValue = 64;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle sourceAlignedBounds = scheme->tileToRectangle(
        TileKey{scheme->id(), 3, 2, 3});
    auto rectangleTile = provider.getTile(projectForProvider(provider, sourceAlignedBounds), 512.0, 512.0);
    ASSERT_NE(nullptr, rectangleTile);
    EXPECT_EQ(5, rectangleTile->getSourceZoom());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*rectangleTile, &budget));
    ASSERT_EQ(16u, imagery.requestedKeys.size());
    EXPECT_EQ(16u, budget.rasterNetworkRequestsIssued());

    for (const TileKey& requested : imagery.requestedKeys) {
        EXPECT_EQ(5, requested.z);
        EXPECT_GE(requested.x, 8);
        EXPECT_LE(requested.x, 11);
        EXPECT_GE(requested.y, 12);
        EXPECT_LE(requested.y, 15);
    }
}

TEST(RasterOverlayLifecycleTest, RectangleSourceRangeTrimsSouthUpTileEdgeTouchesLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    imagery.schemeIdValue = "Geographic-TMS";
    imagery.tileWidthValue = 64;
    imagery.tileHeightValue = 64;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle sourceAlignedBounds = scheme->tileToRectangle(
        TileKey{scheme->id(), 3, 2, 3});
    auto rectangleTile = provider.getTile(projectForProvider(provider, sourceAlignedBounds), 512.0, 512.0);
    ASSERT_NE(nullptr, rectangleTile);
    EXPECT_EQ(5, rectangleTile->getSourceZoom());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*rectangleTile, &budget));
    ASSERT_EQ(16u, imagery.requestedKeys.size());
    EXPECT_EQ(16u, budget.rasterNetworkRequestsIssued());

    for (const TileKey& requested : imagery.requestedKeys) {
        EXPECT_EQ(5, requested.z);
        EXPECT_GE(requested.x, 8);
        EXPECT_LE(requested.x, 11);
        EXPECT_GE(requested.y, 12);
        EXPECT_LE(requested.y, 15);
    }
}

TEST(RasterOverlayLifecycleTest, DirectTileCacheRetainsRecentAndMarkedTiles) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    TileKey key{scheme->id(), 3, 4, 2};

    provider.setFrameNumber(1);
    auto tile = provider.getTile(key);
    ASSERT_NE(nullptr, tile);
    EXPECT_EQ(1, provider.getCachedTileCount());

    provider.setFrameNumber(2);
    provider.trimUnusedTiles();
    EXPECT_EQ(1, provider.getCachedTileCount());

    provider.setFrameNumber(122);
    provider.markUsed(key);
    provider.trimUnusedTiles();
    EXPECT_EQ(1, provider.getCachedTileCount());

    provider.setFrameNumber(243);
    tile.reset();
    provider.trimUnusedTiles();
    EXPECT_EQ(0, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, DefaultMaximumLevelMatchesCesiumNativeUrlTemplate) {
    XYZImageryProvider imagery("https://example.invalid/{z}/{x}/{y}.png");
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    EXPECT_EQ(25, scheme->maxZoom());
    EXPECT_EQ(25, provider.getMaximumLevel());
    EXPECT_NE(nullptr, provider.getTile(TileKey{scheme->id(), 25, 0, 0}));
    EXPECT_EQ(nullptr, provider.getTile(TileKey{scheme->id(), 26, 0, 0}));
}

TEST(RasterOverlayLifecycleTest, RectangleTileKeepsGeometryBoundsAndTargetPixels) {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    auto geometryScheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);

    TileKey geometryKey{geometryScheme->id(), 2, 4, 2};
    Rectangle geometryBounds = geometryScheme->tileToRectangle(geometryKey);
    auto rectangleTile = provider.getTile(projectForProvider(provider, geometryBounds), 512.0, 512.0);

    ASSERT_NE(nullptr, rectangleTile);
    EXPECT_TRUE(rectangleTile->isRectangleTile());
    EXPECT_EQ(projectForProvider(provider, geometryBounds),
              rectangleTile->getRectangle());
    EXPECT_EQ(0u, rectangleTile->getCacheKey().find("rectangle/"));
    EXPECT_EQ(3, rectangleTile->getSourceZoom());
    EXPECT_EQ(512.0, rectangleTile->getTargetScreenPixelsX());
    EXPECT_EQ(512.0, rectangleTile->getTargetScreenPixelsY());
}

TEST(RasterOverlayLifecycleTest, ExactProviderRectangleUsesDirectQuadtreeTile) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(0, 3);

    const TileKey key{scheme->id(), 3, 4, 2};
    const Rectangle bounds = scheme->tileToRectangle(key);
    auto mappedTile = provider.getTile(projectForProvider(provider, bounds), 512.0, 512.0);

    ASSERT_NE(nullptr, mappedTile);
    EXPECT_FALSE(mappedTile->isRectangleTile());
    EXPECT_EQ(key, mappedTile->getTileID());
    EXPECT_EQ(projectForProvider(provider, bounds), mappedTile->getRectangle());
    EXPECT_EQ(scheme->id() + "/3/4/2", mappedTile->getCacheKey());
    EXPECT_EQ(1, provider.getCachedTileCount());

    auto directTile = provider.getTile(key);
    EXPECT_EQ(directTile, mappedTile);
}

TEST(RasterOverlayLifecycleTest, ExactIntermediateRectangleUsesDirectQuadtreeTileLikeCesiumNative) {
    ConfigurableImageryProvider imagery;
    imagery.tileWidthValue = 256;
    imagery.tileHeightValue = 256;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(0, 5);

    const TileKey key{scheme->id(), 3, 4, 2};
    const Rectangle bounds = scheme->tileToRectangle(key);
    auto mappedTile = provider.getTile(projectForProvider(provider, bounds), 512.0, 512.0);

    ASSERT_NE(nullptr, mappedTile);
    EXPECT_FALSE(mappedTile->isRectangleTile());
    EXPECT_EQ(key, mappedTile->getTileID());
    EXPECT_EQ(scheme->id() + "/3/4/2", mappedTile->getCacheKey());
    EXPECT_EQ(1, provider.getCachedTileCount());

    auto directTile = provider.getTile(key);
    EXPECT_EQ(directTile, mappedTile);
}

TEST(RasterOverlayLifecycleTest, RectangleSourceZoomRespectsOverlayMaximumTextureSize) {
    ConfigurableImageryProvider imagery;
    imagery.tileWidthValue = 256;
    imagery.tileHeightValue = 256;
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});

    auto defaultUploader = std::make_unique<CountingRasterUploader>();
    defaultUploader->maxTextureSizeValue = 2048;
    RasterOverlayTileProvider defaultProvider(
        imagery,
        *scheme,
        std::move(defaultUploader));
    auto defaultTile = defaultProvider.getTile(projectForProvider(defaultProvider, rootBounds), 131072.0, 131072.0);
    ASSERT_NE(nullptr, defaultTile);
    EXPECT_EQ(3, defaultTile->getSourceZoom());

    auto constrainedUploader = std::make_unique<CountingRasterUploader>();
    constrainedUploader->maxTextureSizeValue = 2048;
    RasterOverlayTileProvider constrainedProvider(
        imagery,
        *scheme,
        std::move(constrainedUploader));
    constrainedProvider.setMaximumTextureSize(256);
    auto constrainedTile =
        constrainedProvider.getTile(projectForProvider(constrainedProvider, rootBounds), 131072.0, 131072.0);
    ASSERT_NE(nullptr, constrainedTile);
    EXPECT_FALSE(constrainedTile->isRectangleTile());
    EXPECT_EQ(0, constrainedTile->getSourceZoom());

    auto ownedImagery = std::make_unique<ConfigurableImageryProvider>();
    ownedImagery->tileWidthValue = 256;
    ownedImagery->tileHeightValue = 256;
    RasterOverlay::Options options;
    options.maximumTextureSize = 256;
    RasterOverlay overlay(
        std::move(ownedImagery),
        TileScheme::createXYZWebMercator(),
        options);
    auto ownerUploader = std::make_unique<CountingRasterUploader>();
    ownerUploader->maxTextureSizeValue = 2048;
    RasterOverlayTileProvider ownerProvider(
        overlay.getProvider(),
        overlay.getTileScheme(),
        std::move(ownerUploader));
    ownerProvider.setOwner(&overlay);
    EXPECT_EQ(256, ownerProvider.getMaximumTextureSize());
    auto ownerTile = ownerProvider.getTile(projectForProvider(ownerProvider, rootBounds), 131072.0, 131072.0);
    ASSERT_NE(nullptr, ownerTile);
    EXPECT_FALSE(ownerTile->isRectangleTile());
    EXPECT_EQ(0, ownerTile->getSourceZoom());
}

TEST(RasterOverlayLifecycleTest, ExactRootRectangleUsesDirectQuadtreeTileLikeCesiumNative) {
    RgbImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey rootKey{scheme->id(), 0, 0, 0};
    const Rectangle rootBounds = scheme->tileToRectangle(rootKey);
    auto tile = provider.getTile(projectForProvider(provider, rootBounds), 8.0, 8.0);
    ASSERT_NE(nullptr, tile);
    EXPECT_FALSE(tile->isRectangleTile());
    EXPECT_EQ(rootKey, tile->getTileID());
    EXPECT_EQ(scheme->id() + "/0/0/0", tile->getCacheKey());

    ASSERT_TRUE(provider.loadTile(*tile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(rootKey, imagery.requestedKeys.front());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
    ASSERT_FALSE(uploaderPtr->lastUpload.pixels.empty());
    EXPECT_TRUE(std::all_of(
        uploaderPtr->lastUpload.pixels.begin(),
        uploaderPtr->lastUpload.pixels.end(),
        [](uint8_t value) { return value == 0; }));
}

TEST(RasterOverlayLifecycleTest, RectangleSourceFailureFallsBackToParentTile) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const int expectedSourceZoom = 8;
    TileKey centerKey =
        scheme->positionToTile(0.1, 0.2, expectedSourceZoom);
    Rectangle centerBounds = scheme->tileToRectangle(centerKey);
    Rectangle tileBounds(
        centerBounds.west() - centerBounds.width() * 0.5,
        centerBounds.south() - centerBounds.height() * 0.5,
        centerBounds.east() + centerBounds.width() * 0.5,
        centerBounds.north() + centerBounds.height() * 0.5);

    imagery.failingKey =
        scheme->positionToTile(
            tileBounds.east(),
            tileBounds.south(),
            expectedSourceZoom);

    auto rectangleTile = provider.getTile(projectForProvider(provider, tileBounds), 1024.0, 1024.0);
    ASSERT_NE(nullptr, rectangleTile);
    EXPECT_EQ(expectedSourceZoom, rectangleTile->getSourceZoom());

    ASSERT_TRUE(provider.loadTile(*rectangleTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              rectangleTile->getState());
    EXPECT_EQ(1, uploaderPtr->uploadCount);

    const std::vector<uint8_t>& pixels = uploaderPtr->lastUpload.pixels;
    ASSERT_FALSE(pixels.empty());
    EXPECT_TRUE(std::any_of(
        pixels.begin(),
        pixels.end(),
        [](uint8_t value) { return value == 7; }));
    EXPECT_TRUE(std::any_of(
        pixels.begin(),
        pixels.end(),
        [](uint8_t value) { return value == 8; }));
    EXPECT_TRUE(std::find(
        imagery.requestedKeys.begin(),
        imagery.requestedKeys.end(),
        TileKey{scheme->id(), 7, imagery.failingKey.x / 2,
                imagery.failingKey.y / 2}) != imagery.requestedKeys.end());
}

TEST(RasterOverlayLifecycleTest, SourceTileDepotCachesTilesByTileKeyLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    const Rectangle eastHalf(
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.south(),
        sourceBounds.east(),
        sourceBounds.north());

    auto westTile = provider.getTile(projectForProvider(provider, westHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, westTile);
    EXPECT_EQ(sourceKey.z, westTile->getSourceZoom());
    ASSERT_TRUE(provider.loadTile(*westTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    auto eastTile = provider.getTile(projectForProvider(provider, eastHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, eastTile);
    EXPECT_EQ(sourceKey.z, eastTile->getSourceZoom());
    ASSERT_TRUE(provider.loadTile(*eastTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, eastTile->getState());
}

TEST(RasterOverlayLifecycleTest, ConcurrentRectangleTilesShareProviderSourceTileAssetLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    const Rectangle eastHalf(
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.south(),
        sourceBounds.east(),
        sourceBounds.north());

    auto westTile = provider.getTile(projectForProvider(provider, westHalf), 256.0, 512.0);
    auto eastTile = provider.getTile(projectForProvider(provider, eastHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, westTile);
    ASSERT_NE(nullptr, eastTile);

    ASSERT_TRUE(provider.loadTile(*westTile));
    ASSERT_TRUE(provider.loadTile(*eastTile));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    imagery.completeNext();

    EXPECT_EQ(2, provider.processPendingUploads(false));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, westTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, eastTile->getState());
}

TEST(RasterOverlayLifecycleTest, DirectAndRectangleTilesShareProviderSourceTileAssetLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());

    auto directTile = provider.getTile(sourceKey);
    auto rectangleTile = provider.getTile(projectForProvider(provider, westHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, directTile);
    ASSERT_NE(nullptr, rectangleTile);

    ASSERT_TRUE(provider.loadTile(*directTile));
    ASSERT_TRUE(provider.loadTile(*rectangleTile));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    imagery.completeNext();

    EXPECT_EQ(2, provider.processPendingUploads(false));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, directTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, rectangleTile->getState());
}

TEST(RasterOverlayLifecycleTest, DirectTileCallbackAfterProviderDestructionIsIgnored) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    auto provider = std::make_unique<RasterOverlayTileProvider>(
        imagery,
        *scheme,
        std::move(uploader));

    TileKey key{scheme->id(), 1, 0, 0};
    auto tile = provider->getTile(key);
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider->loadTile(*tile));
    ASSERT_EQ(1u, imagery.pending.size());
    provider.reset();

    imagery.completeNext();
    SUCCEED();
}

TEST(RasterOverlayLifecycleTest, RectangleTileCallbackAfterProviderDestructionIsIgnored) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    auto provider = std::make_unique<RasterOverlayTileProvider>(
        imagery,
        *scheme,
        std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());

    auto tile = provider->getTile(
        projectForProvider(*provider, westHalf),
        256.0,
        512.0);
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider->loadTile(*tile));
    ASSERT_EQ(1u, imagery.pending.size());
    provider.reset();

    imagery.completeNext();
    SUCCEED();
}

TEST(RasterOverlayLifecycleTest, SourceTileDepotHonorsSubTileCacheByteBudget) {
    auto ownedImagery = std::make_unique<ParentFallbackImageryProvider>();
    ParentFallbackImageryProvider* imagery = ownedImagery.get();
    RasterOverlay::Options options;
    options.subTileCacheBytes = 0;
    RasterOverlay overlay(
        std::move(ownedImagery),
        TileScheme::createXYZWebMercator(),
        options);
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(
        overlay.getProvider(),
        overlay.getTileScheme(),
        std::move(uploader));
    provider.setOwner(&overlay);

    auto* scheme = &overlay.getTileScheme();
    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    const Rectangle eastHalf(
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.south(),
        sourceBounds.east(),
        sourceBounds.north());

    auto westTile = provider.getTile(projectForProvider(provider, westHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, westTile);
    ASSERT_TRUE(provider.loadTile(*westTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));
    EXPECT_EQ(1, static_cast<int>(imagery->requestedKeys.size()));

    auto eastTile = provider.getTile(projectForProvider(provider, eastHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, eastTile);
    ASSERT_TRUE(provider.loadTile(*eastTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    EXPECT_EQ(2, static_cast<int>(imagery->requestedKeys.size()));
    EXPECT_EQ(sourceKey, imagery->requestedKeys[0]);
    EXPECT_EQ(sourceKey, imagery->requestedKeys[1]);
}

TEST(RasterOverlayLifecycleTest, RectangleSourceFallbacksAreCachedByRequestedTileLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey failingKey{scheme->id(), 3, 2, 3};
    imagery.failingKey = failingKey;
    const Rectangle bounds = scheme->tileToRectangle(failingKey);

    auto firstTile = provider.getTile(projectForProvider(provider, bounds), 512.0, 512.0);
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              firstTile->getState());
    EXPECT_EQ(nullptr, firstTile->getTexture());
    const int requestsAfterFirstLoad =
        static_cast<int>(imagery.requestedKeys.size());
    EXPECT_TRUE(requestsAfterFirstLoad >= 2);

    Rectangle innerBounds(
        bounds.west() + bounds.width() * 0.25,
        bounds.south() + bounds.height() * 0.25,
        bounds.east() - bounds.width() * 0.25,
        bounds.north() - bounds.height() * 0.25);
    auto secondTile = provider.getTile(projectForProvider(provider, innerBounds), 256.0, 256.0);
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(provider.loadTile(*secondTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    EXPECT_EQ(requestsAfterFirstLoad,
              static_cast<int>(imagery.requestedKeys.size()));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              secondTile->getState());
    EXPECT_EQ(nullptr, secondTile->getTexture());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              secondTile->isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, RectangleSiblingFallbacksReuseCachedParentSourceLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey westChild{scheme->id(), 3, 2, 2};
    const TileKey eastChild{scheme->id(), 3, 3, 2};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {westChild, eastChild};

    auto westBounds = scheme->tileToRectangle(westChild);
    const Rectangle westHalf(
        westBounds.west(),
        westBounds.south(),
        westBounds.west() + westBounds.width() * 0.5,
        westBounds.north());
    auto eastBounds = scheme->tileToRectangle(eastChild);
    const Rectangle eastHalf(
        eastBounds.west(),
        eastBounds.south(),
        eastBounds.west() + eastBounds.width() * 0.5,
        eastBounds.north());

    auto firstTile = provider.getTile(projectForProvider(provider, westHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              firstTile->getState());
    EXPECT_EQ(nullptr, firstTile->getTexture());
    const int parentRequestsAfterFirstLoad =
        static_cast<int>(std::count(
            imagery.requestedKeys.begin(),
            imagery.requestedKeys.end(),
            parent));
    EXPECT_EQ(1, parentRequestsAfterFirstLoad);

    auto secondTile = provider.getTile(projectForProvider(provider, eastHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(provider.loadTile(*secondTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              secondTile->getState());
    EXPECT_EQ(nullptr, secondTile->getTexture());
    EXPECT_EQ(
        parentRequestsAfterFirstLoad,
        static_cast<int>(std::count(
            imagery.requestedKeys.begin(),
            imagery.requestedKeys.end(),
            parent)));
}

TEST(RasterOverlayLifecycleTest, ConcurrentSiblingFallbacksShareParentSourceInFlightLikeCesiumNative) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey westChild{scheme->id(), 3, 2, 2};
    const TileKey eastChild{scheme->id(), 3, 3, 2};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {westChild, eastChild};

    const Rectangle westBounds = scheme->tileToRectangle(westChild);
    const Rectangle westHalf(
        westBounds.west(),
        westBounds.south(),
        westBounds.west() + westBounds.width() * 0.5,
        westBounds.north());
    const Rectangle eastBounds = scheme->tileToRectangle(eastChild);
    const Rectangle eastHalf(
        eastBounds.west(),
        eastBounds.south(),
        eastBounds.west() + eastBounds.width() * 0.5,
        eastBounds.north());

    auto westTile = provider.getTile(projectForProvider(provider, westHalf), 256.0, 512.0);
    auto eastTile = provider.getTile(projectForProvider(provider, eastHalf), 256.0, 512.0);
    ASSERT_NE(nullptr, westTile);
    ASSERT_NE(nullptr, eastTile);

    ASSERT_TRUE(provider.loadTile(*westTile));
    ASSERT_TRUE(provider.loadTile(*eastTile));
    ASSERT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(westChild, imagery.pending[0].key);
    EXPECT_EQ(eastChild, imagery.pending[1].key);

    imagery.completeNext();
    ASSERT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(eastChild, imagery.pending[0].key);
    EXPECT_EQ(parent, imagery.pending[1].key);

    imagery.completeNext();
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(parent, imagery.pending.front().key);
    EXPECT_EQ(
        1,
        static_cast<int>(std::count(
            imagery.requestedKeys.begin(),
            imagery.requestedKeys.end(),
            parent)));

    imagery.completeNext();
    EXPECT_TRUE(imagery.pending.empty());
    EXPECT_EQ(2, provider.processPendingUploads(false));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, westTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, eastTile->getState());
    EXPECT_EQ(nullptr, westTile->getTexture());
    EXPECT_EQ(nullptr, eastTile->getTexture());
}

TEST(RasterOverlayLifecycleTest, DirectAncestorFallbackUsesParentTileLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const int expectedSourceZoom = 8;
    TileKey key = scheme->positionToTile(0.1, 0.2, expectedSourceZoom);
    Rectangle bounds = scheme->tileToRectangle(key);
    imagery.failingKey = key;

    auto directTile = provider.getTile(projectForProvider(provider, bounds), 512.0, 512.0);
    ASSERT_NE(nullptr, directTile);
    EXPECT_FALSE(directTile->isRectangleTile());
    EXPECT_EQ(key, directTile->getTileID());

    ASSERT_TRUE(provider.loadTile(*directTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directTile->getState());
    EXPECT_EQ(nullptr, directTile->getTexture());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              directTile->isMoreDetailAvailable());
    EXPECT_TRUE(std::find(
        imagery.requestedKeys.begin(),
        imagery.requestedKeys.end(),
        TileKey{scheme->id(), expectedSourceZoom - 1, key.x / 2, key.y / 2}) !=
        imagery.requestedKeys.end());
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionMixesSourceLevelsAfterFailureLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const int expectedSourceZoom = 8;
    const TileKey centerKey =
        scheme->positionToTile(0.1, 0.2, expectedSourceZoom);
    const Rectangle centerBounds = scheme->tileToRectangle(centerKey);
    const Rectangle tileBounds(
        centerBounds.west() - centerBounds.width() * 0.5,
        centerBounds.south() - centerBounds.height() * 0.5,
        centerBounds.east() + centerBounds.width() * 0.5,
        centerBounds.north() + centerBounds.height() * 0.5);
    const TileKey southeastKey =
        scheme->positionToTile(tileBounds.east(), tileBounds.south(),
                               expectedSourceZoom);
    imagery.failingKey = southeastKey;

    auto rectangleTile = provider.getTile(projectForProvider(provider, tileBounds), 1024.0, 1024.0);
    ASSERT_NE(nullptr, rectangleTile);
    EXPECT_EQ(expectedSourceZoom, rectangleTile->getSourceZoom());

    ASSERT_TRUE(provider.loadTile(*rectangleTile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    ASSERT_EQ(RasterOverlayTile::LoadState::Loaded,
              rectangleTile->getState());
    ASSERT_EQ(1, uploaderPtr->uploadCount);
    const DecodedImage& image = uploaderPtr->lastUpload;
    ASSERT_GT(image.width, 0);
    ASSERT_GT(image.height, 0);
    ASSERT_GE(image.pixels.size(),
              static_cast<size_t>(image.width) *
                  static_cast<size_t>(image.height) * 4u);

    bool hasParentLevelPixel = false;
    bool hasSourceLevelPixel = false;
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        hasParentLevelPixel = hasParentLevelPixel ||
                              image.pixels[i] == expectedSourceZoom - 1;
        hasSourceLevelPixel = hasSourceLevelPixel ||
                              image.pixels[i] == expectedSourceZoom;
    }

    EXPECT_TRUE(hasParentLevelPixel);
    EXPECT_TRUE(hasSourceLevelPixel);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Yes,
              rectangleTile->isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, RectangleSourceFailureDoesNotFallbackBelowOverlayMinimumLevel) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(8, 10);

    const int expectedSourceZoom = 8;
    TileKey centerKey =
        scheme->positionToTile(0.1, 0.2, expectedSourceZoom);
    Rectangle centerBounds = scheme->tileToRectangle(centerKey);
    Rectangle tileBounds(
        centerBounds.west() - centerBounds.width() * 0.5,
        centerBounds.south() - centerBounds.height() * 0.5,
        centerBounds.east() + centerBounds.width() * 0.5,
        centerBounds.north() + centerBounds.height() * 0.5);

    imagery.failingKey =
        scheme->positionToTile(
            tileBounds.east(),
            tileBounds.south(),
            expectedSourceZoom);

    auto rectangleTile = provider.getTile(projectForProvider(provider, tileBounds), 1024.0, 1024.0);
    ASSERT_NE(nullptr, rectangleTile);
    EXPECT_EQ(expectedSourceZoom, rectangleTile->getSourceZoom());

    EXPECT_TRUE(provider.loadTile(*rectangleTile));

    const TileKey parentBelowMinimum{
        scheme->id(),
        expectedSourceZoom - 1,
        imagery.failingKey.x / 2,
        imagery.failingKey.y / 2};
    EXPECT_TRUE(std::find(
        imagery.requestedKeys.begin(),
        imagery.requestedKeys.end(),
        parentBelowMinimum) == imagery.requestedKeys.end());
}

TEST(RasterOverlayLifecycleTest, RectangleTilesShareInFlightSourceTileLikeCesiumNativeDepot) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(3, 3);

    const TileKey sourceKey{scheme->id(), 3, 4, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    const Rectangle eastHalf(
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.south(),
        sourceBounds.east(),
        sourceBounds.north());

    RasterOverlayTileProvider::TilePtr firstTile =
        provider.getTile(projectForProvider(provider, westHalf), 128.0, 128.0);
    RasterOverlayTileProvider::TilePtr secondTile =
        provider.getTile(projectForProvider(provider, eastHalf), 128.0, 128.0);
    ASSERT_NE(nullptr, firstTile);
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(firstTile->isRectangleTile());
    ASSERT_TRUE(secondTile->isRectangleTile());

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 1;
    config.maxRasterNetworkInflight = 1;

    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);
    ASSERT_TRUE(provider.loadTileThrottled(*firstTile, &firstBudget));
    EXPECT_EQ(1u, firstBudget.rasterNetworkRequestsIssued());
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    EXPECT_EQ(1, provider.getActiveRasterSourceRequests());

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, config);
    ASSERT_TRUE(provider.loadTileThrottled(*secondTile, &secondBudget));
    EXPECT_EQ(0u, secondBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(1, provider.getActiveRasterSourceRequests());

    imagery.completeNext();
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(2, provider.getPendingUploadCount());
}

TEST(RasterOverlayLifecycleTest, RectangleAtMaximumSourceZoomReportsNoMoreDetail) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const int expectedSourceZoom = imagery.maxZoom();
    TileKey key = scheme->positionToTile(0.1, 0.2, expectedSourceZoom);
    Rectangle bounds = scheme->tileToRectangle(key);

    auto tile = provider.getTile(projectForProvider(provider, bounds), 512.0, 512.0);
    ASSERT_NE(nullptr, tile);
    EXPECT_FALSE(tile->isRectangleTile());
    EXPECT_EQ(key, tile->getTileID());

    ASSERT_TRUE(provider.loadTile(*tile));
    EXPECT_EQ(1, provider.processPendingUploads(false));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              tile->getState());
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              tile->isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, FailedRasterTilesAreTerminalLikeCesiumNative) {
    NullImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    auto tile = provider.getTile(TileKey{scheme->id(), 2, 1, 1});
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    const int failedTileRequests = imagery.requestCount;

    EXPECT_TRUE(provider.loadTile(*tile));
    EXPECT_TRUE(provider.loadTileThrottled(*tile, nullptr));
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    EXPECT_EQ(failedTileRequests, imagery.requestCount);

    Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto rectangleTile = provider.getTile(projectForProvider(provider, rootBounds), 8.0, 8.0);
    ASSERT_NE(nullptr, rectangleTile);
    ASSERT_TRUE(rectangleTile->isRectangleTile());
    EXPECT_TRUE(provider.loadTileThrottled(*rectangleTile, nullptr));
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed,
              rectangleTile->getState());
    const int failedRectangleRequests = imagery.requestCount;

    EXPECT_TRUE(provider.loadTileThrottled(*rectangleTile, nullptr));
    EXPECT_TRUE(provider.loadTile(*rectangleTile));
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed,
              rectangleTile->getState());
    EXPECT_EQ(failedRectangleRequests, imagery.requestCount);
}

TEST(RasterOverlayLifecycleTest, RectangleLoadClampsCoverageChangedAfterTileCreation) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle initialBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 2, 0, 0});
    auto rectangleTile = provider.getTile(projectForProvider(provider, initialBounds), 8.0, 8.0);
    ASSERT_NE(nullptr, rectangleTile);
    ASSERT_TRUE(rectangleTile->isRectangleTile());

    provider.setCoverageRectangle(
        scheme->tileToRectangle(TileKey{scheme->id(), 2, 3, 3}));

    EXPECT_TRUE(provider.loadTile(*rectangleTile));
    EXPECT_EQ(1, imagery.requestCount);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              rectangleTile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Unknown,
              rectangleTile->isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, MalformedRasterImagesFailBeforeUploadLikeCesiumNative) {
    MalformedImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    auto tile = provider.getTile(TileKey{scheme->id(), 2, 1, 1});
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));

    EXPECT_EQ(1, provider.processPendingUploads(false));
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              tile->isMoreDetailAvailable());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
}

TEST(RasterOverlayLifecycleTest, SharedFrameBudgetLimitsRasterUploadsPerFrame) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    TileKey firstKey{scheme->id(), 1, 0, 0};
    TileKey secondKey{scheme->id(), 1, 1, 0};
    auto firstTile = provider.getTile(firstKey);
    auto secondTile = provider.getTile(secondKey);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_TRUE(provider.loadTile(*secondTile));

    FrameResourceBudgetConfig config;
    config.maxRasterUploadsPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    provider.setFrameNumber(1);
    EXPECT_EQ(1, provider.processPendingUploads(false, &budget));
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(1u, budget.rasterUploadsUsed());

    EXPECT_EQ(0, provider.processPendingUploads(false, &budget));
    EXPECT_EQ(1, uploaderPtr->uploadCount);

    budget.beginFrame(2, config);
    provider.setFrameNumber(2);
    EXPECT_EQ(1, provider.processPendingUploads(false, &budget));
    EXPECT_EQ(2, uploaderPtr->uploadCount);
}

TEST(RasterOverlayLifecycleTest, ElapsedUploadBudgetDefersRemainingUploads) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<SlowRasterUploader>();
    SlowRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    TileKey firstKey{scheme->id(), 1, 0, 0};
    TileKey secondKey{scheme->id(), 1, 1, 0};
    auto firstTile = provider.getTile(firstKey);
    auto secondTile = provider.getTile(secondKey);
    ASSERT_TRUE(provider.loadTileThrottled(*firstTile));
    ASSERT_TRUE(provider.loadTileThrottled(*secondTile));
    ASSERT_EQ(2u, imagery.pending.size());

    imagery.completeNext();
    imagery.completeNext();
    ASSERT_EQ(2, provider.getPendingUploadCount());

    FrameResourceBudgetConfig config;
    config.maxRasterUploadsPerFrame = 4;
    config.mainThreadTimeMs = 0.001;
    FrameResourceBudget firstFrameBudget;
    firstFrameBudget.beginFrame(1, config);

    EXPECT_EQ(1, provider.processPendingUploads(false, &firstFrameBudget));
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(1, provider.getPendingUploadCount());
    EXPECT_EQ(1u, firstFrameBudget.rasterUploadsUsed());

    FrameResourceBudget secondFrameBudget;
    secondFrameBudget.beginFrame(2, config);

    EXPECT_EQ(1, provider.processPendingUploads(false, &secondFrameBudget));
    EXPECT_EQ(2, uploaderPtr->uploadCount);
    EXPECT_EQ(0, provider.getPendingUploadCount());
}

TEST(RasterOverlayLifecycleTest, DefaultFrameBudgetUploadsMultipleRasterTiles) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    TileKey firstKey{scheme->id(), 1, 0, 0};
    TileKey secondKey{scheme->id(), 1, 1, 0};
    auto firstTile = provider.getTile(firstKey);
    auto secondTile = provider.getTile(secondKey);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_TRUE(provider.loadTile(*secondTile));

    provider.setFrameNumber(1);
    EXPECT_EQ(2, provider.processPendingUploads(false));
    EXPECT_EQ(2, uploaderPtr->uploadCount);
}

TEST(RasterOverlayLifecycleTest, FrameBudgetLimitsRasterWorkerRequests) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey firstKey{scheme->id(), 1, 0, 0};
    TileKey secondKey{scheme->id(), 1, 1, 0};
    auto firstTile = provider.getTile(firstKey);
    auto secondTile = provider.getTile(secondKey);

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*firstTile, &budget));
    EXPECT_FALSE(provider.loadTileThrottled(*secondTile, &budget));
    EXPECT_EQ(1, imagery.requestCount);
    EXPECT_EQ(1u, budget.networkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Unloaded,
              secondTile->getState());
}

TEST(RasterOverlayLifecycleTest, NonUnloadedRasterTilesDoNotConsumeRequestBudgetLikeCesiumNative) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    auto loadedTile =
        provider.getTile(TileKey{scheme->id(), 1, 0, 0});
    auto failedTile =
        provider.getTile(TileKey{scheme->id(), 1, 1, 0});
    loadedTile->setTexture(std::make_unique<TestTexture>(2, 2));
    failedTile->setState(RasterOverlayTile::LoadState::Failed);

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*loadedTile, &budget));
    EXPECT_TRUE(provider.loadTileThrottled(*failedTile, &budget));
    EXPECT_EQ(0, imagery.requestCount);
    EXPECT_EQ(0u, budget.networkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              loadedTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed,
              failedTile->getState());
}

TEST(RasterOverlayLifecycleTest, RectangleLoadRequiresBudgetForSourceFanout) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto rectangleTile = provider.getTile(projectForProvider(provider, rootBounds), 8.0, 8.0);
    ASSERT_NE(nullptr, rectangleTile);
    ASSERT_TRUE(rectangleTile->isRectangleTile());

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 20;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 20;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_FALSE(provider.loadTileThrottled(*rectangleTile, &budget));
    EXPECT_EQ(0, imagery.requestCount);
    EXPECT_EQ(0u, budget.networkRequestsIssued());
    EXPECT_EQ(0u, budget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Unloaded,
              rectangleTile->getState());

    config.maxNetworkRequestsPerFrame = 4;
    config.maxRasterNetworkRequestsPerFrame = 4;
    budget.beginFrame(2, config);

    EXPECT_TRUE(provider.loadTileThrottled(*rectangleTile, &budget));
    EXPECT_EQ(4, imagery.requestCount);
    EXPECT_EQ(4u, budget.networkRequestsIssued());
    EXPECT_EQ(4u, budget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              rectangleTile->getState());
}

TEST(RasterOverlayLifecycleTest, LoadingRectangleDoesNotPumpSourcesAcrossFrames) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    auto rectangleTile = provider.getTile(
        Rectangle::fromDegrees(-170.0, -70.0, 170.0, 70.0),
        1024.0,
        1024.0);
    ASSERT_NE(nullptr, rectangleTile);
    ASSERT_TRUE(rectangleTile->isRectangleTile());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 32;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*rectangleTile, &firstBudget));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              rectangleTile->getState());
    const size_t firstBatchSize = imagery.pending.size();
    ASSERT_GT(firstBatchSize, 2u);
    EXPECT_EQ(firstBatchSize, firstBudget.rasterNetworkRequestsIssued());

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, config);

    EXPECT_TRUE(provider.loadTileThrottled(*rectangleTile, &secondBudget));
    EXPECT_EQ(firstBatchSize, imagery.pending.size());
    EXPECT_EQ(0u, secondBudget.rasterNetworkRequestsIssued());
}

TEST(RasterOverlayLifecycleTest, FrameBudgetSeparatesRasterFanoutFromTerrainBudget) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto rectangleTile = provider.getTile(projectForProvider(provider, rootBounds), 8.0, 8.0);
    ASSERT_NE(nullptr, rectangleTile);
    ASSERT_TRUE(rectangleTile->isRectangleTile());

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxTerrainContentNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxNetworkInflight = 1;
    config.maxTerrainContentNetworkInflight = 1;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*rectangleTile, &budget));
    EXPECT_EQ(4, imagery.requestCount);
    EXPECT_EQ(4u, budget.networkRequestsIssued());
    EXPECT_EQ(0u, budget.terrainContentNetworkRequestsIssued());
    EXPECT_EQ(4u, budget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              rectangleTile->getState());
}

TEST(RasterOverlayLifecycleTest, NotReadyProviderMapsPlaceholderLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setReady(false);

    TileKey key{scheme->id(), 2, 1, 1};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    RasterMappedToTilesetTile::MoreDetail moreDetail = mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);

    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_EQ(provider.getPlaceholderTile().get(), mapped.getLoadingTile());
    EXPECT_EQ(RasterOverlayTile::LoadState::Placeholder,
              mapped.getLoadingTile()->getState());
    EXPECT_EQ(-1, mapped.getTextureCoordinateID());
    EXPECT_EQ(RasterMappedToTilesetTile::State::Unattached,
              mapped.getState());
    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::No, moreDetail);
    EXPECT_TRUE(missing.empty());
}

TEST(RasterOverlayLifecycleTest, PlaceholderRemapsWhenProviderBecomesReadyLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setReady(false);

    TileKey key{scheme->id(), 2, 1, 1};
    const Rectangle preciseRectangle = scheme->tileToRectangle(key);
    RasterOverlayDetails details = makeProviderDetails(*scheme, preciseRectangle);
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    ASSERT_EQ(provider.getPlaceholderTile().get(), mapped.getLoadingTile());

    provider.setReady(true);
    missing.clear();
    const RasterMappedToTilesetTile::MoreDetail moreDetail = mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);

    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_TRUE(mapped.getLoadingTile()->isRectangleTile());
    EXPECT_EQ(projectForProvider(provider, preciseRectangle),
              mapped.getLoadingTile()->getRectangle());
    EXPECT_EQ(0, mapped.getTextureCoordinateID());
    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::Unknown, moreDetail);
    EXPECT_TRUE(missing.empty());
    EXPECT_EQ(1, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, RenderContentDetailsRectangleMapsRealTileLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle preciseRectangle =
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0);
    RasterOverlayDetails details = makeProviderDetails(*scheme, preciseRectangle);
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    const RasterMappedToTilesetTile::MoreDetail moreDetail = mapped.update(
        TileKey{"Geographic-TMS", 4, 8, 8},
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);

    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_TRUE(mapped.getLoadingTile()->isRectangleTile());
    EXPECT_EQ(projectForProvider(provider, preciseRectangle),
              mapped.getLoadingTile()->getRectangle());
    EXPECT_EQ(0, mapped.getTextureCoordinateID());
    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::Unknown, moreDetail);
    EXPECT_TRUE(missing.empty());
}

TEST(RasterOverlayLifecycleTest, MissingProjectionUsesOffsetPlaceholderLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 2, 1, 1};
    RasterOverlayDetails staleDetails;
    staleDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    const RasterMappedToTilesetTile::MoreDetail moreDetail = mapped.update(
        key,
        staleDetails,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);

    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_EQ(provider.getPlaceholderTile().get(), mapped.getLoadingTile());
    EXPECT_EQ(RasterOverlayTile::LoadState::Placeholder,
              mapped.getLoadingTile()->getState());
    EXPECT_EQ(1, mapped.getTextureCoordinateID());
    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::No, moreDetail);
    EXPECT_TRUE(mapped.loadThrottled(provider));
    ASSERT_EQ(1u, missing.size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator, missing[0]);
    EXPECT_EQ(0, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, BoundingRegionWithoutRenderDetailsUsesPlaceholderLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    RasterOverlayDetails emptyDetails;
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    const RasterMappedToTilesetTile::MoreDetail moreDetail = mapped.update(
        TileKey{scheme->id(), 4, 8, 8},
        emptyDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0,
        false);

    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_EQ(RasterOverlayTile::LoadState::Placeholder,
              mapped.getLoadingTile()->getState());
    EXPECT_EQ(0, mapped.getTextureCoordinateID());
    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::No, moreDetail);
    ASSERT_EQ(1u, missing.size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator, missing[0]);
    EXPECT_EQ(0, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, AttachedUnknownReportsMoreDetailLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    RasterMappedToTilesetTile::MoreDetail first = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    ASSERT_EQ(RasterMappedToTilesetTile::MoreDetail::Unknown, first);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setTexture(std::make_unique<TestTexture>(4, 4));

    RasterMappedToTilesetTile::MoreDetail promoted = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::Unknown, promoted);
    EXPECT_EQ(RasterMappedToTilesetTile::State::Unattached,
              mapped.getState());
    EXPECT_EQ(loadingTile, mapped.getReadyTile());

    RecordingPrepareRendererResources recorder;
    RasterMappedToTilesetTile::MoreDetail attached = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        &recorder,
        missing);
    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::Unknown, attached);
    EXPECT_EQ(RasterMappedToTilesetTile::State::Attached, mapped.getState());
    EXPECT_EQ(1, recorder.attachCount);
    EXPECT_EQ(loadingTile, recorder.lastRasterTile.get());
    EXPECT_EQ(loadingTile->getTexture(), recorder.lastTexture);

    RasterMappedToTilesetTile::MoreDetail fastPath = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        &recorder,
        missing);
    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::Yes, fastPath);
    EXPECT_FALSE(mapped.isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, FailedTileWithoutAncestorBecomesReadyLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* failedTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, failedTile);
    failedTile->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::Yes);
    failedTile->setState(RasterOverlayTile::LoadState::Failed);

    const RasterMappedToTilesetTile::MoreDetail failedUpdate = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);

    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::No, failedUpdate);
    EXPECT_EQ(failedTile, mapped.getReadyTile());
    EXPECT_EQ(nullptr, mapped.getLoadingTile());
    EXPECT_EQ(RasterMappedToTilesetTile::State::Attached,
              mapped.getState());
    EXPECT_FALSE(mapped.isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, FailedReadyTileDetachSkipsRendererCallbackLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* failedTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, failedTile);
    failedTile->setState(RasterOverlayTile::LoadState::Failed);

    mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    ASSERT_EQ(failedTile, mapped.getReadyTile());
    ASSERT_EQ(RasterMappedToTilesetTile::State::Attached,
              mapped.getState());

    RecordingPrepareRendererResources recorder;
    mapped.detachFromTile(&recorder);

    EXPECT_EQ(0, recorder.detachCount);
    EXPECT_EQ(RasterMappedToTilesetTile::State::Unattached,
              mapped.getState());
}

TEST(RasterOverlayLifecycleTest, LoadInMainThreadOnlyPromotesLoadedTilesLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    provider.getPlaceholderTile()->loadInMainThread();
    EXPECT_EQ(RasterOverlayTile::LoadState::Placeholder,
              provider.getPlaceholderTile()->getState());

    TileKey key{scheme->id(), 1, 0, 0};
    auto tileHandle = provider.getTile(key);
    ASSERT_NE(nullptr, tileHandle);
    RasterOverlayTile* tile = tileHandle.get();

    tile->loadInMainThread();
    EXPECT_EQ(RasterOverlayTile::LoadState::Unloaded, tile->getState());

    tile->setState(RasterOverlayTile::LoadState::Loading);
    tile->loadInMainThread();
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading, tile->getState());

    tile->setState(RasterOverlayTile::LoadState::Failed);
    tile->loadInMainThread();
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());

    tile->setTexture(std::make_unique<TestTexture>(4, 4));
    ASSERT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
    tile->loadInMainThread();
    EXPECT_EQ(RasterOverlayTile::LoadState::Done, tile->getState());

    tile->loadInMainThread();
    EXPECT_EQ(RasterOverlayTile::LoadState::Done, tile->getState());
}

TEST(RasterOverlayLifecycleTest, TemporaryAncestorDoesNotReportMoreDetailLikeCesiumNative) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    RasterOverlayTileProvider provider(
        overlay->getProvider(),
        overlay->getTileScheme(),
        nullptr);
    provider.setOwner(overlay.get());

    TileKey parentKey{overlay->getTileScheme().id(), 2, 1, 1};
    TileKey childKey{overlay->getTileScheme().id(), 3, 2, 2};

    RasterOverlayDetails parentDetails =
        makeProviderDetails(
            overlay->getTileScheme(),
            overlay->getTileScheme().tileToRectangle(parentKey));
    RasterOverlayDetails childDetails =
        makeProviderDetails(
            overlay->getTileScheme(),
            overlay->getTileScheme().tileToRectangle(childKey));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parentTile(
        parentKey,
        overlay->getTileScheme().tileToRectangle(parentKey));
    RasterMappedToTilesetTile& parentMapping =
        parentTile.rasterOverlayState.ensureMapping(0);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0);
    RasterOverlayTile* parentReady = parentMapping.getLoadingTile();
    ASSERT_NE(nullptr, parentReady);
    parentReady->setTexture(std::make_unique<TestTexture>(4, 4));
    parentReady->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::Yes);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0);
    ASSERT_EQ(parentReady, parentMapping.getReadyTile());
    ASSERT_EQ(RasterMappedToTilesetTile::State::Unattached,
              parentMapping.getState());

    RasterMappedToTilesetTile childMapping;
    childMapping.update(
        childKey,
        childDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0);
    ASSERT_NE(nullptr, childMapping.getLoadingTile());

    const RasterMappedToTilesetTile::MoreDetail fallback =
        childMapping.update(
            childKey,
            childDetails,
            512.0,
            512.0,
            provider,
            nullptr,
            missing,
            &parentTile,
            0);

    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::Unknown, fallback);
    EXPECT_EQ(parentReady, childMapping.getReadyTile());
    EXPECT_EQ(RasterMappedToTilesetTile::State::Unattached,
              childMapping.getState());
    EXPECT_FALSE(childMapping.isMoreDetailAvailable());

    RecordingPrepareRendererResources recorder;
    const RasterMappedToTilesetTile::MoreDetail attachedFallback =
        childMapping.update(
            childKey,
            childDetails,
            512.0,
            512.0,
            provider,
            &recorder,
            missing,
            &parentTile,
            0);

    EXPECT_EQ(RasterMappedToTilesetTile::MoreDetail::Unknown,
              attachedFallback);
    EXPECT_EQ(RasterMappedToTilesetTile::State::TemporarilyAttached,
              childMapping.getState());
    EXPECT_EQ(parentReady, childMapping.getReadyTile());
    EXPECT_FALSE(childMapping.isMoreDetailAvailable());
    EXPECT_EQ(1, recorder.attachCount);
    EXPECT_EQ(parentReady, recorder.lastRasterTile.get());
    EXPECT_EQ(parentReady->getTexture(), recorder.lastTexture);
}

TEST(RasterOverlayLifecycleTest, MappedReadyTileRetainsProviderCacheUntilReleased) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    provider.setFrameNumber(1);
    RasterMappedToTilesetTile mapped;
    mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* tile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, tile);
    tile->setTexture(std::make_unique<TestTexture>(64, 32));

    std::weak_ptr<RasterOverlayTile> weakTile =
        mapped.getLoadingTileHandle();
    mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    ASSERT_EQ(mapped.getReadyTile(), tile);

    provider.setFrameNumber(200);
    provider.trimUnusedTiles();

    EXPECT_EQ(provider.getCachedTileCount(), 1);
    EXPECT_FALSE(weakTile.expired());

    mapped.releaseTileReferences(nullptr);
    provider.trimUnusedTiles();

    EXPECT_EQ(provider.getCachedTileCount(), 0);
    EXPECT_TRUE(weakTile.expired());
}

TEST(RasterOverlayLifecycleTest, RenderCommandKeepAliveRetainsRasterAfterMappingRelease) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    provider.setFrameNumber(1);
    RasterMappedToTilesetTile mapped;
    mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* tile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, tile);
    tile->setTexture(std::make_unique<TestTexture>(64, 32));

    std::weak_ptr<RasterOverlayTile> weakTile =
        mapped.getLoadingTileHandle();
    mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);

    SurfaceRasterBinding binding = chooseSurfaceRasterBinding(&mapped);
    ASSERT_EQ(SurfaceRasterBindingKind::RealTile, binding.kind);
    ASSERT_TRUE(binding.tileHandle);

    RenderCommand command;
    command.textures.push_back(binding.tile->getTexture());
    command.resourceKeepAlive.push_back(binding.tileHandle);
    binding.tileHandle.reset();

    mapped.releaseTileReferences(nullptr);
    provider.setFrameNumber(200);
    provider.trimUnusedTiles();

    EXPECT_EQ(provider.getCachedTileCount(), 1);
    EXPECT_FALSE(weakTile.expired());
    EXPECT_EQ(command.textures[0], tile->getTexture());

    command.resourceKeepAlive.clear();
    provider.trimUnusedTiles();
    EXPECT_EQ(provider.getCachedTileCount(), 0);
    EXPECT_TRUE(weakTile.expired());
}

TEST(RasterOverlayLifecycleTest, SurfaceRasterBindingAcceptsOnlyRealLoadedTiles) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 1, 1, 1};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    RasterMappedToTilesetTile mapped;
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);
    RasterOverlayTile* tile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, tile);
    tile->setTexture(std::make_unique<TestTexture>(4, 4));
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);

    SurfaceRasterBinding real = chooseSurfaceRasterBinding(&mapped);
    EXPECT_EQ(SurfaceRasterBindingKind::RealTile, real.kind);
    EXPECT_EQ(real.tile, tile);
    EXPECT_EQ(real.tileHandle.get(), tile);

    tile->loadInMainThread();
    EXPECT_EQ(RasterOverlayTile::LoadState::Done, tile->getState());
    EXPECT_EQ(SurfaceRasterBindingKind::RealTile,
              chooseSurfaceRasterBinding(&mapped).kind);

    tile->setState(RasterOverlayTile::LoadState::Failed);
    EXPECT_EQ(SurfaceRasterBindingKind::None,
              chooseSurfaceRasterBinding(&mapped).kind);

    EXPECT_FALSE(isLegalSurfaceRasterTile(provider.getPlaceholderTile().get()));

    TileKey noTextureKey{scheme->id(), 1, 0, 1};
    RasterOverlayDetails noTextureDetails;
    noTextureDetails.setGeographicRectangle(
        scheme->tileToRectangle(noTextureKey));
    RasterMappedToTilesetTile noTextureMapped;
    noTextureMapped.update(
        noTextureKey,
        noTextureDetails,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* noTextureTile = noTextureMapped.getLoadingTile();
    ASSERT_NE(nullptr, noTextureTile);
    noTextureTile->setState(RasterOverlayTile::LoadState::Loaded);
    noTextureMapped.update(
        noTextureKey,
        noTextureDetails,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    EXPECT_EQ(SurfaceRasterBindingKind::None,
              chooseSurfaceRasterBinding(&noTextureMapped).kind);
}

TEST(RasterOverlayLifecycleTest,
     RasterOverlayStateSeparatesCoverReadyFromDrawableReady) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 1, 1, 1};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile tile(key, scheme->tileToRectangle(key));
    RasterMappedToTilesetTile& mapped =
        tile.rasterOverlayState.ensureMapping(0);
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);
    RasterOverlayTile* failedTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, failedTile);
    failedTile->setState(RasterOverlayTile::LoadState::Failed);

    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);

    EXPECT_EQ(failedTile, mapped.getReadyTile());
    EXPECT_TRUE(tile.rasterOverlayState.hasReadyMapping(0));
    EXPECT_FALSE(tile.rasterOverlayState.hasDrawableReadyMapping(0));
    EXPECT_EQ(SurfaceRasterBindingKind::None,
              chooseSurfaceRasterBinding(&mapped).kind);

    TileKey noTextureKey{scheme->id(), 1, 0, 1};
    RasterOverlayDetails noTextureDetails;
    noTextureDetails.setGeographicRectangle(
        scheme->tileToRectangle(noTextureKey));
    RasterMappedToTilesetTile& noTextureMapped =
        tile.rasterOverlayState.ensureMapping(1);
    noTextureMapped.update(
        noTextureKey,
        noTextureDetails,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* noTextureTile = noTextureMapped.getLoadingTile();
    ASSERT_NE(nullptr, noTextureTile);
    noTextureTile->markLoadedWithoutTexture();
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              noTextureTile->getState());
    EXPECT_EQ(nullptr, noTextureTile->getTexture());

    noTextureMapped.update(
        noTextureKey,
        noTextureDetails,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);

    EXPECT_EQ(noTextureTile, noTextureMapped.getReadyTile());
    EXPECT_TRUE(tile.rasterOverlayState.hasReadyMapping(1));
    EXPECT_FALSE(tile.rasterOverlayState.hasDrawableReadyMapping(1));
    EXPECT_FLOAT_EQ(0.0f, noTextureMapped.getTranslationU());
    EXPECT_FLOAT_EQ(0.0f, noTextureMapped.getTranslationV());
    EXPECT_FLOAT_EQ(1.0f, noTextureMapped.getScaleU());
    EXPECT_FLOAT_EQ(1.0f, noTextureMapped.getScaleV());

    noTextureTile->setTexture(std::make_unique<TestTexture>(4, 4));
    EXPECT_TRUE(tile.rasterOverlayState.hasDrawableReadyMapping(1));
}

TEST(RasterOverlayLifecycleTest,
     SurfaceBuilderRequiresDrawableBaseRasterNotJustCoverReady) {
    RasterOverlay::Options options;
    options.role = RasterOverlayRole::BaseImagery;
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* provider =
        activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);

    TileKey key{overlay->getTileScheme().id(), 1, 1, 1};
    RasterOverlayDetails details;
    details.setGeographicRectangle(
        overlay->getTileScheme().tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile tile(key, overlay->getTileScheme().tileToRectangle(key));
    RasterMappedToTilesetTile& mapped =
        tile.rasterOverlayState.ensureMapping(0);
    mapped.update(key, details, 256.0, 256.0, *provider, nullptr, missing);
    RasterOverlayTile* failedTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, failedTile);
    failedTile->setState(RasterOverlayTile::LoadState::Failed);

    mapped.update(key, details, 256.0, 256.0, *provider, nullptr, missing);

    ASSERT_TRUE(tile.rasterOverlayState.hasReadyMapping(0));
    ASSERT_FALSE(tile.rasterOverlayState.hasDrawableReadyMapping(0));
    EXPECT_FALSE(SurfaceTileDrawCommandBuilder::hasDrawableBaseRaster(
        tile,
        std::vector<ActivatedRasterOverlay*>{&activated}));

    failedTile->setState(RasterOverlayTile::LoadState::Loaded);
    failedTile->setTexture(std::make_unique<TestTexture>(4, 4));

    EXPECT_TRUE(tile.rasterOverlayState.hasDrawableReadyMapping(0));
    EXPECT_TRUE(SurfaceTileDrawCommandBuilder::hasDrawableBaseRaster(
        tile,
        std::vector<ActivatedRasterOverlay*>{&activated}));
}

TEST(RasterOverlayLifecycleTest, SurfaceRasterBindingClassifiesAncestorWhileChildLoads) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey parentKey{scheme->id(), 0, 0, 0};
    TileKey childKey{scheme->id(), 1, 0, 1};

    RasterOverlayDetails details;
    details.setGeographicRectangle(scheme->tileToRectangle(parentKey));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parentTile(parentKey, scheme->tileToRectangle(parentKey));
    RasterMappedToTilesetTile& parentMapping =
        parentTile.rasterOverlayState.ensureMapping(0);
    parentMapping.update(
        parentKey,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* parentRaster =
        parentMapping.getLoadingTile();
    ASSERT_NE(nullptr, parentRaster);
    parentRaster->setTexture(std::make_unique<TestTexture>(4, 4));
    parentMapping.update(
        parentKey,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);
    ASSERT_EQ(parentMapping.getReadyTile(), parentRaster);

    RasterMappedToTilesetTile childMapped;
    RasterOverlayDetails childDetails;
    childDetails.setGeographicRectangle(scheme->tileToRectangle(childKey));
    childMapped.update(
        childKey,
        childDetails,
        256.0,
        256.0,
        provider,
        nullptr,
        missing,
        &parentTile,
        0);

    SurfaceRasterBinding binding = chooseSurfaceRasterBinding(&childMapped);
    EXPECT_EQ(SurfaceRasterBindingKind::AncestorTile, binding.kind);
    EXPECT_EQ(binding.tile, parentRaster);
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionRejectsNoCoverageAndAcceptsFullCoverage) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});
    Rectangle outside(
        target.east(),
        target.south(),
        target.east() + target.width() * 0.5,
        target.north());

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> noCoverage;
    noCoverage.push_back({
        TileKey{scheme->id(), 2, 0, 0},
        outside,
        makeImage(2, 2, 10)});
    auto noCoverageResult =
        RasterOverlayTileProvider::composeRectangleImages(
            *scheme,
            target,
            2,
            std::move(noCoverage),
            8);
    EXPECT_EQ(nullptr, noCoverageResult);

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> full;
    full.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        makeImage(2, 2, 20)});
    auto fullResult =
        RasterOverlayTileProvider::composeRectangleImages(
            *scheme,
            target,
            1,
            std::move(full),
            8);
    ASSERT_NE(nullptr, fullResult);
    EXPECT_GT(fullResult->width, 0);
    EXPECT_GT(fullResult->height, 0);
    EXPECT_EQ(20, fullResult->pixels[0]);
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionRejectsMalformedSourceImages) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    auto malformed = std::make_unique<DecodedImage>();
    malformed->width = 2;
    malformed->height = 2;
    malformed->channels = 4;
    malformed->pixels.resize(4);

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        std::move(malformed)});
    auto result =
        RasterOverlayTileProvider::composeRectangleImages(
            *scheme,
            target,
            1,
            std::move(sources),
            8);
    EXPECT_EQ(nullptr, result);
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionUsesSourceMoreDetailFlagLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        makeImage(2, 2, 20),
        false,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result = RasterOverlayTileProvider::composeRectangleImagesWithDetails(
        *scheme,
        target,
        1,
        std::move(sources),
        4,
        8);

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              result.moreDetailAvailable);
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionReturnsEmptyImageForOnlyAncestorFallbackLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 0, 0, 0},
        target,
        makeImage(2, 2, 20),
        true,
        RasterOverlayTile::MoreDetailAvailable::Yes});

    auto result = RasterOverlayTileProvider::composeRectangleImagesWithDetails(
        *scheme,
        target,
        1,
        std::move(sources),
        4,
        8);

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(0, result.image->width);
    EXPECT_EQ(0, result.image->height);
    EXPECT_EQ(0, result.image->channels);
    EXPECT_TRUE(result.image->pixels.empty());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              result.moreDetailAvailable);
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionReturnsCoveredRectangleLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});
    Rectangle covered(
        target.west(),
        target.south(),
        target.west() + target.width() * 0.5,
        target.north());

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 2, 0, 0},
        covered,
        makeImage(2, 2, 30),
        false,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result = RasterOverlayTileProvider::composeRectangleImagesWithDetails(
        *scheme,
        target,
        2,
        std::move(sources),
        2,
        8);

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(2, result.image->width);
    EXPECT_EQ(2, result.image->height);
    EXPECT_TRUE(result.rectangle.equalsEpsilon(covered, 1e-12));
    EXPECT_EQ(30, result.image->pixels[0]);
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionBlitsSourcePixelBlocksLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey westKey{scheme->id(), 2, 0, 1};
    const TileKey eastKey{scheme->id(), 2, 1, 1};
    const Rectangle westBounds = scheme->tileToRectangle(westKey);
    const Rectangle eastBounds = scheme->tileToRectangle(eastKey);
    const Rectangle target(
        westBounds.west(),
        westBounds.south(),
        eastBounds.east(),
        westBounds.north());

    auto westImage = std::make_unique<DecodedImage>();
    westImage->width = 2;
    westImage->height = 2;
    westImage->channels = 4;
    westImage->pixels = {
        10, 0, 0, 255, 11, 0, 0, 255,
        12, 0, 0, 255, 13, 0, 0, 255};
    auto eastImage = std::make_unique<DecodedImage>();
    eastImage->width = 2;
    eastImage->height = 2;
    eastImage->channels = 4;
    eastImage->pixels = {
        20, 0, 0, 255, 21, 0, 0, 255,
        22, 0, 0, 255, 23, 0, 0, 255};

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> sources;
    sources.push_back({westKey, westBounds, std::move(westImage)});
    sources.push_back({eastKey, eastBounds, std::move(eastImage)});

    auto result = RasterOverlayTileProvider::composeRectangleImagesWithDetails(
        *scheme,
        target,
        westKey.z,
        std::move(sources),
        westKey.z,
        8);

    ASSERT_NE(nullptr, result.image);
    ASSERT_EQ(4, result.image->width);
    ASSERT_EQ(2, result.image->height);
    EXPECT_EQ(10, result.image->pixels[0]);
    EXPECT_EQ(11, result.image->pixels[4]);
    EXPECT_EQ(20, result.image->pixels[8]);
    EXPECT_EQ(21, result.image->pixels[12]);
    EXPECT_EQ(12, result.image->pixels[16]);
    EXPECT_EQ(13, result.image->pixels[20]);
    EXPECT_EQ(22, result.image->pixels[24]);
    EXPECT_EQ(23, result.image->pixels[28]);
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionKeepsTinyProjectedOverlap) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey sourceKey{scheme->id(), 2, 1, 0};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const double tinyHeight = sourceBounds.height() * 0.001;
    const Rectangle target(
        sourceBounds.west(),
        sourceBounds.north() - tinyHeight,
        sourceBounds.east(),
        sourceBounds.north());

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> sources;
    sources.push_back({
        sourceKey,
        sourceBounds,
        makeImage(64, 64, 40)});

    auto result = RasterOverlayTileProvider::composeRectangleImages(
        *scheme,
        target,
        sourceKey.z,
        std::move(sources),
        4096);

    ASSERT_NE(nullptr, result);
    EXPECT_EQ(64, result->width);
    EXPECT_EQ(1, result->height);
}

TEST(RasterOverlayLifecycleTest, RectangleCompositionUsesProjectedWebMercatorHeight) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey sourceKey{scheme->id(), 2, 1, 0};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const double midLatitude =
        (sourceBounds.south() + sourceBounds.north()) * 0.5;
    const Rectangle target(
        sourceBounds.west(),
        midLatitude,
        sourceBounds.east(),
        sourceBounds.north());

    std::vector<RasterOverlayTileProvider::RectangleSourceImage> sources;
    sources.push_back({
        sourceKey,
        sourceBounds,
        makeImage(64, 64, 50)});

    auto result = RasterOverlayTileProvider::composeRectangleImages(
        *scheme,
        target,
        sourceKey.z,
        std::move(sources),
        4096);

    const double sourceProjectedHeight =
        std::log(std::tan(sourceBounds.north() * 0.5 + M_PI * 0.25)) -
        std::log(std::tan(sourceBounds.south() * 0.5 + M_PI * 0.25));
    const double targetProjectedHeight =
        std::log(std::tan(target.north() * 0.5 + M_PI * 0.25)) -
        std::log(std::tan(target.south() * 0.5 + M_PI * 0.25));
    const int expectedHeight = static_cast<int>(
        std::ceil(targetProjectedHeight / (sourceProjectedHeight / 64.0)));

    ASSERT_NE(nullptr, result);
    EXPECT_EQ(64, result->width);
    EXPECT_EQ(expectedHeight, result->height);
    EXPECT_NE(32, expectedHeight);
}

TEST(RasterOverlayLifecycleTest, WebMercatorSourceSamplingUsesProjectedY) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle bounds = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 1, 0});
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;

    const double centerV =
        RasterOverlayTileProvider::projectedVForLatitude(
            *scheme,
            bounds,
            centerLat);
    const double linearV =
        (bounds.north() - centerLat) / bounds.height();

    EXPECT_NEAR(0.0,
                RasterOverlayTileProvider::projectedVForLatitude(
                    *scheme,
                    bounds,
                    bounds.north()),
                1e-12);
    EXPECT_NEAR(1.0,
                RasterOverlayTileProvider::projectedVForLatitude(
                    *scheme,
                    bounds,
                    bounds.south()),
                1e-12);
    EXPECT_NEAR(0.5, linearV, 1e-12);
    EXPECT_GT(std::abs(centerV - linearV), 0.02);
}
