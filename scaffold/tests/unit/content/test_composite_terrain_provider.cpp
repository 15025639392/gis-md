#include <gtest/gtest.h>

#include "earth_engine/content/CompositeTerrainProvider.h"
#include "earth_engine/content/EllipsoidTerrainContentProvider.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileChildMaterializer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileSelectionRootPolicy.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

constexpr const char* kScheme = "Geographic-TMS";
constexpr int kEllipsoidCap = 5;

// Partial-coverage stand-in for the quantized-mesh provider: only the tile
// column at (x==0, y==0) is "covered" at any zoom; everything else is a hole.
class FakePartialTerrainProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "fake-partial-terrain"; }

    bool supportsTile(const TileKey&) const override { return true; }

    bool providesTerrainQuadtree() const override { return true; }

    TileAvailabilityState availabilityState(const TileKey& key) const override {
        return (key.x == 0 && key.y == 0)
                   ? TileAvailabilityState::Available
                   : TileAvailabilityState::NotAvailable;
    }

    void requestTileContent(const TileKey& key,
                            CancellationToken,
                            ContentCallback callback,
                            HttpRequestPriority) override {
        lastRequestedKey = key;
        ++requestCount;
        callback(key, TileContentLoadResult::failed());
    }

    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    mutable int requestCount = 0;
    TileKey lastRequestedKey;
};

std::unique_ptr<CompositeTerrainProvider> makeComposite(
    FakePartialTerrainProvider** primaryOut = nullptr) {
    auto primary = std::make_unique<FakePartialTerrainProvider>();
    if (primaryOut) {
        *primaryOut = primary.get();
    }
    auto ellipsoid = std::make_unique<EllipsoidTerrainContentProvider>(
        kScheme, kEllipsoidCap, /*gridSize=*/16);
    return std::make_unique<CompositeTerrainProvider>(
        std::move(primary), std::move(ellipsoid), kEllipsoidCap);
}

std::string cacheKeyFor(const TileKey& key) {
    return key.schemeId.str() + ":" + std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" + std::to_string(key.y);
}

} // namespace

TEST(CompositeTerrainProviderTest, PureHoleWithinCapReportsAvailable) {
    auto composite = makeComposite();
    // A hole tile (x/y != 0) at z <= cap → ellipsoid floor makes it Available.
    EXPECT_EQ(TileAvailabilityState::Available,
              composite->availabilityState(TileKey{kScheme, kEllipsoidCap, 7, 3}));
}

TEST(CompositeTerrainProviderTest, PrimaryCoveredReportsAvailable) {
    auto composite = makeComposite();
    EXPECT_EQ(TileAvailabilityState::Available,
              composite->availabilityState(TileKey{kScheme, kEllipsoidCap, 0, 0}));
}

TEST(CompositeTerrainProviderTest, PureHoleBeyondCapReportsNotAvailable) {
    auto composite = makeComposite();
    // Hole tile beyond the ellipsoid cap → refinement stops (no floor).
    EXPECT_EQ(TileAvailabilityState::NotAvailable,
              composite->availabilityState(
                  TileKey{kScheme, kEllipsoidCap + 3, 200, 100}));
}

TEST(CompositeTerrainProviderTest, PrimaryCoveredBeyondCapStaysAvailable) {
    auto composite = makeComposite();
    // Primary coverage is never capped by the ellipsoid floor.
    EXPECT_EQ(TileAvailabilityState::Available,
              composite->availabilityState(
                  TileKey{kScheme, kEllipsoidCap + 3, 0, 0}));
}

TEST(CompositeTerrainProviderTest, AllHoleQuadWithinCapMaterializesFullQuad) {
    auto composite = makeComposite();
    auto scheme = TileScheme::createGeographicTMS();

    // Parent well inside the hole region so all 4 children are holes at z<=cap.
    const TileKey parentKey{kScheme, kEllipsoidCap - 1, 5, 5};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;
    parent.refine = TileRefine::Replace;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles
                     .emplace(
                         cacheKey,
                         std::make_unique<TilesetTile>(
                             key, scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    auto availability = [&composite](const TileKey& key) {
        return composite->availabilityState(key);
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent, /*maxZoom=*/20, availability, ensure);

    // The availability floor turns "no child available" (coarse leaf → 385km
    // skirt wall) into a full quad of ellipsoid children.
    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    for (const TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        // All four children are Available (ellipsoid floor), so none is marked
        // a terrain-availability upsample of the parent.
        EXPECT_FALSE(child->content.derivesTerrainFromParent());
    }
}

TEST(CompositeTerrainProviderTest, HolesAdjacentToCoverageAreAvailableNotUpsample) {
    auto composite = makeComposite();
    auto scheme = TileScheme::createGeographicTMS();

    // Parent whose SW child (x==0,y==0) is primary-covered and the other three
    // are holes within the cap. Without the floor the three holes would be
    // marked terrain-availability upsample (making them upsample-source
    // candidates); with the floor they are Available ellipsoid children instead.
    const TileKey parentKey{kScheme, kEllipsoidCap - 1, 0, 0};
    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.geometricError = 100.0;
    parent.refine = TileRefine::Replace;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles
                     .emplace(
                         cacheKey,
                         std::make_unique<TilesetTile>(
                             key, scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    auto availability = [&composite](const TileKey& key) {
        return composite->availabilityState(key);
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent, /*maxZoom=*/20, availability, ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    for (const TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_FALSE(child->content.isTerrainAvailabilityUpsample());
        EXPECT_FALSE(child->content.derivesTerrainFromParent());
    }
}

TEST(CompositeTerrainProviderTest, ContentRoutesToPrimaryWhereCovered) {
    FakePartialTerrainProvider* primary = nullptr;
    auto composite = makeComposite(&primary);
    ASSERT_NE(nullptr, primary);

    bool callbackFired = false;
    composite->requestTileContent(
        TileKey{kScheme, kEllipsoidCap, 0, 0},
        CancellationToken{},
        [&](const TileKey&, TileContentLoadResult) { callbackFired = true; },
        HttpRequestPriority::Normal);

    EXPECT_EQ(1, primary->requestCount);
    EXPECT_TRUE(callbackFired);
}

TEST(CompositeTerrainProviderTest, ContentRoutesToEllipsoidInHole) {
    FakePartialTerrainProvider* primary = nullptr;
    auto composite = makeComposite(&primary);
    ASSERT_NE(nullptr, primary);

    TileContentLoadResult captured;
    composite->requestTileContent(
        TileKey{kScheme, kEllipsoidCap, 7, 3},
        CancellationToken{},
        [&](const TileKey&, TileContentLoadResult result) {
            captured = std::move(result);
        },
        HttpRequestPriority::Normal);

    // Hole tile must NOT hit the primary source; the ellipsoid produces a real
    // terrain render result synchronously.
    EXPECT_EQ(0, primary->requestCount);
    EXPECT_EQ(TileLoadStatus::Renderable, captured.status);
    EXPECT_TRUE(captured.terrainRenderContent);
}
