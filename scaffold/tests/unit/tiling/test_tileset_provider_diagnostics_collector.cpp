#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TilesetProviderDiagnosticsCollector.h"

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
