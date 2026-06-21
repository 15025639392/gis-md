#include <gtest/gtest.h>

#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/ImageryProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetProviderDiagnosticsCollector.h"

#include <memory>

using namespace earth_engine;

namespace {

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
        HeightmapCallback,
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
