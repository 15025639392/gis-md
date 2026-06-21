#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TilesetProviderDiagnosticsCollector.h"

using namespace earth_engine;

namespace {

class DiagnosticTerrainProvider final : public TerrainProvider {
public:
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
        ProviderRequestDiagnostics diagnostics;
        diagnostics.maximumTransportActiveRequests = 11;
        return diagnostics;
    }
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
