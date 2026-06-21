#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
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

class ManualCompletionTerrainProvider final : public TerrainProvider {
public:
    struct PendingRequest {
        TileKey key;
        TerrainCallback callback;
    };

    std::string id() const override { return "manual-lifecycle-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }

    std::string buildUrl(const TileKey&) const override {
        return "memory://manual-lifecycle-terrain";
    }

    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
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

        TerrainCallback callback = std::move(it->callback);
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

TEST(TilesetLifecycleTest, DestructorWaitsForPendingTerrainCallbacks) {
    const TileKey key{"Geographic-TMS", 1, 0, 0};
    std::atomic<bool> completionCalled{false};
    std::thread completer;

    {
        auto provider = std::make_unique<ManualCompletionTerrainProvider>();
        ManualCompletionTerrainProvider* rawProvider = provider.get();
        Tileset tileset(
            std::move(provider),
            TileScheme::createGeographicTMS(),
            {},
            nullptr,
            TilesetOptions{});

        TilesetTestAccess::ensureTile(tileset, key);
        TilesetTestAccess::requestMissingTile(tileset, key);
        ASSERT_EQ(rawProvider->pendingRequests.size(), 1u);

        completer = std::thread([rawProvider, key, &completionCalled]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            completionCalled.store(
                rawProvider->completeWithHeightmap(
                    key,
                    makeFlatHeightmap(3.0f)),
                std::memory_order_release);
        });
    }

    completer.join();
    EXPECT_TRUE(completionCalled.load(std::memory_order_acquire));
}
