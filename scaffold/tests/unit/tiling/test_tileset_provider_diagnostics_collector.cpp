#include <gtest/gtest.h>

#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/providers/ImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/scene/SceneTilesetDiagnostics.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetProviderDiagnosticsCollector.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static void requestMissingTile(Tileset& tileset, const TileKey& key) {
        tileset.requestMissingContent({
            TileLoadRequest{
                key,
                TileLoadPriorityGroup::Normal}});
    }
};
} // namespace earth_engine

namespace {

class BlockingHttpRequest final : public HttpRequest {
public:
    void cancel() override {}
};

class BlockingPlatformBridge final : public PlatformBridge {
public:
    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    std::unique_ptr<HttpRequest> get(
        const std::string&,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions = {}) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback_ = std::move(callback);
            entered_ = true;
            ++enteredCount_;
        }
        cv_.notify_all();
        return std::make_unique<BlockingHttpRequest>();
    }

    int maximumActiveRequests() const override { return 11; }

    bool waitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(
            lock,
            std::chrono::seconds(2),
            [this]() { return entered_; });
    }

    bool waitUntilEnteredCount(int count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(
            lock,
            std::chrono::seconds(2),
            [this, count]() { return enteredCount_ >= count; });
    }

    int enteredCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return enteredCount_;
    }

    bool complete(int statusCode, std::vector<uint8_t> body = {}) {
        std::function<void(int, std::vector<uint8_t>)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = std::move(callback_);
            entered_ = false;
        }
        if (!callback) {
            return false;
        }
        callback(statusCode, std::move(body));
        return true;
    }

    std::string cacheDirectory() const override { return "/tmp"; }
    std::string documentsDirectory() const override { return "/tmp"; }
    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }
    void log(LogLevel, const std::string&, const std::string&) override {}
    DeviceInfo deviceInfo() const override { return {}; }
    std::string getToken(const std::string&) const override { return {}; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::function<void(int, std::vector<uint8_t>)> callback_;
    bool entered_ = false;
    int enteredCount_ = 0;
};

class DiagnosticTerrainProvider final : public TerrainProvider {
public:
    explicit DiagnosticTerrainProvider(
        ProviderRequestDiagnostics diagnostics = {}) {
        diagnostics_ = diagnostics;
        if (diagnostics_.maximumTransportActiveRequests < 0) {
            diagnostics_.maximumTransportActiveRequests = 11;
        }
    }

    std::string id() const override { return "diagnostic-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://diagnostic-terrain";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {}
    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }
    ProviderRequestDiagnostics requestDiagnostics() const override {
        return diagnostics_;
    }

private:
    ProviderRequestDiagnostics diagnostics_;
};

class DiagnosticImageryProvider final : public ImageryProvider {
public:
    DiagnosticImageryProvider(
        std::string providerId,
        ProviderRequestDiagnostics diagnostics)
        : providerId_(std::move(providerId)),
          diagnostics_(diagnostics) {}

    std::string id() const override { return providerId_; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 2; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://diagnostic-imagery";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TileCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {}
    std::unique_ptr<DecodedImage> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }
    ProviderRequestDiagnostics requestDiagnostics() const override {
        return diagnostics_;
    }

private:
    std::string providerId_;
    ProviderRequestDiagnostics diagnostics_;
};

class ImmediateImageImageryProvider final : public ImageryProvider {
public:
    std::string id() const override { return "immediate-image"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 2; }
    int tileWidth() const override { return 4; }
    int tileHeight() const override { return 4; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://immediate-image";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TileCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        auto image = std::make_unique<DecodedImage>();
        image->width = tileWidth();
        image->height = tileHeight();
        image->channels = 4;
        image->pixels.resize(
            static_cast<size_t>(image->width) *
            static_cast<size_t>(image->height) * 4u,
            static_cast<uint8_t>(key.z + 1));
        callback(key, std::move(image));
    }
    std::unique_ptr<DecodedImage> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }
};

class DiagnosticContentProvider final : public TilesetContentProvider {
public:
    explicit DiagnosticContentProvider(ProviderRequestDiagnostics diagnostics)
        : diagnostics_(diagnostics) {}

    std::string id() const override { return "diagnostic-content"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey&,
        CancellationToken,
        ContentCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {}
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
    ProviderRequestDiagnostics requestDiagnostics() const override {
        return diagnostics_;
    }

private:
    ProviderRequestDiagnostics diagnostics_;
};

} // namespace

TEST(
    TilesetProviderDiagnosticsCollectorTest,
    ExposesProviderTransportLaneForFrameBudget) {
    DiagnosticTerrainProvider terrainProvider;

    const TilesetProviderDiagnosticsSnapshot snapshot =
        TilesetProviderDiagnosticsCollector::collect(
            &terrainProvider,
            nullptr,
            {});

    EXPECT_EQ(
        snapshot.terrainProviderRequests.maximumTransportActiveRequests,
        11);
    EXPECT_EQ(snapshot.allProviderRequests.maximumTransportActiveRequests, 11);
    EXPECT_EQ(snapshot.maximumTransportActiveRequests(20), 11u);
}

TEST(
    TilesetProviderDiagnosticsCollectorTest,
    AppliesTerrainProviderRequestDiagnosticsToLoadDiagnostics) {
    ProviderRequestDiagnostics providerDiagnostics;
    providerDiagnostics.requestsStarted = 1;
    providerDiagnostics.requestsCompleted = 0;
    providerDiagnostics.activeWorkerBlockingRequests = 0;
    providerDiagnostics.peakWorkerBlockingRequests = 0;
    providerDiagnostics.maximumTransportActiveRequests = 11;
    DiagnosticTerrainProvider terrainProvider(providerDiagnostics);

    const TilesetProviderDiagnosticsSnapshot snapshot =
        TilesetProviderDiagnosticsCollector::collect(
            &terrainProvider,
            nullptr,
            {});
    TilesetLoadDiagnostics loadDiagnostics;
    snapshot.applyTo(loadDiagnostics);

    EXPECT_EQ(loadDiagnostics.terrainProviderRequests.requestsStarted, 1);
    EXPECT_EQ(loadDiagnostics.terrainProviderRequests.requestsCompleted, 0);
    EXPECT_EQ(
        loadDiagnostics.terrainProviderRequests.activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        loadDiagnostics.terrainProviderRequests.peakWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        loadDiagnostics
            .terrainProviderRequests
            .maximumTransportActiveRequests,
        11);
}

TEST(
    TilesetProviderDiagnosticsCollectorTest,
    ContentAndRasterCollectorDoesNotPullLegacyTerrainProviderIntoMainTilesetPath) {
    ProviderRequestDiagnostics terrainDiagnostics;
    terrainDiagnostics.requestsStarted = 9;
    terrainDiagnostics.maximumTransportActiveRequests = 3;
    DiagnosticTerrainProvider terrainProvider(terrainDiagnostics);

    ProviderRequestDiagnostics contentDiagnostics;
    contentDiagnostics.requestsStarted = 1;
    contentDiagnostics.maximumTransportActiveRequests = 11;
    DiagnosticContentProvider contentProvider(contentDiagnostics);

    const TilesetProviderDiagnosticsSnapshot snapshot =
        TilesetProviderDiagnosticsCollector::collectContentAndRaster(
            &contentProvider,
            {});
    TilesetLoadDiagnostics loadDiagnostics;
    snapshot.applyTo(loadDiagnostics);

    EXPECT_EQ(loadDiagnostics.terrainProviderRequests.requestsStarted, 0);
    EXPECT_EQ(loadDiagnostics.contentProviderRequests.requestsStarted, 1);
    EXPECT_EQ(snapshot.maximumTransportActiveRequests(20), 11u);
    (void)terrainProvider;
}

TEST(
    TilesetProviderDiagnosticsCollectorTest,
    AggregatesRasterProviderRequestPeaks) {
    ProviderRequestDiagnostics firstDiagnostics;
    firstDiagnostics.requestsStarted = 2;
    firstDiagnostics.requestsCompleted = 1;
    firstDiagnostics.activeWorkerBlockingRequests = 1;
    firstDiagnostics.peakWorkerBlockingRequests = 3;
    firstDiagnostics.externalResourceRequestsStarted = 4;
    firstDiagnostics.externalResourceRequestsCompleted = 2;
    firstDiagnostics.activeExternalResourceBlockingRequests = 1;
    firstDiagnostics.peakExternalResourceBlockingRequests = 5;
    firstDiagnostics.maximumTransportActiveRequests = 8;

    ProviderRequestDiagnostics secondDiagnostics;
    secondDiagnostics.requestsStarted = 5;
    secondDiagnostics.requestsCompleted = 4;
    secondDiagnostics.activeWorkerBlockingRequests = 2;
    secondDiagnostics.peakWorkerBlockingRequests = 2;
    secondDiagnostics.externalResourceRequestsStarted = 3;
    secondDiagnostics.externalResourceRequestsCompleted = 3;
    secondDiagnostics.activeExternalResourceBlockingRequests = 2;
    secondDiagnostics.peakExternalResourceBlockingRequests = 4;
    secondDiagnostics.maximumTransportActiveRequests = 11;

    auto firstOverlay = std::make_unique<RasterOverlay>(
        std::make_unique<DiagnosticImageryProvider>(
            "diag-raster-a",
            firstDiagnostics),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    auto secondOverlay = std::make_unique<RasterOverlay>(
        std::make_unique<DiagnosticImageryProvider>(
            "diag-raster-b",
            secondDiagnostics),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay firstActivated(*firstOverlay);
    ActivatedRasterOverlay secondActivated(*secondOverlay);
    firstActivated.ensureTileProvider(nullptr);
    secondActivated.ensureTileProvider(nullptr);

    const TilesetProviderDiagnosticsSnapshot snapshot =
        TilesetProviderDiagnosticsCollector::collect(
            nullptr,
            nullptr,
            {&firstActivated, &secondActivated});
    const ProviderRequestDiagnostics& requests =
        snapshot.rasterProviderRequests;

    EXPECT_EQ(requests.requestsStarted, 7);
    EXPECT_EQ(requests.requestsCompleted, 5);
    EXPECT_EQ(requests.activeWorkerBlockingRequests, 3);
    EXPECT_EQ(requests.peakWorkerBlockingRequests, 3);
    EXPECT_EQ(requests.maximumTransportActiveRequests, 11);
    EXPECT_EQ(requests.externalResourceRequestsStarted, 7);
    EXPECT_EQ(requests.externalResourceRequestsCompleted, 5);
    EXPECT_EQ(requests.activeExternalResourceBlockingRequests, 3);
    EXPECT_EQ(requests.peakExternalResourceBlockingRequests, 5);
}

TEST(
    TilesetProviderDiagnosticsCollectorTest,
    ExposesRasterProviderRequestDiagnosticsThroughTileset) {
    BlockingPlatformBridge bridge;
    auto imageryProvider = std::make_unique<XYZImageryProvider>(
        "https://example.invalid/{z}/{x}/{y}.png");
    imageryProvider->setPlatformBridge(&bridge);
    auto overlay = std::make_unique<RasterOverlay>(
        std::move(imageryProvider),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* rasterProvider =
        activated.ensureTileProvider(nullptr);
    rasterProvider->setReady(true);

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {&activated},
        nullptr,
        TilesetOptions{});

    const TileKey rasterKey{"XYZ-WebMercator", 1, 0, 0};
    RasterOverlayTileProvider::TilePtr rasterTile =
        rasterProvider->getTile(rasterKey);
    ASSERT_TRUE(rasterTile);
    ASSERT_TRUE(rasterProvider->loadTile(*rasterTile));
    ASSERT_TRUE(bridge.waitUntilEntered());

    const TilesetLoadDiagnostics activeDiag = tileset.loadDiagnostics();
    EXPECT_EQ(activeDiag.rasterProviderRequests.requestsStarted, 1);
    EXPECT_EQ(activeDiag.rasterProviderRequests.requestsCompleted, 0);
    EXPECT_EQ(
        activeDiag.rasterProviderRequests.activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(activeDiag.rasterProviderRequests.peakWorkerBlockingRequests, 0);
    EXPECT_EQ(
        activeDiag.rasterProviderRequests.maximumTransportActiveRequests,
        11);
    EXPECT_EQ(activeDiag.rasterOverlayTilesLoading, 1);
    EXPECT_EQ(activeDiag.rasterSourceRequestsInFlight, 1);
    EXPECT_EQ(activeDiag.rasterPendingUploads, 0);
    EXPECT_EQ(activeDiag.pendingTerrainTotal(), 0);
    EXPECT_EQ(activeDiag.pendingContentTotal(), 0);

    ASSERT_TRUE(bridge.complete(404));
    for (int i = 0; i < 200 && bridge.enteredCount() < 2; ++i) {
        rasterProvider->processPendingUploads(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(bridge.waitUntilEnteredCount(2));
    ASSERT_TRUE(bridge.complete(404));
    for (int i = 0; i < 200 &&
                    (rasterProvider->requestDiagnostics().requestsCompleted < 2 ||
                     rasterProvider->getActiveRasterSourceRequests() != 0 ||
                     rasterProvider->getThrottledTilesCurrentlyLoading() != 0);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const TilesetLoadDiagnostics doneDiag = tileset.loadDiagnostics();
    EXPECT_EQ(doneDiag.rasterProviderRequests.requestsCompleted, 2);
    EXPECT_EQ(
        doneDiag.rasterProviderRequests.activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(doneDiag.rasterProviderRequests.peakWorkerBlockingRequests, 0);
    EXPECT_EQ(
        doneDiag.rasterProviderRequests.maximumTransportActiveRequests,
        11);
    EXPECT_EQ(doneDiag.rasterOverlayTilesLoading, 0);
    EXPECT_EQ(doneDiag.rasterSourceRequestsInFlight, 0);
    EXPECT_EQ(doneDiag.rasterPendingUploads, 0);
    EXPECT_EQ(RasterOverlayTile::LoadState::Failed, rasterTile->getState());
}

TEST(
    TilesetProviderDiagnosticsCollectorTest,
    ExposesRasterProviderRequestDiagnosticsThroughScene) {
    BlockingPlatformBridge bridge;
    auto imageryProvider = std::make_unique<XYZImageryProvider>(
        "https://example.invalid/{z}/{x}/{y}.png");
    imageryProvider->setPlatformBridge(&bridge);
    auto overlay = std::make_unique<RasterOverlay>(
        std::move(imageryProvider),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* rasterProvider =
        activated.ensureTileProvider(nullptr);
    rasterProvider->setReady(true);

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {&activated},
        nullptr,
        TilesetOptions{});

    const TileKey rasterKey{"XYZ-WebMercator", 1, 0, 0};
    RasterOverlayTileProvider::TilePtr rasterTile =
        rasterProvider->getTile(rasterKey);
    ASSERT_TRUE(rasterTile);
    ASSERT_TRUE(rasterProvider->loadTile(*rasterTile));
    ASSERT_TRUE(bridge.waitUntilEntered());

    Diagnostics diagnostics;
    SceneTilesetDiagnostics::reset(diagnostics);
    SceneTilesetDiagnostics::addTileset(diagnostics, tileset, true);
    EXPECT_EQ(diagnostics.rasterOverlayTilesLoading, 1);
    EXPECT_EQ(diagnostics.rasterSourceRequestsInFlight, 1);
    EXPECT_EQ(diagnostics.rasterPendingUploads, 0);
    EXPECT_EQ(diagnostics.rasterProviderRequestsStarted, 1);
    EXPECT_EQ(diagnostics.rasterProviderRequestsCompleted, 0);
    EXPECT_EQ(diagnostics.rasterProviderActiveWorkerBlockingRequests, 0);
    EXPECT_EQ(diagnostics.rasterProviderPeakWorkerBlockingRequests, 0);
    EXPECT_EQ(diagnostics.rasterTransportActiveRequestLimit, 11);
    EXPECT_EQ(diagnostics.pendingTerrainRequests, 0);
    EXPECT_EQ(diagnostics.pendingContentRequests, 0);

    ASSERT_TRUE(bridge.complete(404));
    for (int i = 0; i < 200 &&
                    rasterProvider->requestDiagnostics().requestsCompleted == 0;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

TEST(
    TilesetProviderDiagnosticsCollectorTest,
    ExposesRasterPendingUploadAndSourceBytes) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<ImmediateImageImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* rasterProvider =
        activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, rasterProvider);

    const TileKey rasterKey{"XYZ-WebMercator", 1, 0, 0};
    RasterOverlayTileProvider::TilePtr rasterTile =
        rasterProvider->getTile(rasterKey);
    ASSERT_TRUE(rasterTile);
    ASSERT_TRUE(rasterProvider->loadTile(*rasterTile));
    ASSERT_EQ(1, rasterProvider->getPendingUploadCount());
    ASSERT_EQ(0, rasterProvider->getPendingUploadBytes());
    ASSERT_GT(rasterProvider->getPendingUploadBudgetBytes(), 0);
    ASSERT_GT(rasterProvider->getCachedSourceTileBytes(), 0);

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {&activated},
        nullptr,
        TilesetOptions{});

    const TilesetLoadDiagnostics loadDiag = tileset.loadDiagnostics();
    EXPECT_EQ(loadDiag.rasterPendingUploads, 1);
    EXPECT_GT(loadDiag.rasterPendingUploadBytes, 0);
    EXPECT_GT(loadDiag.peakRasterPendingUploadBytes, 0);
    EXPECT_GT(loadDiag.rasterCachedSourceTileBytes, 0);
    EXPECT_GT(loadDiag.peakRasterCachedSourceTileBytes, 0);

    Diagnostics diagnostics;
    SceneTilesetDiagnostics::reset(diagnostics);
    SceneTilesetDiagnostics::addTileset(diagnostics, tileset, true);
    EXPECT_EQ(diagnostics.rasterPendingUploads, 1);
    EXPECT_GT(diagnostics.rasterPendingUploadBytes, 0);
    EXPECT_GT(diagnostics.peakRasterPendingUploadBytes, 0);
    EXPECT_GT(diagnostics.rasterCachedSourceTileBytes, 0);
    EXPECT_GT(diagnostics.peakRasterCachedSourceTileBytes, 0);
}

TEST(
    TilesetProviderDiagnosticsCollectorTest,
    AppliesContentProviderRequestDiagnosticsToLoadDiagnostics) {
    ProviderRequestDiagnostics providerDiagnostics;
    providerDiagnostics.requestsStarted = 1;
    providerDiagnostics.requestsCompleted = 0;
    providerDiagnostics.activeWorkerBlockingRequests = 0;
    providerDiagnostics.peakWorkerBlockingRequests = 0;
    providerDiagnostics.maximumTransportActiveRequests = 11;
    DiagnosticContentProvider contentProvider(providerDiagnostics);

    const TilesetProviderDiagnosticsSnapshot snapshot =
        TilesetProviderDiagnosticsCollector::collect(
            nullptr,
            &contentProvider,
            {});
    TilesetLoadDiagnostics loadDiagnostics;
    snapshot.applyTo(loadDiagnostics);

    EXPECT_EQ(loadDiagnostics.contentProviderRequests.requestsStarted, 1);
    EXPECT_EQ(loadDiagnostics.contentProviderRequests.requestsCompleted, 0);
    EXPECT_EQ(
        loadDiagnostics.contentProviderRequests.activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        loadDiagnostics.contentProviderRequests.peakWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        loadDiagnostics
            .contentProviderRequests
            .maximumTransportActiveRequests,
        11);
}
