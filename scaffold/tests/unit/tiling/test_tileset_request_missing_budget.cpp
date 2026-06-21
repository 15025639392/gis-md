#include <gtest/gtest.h>

#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static TileLoadRequestOutcome requestMissingTilesWithBudget(
        Tileset& tileset,
        FrameResourceBudget& budget,
        const TileKey& firstKey,
        const TileKey& secondKey) {
        return tileset.requestMissingContent(
            {
                TileLoadRequest{
                    firstKey,
                    TileLoadPriorityGroup::Normal,
                    100.0},
                TileLoadRequest{
                    secondKey,
                    TileLoadPriorityGroup::Normal,
                    1.0},
            },
            &budget);
    }
};
} // namespace earth_engine

namespace {

class ManualCompletionTerrainProvider final : public TerrainProvider {
public:
    struct PendingRequest {
        TileKey key;
        HeightmapCallback callback;
    };

    std::string id() const override { return "manual-completion-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }

    std::string buildUrl(const TileKey&) const override {
        return "memory://manual-completion-terrain";
    }

    void requestTile(
        const TileKey& key,
        CancellationToken,
        HeightmapCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        pendingRequests.push_back(PendingRequest{key, std::move(callback)});
    }

    bool completeWithHeightmap(
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        auto it = std::find_if(
            pendingRequests.begin(),
            pendingRequests.end(),
            [&key](const PendingRequest& request) {
                return request.key == key;
            });
        if (it == pendingRequests.end()) {
            return false;
        }

        HeightmapCallback callback = std::move(it->callback);
        pendingRequests.erase(it);
        callback(key, TerrainTileLoadResult::success(std::move(heightmap)));
        return true;
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    std::vector<PendingRequest> pendingRequests;
};

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {heightMeters, heightMeters, heightMeters, heightMeters};
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

} // namespace

TEST(TilesetRequestMissingBudgetTest,
     FrameResourceBudgetLimitsWorkerRequestsByPriority) {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;

    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    Tileset tileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options);

    const TileKey lowPriorityKey{"Geographic-TMS", 1, 0, 0};
    const TileKey highPriorityKey{"Geographic-TMS", 1, 1, 0};
    TilesetTestAccess::ensureTile(tileset, lowPriorityKey);
    TilesetTestAccess::ensureTile(tileset, highPriorityKey);

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxNetworkInflight = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    TileLoadRequestOutcome outcome =
        TilesetTestAccess::requestMissingTilesWithBudget(
            tileset,
            budget,
            lowPriorityKey,
            highPriorityKey);

    ASSERT_EQ(rawProvider->pendingRequests.size(), 1u);
    EXPECT_EQ(rawProvider->pendingRequests.front().key, highPriorityKey);
    EXPECT_EQ(outcome.issued, 1u);
    EXPECT_FALSE(outcome.blockedByInflight);
    EXPECT_EQ(budget.networkRequestsIssued(), 1u);

    EXPECT_TRUE(rawProvider->completeWithHeightmap(
        highPriorityKey,
        makeFlatHeightmap(2.0f)));
}
