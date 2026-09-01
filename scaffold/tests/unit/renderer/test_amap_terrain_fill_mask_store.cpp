#include <gtest/gtest.h>

#include "earth_engine/core/async/WorkLedger.h"
#include "earth_engine/providers/AmapSurfaceMaskImageryProvider.h"
#include "earth_engine/renderer/AmapTerrainFillMaskStore.h"

#include <vector>

using namespace earth_engine;

namespace {

using Done = AmapTerrainFillMaskStore::FeatureFetchCallback;

std::shared_ptr<const std::vector<Feature>> emptyFeatures() {
    return std::make_shared<const std::vector<Feature>>();
}

} // namespace

TEST(AmapTerrainFillMaskStore, CoalescesExactTileAndObservesInlineCacheHit) {
    WorkLedger::shared().resetForTesting();
    int fetches = 0;
    std::vector<Done> callbacks;
    auto style = std::make_shared<AmapSurfaceMaskStyleState>(10.0);
    AmapTerrainFillMaskStore store(
        [&](const TileKey&, CancellationToken, Done done) {
            ++fetches;
            callbacks.push_back(std::move(done));
        },
        style);
    const TileKey key{"XYZ-WebMercator", 4, 8, 7};

    EXPECT_TRUE(store.request(key).pending);
    EXPECT_TRUE(store.request(key).pending);
    EXPECT_EQ(1, fetches);
    EXPECT_EQ(1, WorkLedger::shared().outstandingForLabel(
                     "amapTerrainFillMaskFetch"));
    callbacks.front()(emptyFeatures());
    EXPECT_EQ(0, WorkLedger::shared().outstandingForLabel(
                     "amapTerrainFillMaskFetch"));

    const auto ready = store.request(key);
    ASSERT_NE(nullptr, ready.pixels);
    EXPECT_FALSE(ready.pending);
    EXPECT_EQ(static_cast<size_t>(256 * 256 * 4), ready.pixels->size());
    EXPECT_EQ(1, fetches);

    AmapTerrainFillMaskStore inlineStore(
        [](const TileKey&, CancellationToken, Done done) {
            done(emptyFeatures());
        },
        style);
    EXPECT_NE(nullptr, inlineStore.request(key).pixels)
        << "a decoded-cache hit must be uploadable in the same render frame";
    WorkLedger::shared().resetForTesting();
}

TEST(AmapTerrainFillMaskStore, NewStyleCancelsStalePendingGeneration) {
    std::vector<CancellationToken> tokens;
    std::vector<Done> callbacks;
    auto style = std::make_shared<AmapSurfaceMaskStyleState>(10.0);
    AmapTerrainFillMaskStore store(
        [&](const TileKey&, CancellationToken token, Done done) {
            tokens.push_back(token);
            callbacks.push_back(std::move(done));
        },
        style);
    const TileKey key{"XYZ-WebMercator", 4, 8, 7};

    EXPECT_TRUE(store.request(key).pending);
    style->setDisplayZoom(11.0);
    EXPECT_TRUE(store.request(key).pending);
    ASSERT_EQ(2u, callbacks.size());
    EXPECT_TRUE(tokens.front().isCancelled());

    callbacks.front()(emptyFeatures());
    EXPECT_TRUE(store.request(key).pending)
        << "late stale completion must not replace the new generation";
    callbacks.back()(emptyFeatures());
    const auto ready = store.request(key);
    EXPECT_NE(nullptr, ready.pixels);
    EXPECT_EQ(store.styleRevision(), ready.revision);
}

TEST(AmapTerrainFillMaskStore, BoundsResidentPagesWithLruEviction) {
    auto style = std::make_shared<AmapSurfaceMaskStyleState>(10.0);
    AmapTerrainFillMaskStore store(
        [](const TileKey&, CancellationToken, Done done) {
            done(emptyFeatures());
        },
        style,
        2);

    EXPECT_NE(nullptr,
              store.request(TileKey{"XYZ-WebMercator", 4, 8, 7}).pixels);
    EXPECT_NE(nullptr,
              store.request(TileKey{"XYZ-WebMercator", 4, 9, 7}).pixels);
    EXPECT_NE(nullptr,
              store.request(TileKey{"XYZ-WebMercator", 4, 10, 7}).pixels);
    EXPECT_EQ(2u, store.residentPageCount());
}

TEST(AmapTerrainFillMaskStore, LateCompletionAfterDestructionIsSafe) {
    WorkLedger::shared().resetForTesting();
    Done callback;
    {
        auto style = std::make_shared<AmapSurfaceMaskStyleState>(10.0);
        AmapTerrainFillMaskStore store(
            [&](const TileKey&, CancellationToken, Done done) {
                callback = std::move(done);
            },
            style);
        EXPECT_TRUE(store.request(
            TileKey{"XYZ-WebMercator", 4, 8, 7}).pending);
    }
    ASSERT_TRUE(static_cast<bool>(callback));
    EXPECT_NO_THROW(callback(emptyFeatures()));
    EXPECT_EQ(0, WorkLedger::shared().outstandingForLabel(
                     "amapTerrainFillMaskFetch"));
    WorkLedger::shared().resetForTesting();
}

TEST(AmapTerrainFillMaskStore, LandingTicketCoversPublishAndWake) {
    WorkLedger& ledger = WorkLedger::shared();
    ledger.resetForTesting();
    Done callback;
    auto style = std::make_shared<AmapSurfaceMaskStyleState>(10.0);
    {
        AmapTerrainFillMaskStore store(
            [&](const TileKey&, CancellationToken, Done done) {
                callback = std::move(done);
            },
            style);
        EXPECT_TRUE(store.request(
            TileKey{"XYZ-WebMercator", 4, 8, 7}).pending);
        EXPECT_EQ(1, ledger.outstandingForLabel(
                         "amapTerrainFillMaskFetch"));

        ASSERT_TRUE(static_cast<bool>(callback));
        callback(emptyFeatures());
        EXPECT_EQ(0, ledger.outstandingForLabel(
                         "amapTerrainFillMaskFetch"));
        EXPECT_TRUE(ledger.hasUnconsumedLanding());
        EXPECT_NE(nullptr, store.request(
            TileKey{"XYZ-WebMercator", 4, 8, 7}).pixels);
    }
    EXPECT_TRUE(ledger.consumeLanded(nullptr));
    ledger.resetForTesting();
}

TEST(AmapTerrainFillMaskStore, CancellationReleasesLandingTicketWithoutCallback) {
    WorkLedger& ledger = WorkLedger::shared();
    ledger.resetForTesting();
    auto style = std::make_shared<AmapSurfaceMaskStyleState>(10.0);
    Done ignored;
    {
        AmapTerrainFillMaskStore store(
            [&](const TileKey&, CancellationToken, Done done) {
                ignored = std::move(done);
            },
            style);
        EXPECT_TRUE(store.request(
            TileKey{"XYZ-WebMercator", 4, 8, 7}).pending);
        EXPECT_EQ(1, ledger.outstandingForLabel(
                         "amapTerrainFillMaskFetch"));
        style->setDisplayZoom(11.0);
        store.setStyleState(style);
        EXPECT_EQ(0, ledger.outstandingForLabel(
                         "amapTerrainFillMaskFetch"));
    }
    EXPECT_EQ(0, ledger.outstandingForLabel(
                     "amapTerrainFillMaskFetch"));
    // The transport callback may still arrive after cancellation. It must be
    // harmless and must not resurrect the ticket.
    if (ignored) EXPECT_NO_THROW(ignored(emptyFeatures()));
    EXPECT_EQ(0, ledger.outstandingForLabel(
                     "amapTerrainFillMaskFetch"));
    ledger.resetForTesting();
}

TEST(AmapTerrainFillMaskStore, EvictionReleasesLandingTicketWithoutCallback) {
    WorkLedger& ledger = WorkLedger::shared();
    ledger.resetForTesting();
    std::vector<Done> callbacks;
    auto style = std::make_shared<AmapSurfaceMaskStyleState>(10.0);
    AmapTerrainFillMaskStore store(
        [&](const TileKey&, CancellationToken, Done done) {
            callbacks.push_back(std::move(done));
        },
        style, 1);
    EXPECT_TRUE(store.request(
        TileKey{"XYZ-WebMercator", 4, 8, 7}).pending);
    EXPECT_EQ(1, ledger.outstandingForLabel(
                     "amapTerrainFillMaskFetch"));
    EXPECT_TRUE(store.request(
        TileKey{"XYZ-WebMercator", 4, 9, 7}).pending);
    EXPECT_EQ(1, ledger.outstandingForLabel(
                     "amapTerrainFillMaskFetch"));
    for (Done& callback : callbacks) {
        if (callback) EXPECT_NO_THROW(callback(emptyFeatures()));
    }
    EXPECT_EQ(0, ledger.outstandingForLabel(
                     "amapTerrainFillMaskFetch"));
    ledger.resetForTesting();
}
