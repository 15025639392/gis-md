#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
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

class ManualCompletionContentProvider final : public TilesetContentProvider {
public:
    struct PendingRequest {
        TileKey key;
        ContentCallback callback;
    };

    explicit ManualCompletionContentProvider(TileKey key)
        : key_(std::move(key)) {}

    std::string id() const override { return "manual-lifecycle-content"; }
    bool supportsTile(const TileKey& key) const override {
        return key == key_;
    }

    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        pendingRequests.push_back(PendingRequest{key, std::move(callback)});
    }

    bool completeWithEmpty(const TileKey& key) {
        auto it = std::find_if(
            pendingRequests.begin(),
            pendingRequests.end(),
            [&key](const PendingRequest& request) {
                return request.key == key;
            });
        if (it == pendingRequests.end()) {
            return false;
        }

        ContentCallback callback = std::move(it->callback);
        pendingRequests.erase(it);
        callback(key, TileContentLoadResult::empty());
        return true;
    }

    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    TileKey key_;
    std::vector<PendingRequest> pendingRequests;
};

} // namespace

TEST(TilesetLifecycleTest, DestructorWaitsForPendingContentCallbacks) {
    const TileKey key{"Geographic-TMS", 0, 0, 0};
    std::atomic<bool> completionCalled{false};
    std::thread completer;

    {
        auto provider = std::make_unique<ManualCompletionContentProvider>(key);
        ManualCompletionContentProvider* rawProvider = provider.get();
        Tileset tileset(
            TileScheme::createGeographicTMS(),
            {},
            nullptr,
            TilesetOptions{},
            std::move(provider));

        TilesetTestAccess::ensureTile(tileset, key);
        TilesetTestAccess::requestMissingTile(tileset, key);
        ASSERT_EQ(rawProvider->pendingRequests.size(), 1u);

        completer = std::thread([rawProvider, key, &completionCalled]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            completionCalled.store(
                rawProvider->completeWithEmpty(key),
                std::memory_order_release);
        });
    }

    completer.join();
    EXPECT_TRUE(completionCalled.load(std::memory_order_acquire));
}
