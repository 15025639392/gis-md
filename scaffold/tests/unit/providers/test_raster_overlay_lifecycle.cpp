#include <gtest/gtest.h>

#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/ImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/providers/RasterOverlaySourcePlan.h"
#include "earth_engine/providers/RasterAsset.h"
#include "earth_engine/providers/RasterAssetDepot.h"
#include "earth_engine/providers/RasterTextureUploader.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/core/async/AsyncSystem.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Gcj02CoordinateTransform.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/core/resources/SceneFrameResourceArbiter.h"
#include "earth_engine/debug/Policies.h"
#include "earth_engine/renderer/IPrepareRendererResources.h"
#include "earth_engine/renderer/RenderCommand.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/renderer/TerrainPageStore.h"
#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/RasterBindingSet.h"
#include "earth_engine/tiling/RasterOverlayRuntime.h"
#include "earth_engine/tiling/RasterResolution.h"
#include "earth_engine/tiling/SurfaceRasterBinding.h"
#include "earth_engine/tiling/TileContentCacheManager.h"
#include "earth_engine/tiling/TileContentLifecycleManager.h"
#include "earth_engine/tiling/TileContentResourceInvalidator.h"
#include "earth_engine/tiling/TileContentUploadCommitter.h"
#include "earth_engine/tiling/TileLoadQueue.h"
#include "earth_engine/tiling/TileMeshPreparationManager.h"
#include "earth_engine/tiling/TileRasterOverlayFrameProcessor.h"
#include "earth_engine/tiling/TileRasterOverlayPrefetcher.h"
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/tiling/TileSurface.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "earth_engine/tiling/TileScheme.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <deque>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace earth_engine;

namespace {

class TestTexture final : public Texture {
public:
    TestTexture(int width, int height, size_t sizeBytes = 0)
        : width_(width),
          height_(height),
          sizeBytes_(
              sizeBytes > 0
                  ? sizeBytes
                  : static_cast<size_t>(width) *
                        static_cast<size_t>(height) * 4u) {}

    int width() const override { return width_; }
    int height() const override { return height_; }
    size_t sizeBytes() const override { return sizeBytes_; }

private:
    int width_ = 0;
    int height_ = 0;
    size_t sizeBytes_ = 0;
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

std::unique_ptr<DecodedImage> makeRgbImage(int width,
                                           int height,
                                           uint8_t r,
                                           uint8_t g,
                                           uint8_t b) {
    auto image = std::make_unique<DecodedImage>();
    image->width = width;
    image->height = height;
    image->channels = 3;
    image->pixels.resize(static_cast<size_t>(width) *
                         static_cast<size_t>(height) * 3u);
    for (size_t i = 0; i < image->pixels.size(); i += 3) {
        image->pixels[i + 0] = r;
        image->pixels[i + 1] = g;
        image->pixels[i + 2] = b;
    }
    return image;
}

std::unique_ptr<DecodedImage> makeGrayImage(int width,
                                            int height,
                                            uint8_t value) {
    auto image = std::make_unique<DecodedImage>();
    image->width = width;
    image->height = height;
    image->channels = 1;
    image->pixels.resize(static_cast<size_t>(width) *
                         static_cast<size_t>(height),
                         value);
    return image;
}

int processPendingUploadsUntil(RasterOverlayTileProvider& provider,
                               int expectedUploads) {
    int processed = 0;
    for (int attempt = 0; attempt < 200 && processed < expectedUploads;
         ++attempt) {
        processed += provider.processPendingUploads(false).processedUploads;
        if (processed >= expectedUploads) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return processed;
}

int waitForPendingUploadCount(RasterOverlayTileProvider& provider,
                              int expectedUploads) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        const int pending = provider.getPendingUploadCount();
        if (pending >= expectedUploads) {
            return pending;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return provider.getPendingUploadCount();
}

bool waitForNoPendingWork(RasterOverlayTileProvider& provider) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (!provider.hasPendingWork()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !provider.hasPendingWork();
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
    uint64_t contentRevision() const override {
        return contentRevisionValue;
    }
    std::string buildUrl(const TileKey&) const override { return {}; }
    std::string attribution() const override { return attributionValue; }
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
    uint64_t contentRevisionValue = 0;
    std::string attributionValue;
};

class AdmissionObservingImageryProvider final : public ImageryProvider {
public:
    explicit AdmissionObservingImageryProvider(
        SceneFrameResourceArbiter& resourceArbiter)
        : arbiter(resourceArbiter) {}

    std::string id() const override { return "admission-observing"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 18; }
    int tileWidth() const override { return 2; }
    int tileHeight() const override { return 2; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority =
                         HttpRequestPriority::Normal) override {
        usedAtRequest.push_back(arbiter.used(
            SceneFrameResourceProducer::Raster,
            SceneFrameResourceStage::NetworkRequest,
            FrameResourcePriority::Normal));
        callback(key, makeImage(2, 2, 64));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    SceneFrameResourceArbiter& arbiter;
    std::vector<uint32_t> usedAtRequest;
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

class RecordingImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "recording"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 18; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        requestedKeys.push_back(key);
        callback(key, makeImage(tileWidth(), tileHeight(), 80));
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

class HighBitImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "high-bit"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 18; }
    int tileWidth() const override { return 1; }
    int tileHeight() const override { return 1; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        auto image = std::make_unique<DecodedImage>();
        image->width = 1;
        image->height = 1;
        image->channels = 3;
        image->bytesPerChannel = 2;
        image->pixels = {0x34, 0x12, 0x56, 0x12, 0x78, 0x12};
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
        if ((failAtOrAboveLevel >= 0 && key.z >= failAtOrAboveLevel) ||
            key == failingKey ||
            std::find(failingKeys.begin(), failingKeys.end(), key) !=
                failingKeys.end()) {
            callback(key, nullptr);
            return;
        }
        if (!encodeQuadrants) {
            callback(key, makeImage(256, 256, static_cast<uint8_t>(key.z)));
            return;
        }

        auto image = std::make_unique<DecodedImage>();
        image->width = 256;
        image->height = 256;
        image->channels = 4;
        image->pixels.resize(256u * 256u * 4u);
        for (int y = 0; y < image->height; ++y) {
            for (int x = 0; x < image->width; ++x) {
                const size_t offset =
                    (static_cast<size_t>(y) * 256u +
                     static_cast<size_t>(x)) *
                    4u;
                image->pixels[offset + 0] =
                    static_cast<uint8_t>(key.z);
                image->pixels[offset + 1] =
                    static_cast<uint8_t>(x >= 128);
                image->pixels[offset + 2] =
                    static_cast<uint8_t>(y >= 128);
                image->pixels[offset + 3] = 255;
            }
        }
        callback(key, std::move(image));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    TileKey failingKey{"XYZ-WebMercator", -1, -1, -1};
    std::vector<TileKey> failingKeys;
    int failAtOrAboveLevel = -1;
    std::string schemeIdValue = "XYZ-WebMercator";
    int tileWidthValue = 256;
    int tileHeightValue = 256;
    bool encodeQuadrants = false;
    std::vector<TileKey> requestedKeys;
};

class AlwaysFailingImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "always-failing"; }
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
        callback(key, nullptr);
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    std::vector<TileKey> requestedKeys;
};

class ThrowOnceImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "throw-once"; }
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
        if (throwNext) {
            throwNext = false;
            throw std::runtime_error("simulated raster source failure");
        }
        callback(key, makeImage(256, 256, static_cast<uint8_t>(key.z)));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    bool throwNext = true;
    std::vector<TileKey> requestedKeys;
};

class UnsupportedParentFallbackImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "unsupported-parent-fallback"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 10; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    bool supportsTile(const TileKey& key) const override {
        return key.schemeId == schemeId() &&
               key.z >= minZoom() &&
               key.z <= maxZoom() &&
               key != unsupportedChildKey &&
               key != unsupportedParentKey;
    }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        requestedKeys.push_back(key);
        callback(
            key,
            key == failingChildKey || key == unsupportedChildKey
                ? nullptr
                : makeImage(256, 256, static_cast<uint8_t>(key.z)));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    TileKey failingChildKey{"XYZ-WebMercator", -1, -1, -1};
    TileKey unsupportedChildKey{"XYZ-WebMercator", -1, -1, -1};
    TileKey unsupportedParentKey{"XYZ-WebMercator", -1, -1, -1};
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
        auto image = makeImage(256, 256, static_cast<uint8_t>(item.key.z));
        lastCompletedImage = image.get();
        item.callback(
            item.key,
            std::move(image));
    }

    void failNext() {
        ASSERT_FALSE(pending.empty());
        Pending item = std::move(pending.front());
        pending.pop_front();
        item.callback(item.key, nullptr);
    }

    struct Pending {
        TileKey key;
        TileCallback callback;
    };
    std::vector<TileKey> requestedKeys;
    std::deque<Pending> pending;
    const DecodedImage* lastCompletedImage = nullptr;
};

class SequencedDeferredImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "sequenced-deferred"; }
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

    void completeNext(std::unique_ptr<DecodedImage> image) {
        ASSERT_FALSE(pending.empty());
        Pending item = std::move(pending.front());
        pending.pop_front();
        item.callback(item.key, std::move(image));
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

    void completeKey(const TileKey& key) {
        auto it = std::find_if(
            pending.begin(),
            pending.end(),
            [&key](const Pending& item) { return item.key == key; });
        ASSERT_NE(pending.end(), it);
        Pending item = std::move(*it);
        pending.erase(it);
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
        lastUploadSource = &image;
        lastUpload = image;
        return std::make_unique<TestTexture>(image.width, image.height);
    }

    int uploadCount = 0;
    int maxTextureSizeValue = 2048;
    const DecodedImage* lastUploadSource = nullptr;
    DecodedImage lastUpload;
};

class RejectingHighBitRasterUploader final : public RasterTextureUploader {
public:
    int maxTextureSize() const override { return 2048; }

    std::unique_ptr<Texture> uploadRasterTexture(
        const DecodedImage& image,
        const RasterTextureUploadOptions&) override {
        ++uploadCount;
        lastUpload = image;
        if (image.bytesPerChannel != 1) {
            return nullptr;
        }
        return std::make_unique<TestTexture>(image.width, image.height);
    }

    int uploadCount = 0;
    DecodedImage lastUpload;
};

class SlowRasterUploader final : public RasterTextureUploader {
public:
    int maxTextureSize() const override { return 2048; }

    std::unique_ptr<Texture> uploadRasterTexture(
        const DecodedImage& image,
        const RasterTextureUploadOptions&) override {
        ++uploadCount;
        uploadedRed.push_back(
            image.pixels.empty() ? 0 : image.pixels.front());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return std::make_unique<TestTexture>(image.width, image.height);
    }

    int uploadCount = 0;
    std::vector<uint8_t> uploadedRed;
};

class BudgetInspectingRasterUploader final : public RasterTextureUploader {
public:
    int maxTextureSize() const override { return 2048; }

    std::unique_ptr<Texture> uploadRasterTexture(
        const DecodedImage& image,
        const RasterTextureUploadOptions&) override {
        ++uploadCount;
        if (!triggered && onFirstUpload) {
            triggered = true;
            onFirstUpload();
        }
        if (provider) {
            observedSourceCacheBytesDuringUpload =
                provider->getCachedSourceTileBytes();
        }
        return std::make_unique<TestTexture>(image.width, image.height);
    }

    RasterOverlayTileProvider* provider = nullptr;
    std::function<void()> onFirstUpload;
    int uploadCount = 0;
    bool triggered = false;
    int64_t observedSourceCacheBytesDuringUpload = -1;
};

class RecordingPrepareRendererResources final
    : public IPrepareRendererResources {
public:
    void attachRasterInMainThread(
        const TileKey&,
        int32_t,
        std::shared_ptr<const RasterOverlayTile> rasterTile,
        Texture* texture,
        float translationU,
        float translationV,
        float scaleU,
        float scaleV) override {
        ++attachCount;
        lastRasterTile = std::move(rasterTile);
        lastTexture = texture;
        lastTranslationU = translationU;
        lastTranslationV = translationV;
        lastScaleU = scaleU;
        lastScaleV = scaleV;
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
    float lastTranslationU = 0.0f;
    float lastTranslationV = 0.0f;
    float lastScaleU = 1.0f;
    float lastScaleV = 1.0f;
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

TEST(RasterOverlayLifecycleTest, QuadtreeSourceZoomFollowsCesiumTargetScreenPixels) {
    ConfigurableImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle z3Bounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 3, 2, 3});

    auto matchingTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, z3Bounds), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, matchingTile);
    EXPECT_FALSE(matchingTile->isDirectCompositeTile());
    EXPECT_EQ((TileKey{scheme->id(), 3, 2, 3}), matchingTile->getTileID());

    auto widerTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, z3Bounds), 1024.0, 256.0).tile;
    ASSERT_NE(nullptr, widerTile);
    EXPECT_TRUE(widerTile->isDirectCompositeTile());
    EXPECT_EQ(4, widerTile->getDirectCompositeSourceZoom());

    imagery.minZoomValue = 5;
    RasterOverlayTileProvider minClampedProvider(imagery, *scheme, nullptr);
    auto minClampedTile = minClampedProvider.mapRasterTilesToGeometryTile(projectForProvider(minClampedProvider, z3Bounds), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, minClampedTile);
    EXPECT_EQ(5, minClampedTile->getDirectCompositeSourceZoom());

    imagery.minZoomValue = 0;
    imagery.maxZoomValue = 3;
    RasterOverlayTileProvider maxClampedProvider(imagery, *scheme, nullptr);
    auto maxClampedTile = maxClampedProvider.mapRasterTilesToGeometryTile(projectForProvider(maxClampedProvider, z3Bounds), 1024.0, 256.0).tile;
    ASSERT_NE(nullptr, maxClampedTile);
    EXPECT_FALSE(maxClampedTile->isDirectCompositeTile());
    EXPECT_EQ((TileKey{scheme->id(), 3, 2, 3}), maxClampedTile->getTileID());
}

TEST(RasterOverlaySourcePlanTest, BuildsBackendNeutralPlanForExactAndComposedCoverage) {
    ConfigurableImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);

    const RasterOverlaySourcePlan exact = buildRasterOverlaySourcePlan(
        *scheme,
        imagery,
        sourceBounds,
        sourceBounds,
        256.0,
        512.0,
        1.0,
        2048,
        3,
        3);
    ASSERT_EQ(3, exact.sourceZoom);
    ASSERT_EQ(1u, exact.sourceKeys.size());
    EXPECT_EQ(sourceKey, exact.sourceKeys.front());
    EXPECT_TRUE(exact.exactSingleSource);

    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    const RasterOverlaySourcePlan composed = buildRasterOverlaySourcePlan(
        *scheme,
        imagery,
        westHalf,
        westHalf,
        256.0,
        512.0,
        1.0,
        2048,
        0,
        18);
    EXPECT_FALSE(composed.empty());
    EXPECT_FALSE(composed.exactSingleSource);
    EXPECT_EQ(4, composed.sourceZoom);
    EXPECT_GT(composed.sourceKeys.size(), 1u);
}

TEST(RasterOverlaySourcePlanTest, PreservesAntimeridianSplitAndTextureLimitDowngrade) {
    ConfigurableImageryProvider imagery;
    imagery.minZoomValue = 0;
    imagery.maxZoomValue = 8;
    auto scheme = TileScheme::createXYZWebMercator();
    const Rectangle wrapped = Rectangle::fromDegrees(170.0, -10.0, -170.0, 10.0);

    RasterOverlaySourceCoverage coverage =
        enumerateRasterOverlaySourceCoverage(*scheme, wrapped, 3);
    ASSERT_EQ(2u, coverage.ranges.size());

    const int unrestrictedZoom = chooseRasterOverlaySourceZoom(
        *scheme,
        imagery,
        wrapped,
        wrapped,
        4096.0,
        4096.0,
        1.0,
        1 << 20,
        0,
        8);
    const int constrainedZoom = chooseRasterOverlaySourceZoom(
        *scheme,
        imagery,
        wrapped,
        wrapped,
        4096.0,
        4096.0,
        1.0,
        256,
        0,
        8);
    EXPECT_GE(unrestrictedZoom, constrainedZoom);
}

// GCJ-02 只在源网格是严格 EPSG:3857 时接管。这条钉的是**拒绝要可见**:
// OpenGlobus-Earth 的 crsProfile 是 "EPSG:3857+polar-lonlat",配了 GCJ 也会掉回
// 非 GCJ 投影,而屏幕上「没配」和「配了被吃掉」都是中国境内偏 ~500m,分不开。
// getProjection() 是唯一的生效判据(EnvSnap boot 的 proj= 字段就读它),所以它
// 必须真的报出降级后的值,而不是回显请求值。
TEST(RasterOverlayLifecycleTest,
     Gcj02WebMercatorRejectedOnNonPlainWebMercatorScheme) {
    ConfigurableImageryProvider imagery;
    auto openGlobus = TileScheme::createOpenGlobusEarth();
    ASSERT_NE("EPSG:3857", openGlobus->crsProfile());
    RasterOverlayTileProvider rejected(
        imagery,
        *openGlobus,
        nullptr,
        RasterOverlayGeoreference::Gcj02WebMercator);
    EXPECT_NE(
        RasterOverlayProjection::Gcj02WebMercator,
        rejected.getProjection());

    // 同一 georeference 在严格 EPSG:3857 上必须接管 —— 否则上面的 EXPECT_NE
    // 会在「GCJ 整体失效」时也变绿,守卫自己就成了同义反复。
    auto xyz = TileScheme::createXYZWebMercator();
    ASSERT_EQ("EPSG:3857", xyz->crsProfile());
    RasterOverlayTileProvider accepted(
        imagery,
        *xyz,
        nullptr,
        RasterOverlayGeoreference::Gcj02WebMercator);
    EXPECT_EQ(
        RasterOverlayProjection::Gcj02WebMercator,
        accepted.getProjection());
}

TEST(RasterOverlayLifecycleTest,
     Gcj02WebMercatorPlansSourceTilesInGcjCoordinates) {
    ConfigurableImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(
        imagery,
        *scheme,
        nullptr,
        RasterOverlayGeoreference::Gcj02WebMercator);
    ASSERT_EQ(
        RasterOverlayProjection::Gcj02WebMercator,
        provider.getProjection());

    const Rectangle worldBounds =
        Rectangle::fromDegrees(106.5065, 29.6155, 106.5095, 29.6185);
    const Rectangle projectedBounds =
        projectWorldRectangleForRasterOverlay(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator);
    auto mapping = provider.mapRasterTilesToGeometryTile(
        projectedBounds,
        1024.0,
        1024.0);

    ASSERT_NE(nullptr, mapping.tile);
    ASSERT_FALSE(mapping.sourceTiles.empty());
    EXPECT_GE(mapping.sourceTiles.sourceZoom, 16);
    const Cartographic worldCenter =
        Cartographic::fromDegrees(106.508, 29.617);
    const Cartographic sourceCenter =
        Gcj02CoordinateTransform::fromWgs84(worldCenter);
    const TileKey expectedSourceKey = scheme->positionToTile(
        sourceCenter.longitude(),
        sourceCenter.latitude(),
        mapping.sourceTiles.sourceZoom);
    const TileKey unshiftedKey = scheme->positionToTile(
        worldCenter.longitude(),
        worldCenter.latitude(),
        mapping.sourceTiles.sourceZoom);

    EXPECT_NE(expectedSourceKey, unshiftedKey);
    EXPECT_NE(
        mapping.sourceTiles.sourceKeys.end(),
        std::find(
            mapping.sourceTiles.sourceKeys.begin(),
            mapping.sourceTiles.sourceKeys.end(),
            expectedSourceKey));
}

TEST(RasterOverlayLifecycleTest,
     Gcj02WebMercatorKeepsOutsideChinaExactTileDirect) {
    ConfigurableImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(
        imagery,
        *scheme,
        nullptr,
        RasterOverlayGeoreference::Gcj02WebMercator);
    const TileKey key{scheme->id(), 18, 76012, 99201};
    const Rectangle worldBounds = scheme->tileToRectangle(key);

    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectWorldRectangleForRasterOverlay(
                worldBounds,
                RasterOverlayProjection::Gcj02WebMercator),
            512.0,
            512.0);

    ASSERT_NE(nullptr, mapping.tile);
    EXPECT_TRUE(mapping.directTile);
    EXPECT_FALSE(mapping.tile->isDirectCompositeTile());
    EXPECT_EQ(key, mapping.tile->getTileID());
    ASSERT_EQ(1u, mapping.sourceTiles.sourceKeys.size());
    EXPECT_EQ(key, mapping.sourceTiles.sourceKeys.front());
    EXPECT_TRUE(
        worldBounds.equalsEpsilon(
            mapping.sourceTiles.sourceBounds,
            1e-14));
}

TEST(RasterOverlayLifecycleTest,
     Gcj02WebMercatorKeepsAntimeridianSourcePlanWrapped) {
    ConfigurableImageryProvider imagery;
    imagery.minZoomValue = 3;
    imagery.maxZoomValue = 3;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(
        imagery,
        *scheme,
        nullptr,
        RasterOverlayGeoreference::Gcj02WebMercator);
    const Rectangle worldBounds =
        Rectangle::fromDegrees(170.0, -10.0, -170.0, 10.0);

    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectWorldRectangleForRasterOverlay(
                worldBounds,
                RasterOverlayProjection::Gcj02WebMercator),
            256.0,
            256.0);

    ASSERT_NE(nullptr, mapping.tile);
    EXPECT_TRUE(mapping.sourceTiles.sourceBounds.crossesAntimeridian());
    ASSERT_EQ(4u, mapping.sourceTiles.sourceKeys.size());
    for (const TileKey& sourceKey : mapping.sourceTiles.sourceKeys) {
        EXPECT_EQ(3, sourceKey.z);
        EXPECT_TRUE(sourceKey.x == 0 || sourceKey.x == 7);
        EXPECT_TRUE(sourceKey.y == 3 || sourceKey.y == 4);
    }
}

TEST(RasterOverlayLifecycleTest,
     Gcj02WebMercatorComposesAncestorFallbackInSourceCoordinates) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(
        imagery,
        *scheme,
        std::move(uploader),
        RasterOverlayGeoreference::Gcj02WebMercator);

    const Rectangle worldBounds =
        Rectangle::fromDegrees(106.30, 29.30, 106.80, 29.80);
    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectWorldRectangleForRasterOverlay(
                worldBounds,
                RasterOverlayProjection::Gcj02WebMercator),
            1024.0,
            1024.0);

    ASSERT_NE(nullptr, mapping.tile);
    ASSERT_TRUE(mapping.tile->isDirectCompositeTile());
    ASSERT_GT(mapping.sourceTiles.sourceKeys.size(), 1u);
    imagery.failingKey = mapping.sourceTiles.sourceKeys.back();
    imagery.encodeQuadrants = true;
    const int sourceZoom = mapping.sourceTiles.sourceZoom;

    ASSERT_TRUE(provider.loadTile(*mapping.tile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    ASSERT_EQ(RasterOverlayTile::LoadState::Loaded,
              mapping.tile->getState());
    ASSERT_EQ(1, uploaderPtr->uploadCount);

    const DecodedImage& image = uploaderPtr->lastUpload;
    ASSERT_GT(image.width, 0);
    ASSERT_GT(image.height, 0);
    bool hasParentLevelPixel = false;
    bool hasSourceLevelPixel = false;
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        if (image.pixels[i] == sourceZoom - 1) {
            hasParentLevelPixel = true;
            EXPECT_EQ(
                static_cast<uint8_t>(imagery.failingKey.x & 1),
                image.pixels[i + 1]);
            EXPECT_EQ(
                static_cast<uint8_t>(imagery.failingKey.y & 1),
                image.pixels[i + 2]);
        }
        hasSourceLevelPixel =
            hasSourceLevelPixel || image.pixels[i] == sourceZoom;
    }
    EXPECT_TRUE(hasParentLevelPixel);
    EXPECT_TRUE(hasSourceLevelPixel);
}

TEST(RasterOverlayLifecycleTest, QuadtreeSourceZoomRespectsOverlayLevelRange) {
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
    auto minTile = minProvider.mapRasterTilesToGeometryTile(projectForProvider(minProvider, z3Bounds), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, minTile);
    EXPECT_EQ(5, minTile->getDirectCompositeSourceZoom());

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
    auto maxTile = maxProvider.mapRasterTilesToGeometryTile(projectForProvider(maxProvider, z3Bounds), 1024.0, 256.0).tile;
    ASSERT_NE(nullptr, maxTile);
    EXPECT_FALSE(maxTile->isDirectCompositeTile());
    EXPECT_EQ((TileKey{scheme->id(), 3, 2, 3}), maxTile->getTileID());
}

TEST(RasterOverlayLifecycleTest,
     ActivatedOverlayAppliesRuntimeOptionsAndInvalidatesMappings) {
    auto imagery = std::make_unique<ConfigurableImageryProvider>();
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey firstKey{scheme->id(), 2, 1, 1};
    const TileKey secondKey{scheme->id(), 2, 3, 3};
    const Rectangle firstBounds = scheme->tileToRectangle(firstKey);
    const Rectangle secondBounds = scheme->tileToRectangle(secondKey);

    RasterOverlay::Options options;
    options.coverageRectangle = firstBounds;
    options.maximumSimultaneousTileLoads = 3;
    options.maximumScreenSpaceError = 2.0;
    options.maximumTextureSize = 2048;
    options.subTileCacheBytes = 4096;
    options.role = RasterOverlayRole::AnnotationOverlay;
    RasterOverlay overlay(
        std::move(imagery),
        TileScheme::createXYZWebMercator(),
        options);
    ActivatedRasterOverlay activated(overlay);

    RasterOverlayTileProvider* provider = activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);
    EXPECT_EQ(firstBounds, provider->getCoverageRectangle());
    EXPECT_EQ(3, provider->maximumSimultaneousTileLoads);
    EXPECT_DOUBLE_EQ(2.0, provider->getMaximumScreenSpaceError());
    EXPECT_EQ(2048, provider->getMaximumTextureSize());
    EXPECT_EQ(4096, provider->getSubTileCacheBytes());

    RasterOverlayTileProvider::RasterTileMapping firstMapping =
        provider->mapRasterTilesToGeometryTile(
            projectForProvider(*provider, firstBounds),
            1024.0,
            1024.0);
    ASSERT_NE(nullptr, firstMapping.tile);
    EXPECT_NE(nullptr, provider->getTile(firstKey));

    overlay.getOptions().coverageRectangle = secondBounds;
    overlay.getOptions().maximumSimultaneousTileLoads = 7;
    overlay.getOptions().maximumScreenSpaceError = 4.0;
    overlay.getOptions().maximumTextureSize = 64;
    overlay.getOptions().subTileCacheBytes = 0;
    overlay.getOptions().minimumZoom = 3;
    overlay.getOptions().maximumZoom = 3;

    provider = activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);
    EXPECT_EQ(secondBounds, provider->getCoverageRectangle());
    EXPECT_EQ(7, provider->maximumSimultaneousTileLoads);
    EXPECT_DOUBLE_EQ(4.0, provider->getMaximumScreenSpaceError());
    EXPECT_EQ(64, provider->getMaximumTextureSize());
    EXPECT_EQ(0, provider->getSubTileCacheBytes());
    EXPECT_EQ(3, provider->getMinimumLevel());
    EXPECT_EQ(3, provider->getMaximumLevel());

    EXPECT_EQ(nullptr, provider->getTile(firstKey));
    RasterOverlayTileProvider::RasterTileMapping outsideAnnotationMapping =
        provider->mapRasterTilesToGeometryTile(
            projectForProvider(*provider, firstBounds),
            1024.0,
            1024.0);
    ASSERT_NE(nullptr, outsideAnnotationMapping.tile);
    EXPECT_TRUE(outsideAnnotationMapping.tile->isDirectCompositeTile());
    EXPECT_FALSE(outsideAnnotationMapping.directTile);
    EXPECT_FALSE(outsideAnnotationMapping.sourceTiles.empty());
    EXPECT_EQ(nullptr, provider->getTile(firstKey));

    RasterOverlayTileProvider::RasterTileMapping secondMapping =
        provider->mapRasterTilesToGeometryTile(
            projectForProvider(*provider, secondBounds),
            1024.0,
            1024.0);
    ASSERT_NE(nullptr, secondMapping.tile);
    EXPECT_EQ(3, secondMapping.tile->isDirectCompositeTile()
                     ? secondMapping.tile->getDirectCompositeSourceZoom()
                     : secondMapping.tile->getTileID().z);
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

TEST(RasterOverlayLifecycleTest,
     CoverageChangeEvictsUncoveredDirectTilesLikeCesiumNativeDepotScope) {
    ConfigurableImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey firstKey{scheme->id(), 2, 1, 1};
    const TileKey secondKey{scheme->id(), 2, 3, 3};
    provider.setCoverageRectangle(scheme->tileToRectangle(firstKey));

    auto firstTile = provider.getTile(firstKey);
    ASSERT_NE(nullptr, firstTile);
    EXPECT_EQ(1, provider.getCachedTileCount());

    provider.setCoverageRectangle(scheme->tileToRectangle(secondKey));

    EXPECT_EQ(nullptr, provider.getTile(firstKey));
    EXPECT_EQ(0, provider.getCachedTileCount());

    auto secondTile = provider.getTile(secondKey);
    ASSERT_NE(nullptr, secondTile);
    EXPECT_EQ(secondKey, secondTile->getTileID());
    EXPECT_EQ(1, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest,
     CoverageChangePrunesUncoveredSharedSourceAssetsLikeCesiumNativeDepotScope) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey firstKey{scheme->id(), 2, 1, 1};
    const TileKey secondKey{scheme->id(), 2, 3, 3};
    provider.setCoverageRectangle(scheme->tileToRectangle(firstKey));

    auto firstTile = provider.getTile(firstKey);
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_GT(provider.getCachedSourceTileBytes(), 0);

    provider.setCoverageRectangle(scheme->tileToRectangle(secondKey));

    EXPECT_EQ(0, provider.getCachedSourceTileBytes());
    EXPECT_EQ(nullptr, provider.getTile(firstKey));

    auto secondTile = provider.getTile(secondKey);
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(provider.loadTile(*secondTile));
    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_GT(provider.getCachedSourceTileBytes(), 0);
    EXPECT_EQ(2, imagery.requestCount);
}

TEST(RasterOverlayLifecycleTest,
     CoverageChangeDiscardsStaleInFlightSourceBeforeUploadLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey firstKey{scheme->id(), 2, 1, 1};
    const TileKey secondKey{scheme->id(), 2, 3, 3};
    provider.setCoverageRectangle(scheme->tileToRectangle(firstKey));

    auto firstTile = provider.getTile(firstKey);
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_EQ(1u, imagery.pending.size());

    provider.setCoverageRectangle(scheme->tileToRectangle(secondKey));
    imagery.completeNext();
    EXPECT_EQ(0, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(0, provider.getCachedSourceTileBytes());
    EXPECT_FALSE(provider.hasPendingWork());
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, firstTile->getState());
    EXPECT_EQ(nullptr, provider.getTile(firstKey));

    auto secondTile = provider.getTile(secondKey);
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(provider.loadTile(*secondTile));
    EXPECT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(secondKey, imagery.requestedKeys.back());
}

TEST(RasterOverlayLifecycleTest,
     CoverageChangeDropsStaleDirectPendingUploadImmediately) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey firstKey{scheme->id(), 2, 1, 1};
    const TileKey secondKey{scheme->id(), 2, 3, 3};
    provider.setCoverageRectangle(scheme->tileToRectangle(firstKey));

    auto firstTile = provider.getTile(firstKey);
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_EQ(1, provider.getPendingUploadCount());

    provider.setCoverageRectangle(scheme->tileToRectangle(secondKey));

    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(0, provider.getCachedTileCount());
    provider.processPendingUploads(false);
    EXPECT_EQ(0, uploaderPtr->uploadCount);
}

TEST(RasterOverlayLifecycleTest,
     DestroyedProviderIgnoresLateRasterFailureLikeCesiumNativeDepotLifetime) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey key{scheme->id(), 0, 0, 0};
    std::shared_ptr<RasterOverlayTile> tile;

    {
        RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
        tile = provider.getTile(key);
        ASSERT_NE(nullptr, tile);
        ASSERT_TRUE(provider.loadTile(*tile));
        ASSERT_EQ(1u, imagery.pending.size());
        EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
                  tile->getState());
    }

    imagery.failNext();

    EXPECT_EQ(RasterOverlayTile::LoadState::Loading, tile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Unknown,
              tile->isMoreDetailAvailable());
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
        provider.mapRasterTilesToGeometryTile(projectForProvider(provider, outside), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, outsideTile);
    EXPECT_TRUE(outsideTile->isDirectCompositeTile());
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
    auto tile = overlappingProvider.mapRasterTilesToGeometryTile(
        projectForProvider(overlappingProvider, overlapping),
        512.0,
        512.0).tile;
    ASSERT_NE(nullptr, tile);
    EXPECT_TRUE(tile->isDirectCompositeTile());
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
        provider.mapRasterTilesToGeometryTile(projectForProvider(provider, rootBounds),
                         1024.0,
                         1024.0).tile;
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(tile->isDirectCompositeTile());
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
        provider.mapRasterTilesToGeometryTile(projectForProvider(provider, outsideCoverage), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, tile);
    EXPECT_TRUE(tile->isDirectCompositeTile());
    EXPECT_TRUE(provider.loadTile(*tile));
    ASSERT_FALSE(imagery.requestedKeys.empty());
    for (const TileKey& requested : imagery.requestedKeys) {
        EXPECT_TRUE(scheme->tileToRectangle(requested).intersects(coverage));
    }
}

TEST(RasterOverlayLifecycleTest,
     NonBaseOverlayCoverageMissClampsLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle coverage = scheme->tileToRectangle(
        TileKey{scheme->id(), 3, 2, 3});
    const Rectangle outsideCoverage = scheme->tileToRectangle(
        TileKey{scheme->id(), 3, 2, 5});
    RasterOverlay::Options options;
    options.coverageRectangle = coverage;
    options.role = RasterOverlayRole::AnnotationOverlay;
    options.fallbackPolicy = RasterOverlayFallbackPolicy::SkipUntilReady;
    options.blocksCompleteRenderable = false;
    RasterOverlay overlay(
        std::make_unique<NullImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    provider.setOwner(&overlay);

    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, outsideCoverage),
            512.0,
            512.0);
    ASSERT_NE(nullptr, mapping.tile);
    EXPECT_TRUE(mapping.tile->isDirectCompositeTile());
    EXPECT_FALSE(mapping.directTile);
    EXPECT_FALSE(mapping.sourceTiles.empty());
    EXPECT_EQ(1, provider.getCachedTileCount());

    ASSERT_TRUE(provider.loadTileThrottled(*mapping.tile, nullptr));
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
    auto rectangleMappedTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, sourceBounds), 8.0, 8.0).tile;

    ASSERT_NE(nullptr, rectangleMappedTile);
    EXPECT_FALSE(rectangleMappedTile->isDirectCompositeTile());
    EXPECT_EQ(sourceKey, rectangleMappedTile->getTileID());
    EXPECT_EQ(projectForProvider(provider, sourceBounds),
              rectangleMappedTile->getRectangle());
    EXPECT_EQ(1, provider.getCachedTileCount());

    ASSERT_TRUE(provider.loadTile(*rectangleMappedTile));
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(4, uploaderPtr->lastUpload.width);
    EXPECT_EQ(4, uploaderPtr->lastUpload.height);
    EXPECT_EQ(3, uploaderPtr->lastUpload.channels);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              rectangleMappedTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     DirectRasterTileCarriesProviderAttributionAsTileCredit) {
    ImmediateImageryProvider imagery;
    imagery.attributionValue = "Imagery credit";
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.maximumSimultaneousTileLoads = 1;

    const TileKey key{scheme->id(), 2, 1, 1};
    auto tile = provider.getTile(key);
    ASSERT_NE(nullptr, tile);

    ASSERT_TRUE(provider.loadTile(*tile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
    ASSERT_EQ(1u, tile->credits().size());
    EXPECT_EQ("Imagery credit", tile->credits().front());
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeTileMergesSourceAttributionCreditsLikeCesiumNative) {
    ImmediateImageryProvider imagery;
    imagery.attributionValue = "Imagery credit";
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

    const Rectangle westSourceBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 3, 2, 3});
    const Rectangle eastSourceBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 3, 3, 3});
    const Rectangle geometryBounds(
        westSourceBounds.west(),
        westSourceBounds.south(),
        eastSourceBounds.east(),
        westSourceBounds.north());
    auto mapping = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, geometryBounds),
        4.0,
        2.0);
    ASSERT_NE(nullptr, mapping.tile);
    ASSERT_TRUE(mapping.tile->isDirectCompositeTile());
    EXPECT_FALSE(mapping.directTile);
    ASSERT_EQ(2u, mapping.sourceTiles.sourceKeys.size());

    ASSERT_TRUE(provider.loadTile(*mapping.tile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              mapping.tile->getState());
    ASSERT_EQ(mapping.sourceTiles.sourceKeys.size(),
              mapping.tile->credits().size());
    EXPECT_TRUE(std::all_of(
        mapping.tile->credits().begin(),
        mapping.tile->credits().end(),
        [](const std::string& credit) {
            return credit == "Imagery credit";
        }));
}

TEST(RasterOverlayLifecycleTest, WebMercatorBoundarySlopUsesProjectedGeometrySpanLikeCesiumNative) {
    RecordingImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey sourceKey{scheme->id(), 3, 2, 1};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle projectedBounds =
        projectForProvider(provider, sourceBounds);
    const double projectedSlop = projectedBounds.height() / 512.0;
    const Rectangle barelyCrossingSouthEdge(
        projectedBounds.west(),
        projectedBounds.south() - projectedSlop * 0.75,
        projectedBounds.east(),
        projectedBounds.north());

    auto mappedTile = provider
                          .mapRasterTilesToGeometryTile(
                              barelyCrossingSouthEdge,
                              512.0,
                              512.0)
                          .tile;

    ASSERT_NE(nullptr, mappedTile);
    ASSERT_TRUE(mappedTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*mappedTile));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
}

TEST(RasterOverlayLifecycleTest, DirectCompositeProviderLoadStoresComposedRectangleLikeCesiumNative) {
    RecordingImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 1};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle coveredNorthHalf(
        sourceBounds.west(),
        sourceBounds.south() + sourceBounds.height() * 0.5,
        sourceBounds.east(),
        sourceBounds.north());

    RasterOverlay::Options options;
    options.coverageRectangle = coveredNorthHalf;
    RasterOverlay overlay(
        std::make_unique<NullImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    provider.setOwner(&overlay);

    auto directCompositeTile = provider
                             .mapRasterTilesToGeometryTile(
                                 projectForProvider(provider, sourceBounds),
                                 512.0,
                                 512.0)
                             .tile;

    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*directCompositeTile));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        sourceKey,
        sourceBounds,
        makeImage(256, 256, 80),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Yes});
    auto expected = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        coveredNorthHalf,
        std::move(sources));
    ASSERT_NE(nullptr, expected.image);
    EXPECT_TRUE(directCompositeTile->getRectangle().equalsEpsilon(
        projectForProvider(provider, expected.rectangle),
        1e-7));
}

TEST(
    RasterOverlayLifecycleTest,
    DirectCompositeClippedRectangleAttachComputesUvWindowLikeCesiumNative) {
    RecordingImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 1};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle coveredNorthHalf(
        sourceBounds.west(),
        sourceBounds.south() + sourceBounds.height() * 0.5,
        sourceBounds.east(),
        sourceBounds.north());

    RasterOverlay::Options options;
    options.coverageRectangle = coveredNorthHalf;
    RasterOverlay overlay(
        std::make_unique<NullImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    provider.setOwner(&overlay);

    RasterOverlayDetails details = makeProviderDetails(*scheme, sourceBounds);
    DirectRasterMapping mapped;
    std::vector<RasterOverlayProjection> missing;
    const DirectRasterMapping::MoreDetail initial =
        mapped.update(
            sourceKey,
            details,
            512.0,
            512.0,
            provider,
            nullptr,
            missing,
            nullptr,
            0,
            true);
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, initial);
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    ASSERT_TRUE(mapped.getLoadingTile()->isDirectCompositeTile());

    ASSERT_TRUE(provider.loadTile(*mapped.getLoadingTile()));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    ASSERT_EQ(RasterOverlayTile::LoadState::Loaded,
              mapped.getLoadingTile()->getState());

    const Rectangle composedRectangle = mapped.getLoadingTile()->getRectangle();
    RecordingPrepareRendererResources recorder;
    const DirectRasterMapping::MoreDetail attached =
        mapped.update(
            sourceKey,
            details,
            512.0,
            512.0,
            provider,
            &recorder,
            missing,
            nullptr,
            0,
            true);

    EXPECT_EQ(DirectRasterMapping::MoreDetail::Yes, attached);
    ASSERT_EQ(1, recorder.attachCount);
    ASSERT_NE(nullptr, recorder.lastRasterTile);
    EXPECT_TRUE(recorder.lastRasterTile->getRectangle().equalsEpsilon(
        composedRectangle,
        1e-7));

    const TileTextureWindow nativeWindow =
        TileSurface::computeTranslationAndScale(
            details.rasterOverlayRectangles.front(),
            composedRectangle);
    const TileTextureWindow expectedWindow =
        TileSurface::textureWindowForNorthWestUv(nativeWindow);
    EXPECT_NEAR(expectedWindow.offsetU,
                recorder.lastTranslationU,
                1e-6f);
    EXPECT_NEAR(expectedWindow.offsetV,
                recorder.lastTranslationV,
                1e-6f);
    EXPECT_NEAR(expectedWindow.scaleU, recorder.lastScaleU, 1e-6f);
    EXPECT_NEAR(expectedWindow.scaleV, recorder.lastScaleV, 1e-6f);
    EXPECT_NEAR(expectedWindow.offsetU, mapped.getTranslationU(), 1e-6f);
    EXPECT_NEAR(expectedWindow.offsetV, mapped.getTranslationV(), 1e-6f);
    EXPECT_NEAR(expectedWindow.scaleU, mapped.getScaleU(), 1e-6f);
    EXPECT_NEAR(expectedWindow.scaleV, mapped.getScaleV(), 1e-6f);
}

TEST(
    RasterOverlayLifecycleTest,
    DirectCompositeClippedRectangleAttachHonorsInvertedVOverlayDetails) {
    RecordingImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 1};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle coveredNorthHalf(
        sourceBounds.west(),
        sourceBounds.south() + sourceBounds.height() * 0.5,
        sourceBounds.east(),
        sourceBounds.north());

    RasterOverlay::Options options;
    options.coverageRectangle = coveredNorthHalf;
    RasterOverlay overlay(
        std::make_unique<NullImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    provider.setOwner(&overlay);

    RasterOverlayDetails details = makeProviderDetails(*scheme, sourceBounds);
    details.rasterOverlayInvertedVCoordinates = {true};
    DirectRasterMapping mapped;
    std::vector<RasterOverlayProjection> missing;
    ASSERT_EQ(
        DirectRasterMapping::MoreDetail::Unknown,
        mapped.update(
            sourceKey,
            details,
            512.0,
            512.0,
            provider,
            nullptr,
            missing,
            nullptr,
            0,
            true));
    ASSERT_NE(nullptr, mapped.getLoadingTile());

    ASSERT_TRUE(provider.loadTile(*mapped.getLoadingTile()));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    ASSERT_EQ(RasterOverlayTile::LoadState::Loaded,
              mapped.getLoadingTile()->getState());

    const Rectangle composedRectangle = mapped.getLoadingTile()->getRectangle();
    RecordingPrepareRendererResources recorder;
    EXPECT_EQ(
        DirectRasterMapping::MoreDetail::Yes,
        mapped.update(
            sourceKey,
            details,
            512.0,
            512.0,
            provider,
            &recorder,
            missing,
            nullptr,
            0,
            true));

    ASSERT_EQ(1, recorder.attachCount);
    const TileTextureWindow expectedWindow =
        TileSurface::computeTranslationAndScale(
            details.rasterOverlayRectangles.front(),
            composedRectangle);
    EXPECT_NEAR(expectedWindow.offsetU,
                recorder.lastTranslationU,
                1e-6f);
    EXPECT_NEAR(expectedWindow.offsetV,
                recorder.lastTranslationV,
                1e-6f);
    EXPECT_NEAR(expectedWindow.scaleU, recorder.lastScaleU, 1e-6f);
    EXPECT_NEAR(expectedWindow.scaleV, recorder.lastScaleV, 1e-6f);
    EXPECT_NEAR(expectedWindow.offsetU, mapped.getTranslationU(), 1e-6f);
    EXPECT_NEAR(expectedWindow.offsetV, mapped.getTranslationV(), 1e-6f);
    EXPECT_NEAR(expectedWindow.scaleU, mapped.getScaleU(), 1e-6f);
    EXPECT_NEAR(expectedWindow.scaleV, mapped.getScaleV(), 1e-6f);
}

TEST(RasterOverlayLifecycleTest, LargeAreaUsesRootTileLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey rootKey{scheme->id(), 0, 0, 0};
    const Rectangle worldBounds = scheme->tileToRectangle(rootKey);
    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, worldBounds),
            256.0,
            256.0);

    ASSERT_NE(nullptr, mapping.tile);
    EXPECT_TRUE(mapping.directTile);
    EXPECT_FALSE(mapping.tile->isDirectCompositeTile());
    EXPECT_EQ(rootKey, mapping.tile->getTileID());

    ASSERT_TRUE(provider.loadTile(*mapping.tile));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(rootKey, imagery.requestedKeys.front());
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              mapping.tile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Yes,
              mapping.tile->isMoreDetailAvailable());
    ASSERT_FALSE(uploaderPtr->lastUpload.pixels.empty());
    EXPECT_TRUE(std::all_of(
        uploaderPtr->lastUpload.pixels.begin(),
        uploaderPtr->lastUpload.pixels.end(),
        [](uint8_t value) { return value == 0 || value == 255; }));
}

TEST(RasterOverlayLifecycleTest, DirectExactSourceUploadUsesSharedSourceAssetLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    auto tile = provider.getTile(sourceKey);

    ASSERT_NE(nullptr, tile);
    EXPECT_FALSE(tile->isDirectCompositeTile());

    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));
    imagery.completeNext();

    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(imagery.lastCompletedImage, uploaderPtr->lastUploadSource);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
}

TEST(RasterOverlayLifecycleTest, DirectTileUsesParentSubsetFallbackLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey childKey{scheme->id(), 3, 2, 3};
    const TileKey parentKey{scheme->id(), 2, 1, 1};
    imagery.failingKey = childKey;

    auto tile = provider.getTile(childKey);
    ASSERT_NE(nullptr, tile);

    ASSERT_TRUE(provider.loadTile(*tile));
    int processedUploads =
        provider.processPendingUploads(false).processedUploads;
    ASSERT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(childKey, imagery.requestedKeys[0]);
    EXPECT_EQ(parentKey, imagery.requestedKeys[1]);

    if (processedUploads == 0) {
        processedUploads += processPendingUploadsUntil(provider, 1);
    }
    ASSERT_EQ(1, processedUploads);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
    EXPECT_EQ(nullptr, tile->getTexture());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              tile->isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, RectangleMappingReportsDirectOrComposedPath) {
    RgbImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    RasterOverlayTileProvider::RasterTileMapping direct =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, sourceBounds),
            8.0,
            8.0);

    ASSERT_NE(nullptr, direct.tile);
    EXPECT_TRUE(direct.directTile);
    EXPECT_FALSE(direct.tile->isDirectCompositeTile());
    EXPECT_EQ(sourceKey, direct.tile->getTileID());
    EXPECT_EQ(sourceKey.z, direct.sourceTiles.sourceZoom);
    ASSERT_EQ(1u, direct.sourceTiles.sourceKeys.size());
    EXPECT_EQ(sourceKey, direct.sourceTiles.sourceKeys.front());
    EXPECT_EQ(sourceBounds, direct.sourceTiles.sourceBounds);
    EXPECT_EQ(sourceKey.x, direct.sourceTiles.minX);
    EXPECT_EQ(sourceKey.y, direct.sourceTiles.minY);
    EXPECT_EQ(sourceKey.x, direct.sourceTiles.maxX);
    EXPECT_EQ(sourceKey.y, direct.sourceTiles.maxY);

    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    RasterOverlayTileProvider::RasterTileMapping composed =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            256.0,
            512.0);

    ASSERT_NE(nullptr, composed.tile);
    EXPECT_FALSE(composed.directTile);
    EXPECT_TRUE(composed.tile->isDirectCompositeTile());
    EXPECT_EQ(composed.tile->getDirectCompositeSourceZoom(),
              composed.sourceTiles.sourceZoom);
    EXPECT_FALSE(composed.sourceTiles.empty());
    EXPECT_TRUE(composed.sourceTiles.sourceBounds.equalsEpsilon(
        westHalf,
        1e-12));
    EXPECT_LE(composed.sourceTiles.minX, composed.sourceTiles.maxX);
    EXPECT_LE(composed.sourceTiles.minY, composed.sourceTiles.maxY);
}

TEST(RasterOverlayLifecycleTest,
     MappingExposesMultipleImagerySourceTilesLikeCesiumNative) {
    RgbImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(8, 8);

    const int expectedLevel = 8;
    const TileKey centerKey =
        scheme->positionToTile(0.1, 0.2, expectedLevel);
    const Rectangle centerBounds = scheme->tileToRectangle(centerKey);
    const double cornerLng = centerBounds.west();
    const double cornerLat = centerBounds.south();
    const Rectangle spanningFourTiles(
        cornerLng - centerBounds.width() * 0.5,
        cornerLat - centerBounds.height() * 0.5,
        cornerLng + centerBounds.width() * 0.5,
        cornerLat + centerBounds.height() * 0.5);

    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, spanningFourTiles),
            static_cast<double>(imagery.tileWidth() * 4),
            static_cast<double>(imagery.tileHeight() * 4));

    ASSERT_NE(nullptr, mapping.tile);
    EXPECT_FALSE(mapping.directTile);
    EXPECT_TRUE(mapping.tile->isDirectCompositeTile());
    EXPECT_EQ(expectedLevel, mapping.sourceTiles.sourceZoom);
    EXPECT_TRUE(mapping.sourceTiles.sourceBounds.equalsEpsilon(
        spanningFourTiles,
        1e-12));
    EXPECT_EQ(centerKey.x - 1, mapping.sourceTiles.minX);
    EXPECT_EQ(centerKey.y, mapping.sourceTiles.minY);
    EXPECT_EQ(centerKey.x, mapping.sourceTiles.maxX);
    EXPECT_EQ(centerKey.y + 1, mapping.sourceTiles.maxY);
    ASSERT_EQ(4u, mapping.sourceTiles.sourceKeys.size());

    const std::vector<TileKey> expectedKeys = {
        TileKey{scheme->id(), expectedLevel, centerKey.x - 1, centerKey.y},
        TileKey{scheme->id(), expectedLevel, centerKey.x - 1, centerKey.y + 1},
        TileKey{scheme->id(), expectedLevel, centerKey.x, centerKey.y},
        TileKey{scheme->id(), expectedLevel, centerKey.x, centerKey.y + 1},
    };
    EXPECT_EQ(expectedKeys, mapping.sourceTiles.sourceKeys);
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeCacheKeyIncludesExplicitSourceTilesLikeCesiumNativeDepot) {
    RgbImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(8, 8);

    const int expectedLevel = 8;
    const TileKey centerKey =
        scheme->positionToTile(0.1, 0.2, expectedLevel);
    const Rectangle centerBounds = scheme->tileToRectangle(centerKey);
    const Rectangle spanningFourTiles(
        centerBounds.west() - centerBounds.width() * 0.5,
        centerBounds.south() - centerBounds.height() * 0.5,
        centerBounds.east() + centerBounds.width() * 0.5,
        centerBounds.north() + centerBounds.height() * 0.5);

    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, spanningFourTiles),
            static_cast<double>(imagery.tileWidth() * 4),
            static_cast<double>(imagery.tileHeight() * 4));

    ASSERT_NE(nullptr, mapping.tile);
    ASSERT_TRUE(mapping.tile->isDirectCompositeTile());
    const std::string& cacheKey = mapping.tile->getCacheKey();
    EXPECT_NE(std::string::npos, cacheKey.find("/range/"));
    EXPECT_NE(std::string::npos, cacheKey.find("/tiles/"));
    for (const TileKey& sourceKey : mapping.sourceTiles.sourceKeys) {
        const std::string sourceToken =
            "/" + std::to_string(sourceKey.z) + "/" +
            std::to_string(sourceKey.x) + "/" +
            std::to_string(sourceKey.y);
        EXPECT_NE(std::string::npos, cacheKey.find(sourceToken));
    }
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeUsesMixedLevelsWhenSourceTileFailsLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(0, 8);

    const int expectedLevel = 8;
    const TileKey centerKey =
        scheme->positionToTile(0.1, 0.2, expectedLevel);
    const Rectangle centerBounds = scheme->tileToRectangle(centerKey);
    const Rectangle spanningFourTiles(
        centerBounds.west() - centerBounds.width() * 0.5,
        centerBounds.south() - centerBounds.height() * 0.5,
        centerBounds.west() + centerBounds.width() * 0.5,
        centerBounds.south() + centerBounds.height() * 0.5);

    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, spanningFourTiles),
            static_cast<double>(imagery.tileWidth() * 4),
            static_cast<double>(imagery.tileHeight() * 4));

    ASSERT_NE(nullptr, mapping.tile);
    ASSERT_TRUE(mapping.tile->isDirectCompositeTile());
    ASSERT_EQ(expectedLevel, mapping.sourceTiles.sourceZoom);
    ASSERT_EQ(4u, mapping.sourceTiles.sourceKeys.size());

    imagery.failingKey = mapping.sourceTiles.sourceKeys.back();

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    ASSERT_TRUE(provider.loadTileThrottled(*mapping.tile, &budget));
    EXPECT_EQ(4u, budget.rasterNetworkRequestsIssued());

    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    ASSERT_EQ(RasterOverlayTile::LoadState::Loaded,
              mapping.tile->getState());
    ASSERT_EQ(1, uploaderPtr->uploadCount);
    ASSERT_FALSE(uploaderPtr->lastUpload.pixels.empty());

    const DecodedImage& image = uploaderPtr->lastUpload;
    ASSERT_EQ(4, image.channels);
    bool hasParentLevelPixel = false;
    bool hasSourceLevelPixel = false;
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        for (int channel = 0; channel < 3; ++channel) {
            const uint8_t value = image.pixels[i + static_cast<size_t>(channel)];
            hasParentLevelPixel = hasParentLevelPixel || value == 7;
            hasSourceLevelPixel = hasSourceLevelPixel || value == 8;
        }
    }
    EXPECT_TRUE(hasParentLevelPixel);
    EXPECT_TRUE(hasSourceLevelPixel);
}

TEST(
    RasterOverlayLifecycleTest,
    DirectCompositeTileCarriesDirectCompositeSourceListForLoadLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(5, 5);

    const TileKey sourceKey{scheme->id(), 5, 10, 12};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());

    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            256.0,
            512.0);

    ASSERT_NE(nullptr, mapping.tile);
    ASSERT_TRUE(mapping.tile->isDirectCompositeTile());
    ASSERT_FALSE(mapping.sourceTiles.empty());
    const std::vector<TileKey> expectedSourceKeys =
        mapping.sourceTiles.sourceKeys;
    EXPECT_EQ(expectedSourceKeys, mapping.tile->getDirectCompositeSourceKeys());
    EXPECT_TRUE(mapping.tile->getDirectCompositeSourceBounds().equalsEpsilon(
        mapping.sourceTiles.sourceBounds,
        1e-12));

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*mapping.tile, &budget));
    EXPECT_EQ(expectedSourceKeys, imagery.requestedKeys);
    EXPECT_EQ(expectedSourceKeys.size(), budget.rasterNetworkRequestsIssued());
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

    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, westHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    EXPECT_TRUE(directCompositeTile->isDirectCompositeTile());
    EXPECT_EQ(sourceKey.z, directCompositeTile->getDirectCompositeSourceZoom());
}

TEST(RasterOverlayLifecycleTest, QuadtreeSourceRangeTrimsTileEdgeTouchesLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    imagery.tileWidthValue = 64;
    imagery.tileHeightValue = 64;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle sourceAlignedBounds = scheme->tileToRectangle(
        TileKey{scheme->id(), 3, 2, 3});
    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, sourceAlignedBounds), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    EXPECT_EQ(5, directCompositeTile->getDirectCompositeSourceZoom());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*directCompositeTile, &budget));
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

TEST(RasterOverlayLifecycleTest, QuadtreeSourceRangeTrimsSouthUpTileEdgeTouchesLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    imagery.schemeIdValue = "Geographic-TMS";
    imagery.tileWidthValue = 64;
    imagery.tileHeightValue = 64;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle sourceAlignedBounds = scheme->tileToRectangle(
        TileKey{scheme->id(), 3, 2, 3});
    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, sourceAlignedBounds), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    EXPECT_EQ(5, directCompositeTile->getDirectCompositeSourceZoom());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*directCompositeTile, &budget));
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

TEST(
    RasterOverlayLifecycleTest,
    RasterTextureBytesTrackReplacementAndExternalTileLifetime) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    const TileKey key{scheme->id(), 3, 4, 2};

    auto tile = provider.getTile(key);
    ASSERT_NE(nullptr, tile);
    tile->setTexture(std::make_unique<TestTexture>(4, 4, 19));
    EXPECT_EQ(19, provider.tileTextureBytesUsed());

    tile->setTexture(std::make_unique<TestTexture>(8, 8, 73));
    EXPECT_EQ(73, provider.tileTextureBytesUsed());

    tile->markLoadedWithoutTexture();
    EXPECT_EQ(0, provider.tileTextureBytesUsed());

    tile->setTexture(std::make_unique<TestTexture>(2, 4, 29));
    provider.setReady(false);
    EXPECT_EQ(0, provider.getCachedTileCount());
    EXPECT_EQ(29, provider.tileTextureBytesUsed());

    tile.reset();
    EXPECT_EQ(0, provider.tileTextureBytesUsed());
}

TEST(
    RasterOverlayLifecycleTest,
    RasterLruTouchMovesOnlyTheUsedTileToTheRecentEnd) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    const TileKey firstKey{scheme->id(), 3, 4, 2};
    const TileKey secondKey{scheme->id(), 3, 5, 2};

    provider.setFrameNumber(1);
    auto first = provider.getTile(firstKey);
    provider.setFrameNumber(2);
    auto second = provider.getTile(secondKey);
    std::weak_ptr<RasterOverlayTile> weakFirst = first;
    std::weak_ptr<RasterOverlayTile> weakSecond = second;
    provider.setFrameNumber(100);
    provider.markUsed(firstKey);
    first.reset();
    second.reset();

    provider.setFrameNumber(123);
    provider.trimUnusedTiles();

    EXPECT_EQ(1, provider.getCachedTileCount());
    EXPECT_FALSE(weakFirst.expired());
    EXPECT_TRUE(weakSecond.expired());
}

TEST(
    RasterOverlayLifecycleTest,
    DirectTileTouchDoesNotRelinkAStaleExternallyHeldTile) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    const TileKey key{scheme->id(), 3, 4, 2};

    provider.setFrameNumber(1);
    auto staleTile = provider.getTile(key);
    ASSERT_NE(nullptr, staleTile);

    provider.setReady(false);
    ASSERT_EQ(0, provider.getCachedTileCount());
    provider.setFrameNumber(2);
    provider.markUsed(*staleTile);
    EXPECT_EQ(0, provider.getCachedTileCount());

    provider.setReady(true);
    auto currentTile = provider.getTile(key);
    ASSERT_NE(nullptr, currentTile);
    EXPECT_NE(staleTile.get(), currentTile.get());
    EXPECT_EQ(1, provider.getCachedTileCount());
}

TEST(
    RasterOverlayLifecycleTest,
    CachePressureEvictsYoungUnreferencedTilesButKeepsReferencedTiles) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    const TileKey referencedKey{scheme->id(), 3, 4, 2};
    const TileKey freeKey{scheme->id(), 3, 5, 2};

    provider.setFrameNumber(1);
    auto referenced = provider.getTile(referencedKey);
    auto freeTile = provider.getTile(freeKey);
    freeTile.reset();

    provider.setFrameNumber(2);
    provider.trimUnusedTiles(true);
    EXPECT_EQ(1, provider.getCachedTileCount());

    referenced.reset();
    provider.trimUnusedTiles(true);
    EXPECT_EQ(0, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest,
     PendingRasterUploadRetainsProviderTileLikeCesiumNativeFuture) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey key{scheme->id(), 2, 1, 1};
    RasterOverlayTileProvider::TilePtr tile = provider.getTile(key);
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_GE(waitForPendingUploadCount(provider, 1), 1);
    tile.reset();

    provider.setFrameNumber(200);
    provider.trimUnusedTiles();

    EXPECT_EQ(1, provider.getCachedTileCount());
    EXPECT_EQ(1, provider.processPendingUploads(false).processedUploads);
    EXPECT_EQ(1, uploaderPtr->uploadCount);

    RasterOverlayTileProvider::TilePtr loaded = provider.getTile(key);
    ASSERT_NE(nullptr, loaded);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, loaded->getState());
    EXPECT_NE(nullptr, loaded->getTexture());
    EXPECT_EQ(0, provider.getThrottledTilesCurrentlyLoading());
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

TEST(RasterOverlayLifecycleTest, DirectCompositeTileKeepsGeometryBoundsAndTargetPixels) {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    auto geometryScheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);

    TileKey geometryKey{geometryScheme->id(), 2, 4, 2};
    Rectangle geometryBounds = geometryScheme->tileToRectangle(geometryKey);
    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, geometryBounds), 512.0, 512.0).tile;

    ASSERT_NE(nullptr, directCompositeTile);
    EXPECT_TRUE(directCompositeTile->isDirectCompositeTile());
    EXPECT_EQ(projectForProvider(provider, geometryBounds),
              directCompositeTile->getRectangle());
    EXPECT_EQ(0u, directCompositeTile->getCacheKey().find("direct-composite/"));
    EXPECT_EQ(3, directCompositeTile->getDirectCompositeSourceZoom());
    EXPECT_EQ(512.0, directCompositeTile->getTargetScreenPixelsX());
    EXPECT_EQ(512.0, directCompositeTile->getTargetScreenPixelsY());
}

TEST(RasterOverlayLifecycleTest,
     CachedDirectCompositeTileRefreshesTargetPixels) {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    auto geometryScheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);

    TileKey geometryKey{geometryScheme->id(), 2, 4, 2};
    Rectangle geometryBounds = geometryScheme->tileToRectangle(geometryKey);
    auto firstTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, geometryBounds),
        512.0,
        512.0).tile;
    auto refreshedTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, geometryBounds),
        520.0,
        516.0).tile;

    ASSERT_NE(nullptr, firstTile);
    ASSERT_EQ(firstTile, refreshedTile);
    EXPECT_EQ(520.0, refreshedTile->getTargetScreenPixelsX());
    EXPECT_EQ(516.0, refreshedTile->getTargetScreenPixelsY());
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeTileCacheInvalidatesWhenSourcePlanConfigurationChanges) {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    auto geometryScheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);

    const TileKey directKey{imageryScheme->id(), 3, 4, 2};
    const Rectangle directBounds = imageryScheme->tileToRectangle(directKey);
    auto directTile = provider
                          .mapRasterTilesToGeometryTile(
                              projectForProvider(provider, directBounds),
                              512.0,
                              512.0)
                          .tile;
    ASSERT_NE(nullptr, directTile);
    ASSERT_FALSE(directTile->isDirectCompositeTile());

    const TileKey geometryKey{geometryScheme->id(), 2, 4, 2};
    const Rectangle geometryBounds =
        geometryScheme->tileToRectangle(geometryKey);
    auto firstDirectComposite = provider
                              .mapRasterTilesToGeometryTile(
                                  projectForProvider(provider, geometryBounds),
                                  512.0,
                                  512.0)
                              .tile;

    ASSERT_NE(nullptr, firstDirectComposite);
    ASSERT_TRUE(firstDirectComposite->isDirectCompositeTile());
    const std::string firstDirectCompositeKey = firstDirectComposite->getCacheKey();
    EXPECT_EQ(2, provider.getCachedTileCount());

    provider.setMaximumScreenSpaceError(
        provider.getMaximumScreenSpaceError() * 4.0);

    EXPECT_EQ(1, provider.getCachedTileCount());
    EXPECT_EQ(directTile, provider.getTile(directKey));

    auto secondDirectComposite = provider
                               .mapRasterTilesToGeometryTile(
                                   projectForProvider(provider, geometryBounds),
                                   512.0,
                                   512.0)
                               .tile;

    ASSERT_NE(nullptr, secondDirectComposite);
    ASSERT_TRUE(secondDirectComposite->isDirectCompositeTile());
    EXPECT_NE(firstDirectComposite.get(), secondDirectComposite.get());
    EXPECT_NE(firstDirectCompositeKey, secondDirectComposite->getCacheKey());
    EXPECT_EQ(2, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest,
     DirectMappingDropsStaleProviderCacheEntryBeforeAttachedFastPath) {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    auto geometryScheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);

    const TileKey geometryKey{geometryScheme->id(), 2, 4, 2};
    const Rectangle geometryBounds =
        geometryScheme->tileToRectangle(geometryKey);
    RasterOverlayDetails details =
        makeProviderDetails(*imageryScheme, geometryBounds);
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    ASSERT_EQ(DirectRasterMapping::MoreDetail::Unknown,
              mapped.update(
                  geometryKey,
                  details,
                  512.0,
                  512.0,
                  provider,
                  nullptr,
                  missing));

    RasterOverlayTile* firstLoading = mapped.getLoadingTile();
    ASSERT_NE(nullptr, firstLoading);
    ASSERT_TRUE(firstLoading->isDirectCompositeTile());
    const std::string firstCacheKey = firstLoading->getCacheKey();
    firstLoading->setTexture(std::make_unique<TestTexture>(4, 4));

    RecordingPrepareRendererResources recorder;
    mapped.update(
        geometryKey,
        details,
        512.0,
        512.0,
        provider,
        &recorder,
        missing);
    ASSERT_EQ(DirectRasterMapping::State::Attached,
              mapped.getState());
    ASSERT_EQ(firstLoading, mapped.getReadyTile());
    ASSERT_EQ(1, recorder.attachCount);

    provider.setMaximumScreenSpaceError(
        provider.getMaximumScreenSpaceError() * 4.0);

    mapped.update(
        geometryKey,
        details,
        512.0,
        512.0,
        provider,
        &recorder,
        missing);

    RasterOverlayTile* remappedLoading = mapped.getLoadingTile();
    ASSERT_NE(nullptr, remappedLoading);
    EXPECT_TRUE(remappedLoading->isDirectCompositeTile());
    EXPECT_NE(firstLoading, remappedLoading);
    EXPECT_NE(firstCacheKey, remappedLoading->getCacheKey());
    EXPECT_EQ(nullptr, mapped.getReadyTile());
    EXPECT_EQ(DirectRasterMapping::State::Unattached,
              mapped.getState());
    EXPECT_EQ(1, recorder.detachCount);
}

TEST(RasterOverlayLifecycleTest,
     TemporaryAncestorKeepsPendingMoreDetailAfterProviderConfigChange) {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    auto geometryScheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);

    const TileKey parentKey{imageryScheme->id(), 2, 1, 1};
    const Rectangle parentBounds = imageryScheme->tileToRectangle(parentKey);
    RasterOverlayDetails parentDetails =
        makeProviderDetails(*imageryScheme, parentBounds);
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parentTile(parentKey, parentBounds);
    DirectRasterMapping& parentMapping =
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
    ASSERT_FALSE(parentReady->isDirectCompositeTile());
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
    ASSERT_TRUE(provider.ownsCurrentTile(*parentReady));

    const TileKey childGeometryKey{imageryScheme->id(), 0, 0, 0};
    const Rectangle childGeometryBounds =
        imageryScheme->tileToRectangle(childGeometryKey);
    RasterOverlayDetails childDetails =
        makeProviderDetails(*imageryScheme, childGeometryBounds);

    DirectRasterMapping childMapping;
    childMapping.update(
        childGeometryKey,
        childDetails,
        1024.0,
        1024.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0);
    RasterOverlayTile* firstLoading = childMapping.getLoadingTile();
    ASSERT_NE(nullptr, firstLoading);
    ASSERT_TRUE(firstLoading->isDirectCompositeTile());

    RecordingPrepareRendererResources recorder;
    EXPECT_EQ(
        DirectRasterMapping::MoreDetail::Unknown,
        childMapping.update(
            childGeometryKey,
            childDetails,
            1024.0,
            1024.0,
            provider,
            &recorder,
            missing,
            &parentTile,
            0));
    ASSERT_EQ(parentReady, childMapping.getReadyTile());
    ASSERT_EQ(firstLoading, childMapping.getLoadingTile());
    ASSERT_EQ(DirectRasterMapping::State::TemporarilyAttached,
              childMapping.getState());

    provider.setMaximumScreenSpaceError(
        provider.getMaximumScreenSpaceError() * 4.0);

    const DirectRasterMapping::MoreDetail afterConfigChange =
        childMapping.update(
            childGeometryKey,
            childDetails,
            1024.0,
            1024.0,
            provider,
            &recorder,
            missing,
            &parentTile,
            0);

    RasterOverlayTile* remappedLoading = childMapping.getLoadingTile();
    ASSERT_NE(nullptr, remappedLoading);
    EXPECT_TRUE(provider.ownsCurrentTile(*parentReady));
    EXPECT_EQ(parentReady, childMapping.getReadyTile());
    EXPECT_EQ(DirectRasterMapping::State::TemporarilyAttached,
              childMapping.getState());
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown,
              afterConfigChange);
    EXPECT_FALSE(childMapping.isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest,
     ExactProviderRectangleUsesDirectQuadtreeTileLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(0, 3);

    const TileKey key{scheme->id(), 3, 4, 2};
    const Rectangle bounds = scheme->tileToRectangle(key);
    auto mappedTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, bounds), 512.0, 512.0).tile;

    ASSERT_NE(nullptr, mappedTile);
    EXPECT_FALSE(mappedTile->isDirectCompositeTile());
    EXPECT_EQ(key, mappedTile->getTileID());
    EXPECT_EQ(projectForProvider(provider, bounds), mappedTile->getRectangle());
    EXPECT_EQ("XYZ-WebMercator/3/4/2", mappedTile->getCacheKey());
    EXPECT_EQ(1, provider.getCachedTileCount());

    auto directTile = provider.getTile(key);
    ASSERT_NE(nullptr, directTile);
    EXPECT_EQ(directTile, mappedTile);
    EXPECT_FALSE(directTile->isDirectCompositeTile());
}

TEST(RasterOverlayLifecycleTest,
     ExactIntermediateRectangleUsesDirectSourceTileLikeCesiumNative) {
    ConfigurableImageryProvider imagery;
    imagery.tileWidthValue = 256;
    imagery.tileHeightValue = 256;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(0, 5);

    const TileKey key{scheme->id(), 3, 4, 2};
    const Rectangle bounds = scheme->tileToRectangle(key);
    auto mappedTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, bounds), 512.0, 512.0).tile;

    ASSERT_NE(nullptr, mappedTile);
    EXPECT_FALSE(mappedTile->isDirectCompositeTile());
    EXPECT_NE(0u, mappedTile->getCacheKey().find("direct-composite/"));
    EXPECT_EQ(1, provider.getCachedTileCount());

    auto directTile = provider.getTile(mappedTile->getTileID());
    ASSERT_NE(nullptr, directTile);
    EXPECT_EQ(directTile, mappedTile);
    EXPECT_FALSE(directTile->isDirectCompositeTile());
}

TEST(RasterOverlayLifecycleTest, QuadtreeSourceZoomRespectsOverlayMaximumTextureSize) {
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
    auto defaultTile = defaultProvider.mapRasterTilesToGeometryTile(projectForProvider(defaultProvider, rootBounds), 131072.0, 131072.0).tile;
    ASSERT_NE(nullptr, defaultTile);
    EXPECT_EQ(3, defaultTile->getDirectCompositeSourceZoom());

    auto constrainedUploader = std::make_unique<CountingRasterUploader>();
    constrainedUploader->maxTextureSizeValue = 2048;
    RasterOverlayTileProvider constrainedProvider(
        imagery,
        *scheme,
        std::move(constrainedUploader));
    constrainedProvider.setMaximumTextureSize(256);
    auto constrainedTile =
        constrainedProvider.mapRasterTilesToGeometryTile(projectForProvider(constrainedProvider, rootBounds), 131072.0, 131072.0).tile;
    ASSERT_NE(nullptr, constrainedTile);
    EXPECT_FALSE(constrainedTile->isDirectCompositeTile());
    EXPECT_EQ((TileKey{scheme->id(), 0, 0, 0}), constrainedTile->getTileID());

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
    auto ownerTile = ownerProvider.mapRasterTilesToGeometryTile(projectForProvider(ownerProvider, rootBounds), 131072.0, 131072.0).tile;
    ASSERT_NE(nullptr, ownerTile);
    EXPECT_FALSE(ownerTile->isDirectCompositeTile());
    EXPECT_EQ((TileKey{scheme->id(), 0, 0, 0}), ownerTile->getTileID());
}

TEST(RasterOverlayLifecycleTest,
     DeepQuadtreeSourceZoomTextureLimitDoesNotOverflow) {
    ConfigurableImageryProvider imagery;
    imagery.maxZoomValue = 31;
    imagery.tileWidthValue = 256;
    imagery.tileHeightValue = 256;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(0, 31);
    provider.setMaximumTextureSize(256);

    const TileKey rootKey{scheme->id(), 0, 0, 0};
    const Rectangle rootBounds = scheme->tileToRectangle(rootKey);
    auto tile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, rootBounds),
        1.0e12,
        1.0e12).tile;

    ASSERT_NE(nullptr, tile);
    EXPECT_FALSE(tile->isDirectCompositeTile());
    EXPECT_EQ(rootKey, tile->getTileID());
    EXPECT_EQ(1, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest,
     ExactRootRectangleLoadsThroughDirectQuadtreeTileLikeCesiumNative) {
    RgbImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey rootKey{scheme->id(), 0, 0, 0};
    const Rectangle rootBounds = scheme->tileToRectangle(rootKey);
    auto tile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, rootBounds), 8.0, 8.0).tile;
    ASSERT_NE(nullptr, tile);
    EXPECT_FALSE(tile->isDirectCompositeTile());
    EXPECT_EQ(rootKey, tile->getTileID());
    EXPECT_EQ("XYZ-WebMercator/0/0/0", tile->getCacheKey());

    ASSERT_TRUE(provider.loadTile(*tile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(rootKey, imagery.requestedKeys.front());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
    ASSERT_FALSE(uploaderPtr->lastUpload.pixels.empty());
    EXPECT_TRUE(std::all_of(
        uploaderPtr->lastUpload.pixels.begin(),
        uploaderPtr->lastUpload.pixels.end(),
        [](uint8_t value) { return value == 0; }));
}

TEST(RasterOverlayLifecycleTest, QuadtreeSourceFailureFallsBackToParentTile) {
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

    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, tileBounds), 1024.0, 1024.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    EXPECT_EQ(expectedSourceZoom, directCompositeTile->getDirectCompositeSourceZoom());

    ASSERT_TRUE(provider.loadTile(*directCompositeTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directCompositeTile->getState());
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

TEST(RasterOverlayLifecycleTest,
     SynchronousSourceFailuresCanFallbackToRootWithoutDanglingIssueCallback) {
    AlwaysFailingImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(0, 3);

    const TileKey child{scheme->id(), 3, 5, 2};
    auto tile = provider.getTile(child);
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));

    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(child, imagery.requestedKeys.front());

    EXPECT_EQ(0, provider.processPendingUploads(false).processedUploads);
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    EXPECT_FALSE(provider.hasPendingWork());

    ASSERT_EQ(4u, imagery.requestedKeys.size());
    EXPECT_EQ((TileKey{scheme->id(), 2, 2, 1}), imagery.requestedKeys[1]);
    EXPECT_EQ((TileKey{scheme->id(), 1, 1, 0}), imagery.requestedKeys[2]);
    EXPECT_EQ((TileKey{scheme->id(), 0, 0, 0}), imagery.requestedKeys[3]);
}

TEST(RasterOverlayLifecycleTest,
     SourceTileFallbackIgnoresSupportsTileForParentLikeCesiumNative) {
    UnsupportedParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    imagery.failingChildKey = TileKey{scheme->id(), 3, 2, 3};
    imagery.unsupportedParentKey =
        TileKey{scheme->id(), 2, 1, 1};

    RasterOverlayTileProvider::TilePtr tile =
        provider.getTile(imagery.failingChildKey);
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));

    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
    EXPECT_EQ(nullptr, tile->getTexture());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              tile->isMoreDetailAvailable());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
    EXPECT_TRUE(std::find(
        imagery.requestedKeys.begin(),
        imagery.requestedKeys.end(),
        imagery.failingChildKey) != imagery.requestedKeys.end());
    EXPECT_TRUE(std::find(
        imagery.requestedKeys.begin(),
        imagery.requestedKeys.end(),
        imagery.unsupportedParentKey) != imagery.requestedKeys.end());
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

    auto westTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, westHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, westTile);
    EXPECT_EQ(sourceKey.z, westTile->getDirectCompositeSourceZoom());
    ASSERT_TRUE(provider.loadTile(*westTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    auto eastTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, eastHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, eastTile);
    EXPECT_EQ(sourceKey.z, eastTile->getDirectCompositeSourceZoom());
    ASSERT_TRUE(provider.loadTile(*eastTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, eastTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     SourceTileDepotCacheHitPreservesCreditsLikeCesiumNativeSharedAsset) {
    ImmediateImageryProvider imagery;
    imagery.attributionValue = "Imagery credit";
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

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

    RasterOverlayTileProvider::TilePtr westTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            256.0,
            512.0)
            .tile;
    ASSERT_NE(nullptr, westTile);
    ASSERT_TRUE(provider.loadTile(*westTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    ASSERT_EQ(1u, westTile->credits().size());
    EXPECT_EQ("Imagery credit", westTile->credits().front());
    EXPECT_EQ(1, imagery.requestCount);

    RasterOverlayTileProvider::TilePtr eastTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, eastHalf),
            256.0,
            512.0)
            .tile;
    ASSERT_NE(nullptr, eastTile);
    ASSERT_TRUE(provider.loadTile(*eastTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(1, imagery.requestCount);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, eastTile->getState());
    ASSERT_EQ(1u, eastTile->credits().size());
    EXPECT_EQ("Imagery credit", eastTile->credits().front());
}

TEST(RasterAssetKeyTest, IdentityIncludesProviderRevisionSchemeProjectionEpoch) {
    RasterAssetKey base;
    base.providerInstance = 7;
    base.providerContentRevision = 3;
    base.depotEpoch = 11;
    base.schemeId = "XYZ-WebMercator";
    base.projection = RasterOverlayProjection::WebMercator;
    base.sourceKey = TileKey{"XYZ-WebMercator", 4, 2, 1};

    EXPECT_EQ(base, base);
    std::unordered_set<RasterAssetKey> keys;
    keys.insert(base);
    EXPECT_EQ(keys.count(base), 1u);

    RasterAssetKey changed = base;
    changed.providerInstance++;
    EXPECT_NE(base, changed);
    changed = base;
    changed.providerContentRevision++;
    EXPECT_NE(base, changed);
    changed = base;
    changed.depotEpoch++;
    EXPECT_NE(base, changed);
    changed = base;
    changed.schemeId = "TMS-WebMercator";
    EXPECT_NE(base, changed);
    changed = base;
    changed.projection = RasterOverlayProjection::Geographic;
    EXPECT_NE(base, changed);
    changed = base;
    changed.sourceKey.x++;
    EXPECT_NE(base, changed);

    ImmediateImageryProvider firstImagery;
    ImmediateImageryProvider secondImagery;
    auto firstScheme = TileScheme::createXYZWebMercator();
    auto secondScheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider firstProvider(firstImagery, *firstScheme);
    RasterOverlayTileProvider secondProvider(secondImagery, *secondScheme);
    EXPECT_NE(
        firstProvider.rasterAssetKey(base.sourceKey),
        secondProvider.rasterAssetKey(base.sourceKey))
        << "equal provider id must not alias distinct provider instances";
}

TEST(RasterOverlayLifecycleTest, ExactCachedSourceFacadeReturnsImmutableSnapshot) {
    ImmediateImageryProvider imagery;
    imagery.attributionValue = "shared-credit";
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);
    const TileKey key{scheme->id(), 3, 2, 3};
    auto tile = provider.getTile(key);
    ASSERT_NE(tile, nullptr);
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(processPendingUploadsUntil(provider, 1), 1);

    const auto snapshot = provider.tryGetCachedExactSource(key);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->key.sourceKey, key);
    EXPECT_EQ(snapshot->resolvedKey, key);
    EXPECT_EQ(snapshot->credits, std::vector<std::string>{"shared-credit"});
    ASSERT_NE(snapshot->image, nullptr);
    EXPECT_EQ(snapshot->image->width, 2);
    EXPECT_EQ(imagery.requestCount, 1);
    EXPECT_FALSE(provider.tryGetCachedExactSource(
        TileKey{scheme->id(), 3, 3, 3}).has_value());
}

TEST(RasterAssetDepotTest,
     ExternalImageLeasesRemainAccountedAfterSourceCacheEviction) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(3, 3);
    const TileKey key{scheme->id(), 3, 2, 3};

    RasterAssetDepot depot;
    std::optional<RasterAssetSnapshot> firstLease;
    auto acquired = depot.acquireExactSource(
        RasterAssetConsumer::PageStore,
        provider,
        key,
        []() { return true; },
        [&firstLease](RasterAssetResponse response) {
            ASSERT_TRUE(response.asset.has_value());
            firstLease = std::move(response.asset);
        });
    ASSERT_EQ(acquired.status, RasterAssetAcquireStatus::StartedTransport);
    ASSERT_TRUE(firstLease.has_value());

    const int64_t imageBytes = provider.getCachedSourceTileBytes();
    ASSERT_EQ(imageBytes, 2 * 2 * 4);
    ASSERT_EQ(provider.getSourceDepotResidentBytes(), imageBytes);

    auto secondLease = depot.tryGetCachedExactSource(
        RasterAssetConsumer::PageStore,
        provider,
        key);
    ASSERT_TRUE(secondLease.has_value());
    ASSERT_NE(firstLease->image, nullptr);
    ASSERT_EQ(firstLease->image.get(), secondLease->image.get());
    EXPECT_EQ(provider.getExternalPinnedSourceBytes(), imageBytes);
    EXPECT_EQ(provider.getSourceDepotResidentBytes(), imageBytes)
        << "cache and multiple external leases must count one physical image";

    provider.setSubTileCacheBytes(0);
    EXPECT_EQ(provider.getCachedSourceTileBytes(), 0);
    EXPECT_EQ(provider.getExternalPinnedSourceBytes(), imageBytes);
    EXPECT_EQ(provider.getSourceDepotResidentBytes(), imageBytes)
        << "PageStore ownership must remain visible after cache eviction";

    firstLease.reset();
    EXPECT_EQ(provider.getExternalPinnedSourceBytes(), imageBytes);
    EXPECT_EQ(provider.getSourceDepotResidentBytes(), imageBytes);

    secondLease.reset();
    EXPECT_EQ(provider.getExternalPinnedSourceBytes(), 0);
    EXPECT_EQ(provider.getSourceDepotResidentBytes(), 0);
}

TEST(RasterAssetDepotTest,
     ExternalOnlyResidencyReducesSubsequentSourceCacheBudget) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(3, 3);
    const TileKey firstKey{scheme->id(), 3, 2, 3};
    const TileKey secondKey{scheme->id(), 3, 3, 3};

    RasterAssetDepot depot;
    std::optional<RasterAssetSnapshot> retainedLease;
    auto first = depot.acquireExactSource(
        RasterAssetConsumer::PageStore,
        provider,
        firstKey,
        []() { return true; },
        [&retainedLease](RasterAssetResponse response) {
            ASSERT_TRUE(response.asset.has_value());
            retainedLease = std::move(response.asset);
        });
    ASSERT_EQ(first.status, RasterAssetAcquireStatus::StartedTransport);
    ASSERT_TRUE(retainedLease.has_value());

    const int64_t imageBytes = provider.getCachedSourceTileBytes();
    ASSERT_EQ(imageBytes, 2 * 2 * 4);
    provider.setSubTileCacheBytes(0);
    ASSERT_EQ(provider.getCachedSourceTileBytes(), 0);
    ASSERT_EQ(provider.getSourceDepotResidentBytes(), imageBytes);

    provider.setSubTileCacheBytes(imageBytes);
    int secondReady = 0;
    auto second = depot.acquireExactSource(
        RasterAssetConsumer::PageStore,
        provider,
        secondKey,
        []() { return true; },
        [&secondReady](RasterAssetResponse response) {
            ASSERT_TRUE(response.asset.has_value());
            ++secondReady;
        });
    EXPECT_EQ(second.status, RasterAssetAcquireStatus::StartedTransport);
    EXPECT_EQ(secondReady, 1);
    EXPECT_FALSE(provider.tryGetCachedExactSource(secondKey).has_value())
        << "an external-only image must consume the source cache budget";
    EXPECT_EQ(provider.getCachedSourceTileBytes(), 0);
    EXPECT_EQ(provider.getSourceDepotResidentBytes(), imageBytes)
        << "the second callback lease is transient and must not leave the "
           "provider above its physical decoded-image budget";

    retainedLease.reset();
    EXPECT_EQ(provider.getSourceDepotResidentBytes(), 0);
}

TEST(RasterAssetDepotTest,
     DirectInvalidationPreservesSharedExactSourceAndExternalLease) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);
    const TileKey key{scheme->id(), 3, 2, 3};
    auto tile = provider.getTile(key);
    ASSERT_NE(tile, nullptr);
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(processPendingUploadsUntil(provider, 1), 1);
    ASSERT_EQ(provider.getCachedTileCount(), 1);

    RasterAssetDepot depot;
    auto pageStoreLease = depot.tryGetCachedExactSource(
        RasterAssetConsumer::PageStore,
        provider,
        key);
    ASSERT_TRUE(pageStoreLease.has_value());
    const int64_t sourceBytes = provider.getSourceDepotResidentBytes();
    ASSERT_GT(sourceBytes, 0);

    const uint64_t mappingRevision = provider.mappingRevision();
    provider.invalidateDirectExecutionState();

    EXPECT_GT(provider.mappingRevision(), mappingRevision);
    EXPECT_EQ(provider.getCachedTileCount(), 0)
        << "old Direct GPU/cache state must not survive backend replacement";
    EXPECT_TRUE(provider.tryGetCachedExactSource(key).has_value())
        << "backend-neutral decoded source cache remains shareable";
    EXPECT_EQ(provider.getSourceDepotResidentBytes(), sourceBytes);
    EXPECT_EQ(provider.getExternalPinnedSourceBytes(), sourceBytes);

    pageStoreLease.reset();
    EXPECT_EQ(provider.getExternalPinnedSourceBytes(), 0);
    EXPECT_EQ(provider.getSourceDepotResidentBytes(), sourceBytes)
        << "the provider source cache still owns the decoded image";
}

TEST(RasterOverlayLifecycleTest,
     BackendAssetAcquireSharesInflightAndCancelsOnlyOneLease) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(3, 3);
    const TileKey key{scheme->id(), 3, 2, 3};
    int admissions = 0;
    int firstReady = 0;
    int secondReady = 0;

    auto first = provider.acquireExactSource(
        key,
        [&admissions]() {
            ++admissions;
            return true;
        },
        [&firstReady](RasterAssetResponse response) {
            ASSERT_TRUE(response.asset.has_value());
            ++firstReady;
        });
    auto second = provider.acquireExactSource(
        key,
        [&admissions]() {
            ++admissions;
            return true;
        },
        [&secondReady](RasterAssetResponse) {
            ++secondReady;
        });

    EXPECT_EQ(first.status, RasterAssetAcquireStatus::StartedTransport);
    EXPECT_EQ(second.status, RasterAssetAcquireStatus::JoinedInFlight);
    EXPECT_EQ(admissions, 1);
    ASSERT_EQ(imagery.requestedKeys.size(), 1u);
    EXPECT_EQ(provider.getInFlightSourceWaiterCount(), 2);

    second.handle.cancel();
    EXPECT_EQ(provider.getInFlightSourceWaiterCount(), 1);
    imagery.completeNext();

    EXPECT_EQ(firstReady, 1);
    EXPECT_EQ(secondReady, 0);
    EXPECT_EQ(provider.getInFlightSourceTileCount(), 0);

    auto cached = provider.acquireExactSource(
        key,
        [&admissions]() {
            ++admissions;
            return true;
        },
        [](RasterAssetResponse response) {
            ASSERT_TRUE(response.asset.has_value());
        });
    EXPECT_EQ(cached.status, RasterAssetAcquireStatus::CacheHit);
    EXPECT_EQ(admissions, 1);
    EXPECT_EQ(imagery.requestedKeys.size(), 1u);
}

TEST(RasterOverlayLifecycleTest,
     BackendExactAssetAcquireDoesNotApplyDirectAncestorFallback) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(0, 3);
    const TileKey key{scheme->id(), 3, 2, 3};
    imagery.failingKey = key;
    int callbackCount = 0;
    bool terminalFailure = false;

    auto acquired = provider.acquireExactSource(
        key,
        []() { return true; },
        [&callbackCount, &terminalFailure](RasterAssetResponse response) {
            ++callbackCount;
            terminalFailure = response.terminalFailure;
            EXPECT_FALSE(response.asset.has_value());
        });

    EXPECT_EQ(acquired.status, RasterAssetAcquireStatus::StartedTransport);
    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(terminalFailure);
    ASSERT_EQ(imagery.requestedKeys.size(), 1u);
    EXPECT_EQ(imagery.requestedKeys.front(), key);
}

TEST(RasterOverlayLifecycleTest,
     DirectTileLoadStartsTransportAndBackendAcquireJoinsIt) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(3, 3);
    const TileKey key{scheme->id(), 3, 2, 3};
    auto tile = provider.getTile(key);
    ASSERT_NE(tile, nullptr);
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(imagery.requestedKeys.size(), 1u);

    int admissions = 0;
    int ready = 0;
    auto pageStoreLike = provider.acquireExactSource(
        key,
        [&admissions]() {
            ++admissions;
            return true;
        },
        [&ready](RasterAssetResponse response) {
            ASSERT_TRUE(response.asset.has_value());
            ++ready;
        });
    EXPECT_EQ(pageStoreLike.status,
              RasterAssetAcquireStatus::JoinedInFlight);
    EXPECT_EQ(admissions, 0);
    EXPECT_EQ(imagery.requestedKeys.size(), 1u);

    imagery.completeNext();
    EXPECT_EQ(ready, 1);
}

TEST(RasterOverlayLifecycleTest,
     PageStoreFirstFailureStillLetsJoinedDirectResolveParent) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(0, 3);
    const TileKey child{scheme->id(), 3, 2, 3};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {child};

    int exactCallbacks = 0;
    auto exact = provider.acquireExactSource(
        child,
        []() { return true; },
        [&exactCallbacks](RasterAssetResponse response) {
            ++exactCallbacks;
            EXPECT_TRUE(response.terminalFailure);
            EXPECT_FALSE(response.asset.has_value());
        });
    EXPECT_EQ(exact.status, RasterAssetAcquireStatus::StartedTransport);

    auto directTile = provider.getTile(child);
    ASSERT_NE(directTile, nullptr);
    ASSERT_TRUE(provider.loadTile(*directTile));
    EXPECT_EQ(provider.getInFlightSourceWaiterCount(), 2);
    ASSERT_EQ(imagery.requestedKeys, std::vector<TileKey>{child});

    imagery.completeNext();
    EXPECT_EQ(exactCallbacks, 1)
        << "ExactOnly resolves at child failure instead of waiting for Direct";
    EXPECT_EQ(provider.getPendingSourceFallbackCount(), 1);
    EXPECT_EQ(provider.getInFlightSourceTileCount(), 1);
    FrameResourceBudgetConfig fallbackConfig;
    fallbackConfig.maxRasterNetworkRequestsPerFrame = 4;
    fallbackConfig.maxRasterNetworkInflight = 4;
    FrameResourceBudget fallbackBudget;
    fallbackBudget.beginFrame(1, fallbackConfig);
    EXPECT_EQ(0,
              provider.processPendingUploads(false, &fallbackBudget)
                  .processedUploads);
    EXPECT_EQ(provider.getPendingSourceFallbackCount(), 0);
    EXPECT_EQ(fallbackBudget.rasterNetworkRequestsIssued(), 1u);
    ASSERT_EQ(imagery.requestedKeys,
              (std::vector<TileKey>{child, parent}));

    imagery.completeNext();
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     DirectFirstFailureCompletesJoinedExactBeforeParentResolution) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(0, 3);
    const TileKey child{scheme->id(), 3, 2, 3};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {child};

    auto directTile = provider.getTile(child);
    ASSERT_NE(directTile, nullptr);
    ASSERT_TRUE(provider.loadTile(*directTile));
    int exactCallbacks = 0;
    auto exact = provider.acquireExactSource(
        child,
        []() { return true; },
        [&exactCallbacks](RasterAssetResponse response) {
            ++exactCallbacks;
            EXPECT_TRUE(response.terminalFailure);
            EXPECT_FALSE(response.asset.has_value());
        });
    EXPECT_EQ(exact.status, RasterAssetAcquireStatus::JoinedInFlight);
    EXPECT_EQ(provider.getInFlightSourceWaiterCount(), 2);

    imagery.completeNext();
    EXPECT_EQ(exactCallbacks, 1);
    EXPECT_EQ(provider.getPendingSourceFallbackCount(), 1);
    EXPECT_EQ(provider.getInFlightSourceTileCount(), 1);
    FrameResourceBudgetConfig fallbackConfig;
    fallbackConfig.maxRasterNetworkRequestsPerFrame = 4;
    fallbackConfig.maxRasterNetworkInflight = 4;
    FrameResourceBudget fallbackBudget;
    fallbackBudget.beginFrame(1, fallbackConfig);
    EXPECT_EQ(0,
              provider.processPendingUploads(false, &fallbackBudget)
                  .processedUploads);
    EXPECT_EQ(provider.getPendingSourceFallbackCount(), 0);
    EXPECT_EQ(fallbackBudget.rasterNetworkRequestsIssued(), 1u);
    ASSERT_EQ(imagery.requestedKeys,
              (std::vector<TileKey>{child, parent}));

    imagery.completeNext();
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     CancelledPageStoreFirstWaiterDoesNotStopJoinedDirectFallback) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(0, 3);
    const TileKey child{scheme->id(), 3, 2, 3};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {child};

    int exactCallbacks = 0;
    auto exact = provider.acquireExactSource(
        child,
        []() { return true; },
        [&exactCallbacks](RasterAssetResponse) { ++exactCallbacks; });
    EXPECT_EQ(exact.status, RasterAssetAcquireStatus::StartedTransport);

    auto directTile = provider.getTile(child);
    ASSERT_NE(directTile, nullptr);
    ASSERT_TRUE(provider.loadTile(*directTile));
    EXPECT_EQ(provider.getInFlightSourceWaiterCount(), 2);

    exact.handle.cancel();
    EXPECT_EQ(provider.getInFlightSourceWaiterCount(), 1);
    imagery.completeNext();
    EXPECT_EQ(exactCallbacks, 0);
    EXPECT_EQ(provider.getPendingSourceFallbackCount(), 1);

    FrameResourceBudgetConfig fallbackConfig;
    fallbackConfig.maxRasterNetworkRequestsPerFrame = 4;
    fallbackConfig.maxRasterNetworkInflight = 4;
    FrameResourceBudget fallbackBudget;
    fallbackBudget.beginFrame(1, fallbackConfig);
    EXPECT_EQ(0,
              provider.processPendingUploads(false, &fallbackBudget)
                  .processedUploads);
    ASSERT_EQ(imagery.requestedKeys,
              (std::vector<TileKey>{child, parent}));

    imagery.completeNext();
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     CachedExactFailureDoesNotSuppressLaterDirectParentResolution) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(0, 3);
    const TileKey child{scheme->id(), 3, 2, 3};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {child};

    int exactCallbacks = 0;
    auto exact = provider.acquireExactSource(
        child,
        []() { return true; },
        [&exactCallbacks](RasterAssetResponse response) {
            ++exactCallbacks;
            EXPECT_TRUE(response.terminalFailure);
        });
    EXPECT_EQ(exact.status, RasterAssetAcquireStatus::StartedTransport);
    imagery.completeNext();
    EXPECT_EQ(exactCallbacks, 1);
    ASSERT_EQ(imagery.requestedKeys, std::vector<TileKey>{child});

    auto directTile = provider.getTile(child);
    ASSERT_NE(directTile, nullptr);
    ASSERT_TRUE(provider.loadTile(*directTile));
    EXPECT_EQ(provider.getPendingSourceFallbackCount(), 1);
    FrameResourceBudgetConfig fallbackConfig;
    fallbackConfig.maxRasterNetworkRequestsPerFrame = 4;
    fallbackConfig.maxRasterNetworkInflight = 4;
    FrameResourceBudget fallbackBudget;
    fallbackBudget.beginFrame(1, fallbackConfig);
    EXPECT_EQ(0,
              provider.processPendingUploads(false, &fallbackBudget)
                  .processedUploads);
    ASSERT_EQ(imagery.requestedKeys,
              (std::vector<TileKey>{child, parent}));

    imagery.completeNext();
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directTile->getState());
}

TEST(RasterOverlayLifecycleTest, ContentRevisionInvalidatesExactSourceFacade) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);
    const TileKey key{scheme->id(), 3, 2, 3};
    auto tile = provider.getTile(key);
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(processPendingUploadsUntil(provider, 1), 1);
    ASSERT_TRUE(provider.tryGetCachedExactSource(key).has_value());

    imagery.contentRevisionValue = 1;
    EXPECT_FALSE(provider.tryGetCachedExactSource(key).has_value());
}

TEST(RasterOverlayLifecycleTest, TerminalSourceFailureIsNotAnAssetHit) {
    NullImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme);
    provider.setLevelRange(0, 0);
    const TileKey key{scheme->id(), 0, 0, 0};
    auto tile = provider.getTile(key);
    ASSERT_NE(tile, nullptr);
    ASSERT_TRUE(provider.loadTile(*tile));
    processPendingUploadsUntil(provider, 1);
    EXPECT_FALSE(provider.tryGetCachedExactSource(key).has_value());
}

TEST(RasterOverlayRuntimeTest, PreservesConfiguredOverlayOrder) {
    RasterOverlay::Options options;
    RasterOverlay firstOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    RasterOverlay secondOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay first(firstOverlay);
    ActivatedRasterOverlay second(secondOverlay);
    RasterOverlayRuntime runtime({&first, &second});
    EXPECT_TRUE(runtime.beginFrame(1, nullptr));
    const auto& frame = runtime.frameContext();
    ASSERT_EQ(frame.slots().size(), 2u);
    EXPECT_EQ(frame.slots()[0].overlay, &first);
    EXPECT_EQ(frame.slots()[1].overlay, &second);
    const auto& direct = frame.directProviders();
    ASSERT_EQ(direct.size(), 2u);
    EXPECT_EQ(direct[0], first.getTileProvider());
    EXPECT_EQ(direct[1], second.getTileProvider());
    const auto& pageStore = frame.pageStoreSources();
    ASSERT_EQ(pageStore.size(), 2u);
    EXPECT_EQ(pageStore[0].provider, first.getTileProvider());
    EXPECT_EQ(pageStore[0].runtimeSlot, 0u);
    EXPECT_EQ(pageStore[1].provider, second.getTileProvider());
    EXPECT_EQ(pageStore[1].runtimeSlot, 1u);
}

TEST(RasterOverlayRuntimeTest, BackendEnablementSelectsFailClosedView) {
    RasterOverlay::Options options;
    RasterOverlay overlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay activated(overlay);
    RasterOverlayRuntime runtime({&activated});

    ASSERT_TRUE(runtime.beginFrame(1, nullptr));
    ASSERT_EQ(runtime.frameContext().pageStoreSources().size(), 1u);
    runtime.setBackendEnabled(RasterOverlayBackendKind::PageStore, false);
    EXPECT_FALSE(runtime.backendEnabled(RasterOverlayBackendKind::PageStore));
    EXPECT_FALSE(runtime.beginFrame(2, nullptr));
    EXPECT_TRUE(runtime.frameContext().pageStoreSources().empty());
    EXPECT_TRUE(runtime.backendEnabled(RasterOverlayBackendKind::Direct));
    EXPECT_EQ(runtime.frameContext().directProviders().size(), 1u);
}

namespace {
class TestRasterOverlayBackend final : public RasterOverlayBackend {
public:
    explicit TestRasterOverlayBackend(RasterOverlayBackendKind kind,
                                      RasterOverlayTileProvider* provider)
        : kind_(kind), provider_(provider) {}

    RasterOverlayBackendKind kind() const override { return kind_; }

    const std::vector<RasterOverlayTileProvider*>& providers(
        const std::vector<ActivatedRasterOverlay*>&,
        RenderDevice*) override {
        ++calls;
        providers_.clear();
        if (enabled_ && provider_) {
            providers_.push_back(provider_);
        }
        return providers_;
    }

    void setEnabled(bool enabled) override { enabled_ = enabled; }
    bool enabled() const override { return enabled_; }

    int calls = 0;

private:
    RasterOverlayBackendKind kind_;
    RasterOverlayTileProvider* provider_ = nullptr;
    bool enabled_ = true;
    std::vector<RasterOverlayTileProvider*> providers_;
};
} // namespace

TEST(RasterOverlayRuntimeTest, BackendReplacementIsBehaviorallySelected) {
    RasterOverlay::Options options;
    RasterOverlay overlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay activated(overlay);
    RasterOverlayRuntime runtime({&activated});
    RasterOverlayTileProvider* provider = activated.ensureTileProvider(nullptr);

    auto replacement = std::make_unique<TestRasterOverlayBackend>(
        RasterOverlayBackendKind::PageStore, provider);
    TestRasterOverlayBackend* replacementPtr = replacement.get();
    ASSERT_TRUE(runtime.setBackend(RasterOverlayBackendKind::PageStore,
                                   std::move(replacement)));

    ASSERT_TRUE(runtime.beginFrame(1, nullptr));
    const auto& selected = runtime.frameContext().pageStoreSources();
    ASSERT_EQ(selected.size(), 1u);
    EXPECT_EQ(selected.front().provider, provider);
    EXPECT_EQ(replacementPtr->calls, 1);
    ASSERT_NE(runtime.backend(RasterOverlayBackendKind::PageStore), nullptr);
    EXPECT_EQ(runtime.backend(RasterOverlayBackendKind::PageStore)->kind(),
              RasterOverlayBackendKind::PageStore);
    EXPECT_EQ(provider->assetDepot().get(),
              runtime.frameContext().assetDepotHandle().get());

    auto wrongKind = std::make_unique<TestRasterOverlayBackend>(
        RasterOverlayBackendKind::Direct, provider);
    EXPECT_FALSE(runtime.setBackend(
        RasterOverlayBackendKind::PageStore, std::move(wrongKind)));
    EXPECT_EQ(runtime.backend(RasterOverlayBackendKind::PageStore),
              replacementPtr);

    ASSERT_TRUE(runtime.setBackend(
        RasterOverlayBackendKind::PageStore, nullptr));
    EXPECT_FALSE(runtime.beginFrame(2, nullptr));
    EXPECT_EQ(runtime.frameContext().pageStoreSources().size(), 1u);
}

TEST(RasterOverlayRuntimeTest, BackendProviderViewIsNormalizedToRuntimeOrder) {
    RasterOverlay::Options options;
    RasterOverlay firstOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    RasterOverlay secondOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    RasterOverlay foreignOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay first(firstOverlay);
    ActivatedRasterOverlay second(secondOverlay);
    ActivatedRasterOverlay foreign(foreignOverlay);
    RasterOverlayRuntime runtime({&first, &second});
    RasterOverlayTileProvider* firstProvider =
        first.ensureTileProvider(nullptr);
    RasterOverlayTileProvider* secondProvider =
        second.ensureTileProvider(nullptr);
    RasterOverlayTileProvider* foreignProvider =
        foreign.ensureTileProvider(nullptr);

    class ReorderingBackend final : public RasterOverlayBackend {
    public:
        ReorderingBackend(RasterOverlayTileProvider* firstProvider,
                          RasterOverlayTileProvider* secondProvider,
                          RasterOverlayTileProvider* foreignProvider)
            : firstProvider_(firstProvider),
              secondProvider_(secondProvider),
              foreignProvider_(foreignProvider) {}
        RasterOverlayBackendKind kind() const override {
            return RasterOverlayBackendKind::PageStore;
        }
        const std::vector<RasterOverlayTileProvider*>& providers(
            const std::vector<ActivatedRasterOverlay*>&,
            RenderDevice*) override {
            providers_ = {
                secondProvider_,
                foreignProvider_,
                firstProvider_,
                secondProvider_};
            return providers_;
        }
        void setEnabled(bool enabled) override { enabled_ = enabled; }
        bool enabled() const override { return enabled_; }

    private:
        RasterOverlayTileProvider* firstProvider_ = nullptr;
        RasterOverlayTileProvider* secondProvider_ = nullptr;
        RasterOverlayTileProvider* foreignProvider_ = nullptr;
        bool enabled_ = true;
        std::vector<RasterOverlayTileProvider*> providers_;
    };

    ASSERT_TRUE(runtime.setBackend(
        RasterOverlayBackendKind::PageStore,
        std::make_unique<ReorderingBackend>(
            firstProvider, secondProvider, foreignProvider)));
    ASSERT_TRUE(runtime.beginFrame(1, nullptr));
    const auto& providers = runtime.frameContext().pageStoreSources();
    ASSERT_EQ(providers.size(), 2u);
    EXPECT_EQ(providers[0].provider, firstProvider);
    EXPECT_EQ(providers[0].runtimeSlot, 0u);
    EXPECT_EQ(providers[1].provider, secondProvider);
    EXPECT_EQ(providers[1].runtimeSlot, 1u);

    runtime.setBackendEnabled(RasterOverlayBackendKind::PageStore, false);
    EXPECT_FALSE(runtime.beginFrame(2, nullptr));
    EXPECT_TRUE(runtime.frameContext().pageStoreSources().empty());
}

TEST(RasterOverlayRuntimeTest,
     FrameContextPreservesRuntimeSlotsForFilteredDirectBackend) {
    RasterOverlay::Options options;
    RasterOverlay firstOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    RasterOverlay secondOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay first(firstOverlay);
    ActivatedRasterOverlay second(secondOverlay);
    RasterOverlayRuntime runtime({&first, &second});
    RasterOverlayTileProvider* firstProvider =
        first.ensureTileProvider(nullptr);
    RasterOverlayTileProvider* secondProvider =
        second.ensureTileProvider(nullptr);

    ASSERT_TRUE(runtime.setBackend(
        RasterOverlayBackendKind::Direct,
        std::make_unique<TestRasterOverlayBackend>(
            RasterOverlayBackendKind::Direct,
            secondProvider)));
    EXPECT_TRUE(runtime.beginFrame(17, nullptr));

    const RasterOverlayFrameContext& frame = runtime.frameContext();
    EXPECT_EQ(frame.frameNumber(), 17u);
    ASSERT_EQ(frame.slots().size(), 2u);
    ASSERT_EQ(frame.directOverlays().size(), 2u);
    ASSERT_EQ(frame.directProviders().size(), 1u);
    EXPECT_EQ(frame.slots()[0].runtimeSlot, 0u);
    EXPECT_EQ(frame.slots()[0].overlay, &first);
    EXPECT_EQ(frame.slots()[0].directProvider, nullptr);
    EXPECT_EQ(frame.directOverlays()[0], nullptr);
    EXPECT_EQ(frame.slots()[1].runtimeSlot, 1u);
    EXPECT_EQ(frame.slots()[1].overlay, &second);
    EXPECT_EQ(frame.slots()[1].directProvider, secondProvider);
    EXPECT_EQ(frame.directOverlays()[1], &second);
    EXPECT_EQ(frame.directProviders()[0], secondProvider);
    EXPECT_EQ(firstProvider->assetDepot().get(), frame.assetDepotHandle().get());
    EXPECT_EQ(secondProvider->assetDepot().get(), frame.assetDepotHandle().get());

    const uint64_t generation = frame.generation();
    const uint64_t directGeneration = frame.directGeneration();
    EXPECT_FALSE(runtime.beginFrame(18, nullptr));
    EXPECT_EQ(runtime.frameContext().generation(), generation);
    EXPECT_EQ(runtime.frameContext().directGeneration(), directGeneration);
}

TEST(RasterOverlayRuntimeTest,
     HiddenFifthSourceIsFilteredAndFourSourceFallbackParityRemains) {
    std::vector<std::unique_ptr<RasterOverlay>> overlays;
    std::vector<std::unique_ptr<ActivatedRasterOverlay>> activated;
    std::vector<ActivatedRasterOverlay*> configured;
    for (size_t i = 0; i < 5; ++i) {
        RasterOverlay::Options options;
        options.visible = true;
        options.opacity = i == 4 ? 0.0f : 1.0f;
        options.role = i == 0
            ? RasterOverlayRole::BaseImagery
            : RasterOverlayRole::AnnotationOverlay;
        overlays.push_back(std::make_unique<RasterOverlay>(
            std::make_unique<ImmediateImageryProvider>(),
            TileScheme::createXYZWebMercator(), options));
        activated.push_back(
            std::make_unique<ActivatedRasterOverlay>(*overlays.back()));
        configured.push_back(activated.back().get());
    }

    RasterOverlayRuntime runtime(configured);
    ASSERT_TRUE(runtime.beginFrame(1, nullptr));
    const RasterOverlayFrameContext& frame = runtime.frameContext();
    ASSERT_EQ(frame.pageStoreSources().size(), 4u);
    EXPECT_TRUE(frame.pageStoreHasDirectFallbackParity());
    EXPECT_EQ(
        TerrainPageStore::pageSourceStackCompatibility(
            frame.pageStoreSources(),
            frame.pageStoreHasDirectFallbackParity()),
        TerrainPageStore::PageDomainCompatibility::Compatible);
}

TEST(RasterOverlayRuntimeTest,
     DifferentDirectAndPageStoreBackendStacksPublishNoFallbackParity) {
    RasterOverlay::Options options;
    RasterOverlay firstOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    RasterOverlay secondOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay first(firstOverlay);
    ActivatedRasterOverlay second(secondOverlay);
    RasterOverlayRuntime runtime({&first, &second});
    RasterOverlayTileProvider* secondProvider =
        second.ensureTileProvider(nullptr);

    ASSERT_TRUE(runtime.setBackend(
        RasterOverlayBackendKind::Direct,
        std::make_unique<TestRasterOverlayBackend>(
            RasterOverlayBackendKind::Direct, secondProvider)));
    ASSERT_TRUE(runtime.beginFrame(1, nullptr));

    const RasterOverlayFrameContext& frame = runtime.frameContext();
    EXPECT_FALSE(frame.pageStoreHasDirectFallbackParity());
    EXPECT_EQ(
        TerrainPageStore::pageSourceStackCompatibility(
            frame.pageStoreSources(),
            frame.pageStoreHasDirectFallbackParity()),
        TerrainPageStore::PageDomainCompatibility::
            DirectFallbackStackMismatch);
}

TEST(RasterOverlayRuntimeTest,
     DirectBackendReplacementAdvancesGenerationAndInvalidatesDirectState) {
    RasterOverlay::Options options;
    RasterOverlay overlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay activated(overlay);
    RasterOverlayRuntime runtime({&activated});
    RasterOverlayTileProvider* provider =
        activated.ensureTileProvider(nullptr);
    ASSERT_TRUE(runtime.beginFrame(1, nullptr));

    auto geometryScheme = TileScheme::createGeographicTMS();
    const TileKey geometryKey{geometryScheme->id(), 2, 4, 2};
    const Rectangle geometryBounds =
        geometryScheme->tileToRectangle(geometryKey);
    auto mapped = provider->mapRasterTilesToGeometryTile(
        projectForProvider(*provider, geometryBounds),
        512.0,
        512.0).tile;
    ASSERT_NE(mapped, nullptr);
    ASSERT_TRUE(mapped->isDirectCompositeTile());
    ASSERT_GT(provider->getCachedTileCount(), 0);
    const uint64_t mappingRevision = provider->mappingRevision();
    const uint64_t directGeneration =
        runtime.frameContext().directGeneration();

    ASSERT_TRUE(runtime.setBackend(
        RasterOverlayBackendKind::Direct,
        std::make_unique<TestRasterOverlayBackend>(
            RasterOverlayBackendKind::Direct,
            nullptr)));
    EXPECT_TRUE(runtime.beginFrame(2, nullptr));
    EXPECT_GT(runtime.frameContext().directGeneration(), directGeneration);
    EXPECT_GT(provider->mappingRevision(), mappingRevision);
    EXPECT_EQ(provider->getCachedTileCount(), 0);
    ASSERT_EQ(runtime.frameContext().directOverlays().size(), 1u);
    EXPECT_EQ(runtime.frameContext().directOverlays()[0], nullptr);
    ASSERT_EQ(runtime.frameContext().slots().size(), 1u);
    EXPECT_EQ(runtime.frameContext().slots()[0].runtimeSlot, 0u);
    EXPECT_EQ(runtime.frameContext().slots()[0].directProvider, nullptr);
}

TEST(RasterOverlayRuntimeTest,
     InitialFramePublishesGenerationWithoutInvalidatingExistingDirectWork) {
    RasterOverlay::Options options;
    RasterOverlay overlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay activated(overlay);
    RasterOverlayTileProvider* provider =
        activated.ensureTileProvider(nullptr);
    const TileKey key{"XYZ-WebMercator", 1, 0, 0};
    auto tile = provider->getTile(key);
    ASSERT_NE(tile, nullptr);
    ASSERT_TRUE(provider->loadTile(*tile));
    ASSERT_EQ(provider->getPendingUploadCount(), 1);
    const uint64_t mappingRevision = provider->mappingRevision();

    RasterOverlayRuntime runtime({&activated});
    EXPECT_TRUE(runtime.beginFrame(0, nullptr));

    EXPECT_EQ(provider->getPendingUploadCount(), 1);
    EXPECT_EQ(provider->mappingRevision(), mappingRevision);
    ASSERT_EQ(runtime.frameContext().directOverlays().size(), 1u);
    EXPECT_EQ(runtime.frameContext().directOverlays()[0], &activated);
}

TEST(RasterOverlayRuntimeTest,
     DirectBackendReplacementRejectsLateExactTileUpload) {
    auto imagery = std::make_unique<DeferredImageryProvider>();
    DeferredImageryProvider* imageryPtr = imagery.get();
    RasterOverlay::Options options;
    RasterOverlay overlay(
        std::move(imagery),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay activated(overlay);
    RasterOverlayRuntime runtime({&activated});
    RasterOverlayTileProvider* provider =
        activated.ensureTileProvider(nullptr);
    ASSERT_TRUE(runtime.beginFrame(1, nullptr));

    const TileKey key{"XYZ-WebMercator", 3, 2, 3};
    auto tile = provider->getTile(key);
    ASSERT_NE(tile, nullptr);
    ASSERT_TRUE(provider->loadTile(*tile));
    ASSERT_EQ(imageryPtr->pending.size(), 1u);
    ASSERT_EQ(provider->getActiveDirectCompositeSourceSetOrderCount(), 1);

    ASSERT_TRUE(runtime.setBackend(
        RasterOverlayBackendKind::Direct,
        std::make_unique<TestRasterOverlayBackend>(
            RasterOverlayBackendKind::Direct,
            nullptr)));
    ASSERT_TRUE(runtime.beginFrame(2, nullptr));
    EXPECT_EQ(provider->getActiveDirectCompositeSourceSetOrderCount(), 0);
    EXPECT_EQ(provider->getPendingUploadCount(), 0);

    imageryPtr->completeNext();

    EXPECT_EQ(provider->getPendingUploadCount(), 0);
    EXPECT_EQ(provider->getInFlightSourceWaiterCount(), 0);
    EXPECT_EQ(provider->getActiveDirectCompositeSourceSetOrderCount(), 0);
    EXPECT_FALSE(provider->hasPendingWork());
    EXPECT_TRUE(provider->tryGetCachedExactSource(key).has_value())
        << "late transport may remain useful to the shared Asset Depot";
}

TEST(RasterOverlayRuntimeTest,
     FilteredBackendDoesNotInstantiateExcludedOverlayProviders) {
    RasterOverlay::Options options;
    RasterOverlay firstOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    RasterOverlay secondOverlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay first(firstOverlay);
    ActivatedRasterOverlay second(secondOverlay);
    RasterOverlayRuntime runtime({&first, &second});
    RasterOverlayTileProvider* secondProvider =
        second.ensureTileProvider(nullptr);

    // Frame publication evaluates both consumers. Keep Direct filtered too,
    // otherwise its default provider stack would legitimately instantiate the
    // first overlay before this PageStore-specific assertion runs.
    ASSERT_TRUE(runtime.setBackend(
        RasterOverlayBackendKind::Direct,
        std::make_unique<TestRasterOverlayBackend>(
            RasterOverlayBackendKind::Direct, nullptr)));

    class SingleProviderBackend final : public RasterOverlayBackend {
    public:
        explicit SingleProviderBackend(RasterOverlayTileProvider* provider)
            : provider_(provider) {}
        RasterOverlayBackendKind kind() const override {
            return RasterOverlayBackendKind::PageStore;
        }
        const std::vector<RasterOverlayTileProvider*>& providers(
            const std::vector<ActivatedRasterOverlay*>&,
            RenderDevice*) override {
            providers_ = {provider_};
            return providers_;
        }
        void setEnabled(bool enabled) override { enabled_ = enabled; }
        bool enabled() const override { return enabled_; }

    private:
        RasterOverlayTileProvider* provider_ = nullptr;
        bool enabled_ = true;
        std::vector<RasterOverlayTileProvider*> providers_;
    };

    ASSERT_TRUE(runtime.setBackend(
        RasterOverlayBackendKind::PageStore,
        std::make_unique<SingleProviderBackend>(secondProvider)));
    ASSERT_TRUE(runtime.beginFrame(1, nullptr));
    const auto& providers = runtime.frameContext().pageStoreSources();
    ASSERT_EQ(providers.size(), 1u);
    EXPECT_EQ(providers.front().provider, secondProvider);
    EXPECT_EQ(providers.front().runtimeSlot, 1u);
    EXPECT_EQ(first.getTileProvider(), nullptr);
}

TEST(RasterOverlayRuntimeTest,
     FrameMetadataIsValueSnapshottedAndPublishedNextFrame) {
    RasterOverlay::Options options;
    options.opacity = 0.75f;
    options.role = RasterOverlayRole::AnnotationOverlay;
    options.priority = RasterOverlayPriority::Normal;
    options.fallbackPolicy = RasterOverlayFallbackPolicy::SkipUntilReady;
    options.blocksCompleteRenderable = false;
    RasterOverlay overlay(
        std::make_unique<ImmediateImageryProvider>(),
        TileScheme::createXYZWebMercator(), options);
    ActivatedRasterOverlay activated(overlay);
    RasterOverlayRuntime runtime({&activated});

    ASSERT_TRUE(runtime.beginFrame(10, nullptr));
    const RasterOverlayFrameContext& frame = runtime.frameContext();
    ASSERT_EQ(frame.slots().size(), 1u);
    const RasterOverlayFrameSlot published = frame.slots().front();
    const uint64_t firstGeneration = frame.generation();
    const uint64_t firstPageStoreGeneration = frame.pageStoreGeneration();
    EXPECT_TRUE(published.visible);
    EXPECT_FLOAT_EQ(published.opacity, 0.75f);
    EXPECT_EQ(published.role, RasterOverlayRole::AnnotationOverlay);
    EXPECT_EQ(published.priority, RasterOverlayPriority::Normal);
    EXPECT_EQ(published.fallbackPolicy,
              RasterOverlayFallbackPolicy::SkipUntilReady);
    EXPECT_FALSE(published.blocksCompleteRenderable);
    EXPECT_EQ(published.projection, activated.getProjection());
    EXPECT_EQ(published.generation, firstGeneration);
    ASSERT_EQ(frame.pageStoreSources().size(), 1u);
    EXPECT_FLOAT_EQ(frame.pageStoreSources().front().opacity, 0.75f);

    overlay.setVisible(false);
    overlay.setOpacity(0.25f);
    overlay.getOptions().priority = RasterOverlayPriority::Low;

    // Mutating the configuration cannot alter the already-published value
    // snapshot. The next beginFrame transaction publishes the new state.
    EXPECT_TRUE(published.visible);
    EXPECT_FLOAT_EQ(published.opacity, 0.75f);
    EXPECT_EQ(published.priority, RasterOverlayPriority::Normal);

    EXPECT_FALSE(runtime.beginFrame(11, nullptr));
    const RasterOverlayFrameContext& next = runtime.frameContext();
    ASSERT_EQ(next.slots().size(), 1u);
    EXPECT_FALSE(next.slots().front().visible);
    EXPECT_FLOAT_EQ(next.slots().front().opacity, 0.25f);
    EXPECT_EQ(next.slots().front().priority, RasterOverlayPriority::Low);
    EXPECT_GT(next.generation(), firstGeneration);
    EXPECT_GT(next.pageStoreGeneration(), firstPageStoreGeneration);
    EXPECT_TRUE(next.pageStoreSources().empty());
}

TEST(RasterOverlayLifecycleTest, LevelRangeChangeInvalidatesSourceDepotLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
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
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(firstTile->isDirectCompositeTile());

    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    provider.setLevelRange(3, 4);

    RasterOverlayTileProvider::TilePtr secondTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, eastHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(secondTile->isDirectCompositeTile());

    ASSERT_TRUE(provider.loadTile(*secondTile));
    ASSERT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.back());
    ASSERT_EQ(2u, imagery.pending.size());

    imagery.completeNext();
    EXPECT_EQ(0, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed,
              firstTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              secondTile->getState());

    imagery.completeNext();
    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              secondTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     LevelRangeChangeRecreatesDirectTileLikeCesiumNativeProviderDepot) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

    const TileKey sourceKey{scheme->id(), 3, 4, 3};
    RasterOverlayTileProvider::TilePtr firstTile =
        provider.getTile(sourceKey);
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, firstTile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              firstTile->isMoreDetailAvailable());
    EXPECT_EQ(1, imagery.requestCount);

    provider.setLevelRange(3, 4);

    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, firstTile->getState());
    RasterOverlayTileProvider::TilePtr secondTile =
        provider.getTile(sourceKey);
    ASSERT_NE(nullptr, secondTile);
    EXPECT_NE(firstTile.get(), secondTile.get());
    EXPECT_EQ(RasterOverlayTile::LoadState::Unloaded,
              secondTile->getState());

    ASSERT_TRUE(provider.loadTile(*secondTile));
    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, secondTile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Yes,
              secondTile->isMoreDetailAvailable());
    EXPECT_EQ(2, imagery.requestCount);
}

TEST(RasterOverlayLifecycleTest,
     LevelRangeChangeDropsStaleDirectInFlightTileLikeCesiumNativeDepot) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

    const TileKey sourceKey{scheme->id(), 3, 4, 3};
    RasterOverlayTileProvider::TilePtr staleTile =
        provider.getTile(sourceKey);
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(provider.loadTile(*staleTile));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              staleTile->getState());

    provider.setLevelRange(3, 4);

    EXPECT_EQ(RasterOverlayTile::LoadState::Failed,
              staleTile->getState());
    RasterOverlayTileProvider::TilePtr currentTile =
        provider.getTile(sourceKey);
    ASSERT_NE(nullptr, currentTile);
    EXPECT_NE(staleTile.get(), currentTile.get());

    imagery.completeNext();
    EXPECT_EQ(0, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_FALSE(provider.hasPendingWork());
    EXPECT_EQ(0, provider.getCachedSourceTileBytes());

    ASSERT_TRUE(provider.loadTile(*currentTile));
    EXPECT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.back());
}

TEST(RasterOverlayLifecycleTest,
     LevelRangeChangeDropsStaleDirectPendingUploadImmediately) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

    const TileKey sourceKey{scheme->id(), 3, 4, 3};
    auto staleTile = provider.getTile(sourceKey);
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(provider.loadTile(*staleTile));
    ASSERT_EQ(1, provider.getPendingUploadCount());

    provider.setLevelRange(3, 4);

    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, staleTile->getState());
    EXPECT_EQ(0, provider.getPendingUploadCount());
    provider.processPendingUploads(false);
    EXPECT_EQ(0, uploaderPtr->uploadCount);
}

TEST(RasterOverlayLifecycleTest,
     PendingUploadBackpressureBlocksAdditionalDirectLoadsUntilDrain) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    auto firstTile = provider.getTile(TileKey{scheme->id(), 2, 1, 1});
    auto secondTile = provider.getTile(TileKey{scheme->id(), 2, 1, 2});
    ASSERT_NE(nullptr, firstTile);
    ASSERT_NE(nullptr, secondTile);

    ASSERT_TRUE(provider.loadTileThrottled(*firstTile, nullptr));
    ASSERT_EQ(1u, imagery.pending.size());
    imagery.completeNext();
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
    const int64_t blockingBytes = std::max(
        provider.getPendingUploadBytes(),
        provider.getCachedSourceTileBytes());
    EXPECT_GT(blockingBytes, 0);
    provider.setSubTileCacheBytes(blockingBytes);

    EXPECT_FALSE(provider.loadTileThrottled(*secondTile, nullptr));
    EXPECT_EQ(1u, imagery.requestedKeys.size());

    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(0, provider.getPendingUploadCount());

    ASSERT_TRUE(waitForNoPendingWork(provider));
    EXPECT_TRUE(provider.loadTileThrottled(*secondTile, nullptr));
    EXPECT_EQ(2u, imagery.requestedKeys.size());
}

TEST(RasterOverlayLifecycleTest,
     PendingUploadsConsumeSubTileBudgetBeforeSourceDepot) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(2, 2);

    const TileKey sourceKey{scheme->id(), 2, 1, 1};
    auto firstTile = provider.getTile(sourceKey);
    ASSERT_NE(nullptr, firstTile);

    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_EQ(1u, imagery.pending.size());
    imagery.completeNext();
    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    const int64_t retainedSourceBytes = provider.getCachedSourceTileBytes();
    ASSERT_GT(retainedSourceBytes, 0);

    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    auto secondTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(secondTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*secondTile));
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
    const int64_t pendingBytes = provider.getPendingUploadBytes();
    ASSERT_GT(pendingBytes, 0);

    provider.setSubTileCacheBytes(pendingBytes);

    EXPECT_EQ(0, provider.getCachedSourceTileBytes());
    EXPECT_EQ(pendingBytes, provider.getPendingUploadBytes());
}

TEST(RasterOverlayLifecycleTest,
     SharedDirectPendingUploadDoesNotDoubleCountSourceCpuBytes) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    auto tile = provider.getTile(sourceKey);
    ASSERT_NE(nullptr, tile);

    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(1u, imagery.pending.size());
    imagery.completeNext();

    ASSERT_EQ(1, provider.getPendingUploadCount());
    EXPECT_EQ(0, provider.getPendingUploadBytes());
    EXPECT_GT(provider.getCachedSourceTileBytes(), 0);

    provider.setSubTileCacheBytes(provider.getCachedSourceTileBytes());

    EXPECT_GT(provider.getCachedSourceTileBytes(), 0);
    EXPECT_EQ(0, provider.getPendingUploadBytes());
}

TEST(RasterOverlayLifecycleTest,
     UploadingImageKeepsPendingBudgetUntilMainThreadUploadFinishes) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<BudgetInspectingRasterUploader>();
    BudgetInspectingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    uploaderPtr->provider = &provider;
    provider.setLevelRange(3, 3);
    provider.setSubTileCacheBytes(256 * 256 * 4 + 1);

    const TileKey directKey{scheme->id(), 3, 4, 3};
    auto directTile = provider.getTile(directKey);
    ASSERT_NE(nullptr, directTile);
    ASSERT_TRUE(provider.loadTile(*directTile));
    ASSERT_EQ(1u, imagery.pending.size());

    imagery.completeNext();
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
    const int64_t directSharedSourceBytes = provider.getCachedSourceTileBytes();
    EXPECT_GT(directSharedSourceBytes, 0);

    const TileKey directCompositeSourceKey{scheme->id(), 3, 5, 3};
    const Rectangle directCompositeSourceBounds =
        scheme->tileToRectangle(directCompositeSourceKey);
    const Rectangle mappedHalf(
        directCompositeSourceBounds.west(),
        directCompositeSourceBounds.south(),
        directCompositeSourceBounds.west() + directCompositeSourceBounds.width() * 0.5,
        directCompositeSourceBounds.north());
    auto mappedTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, mappedHalf),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, mappedTile);
    ASSERT_TRUE(mappedTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*mappedTile));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(directCompositeSourceKey, imagery.pending.front().key);

    uploaderPtr->onFirstUpload = [&imagery]() { imagery.completeNext(); };
    FrameResourceBudgetConfig uploadConfig;
    uploadConfig.maxRasterUploadsPerFrame = 1;
    FrameResourceBudget firstFrameBudget;
    firstFrameBudget.beginFrame(1, uploadConfig);
    EXPECT_EQ(
        1,
        provider.processPendingUploads(false, &firstFrameBudget)
            .processedUploads);
    EXPECT_EQ(
        directSharedSourceBytes,
        uploaderPtr->observedSourceCacheBytesDuringUpload);

    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));
    FrameResourceBudget secondFrameBudget;
    secondFrameBudget.beginFrame(2, uploadConfig);
    EXPECT_EQ(
        1,
        provider.processPendingUploads(false, &secondFrameBudget)
            .processedUploads);
    EXPECT_EQ(0, provider.getPendingUploadCount());
}

TEST(RasterOverlayLifecycleTest,
     StaleEpochSourceCompletionDoesNotRepopulateCurrentDepotCache) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
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

    RasterOverlayTileProvider::TilePtr staleTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(provider.loadTile(*staleTile));
    ASSERT_EQ(1u, imagery.requestedKeys.size());

    provider.setLevelRange(3, 4);

    imagery.completeNext();
    EXPECT_EQ(0, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed,
              staleTile->getState());
    EXPECT_FALSE(provider.hasPendingWork());
    EXPECT_EQ(0, provider.getCachedSourceTileBytes());

    RasterOverlayTileProvider::TilePtr currentTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, eastHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, currentTile);
    ASSERT_TRUE(provider.loadTile(*currentTile));
    EXPECT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.back());
}

TEST(RasterOverlayLifecycleTest,
     MaximumTextureSizeChangeRejectsStaleDirectCompositeLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);
    provider.setMaximumTextureSize(2048);

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

    RasterOverlayTileProvider::TilePtr staleTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(staleTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*staleTile));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    provider.setMaximumTextureSize(256);

    imagery.completeNext();
    EXPECT_EQ(0, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed,
              staleTile->getState());
    EXPECT_FALSE(provider.hasPendingWork());
    EXPECT_EQ(0, provider.getCachedSourceTileBytes());

    RasterOverlayTileProvider::TilePtr currentTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, eastHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, currentTile);
    ASSERT_TRUE(provider.loadTile(*currentTile));
    EXPECT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.back());
}

TEST(RasterOverlayLifecycleTest,
     LevelRangeChangeRecreatesCachedSourceAssetLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
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
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(firstTile->isDirectCompositeTile());

    ASSERT_TRUE(provider.loadTile(*firstTile));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    imagery.completeNext();
    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              firstTile->getState());

    provider.setLevelRange(3, 4);

    RasterOverlayTileProvider::TilePtr secondTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, eastHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(secondTile->isDirectCompositeTile());

    ASSERT_TRUE(provider.loadTile(*secondTile));
    ASSERT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.back());
}

TEST(RasterOverlayLifecycleTest,
     RectangleMappingsReuseProviderTileAssetLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());

    auto first = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        256.0,
        512.0).tile;
    auto second = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    EXPECT_EQ(first.get(), second.get());
    EXPECT_TRUE(first->isDirectCompositeTile());
    EXPECT_TRUE(second->isDirectCompositeTile());
    EXPECT_EQ(first->getCacheKey(), second->getCacheKey());

    ASSERT_TRUE(provider.loadTile(*first));
    ASSERT_TRUE(provider.loadTile(*second));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    imagery.completeNext();

    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, first->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, second->getState());
    EXPECT_EQ(1, uploaderPtr->uploadCount);
}

TEST(RasterOverlayLifecycleTest, ConcurrentDirectCompositeTilesShareProviderSourceTileAssetLikeCesiumNative) {
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

    auto westTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, westHalf), 256.0, 512.0).tile;
    auto eastTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, eastHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, westTile);
    ASSERT_NE(nullptr, eastTile);

    ASSERT_TRUE(provider.loadTile(*westTile));
    ASSERT_TRUE(provider.loadTile(*eastTile));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    imagery.completeNext();

    EXPECT_EQ(2, processPendingUploadsUntil(provider, 2));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, westTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, eastTile->getState());
}

TEST(
    RasterOverlayLifecycleTest,
    DirectCompositeMappingsWithSharedSourceExposeOneDepotRequestLikeCesiumNative) {
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

    RasterOverlayTileProvider::RasterTileMapping westMapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            256.0,
            512.0);
    RasterOverlayTileProvider::RasterTileMapping eastMapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, eastHalf),
            256.0,
            512.0);
    ASSERT_NE(nullptr, westMapping.tile);
    ASSERT_NE(nullptr, eastMapping.tile);
    ASSERT_TRUE(westMapping.tile->isDirectCompositeTile());
    ASSERT_TRUE(eastMapping.tile->isDirectCompositeTile());
    ASSERT_EQ(1u, westMapping.sourceTiles.sourceKeys.size());
    ASSERT_EQ(1u, eastMapping.sourceTiles.sourceKeys.size());
    EXPECT_EQ(sourceKey, westMapping.sourceTiles.sourceKeys.front());
    EXPECT_EQ(sourceKey, eastMapping.sourceTiles.sourceKeys.front());
    EXPECT_NE(westMapping.tile->getCacheKey(), eastMapping.tile->getCacheKey());

    ASSERT_TRUE(provider.loadTile(*westMapping.tile));
    ASSERT_TRUE(provider.loadTile(*eastMapping.tile));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    EXPECT_EQ(1, static_cast<int>(
                     std::count(imagery.requestedKeys.begin(),
                                imagery.requestedKeys.end(),
                                sourceKey)));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));
    EXPECT_EQ(1, provider.getActiveRasterSourceRequests());
    ProviderRequestDiagnostics activeDiagnostics =
        provider.requestDiagnostics();
    EXPECT_EQ(1, activeDiagnostics.activeExternalResourceBlockingRequests);

    imagery.completeNext();
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());

    EXPECT_EQ(2, processPendingUploadsUntil(provider, 2));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              westMapping.tile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              eastMapping.tile->getState());
    ProviderRequestDiagnostics completedDiagnostics =
        provider.requestDiagnostics();
    EXPECT_EQ(0, completedDiagnostics.activeExternalResourceBlockingRequests);
    EXPECT_EQ(1, completedDiagnostics.externalResourceRequestsStarted);
    EXPECT_EQ(1, completedDiagnostics.externalResourceRequestsCompleted);
}

TEST(
    RasterOverlayLifecycleTest,
    DirectCompositeTilesJoinSharedSourceInFlightWithoutNewBudgetLikeCesiumNative) {
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

    RasterOverlayTileProvider::RasterTileMapping westMapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            256.0,
            512.0);
    RasterOverlayTileProvider::RasterTileMapping eastMapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, eastHalf),
            256.0,
            512.0);
    ASSERT_NE(nullptr, westMapping.tile);
    ASSERT_NE(nullptr, eastMapping.tile);
    ASSERT_TRUE(westMapping.tile->isDirectCompositeTile());
    ASSERT_TRUE(eastMapping.tile->isDirectCompositeTile());
    ASSERT_NE(westMapping.tile->getCacheKey(),
              eastMapping.tile->getCacheKey());
    ASSERT_EQ(1u, westMapping.sourceTiles.sourceKeys.size());
    ASSERT_EQ(1u, eastMapping.sourceTiles.sourceKeys.size());
    ASSERT_EQ(sourceKey, westMapping.sourceTiles.sourceKeys.front());
    ASSERT_EQ(sourceKey, eastMapping.sourceTiles.sourceKeys.front());

    FrameResourceBudgetConfig firstConfig;
    firstConfig.maxNetworkRequestsPerFrame = 1;
    firstConfig.maxNetworkInflight = 1;
    firstConfig.maxRasterNetworkRequestsPerFrame = 1;
    firstConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, firstConfig);

    ASSERT_TRUE(provider.loadTileThrottled(*westMapping.tile, &firstBudget));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(sourceKey, imagery.pending.front().key);
    EXPECT_EQ(1u, firstBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(1, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(1, provider.getThrottledTilesCurrentlyLoading());

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxNetworkRequestsPerFrame = 0;
    blockedConfig.maxNetworkInflight = 0;
    blockedConfig.maxRasterNetworkRequestsPerFrame = 0;
    blockedConfig.maxRasterNetworkInflight = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(2, blockedConfig);

    EXPECT_TRUE(provider.loadTileThrottled(*eastMapping.tile, &blockedBudget));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              eastMapping.tile->getState());
    EXPECT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(0u, blockedBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(1, provider.getActiveRasterSourceRequests());

    imagery.completeNext();
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(2, processPendingUploadsUntil(provider, 2));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              westMapping.tile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              eastMapping.tile->getState());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
}

TEST(
    RasterOverlayLifecycleTest,
    MixedDirectCompositeFanoutJoinsSharedSourceBeforeNewRequestBudgetLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

    const TileKey sharedSourceKey{scheme->id(), 3, 2, 3};
    const TileKey newSourceKey{scheme->id(), 3, 3, 3};
    const Rectangle sharedSourceBounds =
        scheme->tileToRectangle(sharedSourceKey);
    const Rectangle newSourceBounds = scheme->tileToRectangle(newSourceKey);
    const Rectangle sharedPatch(
        sharedSourceBounds.west(),
        sharedSourceBounds.south(),
        sharedSourceBounds.west() + sharedSourceBounds.width() * 0.5,
        sharedSourceBounds.north());
    const Rectangle mixedBounds(
        sharedSourceBounds.west(),
        sharedSourceBounds.south(),
        newSourceBounds.east(),
        sharedSourceBounds.north());

    RasterOverlayTileProvider::RasterTileMapping sharedMapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, sharedPatch),
            256.0,
            512.0);
    RasterOverlayTileProvider::RasterTileMapping mixedMapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, mixedBounds),
            512.0,
            512.0);
    ASSERT_NE(nullptr, sharedMapping.tile);
    ASSERT_NE(nullptr, mixedMapping.tile);
    ASSERT_TRUE(sharedMapping.tile->isDirectCompositeTile());
    ASSERT_TRUE(mixedMapping.tile->isDirectCompositeTile());
    ASSERT_EQ(1u, sharedMapping.sourceTiles.sourceKeys.size());
    ASSERT_EQ(sharedSourceKey, sharedMapping.sourceTiles.sourceKeys.front());
    ASSERT_EQ(2u, mixedMapping.sourceTiles.sourceKeys.size());
    EXPECT_EQ(sharedSourceKey, mixedMapping.sourceTiles.sourceKeys[0]);
    EXPECT_EQ(newSourceKey, mixedMapping.sourceTiles.sourceKeys[1]);

    FrameResourceBudgetConfig firstConfig;
    firstConfig.maxNetworkRequestsPerFrame = 1;
    firstConfig.maxNetworkInflight = 1;
    firstConfig.maxRasterNetworkRequestsPerFrame = 1;
    firstConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, firstConfig);

    ASSERT_TRUE(provider.loadTileThrottled(*sharedMapping.tile, &firstBudget));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(sharedSourceKey, imagery.pending.front().key);
    EXPECT_EQ(1u, firstBudget.rasterNetworkRequestsIssued());

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxNetworkRequestsPerFrame = 0;
    blockedConfig.maxNetworkInflight = 0;
    blockedConfig.maxRasterNetworkRequestsPerFrame = 0;
    blockedConfig.maxRasterNetworkInflight = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(2, blockedConfig);

    EXPECT_TRUE(provider.loadTileThrottled(*mixedMapping.tile, &blockedBudget));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              mixedMapping.tile->getState());
    EXPECT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(0u, blockedBudget.rasterNetworkRequestsIssued());

    imagery.completeNext();
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));

    FrameResourceBudgetConfig pumpConfig;
    pumpConfig.maxNetworkRequestsPerFrame = 1;
    pumpConfig.maxNetworkInflight = 1;
    pumpConfig.maxRasterNetworkRequestsPerFrame = 1;
    pumpConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget pumpBudget;
    pumpBudget.beginFrame(3, pumpConfig);

    EXPECT_EQ(
        1,
        provider.processPendingUploads(false, &pumpBudget).processedUploads);
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(newSourceKey, imagery.pending.front().key);
    EXPECT_EQ(1u, pumpBudget.rasterNetworkRequestsIssued());

    imagery.completeNext();
    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(1, provider.processPendingUploads(false).processedUploads);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              mixedMapping.tile->getState());
    EXPECT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(sharedSourceKey, imagery.requestedKeys[0]);
    EXPECT_EQ(newSourceKey, imagery.requestedKeys[1]);
}

TEST(
    RasterOverlayLifecycleTest,
    DirectCompositeComposeConsumesCurrentSceneWorkerGrantAndRetriesNextFrame) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

    const TileKey westKey{scheme->id(), 3, 2, 3};
    const TileKey eastKey{scheme->id(), 3, 3, 3};
    const Rectangle westBounds = scheme->tileToRectangle(westKey);
    const Rectangle eastBounds = scheme->tileToRectangle(eastKey);
    const Rectangle mixedBounds(
        westBounds.west(),
        westBounds.south(),
        eastBounds.east(),
        westBounds.north());

    SceneFrameResourceArbiter arbiter;
    SceneFrameResourceArbiterConfig deniedSceneConfig;
    deniedSceneConfig.workerDispatch.maxUnitsPerFrame = 0;
    deniedSceneConfig.gpuUpload.maxUnitsPerFrame = 1;
    arbiter.beginFrame(1, deniedSceneConfig);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        2));
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::WorkerDispatch,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::GpuUpload,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.sealAllocations());

    FrameResourceBudgetConfig localConfig;
    localConfig.maxRasterUploadsPerFrame = 1;
    FrameResourceBudget deniedBudget;
    deniedBudget.beginFrame(1, localConfig);
    deniedBudget.attachSceneArbiter(&arbiter);

    RasterOverlayTileProvider::RasterTileMapping mapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, mixedBounds),
            512.0,
            512.0);
    ASSERT_NE(nullptr, mapping.tile);
    ASSERT_TRUE(mapping.tile->isDirectCompositeTile());
    ASSERT_EQ(2u, mapping.sourceTiles.sourceKeys.size());
    ASSERT_TRUE(provider.loadTile(*mapping.tile, &deniedBudget));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              mapping.tile->getState());
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &deniedBudget)
            .processedUploads);
    EXPECT_EQ(0u, arbiter.used(
                      SceneFrameResourceProducer::Raster,
                      SceneFrameResourceStage::WorkerDispatch,
                      FrameResourcePriority::Normal));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_TRUE(provider.hasPendingWork());

    SceneFrameResourceArbiterConfig allowedSceneConfig;
    allowedSceneConfig.workerDispatch.maxUnitsPerFrame = 1;
    allowedSceneConfig.gpuUpload.maxUnitsPerFrame = 1;
    arbiter.beginFrame(2, allowedSceneConfig);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::WorkerDispatch,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::GpuUpload,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.sealAllocations());

    FrameResourceBudget allowedBudget;
    allowedBudget.beginFrame(2, localConfig);
    allowedBudget.attachSceneArbiter(&arbiter);
    provider.processPendingUploads(false, &allowedBudget);
    EXPECT_EQ(1u, arbiter.used(
                      SceneFrameResourceProducer::Raster,
                      SceneFrameResourceStage::WorkerDispatch,
                      FrameResourcePriority::Normal));
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(
        1,
        provider.processPendingUploads(false, &allowedBudget)
            .processedUploads);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              mapping.tile->getState());
}

TEST(
    RasterOverlayLifecycleTest,
    RasterSourceAcquiresSceneNetworkGrantBeforeProviderRequestSideEffect) {
    SceneFrameResourceArbiter arbiter;
    SceneFrameResourceArbiterConfig sceneConfig;
    sceneConfig.networkRequest.maxUnitsPerFrame = 1;
    arbiter.beginFrame(1, sceneConfig);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.sealAllocations());

    FrameResourceBudgetConfig localConfig;
    localConfig.maxNetworkRequestsPerFrame = 1;
    localConfig.maxNetworkInflight = 1;
    localConfig.maxRasterNetworkRequestsPerFrame = 1;
    localConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, localConfig);
    budget.attachSceneArbiter(&arbiter);

    AdmissionObservingImageryProvider imagery(arbiter);
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(
        imagery, *scheme, std::move(uploader));
    auto tile = provider.getTile(TileKey{scheme->id(), 2, 1, 1});
    ASSERT_NE(nullptr, tile);

    ASSERT_TRUE(provider.loadTile(*tile, &budget));
    ASSERT_EQ(1u, imagery.usedAtRequest.size());
    EXPECT_EQ(1u, imagery.usedAtRequest.front());
    EXPECT_EQ(1u, budget.rasterNetworkRequestsIssued());
    EXPECT_EQ(1u, arbiter.used(
                      SceneFrameResourceProducer::Raster,
                      SceneFrameResourceStage::NetworkRequest,
                      FrameResourcePriority::Normal));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
}

TEST(
    RasterOverlayLifecycleTest,
    RasterSourceSceneAdmissionDenialDoesNotCallProviderAndRetries) {
    SceneFrameResourceArbiter arbiter;
    SceneFrameResourceArbiterConfig deniedConfig;
    deniedConfig.networkRequest.maxUnitsPerFrame = 0;
    arbiter.beginFrame(1, deniedConfig);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.sealAllocations());

    FrameResourceBudgetConfig localConfig;
    localConfig.maxNetworkRequestsPerFrame = 1;
    localConfig.maxNetworkInflight = 1;
    localConfig.maxRasterNetworkRequestsPerFrame = 1;
    localConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget deniedBudget;
    deniedBudget.beginFrame(1, localConfig);
    deniedBudget.attachSceneArbiter(&arbiter);

    AdmissionObservingImageryProvider imagery(arbiter);
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    auto tile = provider.getTile(TileKey{scheme->id(), 2, 2, 1});
    ASSERT_NE(nullptr, tile);

    ASSERT_TRUE(provider.loadTile(*tile, &deniedBudget));
    EXPECT_TRUE(imagery.usedAtRequest.empty());
    EXPECT_EQ(0u, deniedBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(0u, arbiter.used(
                      SceneFrameResourceProducer::Raster,
                      SceneFrameResourceStage::NetworkRequest,
                      FrameResourcePriority::Normal));
    EXPECT_TRUE(provider.hasPendingWork());

    SceneFrameResourceArbiterConfig allowedConfig;
    allowedConfig.networkRequest.maxUnitsPerFrame = 1;
    arbiter.beginFrame(2, allowedConfig);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.sealAllocations());
    FrameResourceBudget allowedBudget;
    allowedBudget.beginFrame(2, localConfig);
    allowedBudget.attachSceneArbiter(&arbiter);

    provider.processPendingUploads(false, &allowedBudget);
    ASSERT_EQ(1u, imagery.usedAtRequest.size());
    EXPECT_EQ(1u, imagery.usedAtRequest.front());
}

TEST(
    RasterOverlayLifecycleTest,
    DirectCompositeFanoutJoinsLaterSharedSourceWhenEarlierSourceIsBudgetBlocked) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);
    provider.setSubTileCacheBytes(0);

    const TileKey newSourceKey{scheme->id(), 3, 2, 3};
    const TileKey sharedSourceKey{scheme->id(), 3, 3, 3};
    const Rectangle newSourceBounds =
        scheme->tileToRectangle(newSourceKey);
    const Rectangle sharedSourceBounds =
        scheme->tileToRectangle(sharedSourceKey);
    const Rectangle sharedPatch(
        sharedSourceBounds.west(),
        sharedSourceBounds.south(),
        sharedSourceBounds.west() + sharedSourceBounds.width() * 0.5,
        sharedSourceBounds.north());
    const Rectangle mixedBounds(
        newSourceBounds.west(),
        sharedSourceBounds.south(),
        sharedSourceBounds.east(),
        sharedSourceBounds.north());

    RasterOverlayTileProvider::RasterTileMapping sharedMapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, sharedPatch),
            256.0,
            512.0);
    RasterOverlayTileProvider::RasterTileMapping mixedMapping =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, mixedBounds),
            512.0,
            512.0);
    ASSERT_NE(nullptr, sharedMapping.tile);
    ASSERT_NE(nullptr, mixedMapping.tile);
    ASSERT_TRUE(sharedMapping.tile->isDirectCompositeTile());
    ASSERT_TRUE(mixedMapping.tile->isDirectCompositeTile());
    ASSERT_EQ(1u, sharedMapping.sourceTiles.sourceKeys.size());
    ASSERT_EQ(sharedSourceKey, sharedMapping.sourceTiles.sourceKeys.front());
    ASSERT_EQ(2u, mixedMapping.sourceTiles.sourceKeys.size());
    EXPECT_EQ(newSourceKey, mixedMapping.sourceTiles.sourceKeys[0]);
    EXPECT_EQ(sharedSourceKey, mixedMapping.sourceTiles.sourceKeys[1]);

    FrameResourceBudgetConfig firstConfig;
    firstConfig.maxNetworkRequestsPerFrame = 1;
    firstConfig.maxNetworkInflight = 1;
    firstConfig.maxRasterNetworkRequestsPerFrame = 1;
    firstConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, firstConfig);

    ASSERT_TRUE(provider.loadTileThrottled(*sharedMapping.tile, &firstBudget));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(sharedSourceKey, imagery.pending.front().key);
    EXPECT_EQ(1u, firstBudget.rasterNetworkRequestsIssued());

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxNetworkRequestsPerFrame = 0;
    blockedConfig.maxNetworkInflight = 0;
    blockedConfig.maxRasterNetworkRequestsPerFrame = 0;
    blockedConfig.maxRasterNetworkInflight = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(2, blockedConfig);

    EXPECT_TRUE(provider.loadTileThrottled(*mixedMapping.tile, &blockedBudget));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              mixedMapping.tile->getState());
    EXPECT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(0u, blockedBudget.rasterNetworkRequestsIssued());

    imagery.completeNext();
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));

    FrameResourceBudgetConfig pumpConfig;
    pumpConfig.maxNetworkRequestsPerFrame = 1;
    pumpConfig.maxNetworkInflight = 1;
    pumpConfig.maxRasterNetworkRequestsPerFrame = 1;
    pumpConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget pumpBudget;
    pumpBudget.beginFrame(3, pumpConfig);

    EXPECT_EQ(
        1,
        provider.processPendingUploads(false, &pumpBudget).processedUploads);
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(newSourceKey, imagery.pending.front().key);
    EXPECT_EQ(1u, pumpBudget.rasterNetworkRequestsIssued());

    imagery.completeNext();
    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(1, provider.processPendingUploads(false).processedUploads);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              mixedMapping.tile->getState());
    EXPECT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(sharedSourceKey, imagery.requestedKeys[0]);
    EXPECT_EQ(newSourceKey, imagery.requestedKeys[1]);
}

TEST(RasterOverlayLifecycleTest, DirectAndDirectCompositeTilesShareProviderSourceTileAssetLikeCesiumNative) {
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
    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, westHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, directTile);
    ASSERT_NE(nullptr, directCompositeTile);

    ASSERT_TRUE(provider.loadTile(*directTile));
    ASSERT_TRUE(provider.loadTile(*directCompositeTile));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    imagery.completeNext();

    EXPECT_EQ(2, processPendingUploadsUntil(provider, 2));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, directTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, directCompositeTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     ExactProviderRectangleReusesDirectQuadtreeTileLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);

    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, sourceBounds),
        512.0,
        512.0).tile;
    auto directTile = provider.getTile(sourceKey);
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_NE(nullptr, directTile);

    EXPECT_EQ(directTile.get(), directCompositeTile.get());
    EXPECT_FALSE(directCompositeTile->isDirectCompositeTile());
    EXPECT_EQ(sourceKey, directCompositeTile->getTileID());
    EXPECT_EQ(1, provider.getCachedTileCount());
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

TEST(RasterOverlayLifecycleTest, DirectCompositeTileCallbackAfterProviderDestructionIsIgnored) {
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

    auto tile = provider->mapRasterTilesToGeometryTile(
        projectForProvider(*provider, westHalf),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider->loadTile(*tile));
    ASSERT_EQ(1u, imagery.pending.size());
    provider.reset();

    imagery.completeNext();
    SUCCEED();
}

TEST(RasterOverlayLifecycleTest,
     SourceFallbackAfterProviderDestructionDoesNotRequestParent) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto provider = std::make_unique<RasterOverlayTileProvider>(
        imagery,
        *scheme,
        nullptr);
    provider->setLevelRange(0, 3);

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    imagery.failingKeys.push_back(sourceKey);
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());

    auto tile = provider->mapRasterTilesToGeometryTile(
        projectForProvider(*provider, westHalf),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider->loadTile(*tile));
    ASSERT_EQ(1u, imagery.pending.size());
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    provider.reset();
    imagery.completeNext();

    EXPECT_TRUE(imagery.pending.empty());
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
}

TEST(RasterOverlayLifecycleTest,
     SharedSourceWaitersAfterProviderDestructionResolveLikeCesiumNativeDepot) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto provider = std::make_unique<RasterOverlayTileProvider>(
        imagery,
        *scheme,
        nullptr);

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

    auto westTile = provider->mapRasterTilesToGeometryTile(
        projectForProvider(*provider, westHalf),
        256.0,
        512.0).tile;
    auto eastTile = provider->mapRasterTilesToGeometryTile(
        projectForProvider(*provider, eastHalf),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, westTile);
    ASSERT_NE(nullptr, eastTile);

    ASSERT_TRUE(provider->loadTile(*westTile));
    ASSERT_TRUE(provider->loadTile(*eastTile));
    ASSERT_EQ(1u, imagery.pending.size());
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());

    std::shared_future<void> destroyed =
        provider->getAsyncDestructionCompleteEvent();
    provider.reset();
    EXPECT_EQ(
        std::future_status::timeout,
        destroyed.wait_for(std::chrono::milliseconds(0)));

    imagery.completeNext();

    EXPECT_TRUE(imagery.pending.empty());
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    EXPECT_EQ(
        std::future_status::ready,
        destroyed.wait_for(std::chrono::seconds(1)));
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

    auto westTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, westHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, westTile);
    ASSERT_TRUE(provider.loadTile(*westTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1, static_cast<int>(imagery->requestedKeys.size()));

    auto eastTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, eastHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, eastTile);
    ASSERT_TRUE(provider.loadTile(*eastTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(2, static_cast<int>(imagery->requestedKeys.size()));
    EXPECT_EQ(sourceKey, imagery->requestedKeys[0]);
    EXPECT_EQ(sourceKey, imagery->requestedKeys[1]);
}

TEST(RasterOverlayLifecycleTest, SourceTileDepotUsesRuntimeSubTileCacheBudget) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

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
    const Rectangle centerQuarter(
        sourceBounds.west() + sourceBounds.width() * 0.25,
        sourceBounds.south() + sourceBounds.height() * 0.25,
        sourceBounds.west() + sourceBounds.width() * 0.75,
        sourceBounds.south() + sourceBounds.height() * 0.75);

    auto westTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, westTile);
    ASSERT_TRUE(provider.loadTile(*westTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1, static_cast<int>(imagery.requestedKeys.size()));

    provider.setSubTileCacheBytes(0);

    auto eastTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, eastHalf),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, eastTile);
    ASSERT_TRUE(provider.loadTile(*eastTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(2, static_cast<int>(imagery.requestedKeys.size()));

    auto centerTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, centerQuarter),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, centerTile);
    ASSERT_TRUE(provider.loadTile(*centerTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(3, static_cast<int>(imagery.requestedKeys.size()));
    EXPECT_EQ(sourceKey, imagery.requestedKeys[0]);
    EXPECT_EQ(sourceKey, imagery.requestedKeys[1]);
    EXPECT_EQ(sourceKey, imagery.requestedKeys[2]);
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeCacheKeyIgnoresSubNanometerRectangleNoise) {
    ParentFallbackImageryProvider imagery;
    imagery.schemeIdValue = "Geographic-TMS";
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(3, 3);

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    const double noise = 2e-16;
    const Rectangle noisyWestHalf(
        westHalf.west() + noise,
        westHalf.south() - noise,
        westHalf.east() + noise,
        westHalf.north() - noise);

    auto stableTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        256.0,
        512.0).tile;
    auto noisyTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, noisyWestHalf),
        256.0,
        512.0).tile;

    ASSERT_NE(nullptr, stableTile);
    EXPECT_EQ(stableTile, noisyTile);
}

TEST(RasterOverlayLifecycleTest,
     TerminalFailureSourceAssetsRespectSubTileCacheBudget) {
    AlwaysFailingImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(3, 3);
    provider.setSubTileCacheBytes(1);

    const TileKey firstSourceKey{scheme->id(), 3, 2, 3};
    const Rectangle firstBounds = scheme->tileToRectangle(firstSourceKey);
    const Rectangle firstWestHalf(
        firstBounds.west(),
        firstBounds.south(),
        firstBounds.west() + firstBounds.width() * 0.5,
        firstBounds.north());
    const Rectangle firstEastHalf(
        firstBounds.west() + firstBounds.width() * 0.5,
        firstBounds.south(),
        firstBounds.east(),
        firstBounds.north());
    const TileKey secondSourceKey{scheme->id(), 3, 4, 3};
    const Rectangle secondBounds = scheme->tileToRectangle(secondSourceKey);

    auto firstTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, firstWestHalf),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    provider.processPendingUploads(false);
    EXPECT_EQ(1, provider.getCachedSourceTileCount());
    EXPECT_EQ(1, provider.getCachedSourceTileBytes());
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(firstSourceKey, imagery.requestedKeys.back());

    auto secondTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, secondBounds),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(provider.loadTile(*secondTile));
    provider.processPendingUploads(false);
    EXPECT_EQ(1, provider.getCachedSourceTileCount());
    EXPECT_EQ(1, provider.getCachedSourceTileBytes());
    ASSERT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(secondSourceKey, imagery.requestedKeys.back());

    auto firstAgainTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, firstEastHalf),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, firstAgainTile);
    ASSERT_TRUE(provider.loadTile(*firstAgainTile));
    provider.processPendingUploads(false);
    EXPECT_EQ(1, provider.getCachedSourceTileCount());
    EXPECT_EQ(1, provider.getCachedSourceTileBytes());
    ASSERT_EQ(3u, imagery.requestedKeys.size());
    EXPECT_EQ(firstSourceKey, imagery.requestedKeys.back());
}

TEST(RasterOverlayLifecycleTest,
     CachedSourceReuseCompactsStaleLruEntries) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);
    provider.setSubTileCacheBytes(4096);

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);

    for (int i = 0; i < 64; ++i) {
        const double westT = 0.05 + 0.004 * static_cast<double>(i % 8);
        const double southT = 0.05 + 0.02 * static_cast<double>(i / 8);
        const Rectangle patch(
            sourceBounds.west() + sourceBounds.width() * westT,
            sourceBounds.south() + sourceBounds.height() * southT,
            sourceBounds.west() + sourceBounds.width() * (westT + 0.20),
            sourceBounds.south() + sourceBounds.height() * (southT + 0.20));

        auto tile = provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, patch),
            128.0,
            128.0).tile;
        ASSERT_NE(nullptr, tile);
        ASSERT_TRUE(tile->isDirectCompositeTile());
        ASSERT_TRUE(provider.loadTile(*tile));
        ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    }

    EXPECT_EQ(1, imagery.requestCount);
    EXPECT_EQ(1, provider.getCachedSourceTileCount());
    EXPECT_LE(provider.getCachedSourceTileLruEntryCount(), 33);
}

TEST(RasterOverlayLifecycleTest,
     PendingUploadBudgetEvictionCompactsStaleSourceLruMetadata) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);
    provider.setSubTileCacheBytes(512 * 1024);

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);

    for (int i = 0; i < 64; ++i) {
        const double westT = 0.05 + 0.004 * static_cast<double>(i % 8);
        const double southT = 0.05 + 0.02 * static_cast<double>(i / 8);
        const Rectangle patch(
            sourceBounds.west() + sourceBounds.width() * westT,
            sourceBounds.south() + sourceBounds.height() * southT,
            sourceBounds.west() + sourceBounds.width() * (westT + 0.20),
            sourceBounds.south() + sourceBounds.height() * (southT + 0.20));

        auto tile = provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, patch),
            128.0,
            128.0).tile;
        ASSERT_NE(nullptr, tile);
        ASSERT_TRUE(tile->isDirectCompositeTile());
        ASSERT_TRUE(provider.loadTile(*tile));
        if (!imagery.pending.empty()) {
            imagery.completeNext();
        }
        ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
        ASSERT_TRUE(waitForNoPendingWork(provider));
    }

    EXPECT_EQ(1, provider.getCachedSourceTileCount());
    ASSERT_GT(provider.getCachedSourceTileLruEntryCount(), 1);

    const Rectangle directWestHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());
    auto directTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, directWestHalf),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, directTile);
    ASSERT_TRUE(directTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*directTile));
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
    const int64_t pendingBytes = provider.getPendingUploadBytes();
    ASSERT_GT(pendingBytes, 0);

    provider.setSubTileCacheBytes(pendingBytes);

    EXPECT_EQ(0, provider.getCachedSourceTileCount());
    EXPECT_EQ(0, provider.getCachedSourceTileBytes());
    EXPECT_LE(provider.getCachedSourceTileLruEntryCount(), 32);
}

TEST(RasterOverlayLifecycleTest,
     SourceTileDepotRetriesRequestExceptionsLikeCesiumNativeSharedAssetDepot) {
    ThrowOnceImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(3, 3);

    const TileKey sourceKey{scheme->id(), 3, 2, 3};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const Rectangle firstPatch(
        sourceBounds.west() + sourceBounds.width() * 0.10,
        sourceBounds.south() + sourceBounds.height() * 0.10,
        sourceBounds.west() + sourceBounds.width() * 0.30,
        sourceBounds.south() + sourceBounds.height() * 0.30);
    const Rectangle secondPatch(
        sourceBounds.west() + sourceBounds.width() * 0.60,
        sourceBounds.south() + sourceBounds.height() * 0.60,
        sourceBounds.west() + sourceBounds.width() * 0.80,
        sourceBounds.south() + sourceBounds.height() * 0.80);

    auto failingTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, firstPatch),
        64.0,
        64.0).tile;
    ASSERT_NE(nullptr, failingTile);
    ASSERT_TRUE(provider.loadTile(*failingTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              failingTile->getState());
    EXPECT_EQ(nullptr, failingTile->getTexture());
    ASSERT_EQ(1u, failingTile->loadDiagnostics().size());
    EXPECT_EQ("Raster source tile request threw before completion",
              failingTile->loadDiagnostics().front());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.back());
    EXPECT_EQ(0, provider.getCachedSourceTileCount());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
    EXPECT_TRUE(waitForNoPendingWork(provider));

    auto retryTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, secondPatch),
        64.0,
        64.0).tile;
    ASSERT_NE(nullptr, retryTile);
    ASSERT_TRUE(provider.loadTile(*retryTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.back());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              retryTile->getState());
    EXPECT_EQ(1, provider.getCachedSourceTileCount());
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_TRUE(retryTile->loadDiagnostics().empty());
    EXPECT_TRUE(waitForNoPendingWork(provider));
}

TEST(RasterOverlayLifecycleTest, QuadtreeSourceFallbacksRetryChildAndReuseCachedParentLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey failingKey{scheme->id(), 3, 2, 3};
    imagery.failingKey = failingKey;
    const Rectangle bounds = scheme->tileToRectangle(failingKey);

    auto firstTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, bounds), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
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
    auto secondTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, innerBounds), 256.0, 256.0).tile;
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(provider.loadTile(*secondTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(requestsAfterFirstLoad + 1,
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

    auto firstTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, westHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(provider.loadTile(*firstTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              firstTile->getState());
    EXPECT_EQ(nullptr, firstTile->getTexture());
    const int parentRequestsAfterFirstLoad =
        static_cast<int>(std::count(
            imagery.requestedKeys.begin(),
            imagery.requestedKeys.end(),
            parent));
    EXPECT_EQ(1, parentRequestsAfterFirstLoad);

    auto secondTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, eastHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(provider.loadTile(*secondTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

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
    provider.setLevelRange(0, 3);

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

    auto westTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, westHalf), 256.0, 512.0).tile;
    auto eastTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, eastHalf), 256.0, 512.0).tile;
    ASSERT_NE(nullptr, westTile);
    ASSERT_NE(nullptr, eastTile);

    ASSERT_TRUE(provider.loadTile(*westTile));
    ASSERT_TRUE(provider.loadTile(*eastTile));
    ASSERT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(westChild, imagery.pending[0].key);
    EXPECT_EQ(eastChild, imagery.pending[1].key);

    imagery.completeNext();
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(eastChild, imagery.pending[0].key);
    EXPECT_EQ(0, provider.processPendingUploads(false).processedUploads);
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
    EXPECT_EQ(2, processPendingUploadsUntil(provider, 2));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, westTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, eastTile->getState());
    EXPECT_EQ(nullptr, westTile->getTexture());
    EXPECT_EQ(nullptr, eastTile->getTexture());
    EXPECT_TRUE(waitForNoPendingWork(provider));

    const size_t requestsAfterConcurrentFallback = imagery.requestedKeys.size();
    const Rectangle eastInner(
        eastBounds.west() + eastBounds.width() * 0.25,
        eastBounds.south() + eastBounds.height() * 0.25,
        eastBounds.east() - eastBounds.width() * 0.25,
        eastBounds.north() - eastBounds.height() * 0.25);
    auto repeatedEastTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, eastInner),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, repeatedEastTile);
    ASSERT_TRUE(provider.loadTile(*repeatedEastTile));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(eastChild, imagery.pending.front().key);
    imagery.completeNext();
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(requestsAfterConcurrentFallback + 1,
              imagery.requestedKeys.size());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              repeatedEastTile->getState());
    EXPECT_EQ(nullptr, repeatedEastTile->getTexture());
    EXPECT_TRUE(waitForNoPendingWork(provider));
}

TEST(RasterOverlayLifecycleTest,
     QueuedSiblingFallbackJoinsParentInFlightWithoutAdditionalBudget) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(0, 3);

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

    auto westTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        256.0,
        512.0).tile;
    auto eastTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, eastHalf),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, westTile);
    ASSERT_NE(nullptr, eastTile);

    ASSERT_TRUE(provider.loadTile(*westTile));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(westChild, imagery.pending.front().key);

    imagery.completeNext();
    FrameResourceBudgetConfig fallbackConfig;
    fallbackConfig.maxNetworkRequestsPerFrame = 20;
    fallbackConfig.maxNetworkInflight = 20;
    fallbackConfig.maxRasterNetworkRequestsPerFrame = 1;
    fallbackConfig.maxRasterNetworkInflight = 2;
    FrameResourceBudget fallbackBudget;
    fallbackBudget.beginFrame(1, fallbackConfig);
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &fallbackBudget)
            .processedUploads);
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(parent, imagery.pending.front().key);

    ASSERT_TRUE(provider.loadTile(*eastTile));
    ASSERT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(eastChild, imagery.pending.back().key);

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxNetworkRequestsPerFrame = 0;
    blockedConfig.maxNetworkInflight = 0;
    blockedConfig.maxRasterNetworkRequestsPerFrame = 0;
    blockedConfig.maxRasterNetworkInflight = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(2, blockedConfig);

    imagery.completeKey(eastChild);
    EXPECT_EQ(1, provider.getPendingSourceFallbackCount());
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &blockedBudget)
            .processedUploads);
    EXPECT_EQ(0, provider.getPendingSourceFallbackCount());
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(parent, imagery.pending.front().key);

    imagery.completeNext();
    EXPECT_EQ(2, processPendingUploadsUntil(provider, 2));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, westTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, eastTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     ParentFallbackSourceRequestsArePumpedByFrameBudget) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(0, 3);

    const TileKey child{scheme->id(), 3, 2, 2};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {child};

    auto tile = provider.getTile(child);
    ASSERT_NE(nullptr, tile);
    ASSERT_FALSE(tile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 8;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);
    ASSERT_TRUE(provider.loadTileThrottled(*tile, &firstBudget));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(child, imagery.pending.front().key);
    EXPECT_EQ(1u, firstBudget.rasterNetworkRequestsIssued());

    imagery.completeNext();
    EXPECT_TRUE(imagery.pending.empty());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(child, imagery.requestedKeys.front());
    EXPECT_TRUE(provider.hasPendingWork());

    FrameResourceBudget blockedBudget;
    config.maxNetworkRequestsPerFrame = 0;
    config.maxNetworkInflight = 0;
    config.maxRasterNetworkRequestsPerFrame = 0;
    config.maxRasterNetworkInflight = 0;
    blockedBudget.beginFrame(2, config);
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &blockedBudget)
            .processedUploads);
    EXPECT_TRUE(imagery.pending.empty());
    EXPECT_EQ(1u, imagery.requestedKeys.size());

    FrameResourceBudget fallbackBudget;
    config.maxNetworkRequestsPerFrame = 20;
    config.maxNetworkInflight = 20;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 8;
    fallbackBudget.beginFrame(3, config);
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &fallbackBudget)
            .processedUploads);
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(parent, imagery.pending.front().key);
    EXPECT_EQ(2u, imagery.requestedKeys.size());
    EXPECT_EQ(parent, imagery.requestedKeys.back());
    EXPECT_EQ(1u, fallbackBudget.rasterNetworkRequestsIssued());

    imagery.completeNext();
    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(1, provider.processPendingUploads(false).processedUploads);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              tile->isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest,
     DirectParentFallbackSurvivesDirectCompositeCacheInvalidation) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));
    provider.setLevelRange(0, 3);

    const TileKey child{scheme->id(), 3, 2, 2};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {child};

    auto tile = provider.getTile(child);
    ASSERT_NE(nullptr, tile);
    ASSERT_FALSE(tile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 8;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);
    ASSERT_TRUE(provider.loadTileThrottled(*tile, &firstBudget));
    ASSERT_EQ(1u, imagery.pending.size());

    imagery.completeNext();
    EXPECT_TRUE(imagery.pending.empty());
    EXPECT_EQ(1, provider.getPendingSourceFallbackCount());

    FrameResourceBudget blockedBudget;
    config.maxNetworkRequestsPerFrame = 0;
    config.maxNetworkInflight = 0;
    config.maxRasterNetworkRequestsPerFrame = 0;
    config.maxRasterNetworkInflight = 0;
    blockedBudget.beginFrame(2, config);
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &blockedBudget)
            .processedUploads);
    EXPECT_EQ(1, provider.getPendingSourceFallbackCount());

    provider.setMaximumScreenSpaceError(
        provider.getMaximumScreenSpaceError() * 2.0);
    EXPECT_EQ(1, provider.getPendingSourceFallbackCount());

    FrameResourceBudget fallbackBudget;
    config.maxNetworkRequestsPerFrame = 20;
    config.maxNetworkInflight = 20;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 8;
    fallbackBudget.beginFrame(3, config);
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &fallbackBudget)
            .processedUploads);
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(parent, imagery.pending.front().key);
    EXPECT_EQ(0, provider.getPendingSourceFallbackCount());

    imagery.completeNext();
    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(1, provider.processPendingUploads(false).processedUploads);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded, tile->getState());
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeInvalidationDetachesIssuedParentFallbackWaiters) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(0, 3);

    const TileKey child{scheme->id(), 3, 2, 2};
    const TileKey parent{scheme->id(), 2, 1, 1};
    imagery.failingKeys = {child};

    const Rectangle childBounds = scheme->tileToRectangle(child);
    const Rectangle westHalf(
        childBounds.west(),
        childBounds.south(),
        childBounds.west() + childBounds.width() * 0.5,
        childBounds.north());

    auto staleTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        256.0,
        512.0).tile;
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(staleTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*staleTile));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(child, imagery.pending.front().key);

    imagery.completeNext();
    FrameResourceBudgetConfig fallbackConfig;
    fallbackConfig.maxNetworkRequestsPerFrame = 20;
    fallbackConfig.maxNetworkInflight = 20;
    fallbackConfig.maxRasterNetworkRequestsPerFrame = 1;
    fallbackConfig.maxRasterNetworkInflight = 8;
    FrameResourceBudget fallbackBudget;
    fallbackBudget.beginFrame(1, fallbackConfig);
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &fallbackBudget)
            .processedUploads);
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(parent, imagery.pending.front().key);
    EXPECT_GE(provider.getInFlightSourceWaiterCount(), 1);

    provider.setMaximumScreenSpaceError(
        provider.getMaximumScreenSpaceError() * 2.0);

    EXPECT_EQ(0, provider.getInFlightSourceWaiterCount());

    imagery.completeNext();
    EXPECT_EQ(0, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_FALSE(provider.hasPendingWork());
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

    auto directTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, bounds), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, directTile);
    EXPECT_FALSE(directTile->isDirectCompositeTile());
    EXPECT_EQ(key, directTile->getTileID());

    ASSERT_TRUE(provider.loadTile(*directTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

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

TEST(RasterOverlayLifecycleTest, CompositeImageMixesSourceLevelsAfterFailureLikeCesiumNative) {
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

    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, tileBounds), 1024.0, 1024.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    EXPECT_EQ(expectedSourceZoom, directCompositeTile->getDirectCompositeSourceZoom());

    ASSERT_TRUE(provider.loadTile(*directCompositeTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    ASSERT_EQ(RasterOverlayTile::LoadState::Loaded,
              directCompositeTile->getState());
    ASSERT_EQ(1, uploaderPtr->uploadCount);
    const DecodedImage& image = uploaderPtr->lastUpload;
    ASSERT_GT(image.width, 0);
    ASSERT_GT(image.height, 0);
    ASSERT_GE(image.pixels.size(),
              static_cast<size_t>(image.width) *
                  static_cast<size_t>(image.height) * 4u);

    bool allPixelsCameFromExpectedLevels = true;
    bool hasParentLevelPixel = false;
    bool hasSourceLevelPixel = false;
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        const bool cameFromExpectedLevel =
            image.pixels[i] == expectedSourceZoom - 1 ||
            image.pixels[i] == expectedSourceZoom;
        allPixelsCameFromExpectedLevels =
            allPixelsCameFromExpectedLevels && cameFromExpectedLevel;
        hasParentLevelPixel = hasParentLevelPixel ||
                              image.pixels[i] == expectedSourceZoom - 1;
        hasSourceLevelPixel = hasSourceLevelPixel ||
                              image.pixels[i] == expectedSourceZoom;
    }

    EXPECT_TRUE(allPixelsCameFromExpectedLevels);
    EXPECT_TRUE(hasParentLevelPixel);
    EXPECT_TRUE(hasSourceLevelPixel);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Yes,
              directCompositeTile->isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, SourcePlanRequestsUnsupportedChildForParentFallbackLikeCesiumNative) {
    UnsupportedParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const int expectedSourceZoom = 8;
    const TileKey childKey{scheme->id(), expectedSourceZoom, 137, 92};
    const TileKey siblingKey{scheme->id(), expectedSourceZoom, 138, 92};
    const TileKey parentKey = childKey.parent();
    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const Rectangle siblingBounds = scheme->tileToRectangle(siblingKey);
    const Rectangle targetBounds(
        childBounds.west(),
        childBounds.south(),
        siblingBounds.east(),
        childBounds.north());

    imagery.unsupportedChildKey = childKey;
    imagery.unsupportedParentKey = parentKey;

    auto directCompositeTile = provider
        .mapRasterTilesToGeometryTile(
            projectForProvider(provider, targetBounds),
            1024.0,
            512.0)
        .tile;
    ASSERT_NE(nullptr, directCompositeTile);
    EXPECT_EQ(expectedSourceZoom, directCompositeTile->getDirectCompositeSourceZoom());

    ASSERT_TRUE(provider.loadTile(*directCompositeTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_NE(
        imagery.requestedKeys.end(),
        std::find(imagery.requestedKeys.begin(),
                  imagery.requestedKeys.end(),
                  childKey));
    EXPECT_NE(
        imagery.requestedKeys.end(),
        std::find(imagery.requestedKeys.begin(),
                  imagery.requestedKeys.end(),
                  parentKey));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directCompositeTile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Yes,
              directCompositeTile->isMoreDetailAvailable());
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    ASSERT_FALSE(uploaderPtr->lastUpload.pixels.empty());
    EXPECT_NE(nullptr, directCompositeTile->getTexture());
}

TEST(RasterOverlayLifecycleTest, DirectCompositeTileWithOnlyAncestorFallbackLoadsWithoutTextureLikeCesiumNative) {
    ParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const int targetLevel = 3;
    const TileKey westKey{scheme->id(), targetLevel, 2, 2};
    const TileKey eastKey{scheme->id(), targetLevel, 3, 2};
    const Rectangle westBounds = scheme->tileToRectangle(westKey);
    const Rectangle eastBounds = scheme->tileToRectangle(eastKey);
    const Rectangle targetBounds(
        westBounds.west(),
        westBounds.south(),
        eastBounds.east(),
        westBounds.north());

    auto directCompositeTile = provider
        .mapRasterTilesToGeometryTile(
            projectForProvider(provider, targetBounds),
            2048.0,
            1024.0)
        .tile;
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());
    const int sourceZoom = directCompositeTile->getDirectCompositeSourceZoom();
    ASSERT_GT(sourceZoom, 0);
    imagery.failAtOrAboveLevel = sourceZoom;

    ASSERT_TRUE(provider.loadTile(*directCompositeTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directCompositeTile->getState());
    EXPECT_EQ(nullptr, directCompositeTile->getTexture());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              directCompositeTile->isMoreDetailAvailable());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
    EXPECT_TRUE(std::any_of(
        imagery.requestedKeys.begin(),
              imagery.requestedKeys.end(),
        [sourceZoom](const TileKey& key) {
            return key.z == sourceZoom - 1;
        }));
}

TEST(RasterOverlayLifecycleTest, DirectCompositeTileWithAllSourcesFailedLoadsWithoutTextureLikeCesiumNative) {
    AlwaysFailingImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto directCompositeTile = provider
        .mapRasterTilesToGeometryTile(
            projectForProvider(provider, rootBounds),
            1024.0,
            1024.0)
        .tile;
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());

    ASSERT_TRUE(provider.loadTile(*directCompositeTile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directCompositeTile->getState());
    EXPECT_EQ(nullptr, directCompositeTile->getTexture());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              directCompositeTile->isMoreDetailAvailable());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
    EXPECT_FALSE(imagery.requestedKeys.empty());
    EXPECT_TRUE(std::any_of(
        imagery.requestedKeys.begin(),
        imagery.requestedKeys.end(),
        [](const TileKey& key) {
            return key.z == 0;
        }));
}

TEST(RasterOverlayLifecycleTest,
     ProviderDestructionFutureWaitsForLateRasterCallbacksLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    std::shared_future<void> destroyed;
    {
        auto provider = std::make_unique<RasterOverlayTileProvider>(
            imagery,
            *scheme);
        provider->setReady(true);
        RasterOverlayTileProvider::TilePtr tile =
            provider->getTile(TileKey{"XYZ-WebMercator", 1, 0, 0});
        ASSERT_NE(nullptr, tile);
        ASSERT_TRUE(provider->loadTile(*tile));
        ASSERT_EQ(1u, imagery.pending.size());

        destroyed = provider->getAsyncDestructionCompleteEvent();
        EXPECT_EQ(
            std::future_status::timeout,
            destroyed.wait_for(std::chrono::milliseconds(0)));
    }

    EXPECT_EQ(
        std::future_status::timeout,
        destroyed.wait_for(std::chrono::milliseconds(0)));
    imagery.completeNext();
    EXPECT_EQ(
        std::future_status::ready,
        destroyed.wait_for(std::chrono::seconds(1)));
}

TEST(RasterOverlayLifecycleTest,
     ProviderDestructionDoesNotBlockWaitingForQueuedComposeLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    const size_t workerCount = AsyncSystem::pool().threadCount();
    ASSERT_GT(workerCount, 0u);

    std::promise<void> releaseWorkersPromise;
    std::shared_future<void> releaseWorkers =
        releaseWorkersPromise.get_future().share();
    struct WorkerReleaseGuard {
        std::promise<void>& promise;
        bool released = false;
        ~WorkerReleaseGuard() {
            if (!released) {
                promise.set_value();
            }
        }
        void release() {
            if (!released) {
                promise.set_value();
                released = true;
            }
        }
    } releaseGuard{releaseWorkersPromise};
    std::atomic<size_t> startedWorkers{0};
    std::vector<std::future<void>> blockers;
    blockers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        blockers.push_back(AsyncSystem::pool().enqueue(
            [releaseWorkers, &startedWorkers]() {
                startedWorkers.fetch_add(1, std::memory_order_release);
                releaseWorkers.wait();
            }));
    }
    for (int attempt = 0; attempt < 200 &&
         startedWorkers.load(std::memory_order_acquire) < workerCount;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(workerCount, startedWorkers.load(std::memory_order_acquire));

    DeferredImageryProvider imagery;
    std::shared_future<void> destroyed;
    {
        auto provider = std::make_unique<RasterOverlayTileProvider>(
            imagery,
            *scheme);
        provider->setReady(true);
        provider->setLevelRange(8, 8);

        const TileKey centerKey =
            scheme->positionToTile(0.1, 0.2, 8);
        const Rectangle centerBounds = scheme->tileToRectangle(centerKey);
        const Rectangle spanningFourTiles(
            centerBounds.west() - centerBounds.width() * 0.5,
            centerBounds.south() - centerBounds.height() * 0.5,
            centerBounds.west() + centerBounds.width() * 0.5,
            centerBounds.south() + centerBounds.height() * 0.5);
        RasterOverlayTileProvider::RasterTileMapping mapping =
            provider->mapRasterTilesToGeometryTile(
                projectForProvider(*provider, spanningFourTiles),
                1024.0,
                1024.0);

        ASSERT_NE(nullptr, mapping.tile);
        ASSERT_TRUE(mapping.tile->isDirectCompositeTile());
        ASSERT_EQ(4u, mapping.sourceTiles.sourceKeys.size());
        ASSERT_TRUE(provider->loadTile(*mapping.tile));
        ASSERT_EQ(4u, imagery.pending.size());

        destroyed = provider->getAsyncDestructionCompleteEvent();
        for (int i = 0; i < 4; ++i) {
            imagery.completeNext();
        }
        EXPECT_EQ(
            std::future_status::timeout,
            destroyed.wait_for(std::chrono::milliseconds(0)));

        const auto beforeDestroy = std::chrono::steady_clock::now();
        provider.reset();
        const auto destroyElapsed =
            std::chrono::steady_clock::now() - beforeDestroy;
        EXPECT_LT(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                destroyElapsed)
                .count(),
            100);
    }

    EXPECT_EQ(
        std::future_status::timeout,
        destroyed.wait_for(std::chrono::milliseconds(0)));
    releaseGuard.release();
    for (auto& blocker : blockers) {
        blocker.get();
    }
    EXPECT_EQ(
        std::future_status::ready,
        destroyed.wait_for(std::chrono::seconds(1)));
}

TEST(RasterOverlayLifecycleTest,
     DeferredUploadReleaseKeepsBackpressureAndDestructionLifetime) {
    auto scheme = TileScheme::createXYZWebMercator();
    const size_t workerCount = AsyncSystem::pool().threadCount();
    ASSERT_GT(workerCount, 0u);

    std::promise<void> releaseWorkersPromise;
    std::shared_future<void> releaseWorkers =
        releaseWorkersPromise.get_future().share();
    struct WorkerReleaseGuard {
        std::promise<void>& promise;
        bool released = false;
        ~WorkerReleaseGuard() {
            if (!released) {
                promise.set_value();
            }
        }
        void release() {
            if (!released) {
                promise.set_value();
                released = true;
            }
        }
    } releaseGuard{releaseWorkersPromise};
    std::atomic<size_t> startedWorkers{0};
    std::vector<std::future<void>> blockers;
    blockers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        blockers.push_back(AsyncSystem::pool().enqueue(
            [releaseWorkers, &startedWorkers]() {
                startedWorkers.fetch_add(1, std::memory_order_release);
                releaseWorkers.wait();
            }));
    }
    for (int attempt = 0; attempt < 200 &&
         startedWorkers.load(std::memory_order_acquire) < workerCount;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(workerCount, startedWorkers.load(std::memory_order_acquire));

    DeferredImageryProvider imagery;
    std::shared_future<void> destroyed;
    {
        auto uploader = std::make_unique<CountingRasterUploader>();
        auto provider = std::make_unique<RasterOverlayTileProvider>(
            imagery,
            *scheme,
            std::move(uploader));
        provider->setReady(true);

        auto firstTile =
            provider->getTile(TileKey{scheme->id(), 2, 1, 1});
        ASSERT_NE(nullptr, firstTile);
        ASSERT_TRUE(provider->loadTileThrottled(*firstTile));
        ASSERT_EQ(1u, imagery.pending.size());
        imagery.completeNext();
        ASSERT_EQ(1, waitForPendingUploadCount(*provider, 1));

        const int64_t pendingBudgetBytes =
            provider->getPendingUploadBudgetBytes();
        ASSERT_GT(pendingBudgetBytes, 0);
        ASSERT_EQ(
            1,
            provider->processPendingUploads(false).processedUploads);
        EXPECT_EQ(0, provider->getPendingUploadCount());
        EXPECT_EQ(
            pendingBudgetBytes,
            provider->getPendingUploadBudgetBytes());
        EXPECT_TRUE(provider->hasPendingWork());

        provider->setReady(false);
        EXPECT_EQ(
            pendingBudgetBytes,
            provider->getPendingUploadBudgetBytes());
        provider->setReady(true);
        provider->setSubTileCacheBytes(pendingBudgetBytes);
        auto secondTile =
            provider->getTile(TileKey{scheme->id(), 2, 2, 1});
        ASSERT_NE(nullptr, secondTile);
        EXPECT_FALSE(provider->loadTileThrottled(*secondTile));
        EXPECT_EQ(1u, imagery.requestedKeys.size());

        destroyed = provider->getAsyncDestructionCompleteEvent();
        provider.reset();
        EXPECT_EQ(
            std::future_status::timeout,
            destroyed.wait_for(std::chrono::milliseconds(0)));
    }

    releaseGuard.release();
    for (auto& blocker : blockers) {
        blocker.get();
    }
    EXPECT_EQ(
        std::future_status::ready,
        destroyed.wait_for(std::chrono::seconds(1)));
}

TEST(RasterOverlayLifecycleTest, QuadtreeSourceFailureDoesNotFallbackBelowOverlayMinimumLevel) {
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

    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, tileBounds), 1024.0, 1024.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    EXPECT_EQ(expectedSourceZoom, directCompositeTile->getDirectCompositeSourceZoom());

    EXPECT_TRUE(provider.loadTile(*directCompositeTile));

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

TEST(RasterOverlayLifecycleTest, DirectCompositeTilesShareInFlightSourceTileLikeCesiumNativeDepot) {
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
        provider.mapRasterTilesToGeometryTile(projectForProvider(provider, westHalf), 128.0, 128.0).tile;
    RasterOverlayTileProvider::TilePtr secondTile =
        provider.mapRasterTilesToGeometryTile(projectForProvider(provider, eastHalf), 128.0, 128.0).tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(firstTile->isDirectCompositeTile());
    ASSERT_TRUE(secondTile->isDirectCompositeTile());

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
    EXPECT_TRUE(provider.hasPendingWork());

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, config);
    ASSERT_TRUE(provider.loadTileThrottled(*secondTile, &secondBudget));
    EXPECT_EQ(0u, secondBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(1, provider.getActiveRasterSourceRequests());
    EXPECT_TRUE(provider.hasPendingWork());

    imagery.completeNext();
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(2, waitForPendingUploadCount(provider, 2));
    EXPECT_TRUE(provider.hasPendingWork());
    EXPECT_EQ(2, processPendingUploadsUntil(provider, 2));
    EXPECT_TRUE(waitForNoPendingWork(provider));
}

TEST(RasterOverlayLifecycleTest, FailedSourceTileIsSharedLikeCesiumNativeDepot) {
    DeferredParentFallbackImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(3, 3);

    const TileKey sourceKey{scheme->id(), 3, 4, 3};
    imagery.failingKeys.push_back(sourceKey);
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
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, westHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(firstTile->isDirectCompositeTile());

    FrameResourceBudgetConfig firstConfig;
    firstConfig.maxNetworkRequestsPerFrame = 1;
    firstConfig.maxRasterNetworkRequestsPerFrame = 1;
    firstConfig.maxNetworkInflight = 1;
    firstConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, firstConfig);

    ASSERT_TRUE(provider.loadTileThrottled(*firstTile, &firstBudget));
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    EXPECT_EQ(1u, firstBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(1, provider.getActiveRasterSourceRequests());

    imagery.completeNext();
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              firstTile->getState());
    EXPECT_EQ(nullptr, firstTile->getTexture());
    const ProviderRequestDiagnostics firstFailureDiagnostics =
        provider.requestDiagnostics();
    EXPECT_EQ(1, firstFailureDiagnostics.externalResourceRequestsFailed);
    EXPECT_EQ(1, firstFailureDiagnostics.externalResourceRequestsCompleted);

    RasterOverlayTileProvider::TilePtr secondTile =
        provider.mapRasterTilesToGeometryTile(
            projectForProvider(provider, eastHalf),
            128.0,
            128.0)
            .tile;
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(secondTile->isDirectCompositeTile());

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxNetworkRequestsPerFrame = 0;
    blockedConfig.maxRasterNetworkRequestsPerFrame = 0;
    blockedConfig.maxNetworkInflight = 0;
    blockedConfig.maxRasterNetworkInflight = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(2, blockedConfig);

    ASSERT_TRUE(provider.loadTileThrottled(*secondTile, &blockedBudget));
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_TRUE(imagery.pending.empty());
    EXPECT_EQ(0u, blockedBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              secondTile->getState());
    EXPECT_EQ(nullptr, secondTile->getTexture());
    const ProviderRequestDiagnostics cachedFailureDiagnostics =
        provider.requestDiagnostics();
    EXPECT_EQ(1, cachedFailureDiagnostics.externalResourceRequestsFailed);
    EXPECT_EQ(1, cachedFailureDiagnostics.externalResourceRequestsCompleted);
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

    auto tile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, bounds), 512.0, 512.0).tile;
    ASSERT_NE(nullptr, tile);
    EXPECT_FALSE(tile->isDirectCompositeTile());
    EXPECT_EQ(key, tile->getTileID());

    ASSERT_TRUE(provider.loadTile(*tile));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));

    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              tile->getState());
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              tile->isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest, FailedRasterTilesFollowCesiumNativeDepotFailureSemantics) {
    NullImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    auto tile = provider.getTile(TileKey{scheme->id(), 2, 1, 1});
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));
    EXPECT_EQ(0, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    EXPECT_EQ(nullptr, tile->getTexture());
    const int failedTileRequests = imagery.requestCount;

    EXPECT_TRUE(provider.loadTile(*tile));
    EXPECT_TRUE(provider.loadTileThrottled(*tile, nullptr));
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    EXPECT_EQ(failedTileRequests, imagery.requestCount);

    Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, rootBounds), 8.0, 8.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());
    EXPECT_TRUE(provider.loadTileThrottled(*directCompositeTile, nullptr));
    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directCompositeTile->getState());
    EXPECT_EQ(nullptr, directCompositeTile->getTexture());
    ASSERT_FALSE(directCompositeTile->loadDiagnostics().empty());
    EXPECT_NE(
        std::find(
            directCompositeTile->loadDiagnostics().begin(),
            directCompositeTile->loadDiagnostics().end(),
            "Raster source tile failed after exhausting parent fallback"),
        directCompositeTile->loadDiagnostics().end());
    const int failedDirectCompositeRequests = imagery.requestCount;

    EXPECT_TRUE(provider.loadTileThrottled(*directCompositeTile, nullptr));
    EXPECT_TRUE(provider.loadTile(*directCompositeTile));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loaded,
              directCompositeTile->getState());
    EXPECT_EQ(failedDirectCompositeRequests, imagery.requestCount);
}

TEST(RasterOverlayLifecycleTest, RectangleLoadClampsCoverageChangedAfterTileCreation) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle initialBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 2, 0, 0});
    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, initialBounds), 8.0, 8.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());

    provider.setCoverageRectangle(
        scheme->tileToRectangle(TileKey{scheme->id(), 2, 3, 3}));

    EXPECT_TRUE(provider.loadTile(*directCompositeTile));
    EXPECT_EQ(1, imagery.requestCount);
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              directCompositeTile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Unknown,
              directCompositeTile->isMoreDetailAvailable());
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

    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              tile->isMoreDetailAvailable());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
}

TEST(RasterOverlayLifecycleTest,
     HighBitRasterImagesFailUploadUntilRendererFormatExists) {
    HighBitImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<RejectingHighBitRasterUploader>();
    RejectingHighBitRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    auto tile = provider.getTile(TileKey{scheme->id(), 2, 1, 1});
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));

    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(2, uploaderPtr->lastUpload.bytesPerChannel);
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              tile->isMoreDetailAvailable());
    EXPECT_EQ(nullptr, tile->getTexture());
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
    EXPECT_EQ(1, provider.processPendingUploads(false, &budget).processedUploads);
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(1u, budget.rasterUploadsUsed());

    EXPECT_EQ(0, provider.processPendingUploads(false, &budget).processedUploads);
    EXPECT_EQ(1, uploaderPtr->uploadCount);

    budget.beginFrame(2, config);
    provider.setFrameNumber(2);
    EXPECT_EQ(1, provider.processPendingUploads(false, &budget).processedUploads);
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

    EXPECT_EQ(
        1,
        provider.processPendingUploads(false, &firstFrameBudget)
            .processedUploads);
    EXPECT_EQ(1, uploaderPtr->uploadCount);
    EXPECT_EQ(1, provider.getPendingUploadCount());
    EXPECT_EQ(1u, firstFrameBudget.rasterUploadsUsed());

    FrameResourceBudget secondFrameBudget;
    secondFrameBudget.beginFrame(2, config);

    EXPECT_EQ(
        1,
        provider.processPendingUploads(false, &secondFrameBudget)
            .processedUploads);
    EXPECT_EQ(2, uploaderPtr->uploadCount);
    EXPECT_EQ(0, provider.getPendingUploadCount());
}

TEST(RasterOverlayLifecycleTest,
     InteractionRequeuePreservesOriginalPendingUploadOrder) {
    SequencedDeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<SlowRasterUploader>();
    SlowRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const std::array<TileKey, 4> keys{
        TileKey{scheme->id(), 2, 0, 0},
        TileKey{scheme->id(), 2, 1, 0},
        TileKey{scheme->id(), 2, 2, 0},
        TileKey{scheme->id(), 2, 3, 0}};
    for (const TileKey& key : keys) {
        auto tile = provider.getTile(key);
        ASSERT_NE(nullptr, tile);
        ASSERT_TRUE(provider.loadTileThrottled(*tile));
    }
    ASSERT_EQ(4u, imagery.pending.size());

    imagery.completeNext(makeImage(513, 1, 10));
    imagery.completeNext(makeImage(256, 1, 20));
    imagery.completeNext(makeImage(513, 1, 30));
    imagery.completeNext(makeImage(256, 1, 40));
    ASSERT_EQ(4, waitForPendingUploadCount(provider, 4));

    FrameResourceBudgetConfig config;
    config.maxRasterUploadsPerFrame = 2;
    config.mainThreadTimeMs = 0.001;
    config.interactionActive = true;
    FrameResourceBudget interactionBudget;
    interactionBudget.beginFrame(1, config);

    EXPECT_EQ(
        1,
        provider.processPendingUploads(true, &interactionBudget)
            .processedUploads);
    ASSERT_EQ(1u, uploaderPtr->uploadedRed.size());
    EXPECT_EQ(20, uploaderPtr->uploadedRed.front());
    EXPECT_EQ(3, provider.getPendingUploadCount());

    EXPECT_EQ(3, processPendingUploadsUntil(provider, 3));
    EXPECT_EQ(
        (std::vector<uint8_t>{20, 10, 30, 40}),
        uploaderPtr->uploadedRed);
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
    EXPECT_EQ(2, processPendingUploadsUntil(provider, 2));
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

TEST(RasterOverlayLifecycleTest, DirectTileLoadConsumesRasterSourceBudget) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    auto tile = provider.getTile(TileKey{scheme->id(), 1, 0, 0});
    ASSERT_NE(nullptr, tile);
    ASSERT_FALSE(tile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 1;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*tile, &budget));
    EXPECT_EQ(1, imagery.requestCount);
    EXPECT_EQ(1u, budget.networkRequestsIssued());
    EXPECT_EQ(1u, budget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              tile->getState());
}

TEST(RasterOverlayLifecycleTest,
     ResolveTileRequiresDrawableTextureForLoadedFallback) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey key{scheme->id(), 2, 1, 1};
    const Rectangle bounds = scheme->tileToRectangle(key);
    auto tile = provider.getTile(key);
    ASSERT_NE(nullptr, tile);

    tile->markLoadedWithoutTexture();

    EXPECT_EQ(nullptr, provider.resolveTile(bounds, key.z));

    tile->setTexture(std::make_unique<TestTexture>(4, 4));

    EXPECT_EQ(tile, provider.resolveTile(bounds, key.z));
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

TEST(RasterOverlayLifecycleTest,
     DirectCompositeRectangleIssuesBudgetedFanoutAfterStartLikeCesiumNative) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, rootBounds), 8.0, 8.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 20;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 20;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*directCompositeTile, &budget));
    EXPECT_EQ(1, imagery.requestCount);
    EXPECT_EQ(1u, budget.networkRequestsIssued());
    EXPECT_EQ(1u, budget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              directCompositeTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     LoadTilePumpsRemainingDirectCompositeSourceFanoutLikeCesiumNativeFuture) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(
        Rectangle::fromDegrees(-170.0, -70.0, 170.0, 70.0),
        1024.0,
        1024.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());
    ASSERT_GT(directCompositeTile->getDirectCompositeSourceKeys().size(), 1u);

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 32;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTile(*directCompositeTile, &firstBudget));
    EXPECT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(1u, firstBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              directCompositeTile->getState());

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, config);

    EXPECT_TRUE(provider.loadTile(*directCompositeTile, &secondBudget));
    EXPECT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(1u, secondBudget.rasterNetworkRequestsIssued());
}

TEST(RasterOverlayLifecycleTest, OversizedRectangleBatchWaitsWhenRasterInflightIsFull) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto firstTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, rootBounds),
        1024.0,
        1024.0)
        .tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_TRUE(firstTile->isDirectCompositeTile());

    FrameResourceBudgetConfig firstConfig;
    firstConfig.maxNetworkRequestsPerFrame = 1;
    firstConfig.maxNetworkInflight = 20;
    firstConfig.maxRasterNetworkRequestsPerFrame = 1;
    firstConfig.maxRasterNetworkInflight = 20;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, firstConfig);

    ASSERT_TRUE(provider.loadTileThrottled(*firstTile, &firstBudget));
    EXPECT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(1u, firstBudget.rasterNetworkRequestsIssued());

    const Rectangle quadrantBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 1, 0, 0});
    auto secondTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, quadrantBounds),
        8.0,
        8.0)
        .tile;
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(secondTile->isDirectCompositeTile());

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxNetworkRequestsPerFrame = 1;
    blockedConfig.maxNetworkInflight = 20;
    blockedConfig.maxRasterNetworkRequestsPerFrame = 1;
    blockedConfig.maxRasterNetworkInflight = 1;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(2, blockedConfig);

    EXPECT_FALSE(provider.loadTileThrottled(*secondTile, &blockedBudget));
    EXPECT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(0u, blockedBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Unloaded,
              secondTile->getState());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              firstTile->getState());
}

TEST(RasterOverlayLifecycleTest,
     FrameProcessingIssuesRemainingDirectCompositeSourceFanoutLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(
        Rectangle::fromDegrees(-170.0, -70.0, 170.0, 70.0),
        1024.0,
        1024.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 2;
    config.maxRasterNetworkInflight = 32;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*directCompositeTile, &firstBudget));
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              directCompositeTile->getState());
    const size_t firstBatchSize = imagery.pending.size();
    EXPECT_EQ(2u, firstBatchSize);
    EXPECT_EQ(2u, firstBudget.rasterNetworkRequestsIssued());
    EXPECT_TRUE(provider.hasPendingWork());

    while (!imagery.pending.empty()) {
        imagery.completeNext();
    }
    EXPECT_TRUE(imagery.pending.empty());
    EXPECT_EQ(0, provider.getActiveRasterSourceRequests());
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_TRUE(provider.hasPendingWork());

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, config);

    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &secondBudget)
            .processedUploads);
    EXPECT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(2u, secondBudget.rasterNetworkRequestsIssued());
    while (!imagery.pending.empty()) {
        imagery.completeNext();
    }
    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(
        1,
        provider.processPendingUploads(false, &secondBudget)
            .processedUploads);
    EXPECT_TRUE(imagery.pending.empty());
}

TEST(RasterOverlayLifecycleTest,
     FrameProcessingPumpsDirectCompositeSourceSetsInCreationOrderLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    const Rectangle westHalf(
        rootBounds.west(),
        rootBounds.south(),
        rootBounds.west() + rootBounds.width() * 0.5,
        rootBounds.north());
    const Rectangle eastHalf(
        rootBounds.west() + rootBounds.width() * 0.5,
        rootBounds.south(),
        rootBounds.east(),
        rootBounds.north());

    auto firstTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        1024.0,
        1024.0).tile;
    auto secondTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, eastHalf),
        1024.0,
        1024.0).tile;
    ASSERT_NE(nullptr, firstTile);
    ASSERT_NE(nullptr, secondTile);
    ASSERT_TRUE(firstTile->isDirectCompositeTile());
    ASSERT_TRUE(secondTile->isDirectCompositeTile());
    ASSERT_GE(firstTile->getDirectCompositeSourceKeys().size(), 2u);
    ASSERT_GE(secondTile->getDirectCompositeSourceKeys().size(), 2u);

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkInflight = 8;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);
    ASSERT_TRUE(provider.loadTileThrottled(*firstTile, &firstBudget));
    ASSERT_EQ(1u, imagery.pending.size());
    ASSERT_EQ(firstTile->getDirectCompositeSourceKeys()[0],
              imagery.requestedKeys.back());

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, config);
    ASSERT_TRUE(provider.loadTileThrottled(*secondTile, &secondBudget));
    ASSERT_EQ(2u, imagery.pending.size());
    ASSERT_EQ(secondTile->getDirectCompositeSourceKeys()[0],
              imagery.requestedKeys.back());

    while (!imagery.pending.empty()) {
        imagery.completeNext();
    }
    ASSERT_EQ(0, provider.getActiveRasterSourceRequests());
    ASSERT_TRUE(provider.hasPendingWork());

    FrameResourceBudget pumpBudget;
    pumpBudget.beginFrame(3, config);
    EXPECT_EQ(
        0,
        provider.processPendingUploads(false, &pumpBudget)
            .processedUploads);
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(firstTile->getDirectCompositeSourceKeys()[1],
              imagery.pending.front().key);
    EXPECT_EQ(firstTile->getDirectCompositeSourceKeys()[1],
              imagery.requestedKeys.back());
    EXPECT_EQ(1u, pumpBudget.rasterNetworkRequestsIssued());
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeConfigChangeAbandonsUnissuedSourceFanout) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto staleTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, rootBounds),
        1024.0,
        1024.0).tile;
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(staleTile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 2;
    config.maxRasterNetworkInflight = 16;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);

    ASSERT_TRUE(provider.loadTileThrottled(*staleTile, &firstBudget));
    EXPECT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(2u, firstBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(2, provider.getInFlightSourceTileCount());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading, staleTile->getState());
    EXPECT_TRUE(provider.hasPendingWork());

    provider.setMaximumScreenSpaceError(
        provider.getMaximumScreenSpaceError() * 2.0);

    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, staleTile->getState());
    EXPECT_EQ(2, provider.getInFlightSourceTileCount());
    EXPECT_TRUE(provider.hasPendingWork());

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, config);
    EXPECT_TRUE(provider.loadTileThrottled(*staleTile, &secondBudget));
    EXPECT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(0u, secondBudget.rasterNetworkRequestsIssued());

    while (!imagery.pending.empty()) {
        imagery.completeNext();
    }
    EXPECT_FALSE(provider.hasPendingWork());
    EXPECT_EQ(0, provider.getPendingUploadCount());
}

TEST(RasterOverlayLifecycleTest,
     SourceDepotInvalidationDropsAbandonedInFlightEntriesImmediately) {
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

    auto staleTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(staleTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*staleTile));
    ASSERT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(1, provider.getInFlightSourceTileCount());
    EXPECT_TRUE(provider.hasPendingWork());

    provider.setMaximumTextureSize(256);

    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, staleTile->getState());
    EXPECT_EQ(0, provider.getInFlightSourceTileCount());
    EXPECT_TRUE(provider.hasPendingWork());

    imagery.completeNext();
    EXPECT_EQ(0, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_FALSE(provider.hasPendingWork());
}

TEST(RasterOverlayLifecycleTest,
     CompletedDirectCompositeSourceSetDoesNotLeaveOrderEntriesBehind) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto tile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, rootBounds),
        1024.0,
        1024.0).tile;
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(tile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 8;
    config.maxRasterNetworkInflight = 16;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    ASSERT_TRUE(provider.loadTileThrottled(*tile, &budget));
    ASSERT_GT(provider.getActiveDirectCompositeSourceSetOrderCount(), 0);
    while (!imagery.pending.empty()) {
        imagery.completeNext();
    }
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(1, provider.processPendingUploads(false).processedUploads);
    EXPECT_TRUE(waitForNoPendingWork(provider));
    EXPECT_EQ(0, provider.getActiveDirectCompositeSourceSetOrderCount());
}

TEST(RasterOverlayLifecycleTest,
     AbandonedDirectCompositeSourceReuseKeepsOriginalSourceRequestInFlight) {
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

    auto staleTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(staleTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*staleTile));
    ASSERT_EQ(1u, imagery.pending.size());
    ASSERT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(sourceKey, imagery.requestedKeys.front());
    EXPECT_EQ(1, provider.getInFlightSourceTileCount());

    provider.setMaximumScreenSpaceError(
        provider.getMaximumScreenSpaceError() * 2.0);

    auto remappedTile = provider.mapRasterTilesToGeometryTile(
        projectForProvider(provider, westHalf),
        128.0,
        128.0).tile;
    ASSERT_NE(nullptr, remappedTile);
    ASSERT_TRUE(remappedTile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*remappedTile));
    EXPECT_EQ(1u, imagery.pending.size());
    EXPECT_EQ(1u, imagery.requestedKeys.size());
    EXPECT_EQ(1, provider.getInFlightSourceTileCount());

    imagery.completeNext();
    EXPECT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(1, provider.processPendingUploads(false).processedUploads);
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeConfigChangeRemapsHeldLoadingTileInsteadOfFailingOverlay) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey rootKey{scheme->id(), 0, 0, 0};
    const Rectangle rootBounds = scheme->tileToRectangle(rootKey);
    RasterOverlayDetails details = makeProviderDetails(*scheme, rootBounds);
    DirectRasterMapping mapped;
    std::vector<RasterOverlayProjection> missing;

    EXPECT_EQ(
        DirectRasterMapping::MoreDetail::Unknown,
        mapped.update(
            rootKey,
            details,
            1024.0,
            1024.0,
            provider,
            nullptr,
            missing,
            nullptr,
            0,
            true));
    RasterOverlayTile* staleTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, staleTile);
    ASSERT_TRUE(staleTile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 2;
    config.maxRasterNetworkInflight = 16;
    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(1, config);

    ASSERT_TRUE(mapped.loadThrottled(provider, &firstBudget));
    EXPECT_EQ(2u, imagery.pending.size());
    EXPECT_EQ(2u, firstBudget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading, staleTile->getState());

    provider.setMaximumScreenSpaceError(
        provider.getMaximumScreenSpaceError() * 2.0);
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, staleTile->getState());

    EXPECT_EQ(
        DirectRasterMapping::MoreDetail::Unknown,
        mapped.update(
            rootKey,
            details,
            1024.0,
            1024.0,
            provider,
            nullptr,
            missing,
            nullptr,
            0,
            true));
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_NE(staleTile, mapped.getLoadingTile());
    EXPECT_EQ(
        RasterOverlayTile::LoadState::Unloaded,
        mapped.getLoadingTile()->getState());

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, config);
    ASSERT_TRUE(mapped.loadThrottled(provider, &secondBudget));
    EXPECT_GT(imagery.pending.size(), 2u);
    EXPECT_GT(secondBudget.rasterNetworkRequestsIssued(), 0u);

    while (!imagery.pending.empty()) {
        imagery.completeNext();
    }
    processPendingUploadsUntil(provider, 1);
    EXPECT_TRUE(waitForNoPendingWork(provider));
    EXPECT_EQ(0, provider.getPendingUploadCount());
}

TEST(RasterOverlayLifecycleTest, FrameBudgetSeparatesRasterFanoutFromTerrainBudget) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});
    auto directCompositeTile = provider.mapRasterTilesToGeometryTile(projectForProvider(provider, rootBounds), 8.0, 8.0).tile;
    ASSERT_NE(nullptr, directCompositeTile);
    ASSERT_TRUE(directCompositeTile->isDirectCompositeTile());

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxTerrainContentNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxNetworkInflight = 1;
    config.maxTerrainContentNetworkInflight = 1;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(provider.loadTileThrottled(*directCompositeTile, &budget));
    EXPECT_EQ(4, imagery.requestCount);
    EXPECT_EQ(4u, budget.networkRequestsIssued());
    EXPECT_EQ(0u, budget.terrainContentNetworkRequestsIssued());
    EXPECT_EQ(4u, budget.rasterNetworkRequestsIssued());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              directCompositeTile->getState());
}

TEST(RasterOverlayLifecycleTest, NotReadyProviderMapsPlaceholderLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setReady(false);

    TileKey key{scheme->id(), 2, 1, 1};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    DirectRasterMapping::MoreDetail moreDetail = mapped.update(
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
    EXPECT_EQ(DirectRasterMapping::State::Unattached,
              mapped.getState());
    EXPECT_EQ(DirectRasterMapping::MoreDetail::No, moreDetail);
    EXPECT_TRUE(mapped.getDirectCompositeSourceTiles().empty());
    EXPECT_FALSE(mapped.usesDirectRasterTile());
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

    DirectRasterMapping mapped;
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
    const DirectRasterMapping::MoreDetail moreDetail = mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);

    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_TRUE(mapped.getLoadingTile()->isDirectCompositeTile());
    EXPECT_EQ(projectForProvider(provider, preciseRectangle),
              mapped.getLoadingTile()->getRectangle());
    EXPECT_EQ(0, mapped.getTextureCoordinateID());
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, moreDetail);
    EXPECT_TRUE(missing.empty());
    EXPECT_EQ(1, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, ReadyFalseInvalidatesDirectMappingLifecycleLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey key{scheme->id(), 3, 2, 3};
    const Rectangle rectangle = scheme->tileToRectangle(key);
    RasterOverlayDetails details = makeProviderDetails(*scheme, rectangle);
    std::vector<RasterOverlayProjection> missing;
    DirectRasterMapping mapped;

    ASSERT_EQ(DirectRasterMapping::MoreDetail::Unknown,
              mapped.update(
                  key,
                  details,
                  256.0,
                  256.0,
                  provider,
                  nullptr,
                  missing));
    std::shared_ptr<RasterOverlayTile> oldTile = mapped.getLoadingTileHandle();
    ASSERT_NE(nullptr, oldTile);
    ASSERT_TRUE(provider.ownsCurrentTile(*oldTile));
    ASSERT_TRUE(provider.loadTile(*oldTile));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));

    imagery.completeNext();
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
    ASSERT_TRUE(provider.hasPendingWork());

    provider.setReady(false);

    EXPECT_FALSE(provider.isReady());
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, oldTile->getState());
    EXPECT_FALSE(provider.ownsCurrentTile(*oldTile));
    EXPECT_EQ(0, provider.getCachedTileCount());
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(0, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(0, uploaderPtr->uploadCount);
    EXPECT_FALSE(provider.hasPendingWork());
}

TEST(RasterOverlayLifecycleTest, ReadyFalseRejectsLateDirectSourceCompletionLikeCesiumNative) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey key{scheme->id(), 2, 1, 1};
    std::shared_ptr<RasterOverlayTile> tile = provider.getTile(key);
    ASSERT_NE(nullptr, tile);
    ASSERT_FALSE(tile->isDirectCompositeTile());
    ASSERT_TRUE(provider.ownsCurrentTile(*tile));
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(1, static_cast<int>(imagery.pending.size()));
    ASSERT_TRUE(provider.hasPendingWork());

    provider.setReady(false);

    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
    EXPECT_FALSE(provider.ownsCurrentTile(*tile));
    EXPECT_EQ(0, provider.getCachedTileCount());
    EXPECT_TRUE(provider.hasPendingWork());

    imagery.completeNext();

    EXPECT_EQ(0, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(0, uploaderPtr->uploadCount);
    EXPECT_FALSE(provider.hasPendingWork());
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, tile->getState());
}

TEST(RasterOverlayLifecycleTest, ReadyFalseThenTrueRemapsWithoutStaleTileReuse) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey key{scheme->id(), 2, 1, 1};
    const Rectangle rectangle = scheme->tileToRectangle(key);
    RasterOverlayDetails details = makeProviderDetails(*scheme, rectangle);
    std::vector<RasterOverlayProjection> missing;
    DirectRasterMapping mapped;

    ASSERT_EQ(DirectRasterMapping::MoreDetail::Unknown,
              mapped.update(
                  key,
                  details,
                  256.0,
                  256.0,
                  provider,
                  nullptr,
                  missing));
    std::shared_ptr<RasterOverlayTile> oldTile = mapped.getLoadingTileHandle();
    ASSERT_NE(nullptr, oldTile);
    ASSERT_TRUE(provider.ownsCurrentTile(*oldTile));

    provider.setReady(false);
    missing.clear();
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
    const DirectRasterMapping::MoreDetail remapped = mapped.update(
        key,
        details,
        256.0,
        256.0,
        provider,
        nullptr,
        missing);

    std::shared_ptr<RasterOverlayTile> newTile = mapped.getLoadingTileHandle();
    ASSERT_NE(nullptr, newTile);
    EXPECT_NE(oldTile.get(), newTile.get());
    EXPECT_FALSE(provider.ownsCurrentTile(*oldTile));
    EXPECT_TRUE(provider.ownsCurrentTile(*newTile));
    EXPECT_TRUE(newTile->isDirectCompositeTile());
    EXPECT_EQ(projectForProvider(provider, rectangle), newTile->getRectangle());
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, remapped);
    EXPECT_TRUE(missing.empty());
}

TEST(RasterOverlayLifecycleTest, RenderContentDetailsRectangleMapsRealTileLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const Rectangle preciseRectangle =
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0);
    RasterOverlayDetails details = makeProviderDetails(*scheme, preciseRectangle);
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    const DirectRasterMapping::MoreDetail moreDetail = mapped.update(
        TileKey{"Geographic-TMS", 4, 8, 8},
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);

    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_TRUE(mapped.getLoadingTile()->isDirectCompositeTile());
    EXPECT_EQ(projectForProvider(provider, preciseRectangle),
              mapped.getLoadingTile()->getRectangle());
    EXPECT_EQ(0, mapped.getTextureCoordinateID());
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, moreDetail);
    EXPECT_FALSE(mapped.usesDirectRasterTile());
    EXPECT_FALSE(mapped.getDirectCompositeSourceTiles().empty());
    EXPECT_TRUE(mapped.getDirectCompositeSourceTiles().sourceBounds.equalsEpsilon(
        preciseRectangle,
        1e-12));
    EXPECT_LE(mapped.getDirectCompositeSourceTiles().minX,
              mapped.getDirectCompositeSourceTiles().maxX);
    EXPECT_LE(mapped.getDirectCompositeSourceTiles().minY,
              mapped.getDirectCompositeSourceTiles().maxY);
    for (const TileKey& sourceKey :
         mapped.getDirectCompositeSourceTiles().sourceKeys) {
        EXPECT_GE(sourceKey.x, mapped.getDirectCompositeSourceTiles().minX);
        EXPECT_LE(sourceKey.x, mapped.getDirectCompositeSourceTiles().maxX);
        EXPECT_GE(sourceKey.y, mapped.getDirectCompositeSourceTiles().minY);
        EXPECT_LE(sourceKey.y, mapped.getDirectCompositeSourceTiles().maxY);
    }
    EXPECT_TRUE(missing.empty());
}

TEST(RasterOverlayLifecycleTest, RenderContentDetailsAlignedRectangleMapsDirectTile) {
    RgbImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey imageryKey{scheme->id(), 3, 2, 3};
    const Rectangle preciseRectangle = scheme->tileToRectangle(imageryKey);
    RasterOverlayDetails details = makeProviderDetails(*scheme, preciseRectangle);
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    const DirectRasterMapping::MoreDetail moreDetail = mapped.update(
        TileKey{"Geographic-TMS", 4, 8, 8},
        details,
        8.0,
        8.0,
        provider,
        nullptr,
        missing);

    ASSERT_NE(nullptr, mapped.getLoadingTile());
    EXPECT_FALSE(mapped.getLoadingTile()->isDirectCompositeTile());
    EXPECT_EQ(imageryKey, mapped.getLoadingTile()->getTileID());
    EXPECT_EQ(projectForProvider(provider, preciseRectangle),
              mapped.getLoadingTile()->getRectangle());
    EXPECT_EQ(0, mapped.getTextureCoordinateID());
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, moreDetail);
    EXPECT_TRUE(mapped.usesDirectRasterTile());
    ASSERT_EQ(1u, mapped.getDirectCompositeSourceTiles().sourceKeys.size());
    EXPECT_EQ(imageryKey, mapped.getDirectCompositeSourceTiles().sourceKeys.front());
    EXPECT_EQ(imageryKey.z, mapped.getDirectCompositeSourceTiles().sourceZoom);
    EXPECT_EQ(preciseRectangle, mapped.getDirectCompositeSourceTiles().sourceBounds);
    EXPECT_EQ(imageryKey.x, mapped.getDirectCompositeSourceTiles().minX);
    EXPECT_EQ(imageryKey.y, mapped.getDirectCompositeSourceTiles().minY);
    EXPECT_EQ(imageryKey.x, mapped.getDirectCompositeSourceTiles().maxX);
    EXPECT_EQ(imageryKey.y, mapped.getDirectCompositeSourceTiles().maxY);
    EXPECT_TRUE(missing.empty());
    EXPECT_EQ(1, provider.getCachedTileCount());
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

    DirectRasterMapping mapped;
    const DirectRasterMapping::MoreDetail moreDetail = mapped.update(
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
    EXPECT_EQ(DirectRasterMapping::MoreDetail::No, moreDetail);
    EXPECT_TRUE(mapped.loadThrottled(provider));
    ASSERT_EQ(1u, missing.size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator, missing[0]);
    EXPECT_EQ(0, provider.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, PrefetchRecordsMissingProjectionForContentReloadLikeCesiumNative) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);
    model->rasterOverlayDetails.rasterOverlayRectangles.push_back(tile.bounds);
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;
    tile.geometricError = 100.0;

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::vector<ActivatedRasterOverlay*> overlays{&activated};
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    ASSERT_EQ(1u, tile.rasterOverlayState.missingProjections().size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator,
              tile.rasterOverlayState.missingProjections()[0]);
    DirectRasterMapping* mapped = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mapped);
    ASSERT_NE(nullptr, mapped->getLoadingTile());
    EXPECT_EQ(RasterOverlayTile::LoadState::Placeholder,
              mapped->getLoadingTile()->getState());
    EXPECT_EQ(0, activated.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest,
     PrefetchDetachesStaleRasterWithRendererLifecycleLikeCesiumNative) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* provider =
        activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);
    provider->setLevelRange(3, 3);

    const TileKey sourceKey{overlay->getTileScheme().id(), 3, 4, 3};
    const Rectangle sourceBounds =
        overlay->getTileScheme().tileToRectangle(sourceKey);
    const Rectangle westHalf(
        sourceBounds.west(),
        sourceBounds.south(),
        sourceBounds.west() + sourceBounds.width() * 0.5,
        sourceBounds.north());

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        westHalf);
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.setGeographicRectangle(westHalf);
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.renderContent.setGltfResourcesReady(true);
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;
    tile.geometricError = 100.0;

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<ActivatedRasterOverlay*> overlays{&activated};
    RecordingPrepareRendererResources recorder;

    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget,
        &recorder);
    DirectRasterMapping* mapped = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mapped);
    RasterOverlayTile* firstLoading = mapped->getLoadingTile();
    ASSERT_NE(nullptr, firstLoading);
    firstLoading->setTexture(std::make_unique<TestTexture>(4, 4));

    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget,
        &recorder);
    ASSERT_EQ(DirectRasterMapping::State::Attached,
              mapped->getState());
    ASSERT_EQ(1, recorder.attachCount);
    ASSERT_EQ(0, recorder.detachCount);

    provider->setLevelRange(3, 4);

    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget,
        &recorder);

    EXPECT_EQ(1, recorder.detachCount);
}

TEST(
    RasterOverlayLifecycleTest,
    StableAttachedMappingReusesAcrossFramesAndRenderPlanDeduplicatesTiles) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* provider =
        activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);
    provider->setLevelRange(3, 3);

    const TileKey sourceKey{overlay->getTileScheme().id(), 3, 4, 3};
    const Rectangle sourceBounds =
        overlay->getTileScheme().tileToRectangle(sourceKey);
    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        sourceBounds);
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.setGeographicRectangle(tile.bounds);
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.renderContent.setGltfResourcesReady(true);
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;
    tile.geometricError = 100.0;

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    std::vector<ActivatedRasterOverlay*> overlays{&activated};
    RecordingPrepareRendererResources recorder;

    budget.beginFrame(1, config);
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget,
        &recorder,
        1);
    DirectRasterMapping* mapped =
        tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mapped);
    RasterOverlayTile* loading = mapped->getLoadingTile();
    ASSERT_NE(nullptr, loading);
    loading->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    loading->setTexture(std::make_unique<TestTexture>(4, 4));

    budget.beginFrame(2, config);
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget,
        &recorder,
        2);
    ASSERT_EQ(
        DirectRasterMapping::State::Attached,
        mapped->getState());
    ASSERT_EQ(1, recorder.attachCount);
    const uint64_t stableUpdateCount =
        tile.rasterOverlayState.authoritativeUpdateCount();

    TilePlan plan;
    plan.frameId = 3;
    TileRenderEntry selectedEntry;
    selectedEntry.selectedTile = &tile;
    selectedEntry.renderTile = &tile;
    selectedEntry.selectedKey = tile.key;
    selectedEntry.renderKey = tile.key;
    plan.renderEntries.push_back(selectedEntry);
    TileRenderEntry fadingEntry = selectedEntry;
    fadingEntry.selectedThisFrame = false;
    plan.renderEntries.push_back(fadingEntry);

    budget.beginFrame(3, config);
    const TileRasterOverlayRenderPlanPrepareResult result =
        TileRasterOverlayFrameProcessor::prepareRenderPlan(
            plan,
            overlays,
            std::vector<size_t>{0},
            nullptr,
            16.0,
            budget,
            &recorder,
            [](TilesetTile&) {
                FAIL() << "stable mapping must not unload content";
            },
            [](TilesetTile&) {
                FAIL() << "stable mapping must not create children";
            },
            [](const TileKey&, TileLoadPriorityGroup, double) {
                FAIL() << "stable mapping must not queue a reload";
            },
            []() {
                FAIL() << "stable mapping must not refresh the render plan";
            });

    EXPECT_EQ(1, result.tilesConsidered);
    EXPECT_EQ(0, result.authoritativeUpdates);
    EXPECT_EQ(1, result.stableReuses);
    EXPECT_EQ(
        stableUpdateCount,
        tile.rasterOverlayState.authoritativeUpdateCount());
    EXPECT_EQ(1, recorder.attachCount);

    loading->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::Yes);
    plan.frameId = 4;
    budget.beginFrame(4, config);
    int upsampleActionCount = 0;
    const TileRasterOverlayRenderPlanPrepareResult upsampleResult =
        TileRasterOverlayFrameProcessor::prepareRenderPlan(
            plan,
            overlays,
            std::vector<size_t>{0},
            nullptr,
            16.0,
            budget,
            &recorder,
            [](TilesetTile&) {
                FAIL() << "more-detail mapping must not unload content";
            },
            [&](TilesetTile& actionTile) {
                EXPECT_EQ(&tile, &actionTile);
                ++upsampleActionCount;
            },
            [](const TileKey&, TileLoadPriorityGroup, double) {
                FAIL() << "more-detail mapping must not queue a reload";
            },
            []() {
                FAIL() << "more-detail mapping must not refresh the render plan";
            });
    EXPECT_EQ(1, upsampleActionCount);
    EXPECT_EQ(1, upsampleResult.upsampleActions);
    EXPECT_EQ(1, upsampleResult.authoritativeUpdates);
    const uint64_t postUpsampleUpdateCount =
        tile.rasterOverlayState.authoritativeUpdateCount();

    const int detachCountBeforeConfigurationChange = recorder.detachCount;
    provider->setLevelRange(3, 4);
    budget.beginFrame(5, config);
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget,
        &recorder,
        5);
    EXPECT_EQ(
        postUpsampleUpdateCount + 1,
        tile.rasterOverlayState.authoritativeUpdateCount());
    EXPECT_EQ(
        detachCountBeforeConfigurationChange + 1,
        recorder.detachCount);
}

TEST(RasterOverlayLifecycleTest,
     UpsampleSourcePreparationDetachesStaleRasterResidueLikeCesiumNative) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* provider =
        activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);

    TileContentCacheManager cache;
    uint64_t resourceRevision = 1;
    TileContentResourceInvalidator invalidator(resourceRevision, cache);
    TileLoadQueue loadQueue;
    TileMeshPreparationManager manager(invalidator, loadQueue);

    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 180.0, 90.0));
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.setGeographicRectangle(parent.bounds);
    parent.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    parent.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    parent.content.renderContent.setGltfResourcesReady(true);
    parent.content.contentKind = TileContentKind::Render;
    parent.content.loadState = TileLoadState::Done;

    std::vector<RasterOverlayProjection> missing;
    DirectRasterMapping& mapped =
        parent.rasterOverlayState.ensureMapping(0);
    mapped.update(
        parent.key,
        parent.content.renderContent.rasterOverlayDetails(),
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0,
        true);
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    mapped.getLoadingTile()->setTexture(std::make_unique<TestTexture>(4, 4));
    RecordingPrepareRendererResources recorder;
    mapped.update(
        parent.key,
        parent.content.renderContent.rasterOverlayDetails(),
        512.0,
        512.0,
        *provider,
        &recorder,
        missing,
        nullptr,
        0,
        true);
    ASSERT_EQ(DirectRasterMapping::State::Attached,
              mapped.getState());
    ASSERT_EQ(1, recorder.attachCount);

    TilesetTile child(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, 0.0, 0.0, 90.0),
        &parent);
    parent.children.push_back(&child);
    child.content.markTerrainAvailabilityUpsample();

    EXPECT_FALSE(manager.prepareUpsampleSourceTile(
        child,
        10.0,
        &recorder));

    EXPECT_EQ(1, recorder.detachCount);
    EXPECT_EQ(0u, parent.rasterOverlayState.mappingCount());
    EXPECT_FALSE(parent.content.renderContent.hasGltfContent());
    EXPECT_EQ(2u, resourceRevision);
    EXPECT_EQ(0, cache.totalBytesUsed());
    EXPECT_TRUE(cache.unloadQueue().contains(
        TileCacheKey::forTile(parent.key)));
}

TEST(RasterOverlayLifecycleTest,
     MissingProjectionPlaceholderRemapsAfterContentReloadLikeCesiumNative) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);
    model->rasterOverlayDetails.rasterOverlayRectangles.push_back(tile.bounds);
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;
    tile.geometricError = 100.0;

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    std::vector<ActivatedRasterOverlay*> overlays{&activated};

    budget.beginFrame(1, config);
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    DirectRasterMapping* mapped = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mapped);
    ASSERT_NE(nullptr, mapped->getLoadingTile());
    EXPECT_EQ(RasterOverlayTile::LoadState::Placeholder,
              mapped->getLoadingTile()->getState());
    EXPECT_EQ(1, mapped->getTextureCoordinateID());
    ASSERT_EQ(1u, tile.rasterOverlayState.missingProjections().size());

    RasterOverlayDetails* reloadedDetails =
        tile.content.renderContent.mutableRasterOverlayDetails();
    ASSERT_NE(nullptr, reloadedDetails);
    reloadedDetails->rasterOverlayProjections.push_back(
        RasterOverlayProjection::WebMercator);
    const Rectangle reloadedRectangle =
        projectForProvider(overlay->getTileScheme(), tile.bounds);
    reloadedDetails->rasterOverlayRectangles.push_back(reloadedRectangle);

    budget.beginFrame(2, config);
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    EXPECT_TRUE(tile.rasterOverlayState.missingProjections().empty());
    ASSERT_NE(nullptr, mapped->getLoadingTile());
    EXPECT_NE(activated.getTileProvider()->getPlaceholderTile().get(),
              mapped->getLoadingTile());
    EXPECT_TRUE(mapped->getLoadingTile()->isDirectCompositeTile());
    EXPECT_EQ(reloadedRectangle, mapped->getLoadingTile()->getRectangle());
    EXPECT_EQ(1, mapped->getTextureCoordinateID());
    EXPECT_EQ(1, activated.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest,
     PrefetchUsesOrdinaryGltfRasterOverlayDetailsLikeCesiumNativeRenderContent) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-20.0, -10.0, 20.0, 10.0));
    tile.geometricError = 100.0;
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;

    const Rectangle contentRectangle =
        Rectangle::fromDegrees(-4.0, -3.0, 6.0, 5.0);
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails =
        makeProviderDetails(overlay->getTileScheme(), contentRectangle);
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::vector<ActivatedRasterOverlay*> overlays{&activated};
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    DirectRasterMapping* mapped = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mapped);
    ASSERT_NE(nullptr, mapped->getLoadingTile());
    const Rectangle expected =
        projectForProvider(overlay->getTileScheme(), contentRectangle);
    const Rectangle tileProjection =
        projectForProvider(overlay->getTileScheme(), tile.bounds);
    EXPECT_EQ(expected, mapped->getLoadingTile()->getRectangle());
    EXPECT_NE(tileProjection, mapped->getLoadingTile()->getRectangle());
    EXPECT_TRUE(tile.rasterOverlayState.missingProjections().empty());
}

TEST(RasterOverlayLifecycleTest,
     PrefetchPromotesAncestorFallbackWhileChildLoadingLikeCesiumNative) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* provider = activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);

    const TileKey parentKey{overlay->getTileScheme().id(), 2, 1, 1};
    const TileKey childKey{overlay->getTileScheme().id(), 3, 2, 2};
    const Rectangle parentBounds =
        overlay->getTileScheme().tileToRectangle(parentKey);
    const Rectangle childBounds =
        overlay->getTileScheme().tileToRectangle(childKey);
    RasterOverlayDetails parentDetails =
        makeProviderDetails(overlay->getTileScheme(), parentBounds);
    RasterOverlayDetails childDetails =
        makeProviderDetails(overlay->getTileScheme(), childBounds);
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parentTile(parentKey, parentBounds);
    DirectRasterMapping& parentMapping =
        parentTile.rasterOverlayState.ensureMapping(0);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0);
    RasterOverlayTile* parentRaster = parentMapping.getLoadingTile();
    ASSERT_NE(nullptr, parentRaster);
    parentRaster->setTexture(std::make_unique<TestTexture>(4, 4));
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0);
    ASSERT_EQ(parentRaster, parentMapping.getReadyTile());

    TilesetTile childTile(childKey, childBounds, &parentTile);
    childTile.geometricError = 100.0;
    DirectRasterMapping& childMapping =
        childTile.rasterOverlayState.ensureMapping(0);
    childMapping.update(
        childKey,
        childDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        &parentTile,
        0);
    RasterOverlayTile* childRaster = childMapping.getLoadingTile();
    ASSERT_NE(nullptr, childRaster);
    ASSERT_EQ(RasterOverlayTile::LoadState::Unloaded,
              childRaster->getState());

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::vector<ActivatedRasterOverlay*> overlays{&activated};
    TileRasterOverlayPrefetcher::prefetch(
        childTile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    EXPECT_TRUE(childTile.rasterOverlayState.hasReadyMapping(0));
    EXPECT_EQ(parentRaster, childMapping.getReadyTile());
    EXPECT_EQ(childRaster, childMapping.getLoadingTile());
    EXPECT_EQ(DirectRasterMapping::ReadyTileSource::Ancestor,
              childMapping.getReadyTileSource());
    EXPECT_EQ(DirectRasterMapping::State::Unattached,
              childMapping.getState());
}

TEST(RasterOverlayLifecycleTest,
     PrefetchKeepsLoadingFallbackStableWithoutRemappingLikeCesiumNative) {
    auto imagery = std::make_unique<DeferredImageryProvider>();
    DeferredImageryProvider* imageryPtr = imagery.get();
    auto overlay = std::make_unique<RasterOverlay>(
        std::move(imagery),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* provider = activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);

    const TileKey parentKey{overlay->getTileScheme().id(), 2, 1, 1};
    const TileKey childKey{overlay->getTileScheme().id(), 3, 2, 2};
    const Rectangle parentBounds =
        overlay->getTileScheme().tileToRectangle(parentKey);
    const Rectangle childBounds =
        overlay->getTileScheme().tileToRectangle(childKey);
    RasterOverlayDetails parentDetails =
        makeProviderDetails(overlay->getTileScheme(), parentBounds);
    RasterOverlayDetails childDetails =
        makeProviderDetails(overlay->getTileScheme(), childBounds);
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parentTile(parentKey, parentBounds);
    DirectRasterMapping& parentMapping =
        parentTile.rasterOverlayState.ensureMapping(0);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0);
    RasterOverlayTile* parentRaster = parentMapping.getLoadingTile();
    ASSERT_NE(nullptr, parentRaster);
    parentRaster->setTexture(std::make_unique<TestTexture>(4, 4));
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0);
    ASSERT_EQ(parentRaster, parentMapping.getReadyTile());

    TilesetTile childTile(childKey, childBounds, &parentTile);
    childTile.geometricError = 100.0;
    DirectRasterMapping& childMapping =
        childTile.rasterOverlayState.ensureMapping(0);
    childMapping.update(
        childKey,
        childDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        &parentTile,
        0);
    RasterOverlayTile* childRaster = childMapping.getLoadingTile();
    ASSERT_NE(nullptr, childRaster);

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    std::vector<ActivatedRasterOverlay*> overlays{&activated};

    budget.beginFrame(1, config);
    TileRasterOverlayPrefetcher::prefetch(
        childTile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    ASSERT_EQ(RasterOverlayTile::LoadState::Loading,
              childRaster->getState());
    ASSERT_EQ(1u, imageryPtr->requestedKeys.size());
    ASSERT_EQ(1u, imageryPtr->pending.size());
    ASSERT_EQ(parentRaster, childMapping.getReadyTile());
    ASSERT_EQ(DirectRasterMapping::ReadyTileSource::Ancestor,
              childMapping.getReadyTileSource());

    budget.beginFrame(2, config);
    TileRasterOverlayPrefetcher::prefetch(
        childTile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    EXPECT_EQ(childRaster, childMapping.getLoadingTile());
    EXPECT_EQ(parentRaster, childMapping.getReadyTile());
    EXPECT_EQ(DirectRasterMapping::ReadyTileSource::Ancestor,
              childMapping.getReadyTileSource());
    EXPECT_EQ(RasterOverlayTile::LoadState::Loading,
              childRaster->getState());
    EXPECT_EQ(1u, imageryPtr->requestedKeys.size());
    EXPECT_EQ(1u, imageryPtr->pending.size());
}

TEST(RasterOverlayLifecycleTest, EmptyPrefetchClearsRasterOverlayStateLikeCesiumNativeRemove) {
    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;
    tile.rasterOverlayState.ensureMapping(0);
    tile.rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::WebMercator);

    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    TileRasterOverlayPrefetcher::prefetch(
        tile,
        {},
        {},
        nullptr,
        16.0,
        budget);

    EXPECT_TRUE(tile.rasterOverlayState.mappings().empty());
    EXPECT_TRUE(tile.rasterOverlayState.missingProjections().empty());
}

TEST(RasterOverlayLifecycleTest,
     DoneEmptyTilePrefetchClearsRasterOverlayStateLikeCesiumNative) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Empty;
    tile.rasterOverlayState.ensureMapping(0);
    tile.rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::WebMercator);

    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<ActivatedRasterOverlay*> overlays{&activated};

    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    EXPECT_TRUE(tile.rasterOverlayState.mappings().empty());
    EXPECT_TRUE(tile.rasterOverlayState.missingProjections().empty());
    EXPECT_EQ(0, activated.getCachedTileCount());
}

TEST(RasterOverlayLifecycleTest, PrefetchClearsStaleMappingWhenOverlaySlotIdentityChanges) {
    auto overlayA = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    auto overlayB = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activatedA(*overlayA);
    ActivatedRasterOverlay activatedB(*overlayB);

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::WebMercator);
    model->rasterOverlayDetails.rasterOverlayRectangles.push_back(
        projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            tile.bounds));
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;
    tile.geometricError = 100.0;

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::vector<ActivatedRasterOverlay*> overlays{&activatedA};
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);
    DirectRasterMapping* mappedA = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mappedA);
    ASSERT_NE(nullptr, mappedA->getLoadingTile());
    EXPECT_EQ(activatedA.getTileProvider(),
              &mappedA->getLoadingTile()->getTileProvider());

    overlays[0] = &activatedB;
    budget.beginFrame(2, config);
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    DirectRasterMapping* mappedB = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mappedB);
    ASSERT_NE(nullptr, mappedB->getLoadingTile());
    EXPECT_EQ(activatedB.getTileProvider(),
              &mappedB->getLoadingTile()->getTileProvider());
    EXPECT_NE(activatedA.getTileProvider(),
              &mappedB->getLoadingTile()->getTileProvider());
}

TEST(RasterOverlayLifecycleTest,
     PrefetchDetachesAttachedRasterWhenOverlaySlotIdentityChangesLikeCesiumNativeRemove) {
    auto overlayA = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    auto overlayB = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activatedA(*overlayA);
    ActivatedRasterOverlay activatedB(*overlayB);

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::WebMercator);
    model->rasterOverlayDetails.rasterOverlayRectangles.push_back(
        projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            tile.bounds));
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;
    tile.geometricError = 100.0;

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::vector<ActivatedRasterOverlay*> overlays{&activatedA};
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);
    DirectRasterMapping* mappedA = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mappedA);
    RasterOverlayTile* loadingA = mappedA->getLoadingTile();
    ASSERT_NE(nullptr, loadingA);
    loadingA->setTexture(std::make_unique<TestTexture>(4, 4));

    RecordingPrepareRendererResources recorder;
    std::vector<RasterOverlayProjection> missing;
    mappedA->update(
        tile.key,
        tile.content.renderContent.rasterOverlayDetails(),
        512.0,
        512.0,
        *activatedA.getTileProvider(),
        &recorder,
        missing,
        tile.parent,
        0,
        true);
    ASSERT_EQ(DirectRasterMapping::State::Attached,
              mappedA->getState());
    ASSERT_EQ(1, recorder.attachCount);
    ASSERT_EQ(0, recorder.detachCount);

    overlays[0] = &activatedB;
    budget.beginFrame(2, config);
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget,
        &recorder);

    EXPECT_EQ(1, recorder.detachCount);
    DirectRasterMapping* mappedB = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mappedB);
    ASSERT_NE(nullptr, mappedB->getLoadingTile());
    EXPECT_EQ(activatedB.getTileProvider(),
              &mappedB->getLoadingTile()->getTileProvider());
}

TEST(RasterOverlayLifecycleTest,
     RenderContentCommitDetachesAttachedRasterBeforeClearingMappingsLikeCesiumNativeAddTileOverlays) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    auto existingModel = std::make_unique<GltfModel>();
    existingModel->rasterOverlayDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::WebMercator);
    existingModel->rasterOverlayDetails.rasterOverlayRectangles.push_back(
        projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            tile.bounds));
    tile.content.renderContent.prepareGltfContent(
        std::move(existingModel),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;

    RasterOverlayTileProvider* provider = activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);
    DirectRasterMapping& mapped =
        tile.rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missing;
    ASSERT_EQ(
        DirectRasterMapping::MoreDetail::Unknown,
        mapped.update(
            tile.key,
            tile.content.renderContent.rasterOverlayDetails(),
            512.0,
            512.0,
            *provider,
            nullptr,
            missing));
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    mapped.getLoadingTile()->setTexture(std::make_unique<TestTexture>(4, 4));

    RecordingPrepareRendererResources recorder;
    mapped.update(
        tile.key,
        tile.content.renderContent.rasterOverlayDetails(),
        512.0,
        512.0,
        *provider,
        &recorder,
        missing);
    ASSERT_EQ(DirectRasterMapping::State::Attached,
              mapped.getState());
    ASSERT_EQ(1, recorder.attachCount);
    ASSERT_EQ(0, recorder.detachCount);

    TileContentLoadResult replacementResult =
        TileContentLoadResult::render(std::make_unique<GltfModel>());
    TileContentUploadCommitter::prepareRenderContent(
        tile,
        TileLoadedContent::fromContentResult(std::move(replacementResult)),
        {},
        nullptr,
        &recorder);

    EXPECT_EQ(1, recorder.detachCount);
    EXPECT_EQ(0u, tile.rasterOverlayState.mappingCount());
    EXPECT_TRUE(tile.content.renderContent.hasGltfModel());
}

TEST(RasterOverlayLifecycleTest, MissingPreciseRectangleWithoutRenderDetailsUsesPlaceholder) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    RasterOverlayDetails emptyDetails;
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    const DirectRasterMapping::MoreDetail moreDetail = mapped.update(
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
    EXPECT_EQ(DirectRasterMapping::MoreDetail::No, moreDetail);
    ASSERT_EQ(1u, missing.size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator, missing[0]);
    EXPECT_EQ(0, provider.getCachedTileCount());
}

TEST(
    RasterOverlayLifecycleTest,
    FailedRasterFallbackSelectsAncestorByOverlayOwnerLikeCesiumNative) {
    auto overlayA = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    auto overlayB = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider providerA(
        overlayA->getProvider(),
        *scheme,
        nullptr);
    RasterOverlayTileProvider providerB(
        overlayB->getProvider(),
        *scheme,
        nullptr);
    providerA.setOwner(overlayA.get());
    providerB.setOwner(overlayB.get());

    const TileKey parentKey{scheme->id(), 2, 1, 1};
    const TileKey childKey{scheme->id(), 3, 2, 2};
    const Rectangle parentBounds = scheme->tileToRectangle(parentKey);
    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    RasterOverlayDetails parentDetails =
        makeProviderDetails(*scheme, parentBounds);
    RasterOverlayDetails childDetails =
        makeProviderDetails(*scheme, childBounds);

    TilesetTile parent(parentKey, parentBounds);
    auto wrongOwner = std::make_unique<DirectRasterMapping>();
    std::vector<RasterOverlayProjection> wrongMissing;
    wrongOwner->update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        providerB,
        nullptr,
        wrongMissing,
        nullptr,
        0);
    ASSERT_NE(nullptr, wrongOwner->getLoadingTile());
    wrongOwner->getLoadingTile()->setTexture(
        std::make_unique<TestTexture>(4, 4));
    wrongOwner->update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        providerB,
        nullptr,
        wrongMissing,
        nullptr,
        0);
    RasterOverlayTile* wrongReady = wrongOwner->getReadyTile();
    ASSERT_NE(nullptr, wrongReady);
    parent.rasterOverlayState.mappings().push_back(std::move(wrongOwner));

    auto matchingOwner = std::make_unique<DirectRasterMapping>();
    std::vector<RasterOverlayProjection> matchingMissing;
    matchingOwner->update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        providerA,
        nullptr,
        matchingMissing,
        nullptr,
        1);
    ASSERT_NE(nullptr, matchingOwner->getLoadingTile());
    matchingOwner->getLoadingTile()->setTexture(
        std::make_unique<TestTexture>(4, 4));
    matchingOwner->update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        providerA,
        nullptr,
        matchingMissing,
        nullptr,
        1);
    RasterOverlayTile* matchingReady = matchingOwner->getReadyTile();
    ASSERT_NE(nullptr, matchingReady);
    parent.rasterOverlayState.mappings().push_back(std::move(matchingOwner));

    TilesetTile child(childKey, childBounds, &parent);
    DirectRasterMapping childMapped;
    std::vector<RasterOverlayProjection> childMissing;
    childMapped.update(
        child.key,
        childDetails,
        512.0,
        512.0,
        providerA,
        nullptr,
        childMissing,
        &parent,
        0);
    ASSERT_NE(nullptr, childMapped.getLoadingTile());
    childMapped.getLoadingTile()->setState(
        RasterOverlayTile::LoadState::Failed);

    const DirectRasterMapping::MoreDetail fallbackUpdate =
        childMapped.update(
            child.key,
            childDetails,
            512.0,
            512.0,
            providerA,
            nullptr,
            childMissing,
            &parent,
            0);

    EXPECT_EQ(DirectRasterMapping::MoreDetail::No, fallbackUpdate);
    EXPECT_EQ(matchingReady, childMapped.getReadyTile());
    EXPECT_NE(wrongReady, childMapped.getReadyTile());
    EXPECT_EQ(nullptr, childMapped.getLoadingTile());
    EXPECT_EQ(DirectRasterMapping::ReadyTileSource::Ancestor,
              childMapped.getReadyTileSource());
}

TEST(RasterOverlayLifecycleTest, AttachedUnknownReportsMoreDetailLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    DirectRasterMapping::MoreDetail first = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    ASSERT_EQ(DirectRasterMapping::MoreDetail::Unknown, first);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setTexture(std::make_unique<TestTexture>(4, 4));

    DirectRasterMapping::MoreDetail promoted = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, promoted);
    EXPECT_EQ(DirectRasterMapping::State::Unattached,
              mapped.getState());
    EXPECT_EQ(loadingTile, mapped.getReadyTile());

    RecordingPrepareRendererResources recorder;
    DirectRasterMapping::MoreDetail attached = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        &recorder,
        missing);
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, attached);
    EXPECT_EQ(DirectRasterMapping::State::Attached, mapped.getState());
    EXPECT_EQ(1, recorder.attachCount);
    EXPECT_EQ(loadingTile, recorder.lastRasterTile.get());
    EXPECT_EQ(loadingTile->getTexture(), recorder.lastTexture);

    DirectRasterMapping::MoreDetail fastPath = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        &recorder,
        missing);
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Yes, fastPath);
    EXPECT_FALSE(mapped.isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest,
     AttachedTileWithLostRendererResourcesReattachesBeforeFastPath) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details =
        makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setTexture(std::make_unique<TestTexture>(4, 4));

    RecordingPrepareRendererResources recorder;
    mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        &recorder,
        missing);
    ASSERT_EQ(DirectRasterMapping::State::Attached, mapped.getState());
    ASSERT_EQ(1, recorder.attachCount);
    ASSERT_NE(nullptr, mapped.texture());

    loadingTile->setRendererResources(nullptr);
    DirectRasterMapping::MoreDetail recovered = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        &recorder,
        missing);

    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, recovered);
    EXPECT_EQ(DirectRasterMapping::State::Attached, mapped.getState());
    EXPECT_EQ(2, recorder.attachCount);
    EXPECT_EQ(loadingTile, recorder.lastRasterTile.get());
    EXPECT_EQ(loadingTile->getTexture(), recorder.lastTexture);
}

TEST(RasterOverlayLifecycleTest,
     ReadyTileTextureAccessorReflectsLateTextureRecovery) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details =
        makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->markLoadedWithoutTexture();

    mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);
    ASSERT_EQ(loadingTile, mapped.getReadyTile());
    EXPECT_EQ(nullptr, mapped.texture());

    loadingTile->setTexture(std::make_unique<TestTexture>(8, 8));

    EXPECT_EQ(loadingTile->getTexture(), mapped.texture());
}

TEST(RasterOverlayLifecycleTest, FailedTileWithoutAncestorBecomesReadyLikeCesiumNative) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
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

    const DirectRasterMapping::MoreDetail failedUpdate = mapped.update(
        key,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missing);

    EXPECT_EQ(DirectRasterMapping::MoreDetail::No, failedUpdate);
    EXPECT_EQ(failedTile, mapped.getReadyTile());
    EXPECT_EQ(nullptr, mapped.getLoadingTile());
    EXPECT_EQ(DirectRasterMapping::State::Attached,
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

    DirectRasterMapping mapped;
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
    ASSERT_EQ(DirectRasterMapping::State::Attached,
              mapped.getState());

    RecordingPrepareRendererResources recorder;
    mapped.detachFromTile(&recorder);

    EXPECT_EQ(0, recorder.detachCount);
    EXPECT_EQ(DirectRasterMapping::State::Unattached,
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
    DirectRasterMapping& parentMapping =
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
    ASSERT_EQ(DirectRasterMapping::State::Unattached,
              parentMapping.getState());

    DirectRasterMapping childMapping;
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

    const DirectRasterMapping::MoreDetail fallback =
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

    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, fallback);
    EXPECT_EQ(parentReady, childMapping.getReadyTile());
    EXPECT_EQ(DirectRasterMapping::State::Unattached,
              childMapping.getState());
    EXPECT_FALSE(childMapping.isMoreDetailAvailable());

    RecordingPrepareRendererResources recorder;
    const DirectRasterMapping::MoreDetail attachedFallback =
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

    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown,
              attachedFallback);
    EXPECT_EQ(DirectRasterMapping::State::TemporarilyAttached,
              childMapping.getState());
    EXPECT_EQ(parentReady, childMapping.getReadyTile());
    EXPECT_FALSE(childMapping.isMoreDetailAvailable());
    EXPECT_EQ(1, recorder.attachCount);
    EXPECT_EQ(parentReady, recorder.lastRasterTile.get());
    EXPECT_EQ(parentReady->getTexture(), recorder.lastTexture);
}

// 祖先-only 合成的空瓦(Loaded 且无渲染资源)不是可用替身。
//
// 旧契约认下它:空瓦自己的 mapping 停在空瓦上永不替换,子瓦的父链遍历也 break
// 在它身上,于是明明祖父有纹理可画,父与整条子孙链全是空洞。本测试钉死新契约
// —— 空瓦被跳过,父与子都落到能画的祖父上。
TEST(RasterOverlayLifecycleTest,
     BlankAncestorOnlyRasterFallsThroughToDrawableGrandparentForSelfAndChild) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    RasterOverlayTileProvider provider(
        overlay->getProvider(),
        overlay->getTileScheme(),
        nullptr);
    provider.setOwner(overlay.get());

    const TileKey grandparentKey{overlay->getTileScheme().id(), 1, 0, 0};
    const TileKey parentKey{overlay->getTileScheme().id(), 2, 0, 0};
    const TileKey childKey{overlay->getTileScheme().id(), 3, 0, 0};
    RasterOverlayDetails grandparentDetails = makeProviderDetails(
        overlay->getTileScheme(),
        overlay->getTileScheme().tileToRectangle(grandparentKey));
    RasterOverlayDetails parentDetails = makeProviderDetails(
        overlay->getTileScheme(),
        overlay->getTileScheme().tileToRectangle(parentKey));
    RasterOverlayDetails childDetails = makeProviderDetails(
        overlay->getTileScheme(),
        overlay->getTileScheme().tileToRectangle(childKey));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile grandparentTile(
        grandparentKey,
        overlay->getTileScheme().tileToRectangle(grandparentKey));
    DirectRasterMapping& grandparentMapping =
        grandparentTile.rasterOverlayState.ensureMapping(0);
    grandparentMapping.update(
        grandparentKey,
        grandparentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0);
    RasterOverlayTile* grandparentReady = grandparentMapping.getLoadingTile();
    ASSERT_NE(nullptr, grandparentReady);
    grandparentReady->setTexture(std::make_unique<TestTexture>(4, 4));
    grandparentReady->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    grandparentMapping.update(
        grandparentKey,
        grandparentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0);

    TilesetTile parentTile(
        parentKey,
        overlay->getTileScheme().tileToRectangle(parentKey),
        &grandparentTile);
    DirectRasterMapping& parentMapping =
        parentTile.rasterOverlayState.ensureMapping(0);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        &grandparentTile,
        0);
    RasterOverlayTile* parentReady = parentMapping.getLoadingTile();
    ASSERT_NE(nullptr, parentReady);
    parentReady->markLoadedWithoutTexture();
    parentReady->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        &grandparentTile,
        0);
    // D2:空瓦自己的 mapping 不再停在空瓦上,换成能画的祖父。
    ASSERT_EQ(nullptr, parentReady->getTexture());
    EXPECT_NE(parentReady, parentMapping.getReadyTile());
    EXPECT_EQ(grandparentReady, parentMapping.getReadyTile());
    EXPECT_EQ(DirectRasterMapping::ReadyTileSource::Ancestor,
              parentMapping.getReadyTileSource());

    DirectRasterMapping childMapping;
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

    RecordingPrepareRendererResources recorder;
    const DirectRasterMapping::MoreDetail fallback =
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

    // D1:子瓦的父链遍历跳过空的父,走到能画的祖父,并且真的贴上去。
    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown, fallback);
    EXPECT_NE(parentReady, childMapping.getReadyTile());
    EXPECT_EQ(grandparentReady, childMapping.getReadyTile());
    EXPECT_EQ(DirectRasterMapping::ReadyTileSource::Ancestor,
              childMapping.getReadyTileSource());
    EXPECT_EQ(1, recorder.attachCount);
    EXPECT_EQ(grandparentReady->getTexture(), recorder.lastTexture);
    EXPECT_NE(SurfaceRasterBindingKind::None,
              chooseSurfaceRasterBinding(&childMapping).kind);
}

TEST(RasterOverlayLifecycleTest,
     FailedRasterFallbackPrefersParentLoadingTileLikeCesiumNative) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    RasterOverlayTileProvider provider(
        overlay->getProvider(),
        overlay->getTileScheme(),
        nullptr);
    provider.setOwner(overlay.get());

    const TileKey grandparentKey{overlay->getTileScheme().id(), 1, 0, 0};
    const TileKey parentKey{overlay->getTileScheme().id(), 2, 0, 0};
    const TileKey childKey{overlay->getTileScheme().id(), 3, 0, 0};
    RasterOverlayDetails grandparentDetails = makeProviderDetails(
        overlay->getTileScheme(),
        overlay->getTileScheme().tileToRectangle(grandparentKey));
    RasterOverlayDetails parentDetails = makeProviderDetails(
        overlay->getTileScheme(),
        overlay->getTileScheme().tileToRectangle(parentKey));
    RasterOverlayDetails childDetails = makeProviderDetails(
        overlay->getTileScheme(),
        overlay->getTileScheme().tileToRectangle(childKey));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile grandparentTile(
        grandparentKey,
        overlay->getTileScheme().tileToRectangle(grandparentKey));
    DirectRasterMapping& grandparentMapping =
        grandparentTile.rasterOverlayState.ensureMapping(0);
    grandparentMapping.update(
        grandparentKey,
        grandparentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0);
    RasterOverlayTile* grandparentReady =
        grandparentMapping.getLoadingTile();
    ASSERT_NE(nullptr, grandparentReady);
    grandparentReady->setTexture(std::make_unique<TestTexture>(4, 4));
    grandparentMapping.update(
        grandparentKey,
        grandparentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0);
    ASSERT_EQ(grandparentReady, grandparentMapping.getReadyTile());

    TilesetTile parentTile(
        parentKey,
        overlay->getTileScheme().tileToRectangle(parentKey),
        &grandparentTile);
    DirectRasterMapping& parentMapping =
        parentTile.rasterOverlayState.ensureMapping(0);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        &grandparentTile,
        0);
    RasterOverlayTile* parentLoading = parentMapping.getLoadingTile();
    ASSERT_NE(nullptr, parentLoading);

    RecordingPrepareRendererResources recorder;
    const DirectRasterMapping::MoreDetail parentFallback =
        parentMapping.update(
            parentKey,
            parentDetails,
            512.0,
            512.0,
            provider,
            &recorder,
            missing,
            &grandparentTile,
            0);
    ASSERT_EQ(DirectRasterMapping::MoreDetail::Unknown,
              parentFallback);
    ASSERT_EQ(parentLoading, parentMapping.getLoadingTile());
    ASSERT_EQ(grandparentReady, parentMapping.getReadyTile());
    ASSERT_EQ(DirectRasterMapping::State::TemporarilyAttached,
              parentMapping.getState());

    DirectRasterMapping childMapping;
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
    RasterOverlayTile* childOriginal = childMapping.getLoadingTile();
    ASSERT_NE(nullptr, childOriginal);
    childOriginal->setState(RasterOverlayTile::LoadState::Failed);

    const DirectRasterMapping::MoreDetail childFallback =
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

    EXPECT_EQ(DirectRasterMapping::MoreDetail::Unknown,
              childFallback);
    EXPECT_EQ(parentLoading, childMapping.getLoadingTile());
    EXPECT_EQ(grandparentReady, childMapping.getReadyTile());
    EXPECT_NE(childOriginal, childMapping.getLoadingTile());
    EXPECT_FALSE(childMapping.isMoreDetailAvailable());
}

TEST(RasterOverlayLifecycleTest,
     BoundingRegionAncestorFallbackComputesChildUvWindow) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    RasterOverlayTileProvider provider(
        overlay->getProvider(),
        overlay->getTileScheme(),
        nullptr);
    provider.setOwner(overlay.get());

    const TileKey parentKey{overlay->getTileScheme().id(), 2, 1, 1};
    const TileKey childKey{overlay->getTileScheme().id(), 3, 2, 2};
    const Rectangle parentRectangle =
        overlay->getTileScheme().tileToRectangle(parentKey);
    const Rectangle childRectangle =
        overlay->getTileScheme().tileToRectangle(childKey);
    const Rectangle projectedChildRectangle =
        projectForProvider(provider, childRectangle);
    RasterOverlayDetails parentDetails =
        makeProviderDetails(overlay->getTileScheme(), parentRectangle);
    RasterOverlayDetails emptyDetails;
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parentTile(parentKey, parentRectangle);
    DirectRasterMapping& parentMapping =
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

    DirectRasterMapping childMapping;
    childMapping.update(
        childKey,
        emptyDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        nullptr,
        0,
        false,
        projectedChildRectangle);
    ASSERT_NE(nullptr, childMapping.getLoadingTile());

    childMapping.update(
        childKey,
        emptyDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missing,
        &parentTile,
        0,
        false,
        projectedChildRectangle);

    EXPECT_EQ(parentReady, childMapping.getReadyTile());
    const TileTextureWindow expectedWindow =
        TileSurface::textureWindowForNorthWestUv(
            TileSurface::computeTranslationAndScale(
                projectedChildRectangle,
                parentReady->getRectangle()));
    EXPECT_NEAR(
        expectedWindow.offsetU,
        childMapping.getTranslationU(),
        1e-6f);
    EXPECT_NEAR(
        expectedWindow.offsetV,
        childMapping.getTranslationV(),
        1e-6f);
    EXPECT_NEAR(expectedWindow.scaleU, childMapping.getScaleU(), 1e-6f);
    EXPECT_NEAR(expectedWindow.scaleV, childMapping.getScaleV(), 1e-6f);
}

TEST(RasterOverlayLifecycleTest, DirectMappingReadyTileRetainsProviderCacheUntilReleased) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey key{scheme->id(), 3, 4, 2};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    provider.setFrameNumber(1);
    DirectRasterMapping mapped;
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
    DirectRasterMapping mapped;
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
    // resourceKeepAlive 已移除(改 Renderer 帧级保活)。本测只需一个 shared_ptr
    // 持有者验证 provider trim 尊重外部引用 —— 用局部变量替代原字段,语义等价。
    std::shared_ptr<const void> keepAlive = binding.tileHandle;
    binding.tileHandle.reset();

    mapped.releaseTileReferences(nullptr);
    provider.setFrameNumber(200);
    provider.trimUnusedTiles();

    EXPECT_EQ(provider.getCachedTileCount(), 1);
    EXPECT_FALSE(weakTile.expired());
    EXPECT_EQ(command.textures[0], tile->getTexture());

    keepAlive.reset();
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

    DirectRasterMapping mapped;
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
    DirectRasterMapping noTextureMapped;
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

TEST(RasterOverlayLifecycleTest, RasterResolutionClassifiesExactDirectTile) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(1, 1);
    const TileKey key{scheme->id(), 1, 1, 1};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);
    ASSERT_NE(mapped.getLoadingTile(), nullptr);
    mapped.getLoadingTile()->setTexture(std::make_unique<TestTexture>(4, 4));
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);

    const DirectRasterBindingResolution binding =
        resolveDirectRasterBinding(&mapped);
    const RasterResolution& resolution = binding.resolution;
    EXPECT_EQ(resolution.contentForm, RasterContentForm::Exact);
    EXPECT_EQ(resolution.sourceRelation, RasterSourceRelation::Own);
    EXPECT_EQ(resolution.requestState, RasterRequestState::Loaded);
    EXPECT_EQ(resolution.attachment, RasterAttachmentState::Unattached);
    EXPECT_TRUE(resolution.hasCoverage);
    EXPECT_TRUE(resolution.coverReady);
    EXPECT_TRUE(resolution.drawable);
    EXPECT_NE(binding.sample.texture, nullptr);
}

TEST(RasterOverlayLifecycleTest, RasterResolutionClassifiesComposedAndAncestor) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    const TileKey parentKey{scheme->id(), 1, 1, 1};
    const TileKey childKey{scheme->id(), 2, 2, 2};
    RasterOverlayDetails parentDetails =
        makeProviderDetails(*scheme, scheme->tileToRectangle(parentKey));
    RasterOverlayDetails childDetails =
        makeProviderDetails(*scheme, scheme->tileToRectangle(childKey));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    DirectRasterMapping& parentMapped =
        parent.rasterOverlayState.ensureMapping(0);
    parentMapped.update(parentKey, parentDetails, 256.0, 256.0, provider,
                        nullptr, missing);
    ASSERT_NE(parentMapped.getLoadingTile(), nullptr);
    parentMapped.getLoadingTile()->setTexture(
        std::make_unique<TestTexture>(4, 4));
    parentMapped.update(parentKey, parentDetails, 256.0, 256.0, provider,
                        nullptr, missing);

    DirectRasterMapping composed;
    const Rectangle source = scheme->tileToRectangle(parentKey);
    const Rectangle partial(source.west(), source.south(),
                            source.west() + source.width() * 0.75,
                            source.north());
    RasterOverlayDetails partialDetails = makeProviderDetails(*scheme, partial);
    composed.update(parentKey, partialDetails, 256.0, 256.0, provider,
                    nullptr, missing);
    ASSERT_NE(composed.getLoadingTile(), nullptr);
    ASSERT_TRUE(composed.getLoadingTile()->isDirectCompositeTile());
    composed.getLoadingTile()->setTexture(std::make_unique<TestTexture>(4, 4));
    composed.update(parentKey, partialDetails, 256.0, 256.0, provider,
                    nullptr, missing);
    const RasterResolution composedResolution =
        resolveDirectRasterResolution(&composed);
    EXPECT_EQ(composedResolution.contentForm, RasterContentForm::Composed);
    EXPECT_EQ(composedResolution.sourceRelation, RasterSourceRelation::Own);

    DirectRasterMapping child;
    child.update(childKey, childDetails, 256.0, 256.0, provider, nullptr,
                 missing, &parent, 0);
    const RasterResolution ancestorResolution =
        resolveDirectRasterResolution(&child);
    EXPECT_EQ(ancestorResolution.sourceRelation, RasterSourceRelation::Ancestor);
    EXPECT_TRUE(ancestorResolution.drawable);
}

TEST(RasterOverlayLifecycleTest, RasterResolutionRetainsFailedTargetState) {
    NullImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    const TileKey key{scheme->id(), 1, 1, 1};
    RasterOverlayDetails details = makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);
    ASSERT_NE(mapped.getLoadingTile(), nullptr);
    mapped.getLoadingTile()->setState(RasterOverlayTile::LoadState::Failed);
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);

    const RasterResolution resolution = resolveDirectRasterResolution(&mapped);
    EXPECT_EQ(resolution.requestState, RasterRequestState::Failed);
    EXPECT_TRUE(resolution.targetFailed);
    EXPECT_EQ(resolution.contentForm, RasterContentForm::None);
    EXPECT_FALSE(resolution.drawable);
}

TEST(RasterOverlayLifecycleTest,
     RasterResolutionSeparatesPlaceholderLoadingAndCoverage) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(2, 2);
    const TileKey key{scheme->id(), 2, 1, 1};
    RasterOverlayDetails details =
        makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    provider.setReady(false);
    DirectRasterMapping placeholder;
    placeholder.update(
        key, details, 256.0, 256.0, provider, nullptr, missing);
    const RasterResolution placeholderResolution =
        resolveDirectRasterResolution(&placeholder);
    EXPECT_EQ(placeholderResolution.requestState,
              RasterRequestState::Placeholder);
    EXPECT_FALSE(placeholderResolution.hasCoverage);
    EXPECT_FALSE(placeholderResolution.targetPending);
    EXPECT_FALSE(placeholderResolution.drawable);
    EXPECT_EQ(placeholderResolution.desiredZoom, -1);

    provider.setReady(true);
    DirectRasterMapping loading;
    loading.update(key, details, 256.0, 256.0, provider, nullptr, missing);
    ASSERT_NE(loading.getLoadingTile(), nullptr);
    const RasterResolution unloadedResolution =
        resolveDirectRasterResolution(&loading);
    EXPECT_EQ(unloadedResolution.requestState, RasterRequestState::Unloaded);
    EXPECT_TRUE(unloadedResolution.hasCoverage);
    EXPECT_TRUE(unloadedResolution.targetPending);
    EXPECT_EQ(unloadedResolution.desiredZoom, 2);

    loading.getLoadingTile()->setState(RasterOverlayTile::LoadState::Loading);
    const RasterResolution loadingResolution =
        resolveDirectRasterResolution(&loading);
    EXPECT_EQ(loadingResolution.requestState, RasterRequestState::Loading);
    EXPECT_TRUE(loadingResolution.targetPending);
}

TEST(RasterOverlayLifecycleTest,
     RasterResolutionRetainsFailedTargetWhileDrawingAncestor) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(1, 2);
    const TileKey parentKey{scheme->id(), 1, 0, 0};
    const TileKey childKey{scheme->id(), 2, 0, 0};
    RasterOverlayDetails parentDetails =
        makeProviderDetails(*scheme, scheme->tileToRectangle(parentKey));
    RasterOverlayDetails childDetails =
        makeProviderDetails(*scheme, scheme->tileToRectangle(childKey));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    DirectRasterMapping& parentMapped =
        parent.rasterOverlayState.ensureMapping(0);
    parentMapped.update(
        parentKey, parentDetails, 256.0, 256.0, provider, nullptr, missing);
    ASSERT_NE(parentMapped.getLoadingTile(), nullptr);
    parentMapped.getLoadingTile()->setTexture(
        std::make_unique<TestTexture>(4, 4));
    parentMapped.update(
        parentKey, parentDetails, 256.0, 256.0, provider, nullptr, missing);

    DirectRasterMapping child;
    child.update(
        childKey, childDetails, 256.0, 256.0, provider, nullptr, missing,
        &parent, 0);
    ASSERT_NE(child.getLoadingTile(), nullptr);
    child.getLoadingTile()->setState(RasterOverlayTile::LoadState::Failed);
    child.update(
        childKey, childDetails, 256.0, 256.0, provider, nullptr, missing,
        &parent, 0);

    const RasterResolution resolution = resolveDirectRasterResolution(&child);
    EXPECT_EQ(resolution.requestState, RasterRequestState::Failed);
    EXPECT_TRUE(resolution.targetFailed);
    EXPECT_EQ(resolution.sourceRelation, RasterSourceRelation::Ancestor);
    EXPECT_TRUE(resolution.drawable);
    EXPECT_EQ(resolution.desiredZoom, 1);
    EXPECT_EQ(resolution.resolvedZoom, 1);
}

TEST(RasterOverlayLifecycleTest,
     RasterResolutionLeaseKeepsSampleAliveAfterMappingRelease) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setLevelRange(1, 1);
    const TileKey key{scheme->id(), 1, 1, 1};
    RasterOverlayDetails details =
        makeProviderDetails(*scheme, scheme->tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;

    DirectRasterMapping mapped;
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);
    ASSERT_NE(mapped.getLoadingTile(), nullptr);
    mapped.getLoadingTile()->setTexture(std::make_unique<TestTexture>(4, 4));
    mapped.update(key, details, 256.0, 256.0, provider, nullptr, missing);
    std::weak_ptr<RasterOverlayTile> weak = mapped.getReadyTileHandle();

    DirectRasterBindingResolution binding = resolveDirectRasterBinding(&mapped);
    ASSERT_TRUE(binding.sample.resourceLease);
    const long withMappingAndLease = weak.use_count();
    mapped.releaseTileReferences(nullptr);
    EXPECT_FALSE(weak.expired());
    EXPECT_LT(weak.use_count(), withMappingAndLease);
    const long withLeaseOnly = weak.use_count();
    binding.sample.resourceLease.reset();
    EXPECT_LT(weak.use_count(), withLeaseOnly);
}

TEST(RasterOverlayLifecycleTest,
     RasterBindingSetUsesFrozenFrameMetadataAndGeneration) {
    RasterOverlay::Options options;
    options.opacity = 0.65f;
    options.role = RasterOverlayRole::AnnotationOverlay;
    options.priority = RasterOverlayPriority::Normal;
    options.fallbackPolicy = RasterOverlayFallbackPolicy::SkipUntilReady;
    options.blocksCompleteRenderable = false;
    RasterOverlay overlay(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    ActivatedRasterOverlay activated(overlay);
    RasterOverlayRuntime runtime({&activated});
    RasterOverlayTileProvider* provider = activated.ensureTileProvider(nullptr);
    provider->setLevelRange(1, 1);
    ASSERT_TRUE(runtime.beginFrame(91, nullptr));
    const RasterOverlayFrameContext& frame = runtime.frameContext();

    const TileKey key{overlay.getTileScheme().id(), 1, 1, 1};
    TilesetTile tile(key, overlay.getTileScheme().tileToRectangle(key));
    RasterOverlayDetails details = makeProviderDetails(
        overlay.getTileScheme(), overlay.getTileScheme().tileToRectangle(key));
    std::vector<RasterOverlayProjection> missing;
    DirectRasterMapping& mapped =
        tile.rasterOverlayState.ensureMapping(0);
    mapped.update(key, details, 256.0, 256.0, *provider, nullptr, missing);
    ASSERT_NE(mapped.getLoadingTile(), nullptr);
    mapped.getLoadingTile()->setTexture(std::make_unique<TestTexture>(4, 4));
    mapped.update(key, details, 256.0, 256.0, *provider, nullptr, missing);

    overlay.setVisible(false);
    overlay.setOpacity(0.1f);
    overlay.getOptions().role = RasterOverlayRole::BaseImagery;
    overlay.getOptions().priority = RasterOverlayPriority::Low;
    overlay.getOptions().fallbackPolicy =
        RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
    overlay.getOptions().blocksCompleteRenderable = true;

    const RasterBindingSet bindings = RasterBindingSet::resolve(tile, frame);
    ASSERT_EQ(bindings.size(), 1u);
    const RasterResolution& resolution = bindings.bindings().front().resolution;
    EXPECT_TRUE(resolution.visible);
    EXPECT_FLOAT_EQ(resolution.opacity, 0.65f);
    EXPECT_EQ(resolution.role, RasterOverlayRole::AnnotationOverlay);
    EXPECT_EQ(resolution.priority, RasterOverlayPriority::Normal);
    EXPECT_EQ(resolution.fallbackPolicy,
              RasterOverlayFallbackPolicy::SkipUntilReady);
    EXPECT_FALSE(resolution.blocksCompleteRenderable);
    EXPECT_EQ(resolution.generation, frame.generation());
    EXPECT_TRUE(resolution.allowedByPolicy);
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
    DirectRasterMapping& mapped =
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
    DirectRasterMapping& noTextureMapped =
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
    DirectRasterMapping& mapped =
        tile.rasterOverlayState.ensureMapping(0);
    mapped.update(key, details, 256.0, 256.0, *provider, nullptr, missing);
    RasterOverlayTile* failedTile = mapped.getLoadingTile();
    ASSERT_NE(nullptr, failedTile);
    failedTile->setState(RasterOverlayTile::LoadState::Failed);

    mapped.update(key, details, 256.0, 256.0, *provider, nullptr, missing);

    ASSERT_TRUE(tile.rasterOverlayState.hasReadyMapping(0));
    ASSERT_FALSE(tile.rasterOverlayState.hasDrawableReadyMapping(0));

    failedTile->setState(RasterOverlayTile::LoadState::Loaded);
    failedTile->setTexture(std::make_unique<TestTexture>(4, 4));

    EXPECT_TRUE(tile.rasterOverlayState.hasDrawableReadyMapping(0));
}

TEST(RasterOverlayLifecycleTest, SurfaceRasterBindingClassifiesSharedReadyTileAsRealTile) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    TileKey parentKey{scheme->id(), 0, 0, 0};
    TileKey childKey{scheme->id(), 1, 0, 1};

    RasterOverlayDetails details;
    details.setGeographicRectangle(scheme->tileToRectangle(parentKey));
    std::vector<RasterOverlayProjection> missing;

    TilesetTile parentTile(parentKey, scheme->tileToRectangle(parentKey));
    DirectRasterMapping& parentMapping =
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

    DirectRasterMapping childMapped;
    RasterOverlayDetails childDetails;
    childDetails.setGeographicRectangle(scheme->tileToRectangle(childKey));
    childMapped.update(
        childKey,
        childDetails,
        4096.0,
        4096.0,
        provider,
        nullptr,
        missing,
        &parentTile,
        0);

    SurfaceRasterBinding binding = chooseSurfaceRasterBinding(&childMapped);
    EXPECT_EQ(SurfaceRasterBindingKind::RealTile, binding.kind);
    EXPECT_EQ(binding.tile, parentRaster);
}

TEST(RasterOverlayLifecycleTest, CompositeImageCompletesNoCoverageLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});
    Rectangle outside(
        target.east(),
        target.south(),
        target.east() + target.width() * 0.5,
        target.north());

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> noCoverage;
    noCoverage.push_back({
        TileKey{scheme->id(), 2, 0, 0},
        outside,
        makeImage(2, 2, 10),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Unknown});
    auto noCoverageResult =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            target,
            std::move(noCoverage));
    EXPECT_EQ(nullptr, noCoverageResult.image);
    EXPECT_EQ(target, noCoverageResult.rectangle);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::Yes,
              noCoverageResult.moreDetailAvailable);

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> full;
    full.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        makeImage(2, 2, 20),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Unknown});
    auto fullResult =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            target,
            std::move(full));
    ASSERT_NE(nullptr, fullResult.image);
    EXPECT_GT(fullResult.image->width, 0);
    EXPECT_GT(fullResult.image->height, 0);
    EXPECT_EQ(20, fullResult.image->pixels[0]);
}

TEST(RasterOverlayLifecycleTest, CompositeImageRejectsMalformedSourceImages) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    auto malformed = std::make_unique<DecodedImage>();
    malformed->width = 2;
    malformed->height = 2;
    malformed->channels = 4;
    malformed->pixels.resize(4);

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        std::move(malformed),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Unknown});
    auto result =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            target,
            std::move(sources));
    EXPECT_EQ(nullptr, result.image);
}

TEST(RasterOverlayLifecycleTest,
     CompositeImagePreservesSourceDiagnosticsLikeCesiumNativeErrorList) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    auto malformed = std::make_unique<DecodedImage>();
    malformed->width = 2;
    malformed->height = 2;
    malformed->channels = 4;
    malformed->pixels.resize(4);

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        makeImage(2, 2, 20),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No,
        {"valid source warning"}});
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        std::move(malformed),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Unknown,
        {"malformed source error"}});

    auto result =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            target,
            std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(2u, result.diagnostics.size());
    EXPECT_EQ("valid source warning", result.diagnostics[0]);
    EXPECT_EQ("malformed source error", result.diagnostics[1]);

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> ancestorOnly;
    ancestorOnly.push_back({
        TileKey{scheme->id(), 0, 0, 0},
        target,
        makeImage(2, 2, 30),
        target,
        RasterOverlayTile::MoreDetailAvailable::Yes,
        {"ancestor fallback warning"}});

    auto emptyResult =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            target,
            std::move(ancestorOnly));

    ASSERT_NE(nullptr, emptyResult.image);
    EXPECT_TRUE(emptyResult.image->pixels.empty());
    ASSERT_EQ(1u, emptyResult.diagnostics.size());
    EXPECT_EQ("ancestor fallback warning", emptyResult.diagnostics[0]);
}

TEST(RasterOverlayLifecycleTest, CompositeImageUsesSourceMoreDetailFlagLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        makeImage(2, 2, 20),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              result.moreDetailAvailable);
}

TEST(RasterOverlayLifecycleTest, CompositeImagePreservesRgbSourceChannelsLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        makeRgbImage(2, 2, 20, 30, 40),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(3, result.image->channels);
    ASSERT_GE(result.image->pixels.size(), 3u);
    EXPECT_EQ(20, result.image->pixels[0]);
    EXPECT_EQ(30, result.image->pixels[1]);
    EXPECT_EQ(40, result.image->pixels[2]);
    EXPECT_EQ(
        static_cast<size_t>(result.image->width) *
            static_cast<size_t>(result.image->height) * 3u,
        result.image->pixels.size());
}

TEST(RasterOverlayLifecycleTest,
     CompositeImagePreservesSingleChannelSourceLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        makeGrayImage(2, 2, 77),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            target,
            std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(1, result.image->channels);
    ASSERT_EQ(4u, result.image->pixels.size());
    EXPECT_EQ(77, result.image->pixels[0]);
    EXPECT_EQ(
        static_cast<size_t>(result.image->width) *
            static_cast<size_t>(result.image->height),
        result.image->pixels.size());
}

TEST(RasterOverlayLifecycleTest, CompositeImageUsesLargestSourceChannelCountLikeCesiumNative) {
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

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        westKey,
        westBounds,
        makeRgbImage(1, 1, 10, 11, 12),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});
    sources.push_back({
        eastKey,
        eastBounds,
        makeImage(1, 1, 20, 21, 22, 23),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(4, result.image->channels);
    ASSERT_GE(result.image->pixels.size(), 8u);
    EXPECT_EQ(10, result.image->pixels[0]);
    EXPECT_EQ(11, result.image->pixels[1]);
    EXPECT_EQ(12, result.image->pixels[2]);
    EXPECT_EQ(255, result.image->pixels[3]);
    EXPECT_EQ(20, result.image->pixels[4]);
    EXPECT_EQ(21, result.image->pixels[5]);
    EXPECT_EQ(22, result.image->pixels[6]);
    EXPECT_EQ(23, result.image->pixels[7]);
}

TEST(RasterOverlayLifecycleTest,
     CompositeImageUsesLargestBytesPerChannelLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey key{scheme->id(), 1, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(key);

    auto image = std::make_unique<DecodedImage>();
    image->width = 1;
    image->height = 1;
    image->channels = 1;
    image->bytesPerChannel = 2;
    image->pixels = {0x34, 0x12};

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        key,
        bounds,
        std::move(image),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            bounds,
            std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(1, result.image->channels);
    EXPECT_EQ(2, result.image->bytesPerChannel);
    ASSERT_EQ(2u, result.image->pixels.size());
    EXPECT_EQ(0x34, result.image->pixels[0]);
    EXPECT_EQ(0x12, result.image->pixels[1]);
}

TEST(RasterOverlayLifecycleTest,
     CompositeImagePreservesDuplicateCreditsLikeCesiumNative) {
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

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        westKey,
        westBounds,
        makeRgbImage(1, 1, 10, 11, 12),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});
    sources.back().credits = {"Imagery credit"};
    sources.push_back({
        eastKey,
        eastBounds,
        makeRgbImage(1, 1, 20, 21, 22),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});
    sources.back().credits = {"Imagery credit"};

    auto result =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            target,
            std::move(sources));

    ASSERT_NE(nullptr, result.image);
    ASSERT_EQ(2u, result.credits.size());
    EXPECT_EQ("Imagery credit", result.credits[0]);
    EXPECT_EQ("Imagery credit", result.credits[1]);
}

TEST(RasterOverlayLifecycleTest,
     CompositeImageClearsWiderTargetChannelBytesWhenNarrowerSourceOverwrites) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey key{scheme->id(), 1, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(key);

    auto wideImage = std::make_unique<DecodedImage>();
    wideImage->width = 1;
    wideImage->height = 1;
    wideImage->channels = 3;
    wideImage->bytesPerChannel = 2;
    wideImage->pixels = {0x01, 0x80, 0x02, 0x80, 0x03, 0x80};

    auto narrowImage = std::make_unique<DecodedImage>();
    narrowImage->width = 1;
    narrowImage->height = 1;
    narrowImage->channels = 3;
    narrowImage->bytesPerChannel = 1;
    narrowImage->pixels = {0x11, 0x22, 0x33};

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        key,
        bounds,
        std::move(wideImage),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});
    sources.push_back({
        key,
        bounds,
        std::move(narrowImage),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            bounds,
            std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(3, result.image->channels);
    EXPECT_EQ(2, result.image->bytesPerChannel);
    ASSERT_EQ(6u, result.image->pixels.size());
    EXPECT_EQ(0x11, result.image->pixels[0]);
    EXPECT_EQ(0x00, result.image->pixels[1]);
    EXPECT_EQ(0x22, result.image->pixels[2]);
    EXPECT_EQ(0x00, result.image->pixels[3]);
    EXPECT_EQ(0x33, result.image->pixels[4]);
    EXPECT_EQ(0x00, result.image->pixels[5]);
}

TEST(RasterOverlayLifecycleTest,
     CompositeImageDoesNotClampMeasuredOutputLikeCesiumNative) {
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

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        westKey,
        westBounds,
        makeImage(512, 512, 10),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});
    sources.push_back({
        eastKey,
        eastBounds,
        makeImage(512, 512, 20),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result =
        RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
            *scheme,
            target,
            std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(1024, result.image->width);
    EXPECT_EQ(512, result.image->height);
    ASSERT_GE(result.image->pixels.size(), 1024u * 512u * 4u);
    EXPECT_EQ(10, result.image->pixels[0]);
    EXPECT_EQ(20, result.image->pixels[512u * 4u]);
}

TEST(RasterOverlayLifecycleTest,
     CompositeImageReturnsEmptyForAncestorOnlySourcesLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 0, 0, 0},
        target,
        makeImage(2, 2, 20),
        target,
        RasterOverlayTile::MoreDetailAvailable::Yes});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(0, result.image->width);
    EXPECT_EQ(0, result.image->height);
    EXPECT_EQ(0, result.image->channels);
    EXPECT_TRUE(result.image->pixels.empty());
    EXPECT_EQ(Rectangle{}, result.rectangle);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              result.moreDetailAvailable);
}

TEST(RasterOverlayLifecycleTest, CompositeImageReturnsCoveredRectangleLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle target = scheme->tileToRectangle(
        TileKey{scheme->id(), 1, 0, 0});
    Rectangle covered(
        target.west(),
        target.south(),
        target.west() + target.width() * 0.5,
        target.north());

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        TileKey{scheme->id(), 2, 0, 0},
        covered,
        makeImage(2, 2, 30),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(2, result.image->width);
    EXPECT_EQ(2, result.image->height);
    EXPECT_TRUE(result.rectangle.equalsEpsilon(covered, 1e-12));
    EXPECT_EQ(30, result.image->pixels[0]);
}

TEST(RasterOverlayLifecycleTest, CompositeImageBlitsSourcePixelBlocksLikeCesiumNative) {
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

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        westKey,
        westBounds,
        std::move(westImage),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Unknown});
    sources.push_back({
        eastKey,
        eastBounds,
        std::move(eastImage),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Unknown});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

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

TEST(RasterOverlayLifecycleTest, CompositeImageUsesAncestorSubsetLikeCesiumNative) {
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

    auto eastImage = makeImage(2, 2, 20);
    auto ancestorImage = makeImage(4, 2, 10);

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        eastKey,
        eastBounds,
        std::move(eastImage),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::No});
    sources.push_back({
        TileKey{scheme->id(), 1, 0, 0},
        target,
        std::move(ancestorImage),
        westBounds,
        RasterOverlayTile::MoreDetailAvailable::Yes});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

    ASSERT_NE(nullptr, result.image);
    ASSERT_EQ(4, result.image->width);
    ASSERT_EQ(2, result.image->height);
    EXPECT_EQ(10, result.image->pixels[0]);
    EXPECT_EQ(10, result.image->pixels[4]);
    EXPECT_EQ(20, result.image->pixels[8]);
    EXPECT_EQ(20, result.image->pixels[12]);
    EXPECT_EQ(RasterOverlayTile::MoreDetailAvailable::No,
              result.moreDetailAvailable);
}

TEST(RasterOverlayLifecycleTest, CompositeImageKeepsTinyProjectedOverlap) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey sourceKey{scheme->id(), 2, 1, 0};
    const Rectangle sourceBounds = scheme->tileToRectangle(sourceKey);
    const double tinyHeight = sourceBounds.height() * 0.001;
    const Rectangle target(
        sourceBounds.west(),
        sourceBounds.north() - tinyHeight,
        sourceBounds.east(),
        sourceBounds.north());

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        sourceKey,
        sourceBounds,
        makeImage(64, 64, 40),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Unknown});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(64, result.image->width);
    EXPECT_EQ(1, result.image->height);
}

TEST(RasterOverlayLifecycleTest, CompositeImageUsesProjectedWebMercatorHeight) {
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

    std::vector<RasterOverlayTileProvider::QuadtreeSourceImage> sources;
    sources.push_back({
        sourceKey,
        sourceBounds,
        makeImage(64, 64, 50),
        std::nullopt,
        RasterOverlayTile::MoreDetailAvailable::Unknown});

    auto result = RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
        *scheme,
        target,
        std::move(sources));

    const double sourceProjectedHeight =
        std::log(std::tan(sourceBounds.north() * 0.5 + M_PI * 0.25)) -
        std::log(std::tan(sourceBounds.south() * 0.5 + M_PI * 0.25));
    const double targetProjectedHeight =
        std::log(std::tan(target.north() * 0.5 + M_PI * 0.25)) -
        std::log(std::tan(target.south() * 0.5 + M_PI * 0.25));
    const int expectedHeight = static_cast<int>(
        std::ceil(targetProjectedHeight / (sourceProjectedHeight / 64.0)));

    ASSERT_NE(nullptr, result.image);
    EXPECT_EQ(64, result.image->width);
    EXPECT_EQ(expectedHeight, result.image->height);
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

// cesium _totalTilesCurrentlyLoading 语义：节流名额在加载（下载+合成）
// 完成时释放，GPU 上传由 RasterTextureUpload lane 单独限速。名额若持有
// 到上传消费，交互期上传被 defer 时节流会被积压占满（真机 54/20），
// 新加载全部被卡。
TEST(RasterOverlayLifecycleTest,
     ThrottleSlotReleasedWhenLoadCompletesBeforeUploadLikeCesiumNative) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey key{scheme->id(), 2, 1, 1};
    provider.setCoverageRectangle(scheme->tileToRectangle(key));
    auto tile = provider.getTile(key);
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));

    // 加载完成（pendingUploads 入队）后、上传消费前：名额已释放
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
    EXPECT_EQ(0, provider.getThrottledTilesCurrentlyLoading());

    EXPECT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(0, provider.getThrottledTilesCurrentlyLoading());
    EXPECT_EQ(0, provider.getPendingUploadCount());
}

// 交互期 Direct raster 上传按 budget lane 涓流消费，而不是无条件 defer
// （旧行为对 "direct-composite/" 前缀一律拒绝，长交互积压 60+ 影像停更）。
TEST(RasterOverlayLifecycleTest,
     InteractionConsumesDirectCompositeUploadWithinSizeGate) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey coveredKey{scheme->id(), 1, 0, 0};
    provider.setCoverageRectangle(scheme->tileToRectangle(coveredKey));
    const Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});

    RasterOverlayTileProvider::TilePtr tile =
        provider
            .mapRasterTilesToGeometryTile(
                projectForProvider(provider, rootBounds),
                256.0,
                256.0)
            .tile;
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(tile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));

    // interactionActive = true：≤512² 的 Direct raster 必须被消费
    int processed = 0;
    for (int attempt = 0; attempt < 200 && processed < 1; ++attempt) {
        processed += provider.processPendingUploads(true).processedUploads;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(1, processed)
        << "交互期 Direct raster 上传仍被无条件 defer（积压回归）";
    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(0, provider.getThrottledTilesCurrentlyLoading());
}

TEST(RasterOverlayLifecycleTest,
     DirectCompositeCacheInvalidationDropsStalePendingUploadsBeforeGpuUpload) {
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    auto uploader = std::make_unique<CountingRasterUploader>();
    CountingRasterUploader* uploaderPtr = uploader.get();
    RasterOverlayTileProvider provider(imagery, *scheme, std::move(uploader));

    const TileKey coveredKey{scheme->id(), 1, 0, 0};
    provider.setCoverageRectangle(scheme->tileToRectangle(coveredKey));
    const Rectangle rootBounds =
        scheme->tileToRectangle(TileKey{scheme->id(), 0, 0, 0});

    RasterOverlayTileProvider::TilePtr tile =
        provider
            .mapRasterTilesToGeometryTile(
                projectForProvider(provider, rootBounds),
                256.0,
                256.0)
            .tile;
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(tile->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));

    std::weak_ptr<RasterOverlayTile> weakTile = tile;
    tile.reset();

    provider.setMaximumScreenSpaceError(1.0);

    EXPECT_EQ(0, provider.getPendingUploadCount());
    EXPECT_EQ(0, provider.getCachedTileCount());
    EXPECT_TRUE(weakTile.expired());

    provider.processPendingUploads(false);
    EXPECT_EQ(0, uploaderPtr->uploadCount);
}

// abandon（setReady(false)/析构）与迟到的完成回调竞态时，节流名额只能
// 释放一次：finishOneSource 置 completed 到回调 erase 之间条目仍在
// activeDirectCompositeSourceSets，两侧都会认领释放权；双重释放会把后续在途
// 加载的名额偷走（计数提前归零，节流放行超额加载）。
TEST(RasterOverlayLifecycleTest,
     AbandonRacingLateComposeReleasesThrottleSlotExactlyOnce) {
    DeferredImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey keyA{scheme->id(), 3, 2, 1};
    const TileKey keyB{scheme->id(), 3, 1, 1};
    const Rectangle boundsA = scheme->tileToRectangle(keyA);
    const Rectangle boundsB = scheme->tileToRectangle(keyB);
    // coverage 取两瓦片北半幅：目标窗口 ≠ 源矩形，强制走异步 compose 路径
    const Rectangle coverage(
        boundsB.west(),
        boundsA.south() + boundsA.height() * 0.5,
        boundsA.east(),
        boundsA.north());
    provider.setCoverageRectangle(coverage);

    RasterOverlayTileProvider::TilePtr tileA =
        provider
            .mapRasterTilesToGeometryTile(
                projectForProvider(provider, boundsA), 512.0, 512.0)
            .tile;
    ASSERT_NE(nullptr, tileA);
    ASSERT_TRUE(tileA->isDirectCompositeTile());
    ASSERT_TRUE(provider.loadTile(*tileA));
    ASSERT_EQ(1, provider.getThrottledTilesCurrentlyLoading());
    ASSERT_FALSE(imagery.pending.empty());

    // 占满线程池：A 的 compose 只能排队，"completed 已置位、回调未执行"
    // 的竞态窗口由闸门确定性地撑开
    auto gate = std::make_shared<std::promise<void>>();
    std::shared_future<void> gateFuture = gate->get_future().share();
    auto blockersRunning = std::make_shared<std::atomic<int>>(0);
    const int poolThreads =
        static_cast<int>(AsyncSystem::pool().threadCount());
    for (int i = 0; i < poolThreads; ++i) {
        AsyncSystem::pool().enqueue([gateFuture, blockersRunning]() {
            blockersRunning->fetch_add(1);
            gateFuture.wait();
        });
    }
    for (int attempt = 0;
         attempt < 2000 && blockersRunning->load() < poolThreads;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(poolThreads, blockersRunning->load());

    // 源全部完成：finishOneSource 把 compose 入队（被闸门挡住），
    // activeDirectCompositeSourceSets 条目仍在、名额仍被 A 持有
    while (!imagery.pending.empty()) {
        imagery.completeNext();
    }
    ASSERT_EQ(1, provider.getThrottledTilesCurrentlyLoading());

    provider.setReady(false);  // abandon：接管条目并释放名额（唯一一次）
    EXPECT_EQ(0, provider.getThrottledTilesCurrentlyLoading());
    provider.setReady(true);

    // B 入场占据一个名额；A 的迟到回调若再次释放就会偷走它
    RasterOverlayTileProvider::TilePtr tileB =
        provider
            .mapRasterTilesToGeometryTile(
                projectForProvider(provider, boundsB), 512.0, 512.0)
            .tile;
    ASSERT_NE(nullptr, tileB);
    ASSERT_TRUE(provider.loadTile(*tileB));
    ASSERT_EQ(1, provider.getThrottledTilesCurrentlyLoading());

    // 放行 compose；A 的回调走 depot epoch 不匹配分支并 bump revision，
    // 以 revision 变化作为回调已执行完的锚点（buggy 递减先于 bump）
    const uint64_t revisionBefore = provider.revision();
    gate->set_value();
    for (int attempt = 0;
         attempt < 2000 && provider.revision() == revisionBefore;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_NE(revisionBefore, provider.revision());

    EXPECT_EQ(1, provider.getThrottledTilesCurrentlyLoading())
        << "A 的迟到回调重复释放名额，偷走了 B 的在途名额";
}

namespace {

// 每张源瓦片把每个像素行涂成"全局 mercator 像素行号 % 251"，
// 合成输出的任何竖向内容位移都会变成可解码的数值偏移
class GroundTruthRowImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "ground-truth-rows"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 13; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     TileCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        requestedKeys.push_back(key);
        auto image = std::make_unique<DecodedImage>();
        image->width = 256;
        image->height = 256;
        image->channels = 1;
        image->pixels.resize(256u * 256u);
        for (int r = 0; r < 256; ++r) {
            const uint8_t value = static_cast<uint8_t>(
                (static_cast<long long>(key.y) * 256 + r) % 251);
            std::fill_n(image->pixels.begin() + r * 256, 256, value);
        }
        callback(key, std::move(image));
    }
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }
    std::vector<TileKey> requestedKeys;
};

// 用声明矩形（provider 投影空间）把纬度映射到合成图行，解码行号编码，
// 返回 内容行号 − 期望行号（mod 251 折回带符号），即竖向位移（源像素）
double composedRowShiftAtLatitude(const RasterOverlayTileProvider& provider,
                                  const DecodedImage& composed,
                                  const Rectangle& projectedRect,
                                  const Rectangle& geographicRect,
                                  double lat) {
    // 该纬度（弧度）的投影 y：用工具函数投影一个以 lat 为北界的退化矩形
    const Rectangle probe(
        geographicRect.west(),
        geographicRect.south(),
        geographicRect.east(),
        lat);
    const double projY = projectForProvider(provider, probe).north();
    const double v =
        (projectedRect.north() - projY) /
        std::max(1e-12, projectedRect.height());
    const int row = std::clamp(
        static_cast<int>(std::floor(v * composed.height)),
        0,
        composed.height - 1);
    const size_t idx =
        (static_cast<size_t>(row) * composed.width +
         composed.width / 2) * static_cast<size_t>(composed.channels);
    const int sampled = composed.pixels[idx];

    // 期望：该纬度落在的全局 z13 mercator 像素行
    constexpr double kPi = 3.14159265358979323846;
    const double mercY =
        std::log(std::tan(lat * 0.5 + kPi * 0.25));
    const double topDown = (kPi - mercY) / (2.0 * kPi);
    const long long globalRow = static_cast<long long>(
        std::floor(topDown * 8192.0 * 256.0));
    const int expected = static_cast<int>(globalRow % 251);

    int delta = (sampled - expected) % 251;
    if (delta > 125) delta -= 251;
    if (delta < -125) delta += 251;
    return static_cast<double>(delta);
}

} // namespace

// 真机横带假设的 CPU 裁定：z12 地理地形瓦片 ↔ srcz13 webmercator 1×2 行
// 合成。若 blit 锚定丢失瓦片在源纹素网格中的分数相位（1.152 行/瓦片，
// 相位每行 +0.152 ≈ 39 源像素），合成内容会整体竖移且相邻地形行位移
// 不同——这正是 30km 屏上 394px 横带的预测机制。本测试用行号编码源
// 逐纬度核对合成内容与声明矩形自洽，并核对相邻两行地形瓦片。
TEST(RasterOverlayLifecycleTest,
     DirectCompositeContentMatchesDeclaredRectAcrossAdjacentTerrainRows) {
    auto scheme = TileScheme::createXYZWebMercator();

    // 重庆 demo 视角纬度上的两个相邻 z12 地理地形瓦片行（引擎内部弧度）
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kDegToRad = kPi / 180.0;
    const double tileSize = kPi / 4096.0;  // geographic z12 格（弧度）
    const double demoLat = 29.617 * kDegToRad;
    const int rowIndex = static_cast<int>((kPi * 0.5 - demoLat) / tileSize);
    const double northA = kPi * 0.5 - rowIndex * tileSize;
    // 经度吸附到 z12 地理网格（与 z13 mercator 列同网格 → 1 列源计划）
    const int colIndex = static_cast<int>(
        (106.508 * kDegToRad + kPi) / tileSize);
    const double west = -kPi + colIndex * tileSize;
    const Rectangle terrainA(west, northA - tileSize, west + tileSize,
                             northA);
    const Rectangle terrainB(west, northA - 2.0 * tileSize,
                             west + tileSize, northA - tileSize);

    for (const Rectangle& terrainRect : {terrainA, terrainB}) {
        GroundTruthRowImageryProvider imagery;
        auto uploader = std::make_unique<CountingRasterUploader>();
        CountingRasterUploader* uploaderPtr = uploader.get();
        RasterOverlayTileProvider provider(imagery, *scheme,
                                           std::move(uploader));

        RasterOverlayTileProvider::TilePtr tile =
            provider
                .mapRasterTilesToGeometryTile(
                    projectForProvider(provider, terrainRect),
                    453.0,
                    518.0)
                .tile;
        ASSERT_NE(nullptr, tile);
        ASSERT_TRUE(tile->isDirectCompositeTile());
        ASSERT_TRUE(provider.loadTile(*tile));
        ASSERT_EQ(1, waitForPendingUploadCount(provider, 1));
        ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));

        // 场景前提自检：srcz13、单列多行（与真机 30km 视角一致；
        // 1.152 行/瓦片按相位落 2 或 3 行）
        ASSERT_GE(imagery.requestedKeys.size(), 2u);
        ASSERT_LE(imagery.requestedKeys.size(), 3u);
        for (const TileKey& k : imagery.requestedKeys) {
            EXPECT_EQ(13, k.z);
            EXPECT_EQ(imagery.requestedKeys.front().x, k.x);
        }

        const DecodedImage& composed = uploaderPtr->lastUpload;
        ASSERT_GT(composed.width, 0);
        ASSERT_GT(composed.height, 0);
        const Rectangle projectedRect = tile->getRectangle();
        ASSERT_FALSE(projectedRect.isEmpty());

        // 逐纬度核对（避开上下边缘半纹素）
        double maxAbsShift = 0.0;
        for (int i = 5; i <= 95; ++i) {
            const double lat =
                terrainRect.south() +
                terrainRect.height() * (static_cast<double>(i) / 100.0);
            const double shift = composedRowShiftAtLatitude(
                provider, composed, projectedRect, terrainRect, lat);
            maxAbsShift = std::max(maxAbsShift, std::abs(shift));
        }
        EXPECT_LE(maxAbsShift, 2.0)
            << "合成内容相对声明矩形竖移 " << maxAbsShift
            << " 源像素（terrain north=" << terrainRect.north()
            << "）——blit 锚定丢相位？";
    }
}

// ---------------------------------------------------------------------------
// RasterUploadProgress 守卫:正例推进、反例冻结、空闲不入账。这条 lane 正是
// "交互期硬冻结改 budget 涓流"那次修复的现场,守卫钉住"有符合资格的积压
// 却推进恒 0"不再复发。
// ---------------------------------------------------------------------------

TEST(RasterOverlayLifecycleTest, RasterUploadProgressPolicyObservesAdvanceFrozenIdle) {
    using earth_engine::policy::Id;
    ImmediateImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);

    const TileKey key{scheme->id(), 2, 1, 1};
    provider.setCoverageRectangle(scheme->tileToRectangle(key));
    auto tile = provider.getTile(key);
    ASSERT_NE(nullptr, tile);
    ASSERT_TRUE(provider.loadTile(*tile));
    ASSERT_EQ(1, provider.getPendingUploadCount());

    // 反例(冻结):有积压且非交互期(全部符合资格),预算把 lane 闸死 →
    // 分母进账、分子不进。持续如此即把窗口比率压向 0,越过下界报警。
    FrameResourceBudgetConfig frozenConfig;
    frozenConfig.maxRasterUploadsPerFrame = 0;
    FrameResourceBudget frozenBudget;
    frozenBudget.beginFrame(1, frozenConfig);

    const uint64_t num0 = policy::windowNumerator(Id::RasterUploadProgress);
    const uint64_t den0 = policy::windowDenominator(Id::RasterUploadProgress);

    provider.processPendingUploads(false, &frozenBudget);
    EXPECT_EQ(1, provider.getPendingUploadCount());
    EXPECT_EQ(0u, policy::windowNumerator(Id::RasterUploadProgress) - num0);
    EXPECT_EQ(1u, policy::windowDenominator(Id::RasterUploadProgress) - den0);

    // 正例(推进):默认预算放行 → 1/1。
    ASSERT_EQ(1, processPendingUploadsUntil(provider, 1));
    EXPECT_EQ(1u, policy::windowNumerator(Id::RasterUploadProgress) - num0);
    EXPECT_EQ(2u, policy::windowDenominator(Id::RasterUploadProgress) - den0);

    // 空闲(无积压)不入账。
    provider.processPendingUploads(false);
    EXPECT_EQ(2u, policy::windowDenominator(Id::RasterUploadProgress) - den0);
}
