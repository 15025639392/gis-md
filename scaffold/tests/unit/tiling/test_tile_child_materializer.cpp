#include <gtest/gtest.h>

#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/tiling/SurfaceRasterBinding.h"
#include "earth_engine/tiling/SurfaceTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileChildMaterializer.h"
#include "earth_engine/tiling/TileRasterUpsampledChildMaterializer.h"
#include "earth_engine/tiling/TileScheme.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}

    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
};

std::string cacheKeyFor(const TileKey& key) {
    return key.schemeId + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

RasterMappedToTilesetTile& addMoreDetailRasterMapping(
    TilesetTile& tile,
    RasterOverlayTileProvider& provider) {
    tile.content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>());
    tile.content.renderContent.setMeshReady(true);
    tile.content.renderContent.mutableRasterOverlayDetails()
        ->setGeographicRectangle(tile.bounds);

    auto& mapped = tile.rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    EXPECT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Unknown,
        mapped.update(
            tile.key,
            tile.content.renderContent.rasterOverlayDetails(),
            256.0,
            256.0,
            provider,
            nullptr,
            missingProjections));

    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    EXPECT_NE(nullptr, loadingTile);
    loadingTile->setState(RasterOverlayTile::LoadState::Loaded);
    loadingTile->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::Yes);

    EXPECT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Yes,
        mapped.update(
            tile.key,
            tile.content.renderContent.rasterOverlayDetails(),
            256.0,
            256.0,
            provider,
            nullptr,
            missingProjections));
    return mapped;
}

} // namespace

TEST(TileChildMaterializerTest, LinkContentChildrenWithoutDuplicates) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey firstKey{"test", 1, 0, 0};
    const TileKey secondKey{"test", 1, 1, 0};
    TilesetTile parent(parentKey, Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace(
        "test:1:0:0",
        std::make_unique<TilesetTile>(firstKey, Rectangle{}));
    tiles.emplace(
        "test:1:1:0",
        std::make_unique<TilesetTile>(secondKey, Rectangle{}));

    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        auto it = tiles.find(cacheKeyFor(key));
        return it == tiles.end() ? nullptr : it->second.get();
    };
    const std::vector<TileKey> childKeys{firstKey, secondKey, firstKey};

    const bool changed =
        TileChildMaterializer::linkContentChildren(parent, childKeys, ensure);
    const bool changedAgain =
        TileChildMaterializer::linkContentChildren(parent, childKeys, ensure);

    EXPECT_TRUE(changed);
    EXPECT_FALSE(changedAgain);
    ASSERT_EQ(2u, parent.children.size());
    EXPECT_EQ(tiles["test:1:0:0"].get(), parent.children[0]);
    EXPECT_EQ(tiles["test:1:1:0"].get(), parent.children[1]);
    EXPECT_EQ(&parent, parent.children[0]->parent);
    EXPECT_EQ(&parent, parent.children[1]->parent);
}

TEST(TileChildMaterializerTest, AnyAvailableTerrainChildCreatesFullQuadLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0}));
    parent.geometricError = 100.0;
    parent.refine = TileRefine::Add;
    parent.content.renderContent.setTerrainHeightRange(-10.0, 90.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };
    auto availability = [](const TileKey& key) {
        return key.x == 0 && key.y == 0
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        availability,
        ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    TilesetTile* sw = parent.children[0];
    TilesetTile* se = parent.children[1];
    TilesetTile* nw = parent.children[2];
    TilesetTile* ne = parent.children[3];

    EXPECT_FALSE(sw->content.upsampledFromParent);
    EXPECT_TRUE(se->content.upsampledFromParent);
    EXPECT_TRUE(nw->content.upsampledFromParent);
    EXPECT_TRUE(ne->content.upsampledFromParent);

    EXPECT_DOUBLE_EQ(50.0, sw->geometricError);
    EXPECT_DOUBLE_EQ(50.0, se->geometricError);
    EXPECT_EQ(TileRefine::Add, sw->refine);
    EXPECT_EQ(TileRefine::Add, se->refine);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 0}), sw->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 1, 0}), se->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 1}), nw->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 1, 1}), ne->key);
    EXPECT_NEAR(-MathUtils::OnePi, sw->bounds.west(), 1e-9);
    EXPECT_NEAR(-MathUtils::PiOverTwo, sw->bounds.south(), 1e-9);
    EXPECT_NEAR(-MathUtils::PiOverTwo, sw->bounds.east(), 1e-9);
    EXPECT_NEAR(0.0, sw->bounds.north(), 1e-9);
    EXPECT_NEAR(-MathUtils::PiOverTwo, ne->bounds.west(), 1e-9);
    EXPECT_NEAR(0.0, ne->bounds.south(), 1e-9);
    EXPECT_NEAR(0.0, ne->bounds.east(), 1e-9);
    EXPECT_NEAR(MathUtils::PiOverTwo, ne->bounds.north(), 1e-9);
    for (const TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        ASSERT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            -10.0,
            child->content.renderContent.terrainMinimumHeight());
        EXPECT_DOUBLE_EQ(
            90.0,
            child->content.renderContent.terrainMaximumHeight());
        ASSERT_TRUE(child->boundingVolume.has_value());
        EXPECT_EQ(TileBoundingVolumeKind::Region, child->boundingVolume->kind);
        EXPECT_DOUBLE_EQ(-10.0, child->boundingVolume->minimumHeight);
        EXPECT_DOUBLE_EQ(90.0, child->boundingVolume->maximumHeight);
        ASSERT_TRUE(child->contentBoundingVolume.has_value());
        EXPECT_EQ(
            TileBoundingVolumeKind::Region,
            child->contentBoundingVolume->kind);
        EXPECT_DOUBLE_EQ(-10.0, child->contentBoundingVolume->minimumHeight);
        EXPECT_DOUBLE_EQ(90.0, child->contentBoundingVolume->maximumHeight);
    }
}

TEST(TileChildMaterializerTest, NoAvailableTerrainChildrenCreatesNoneLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle{});

    int ensureCalls = 0;
    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        [](const TileKey&) {
            return TileAvailabilityState::NotAvailable;
        },
        [&ensureCalls](const TileKey&) -> TilesetTile* {
            ++ensureCalls;
            return nullptr;
        });

    EXPECT_FALSE(changed);
    EXPECT_EQ(0, ensureCalls);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest, MaterializeTerrainChildrenSkipsOutOfRangeGeographicTmsChildren) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 2, 0},
        Rectangle{});
    int availabilityChecks = 0;
    int ensureCalls = 0;

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        [&availabilityChecks](const TileKey&) {
            ++availabilityChecks;
            return TileAvailabilityState::Available;
        },
        [&ensureCalls](const TileKey&) -> TilesetTile* {
            ++ensureCalls;
            return nullptr;
        });

    EXPECT_FALSE(changed);
    EXPECT_EQ(0, availabilityChecks);
    EXPECT_EQ(0, ensureCalls);
    EXPECT_TRUE(parent.children.empty());
}

TEST(TileChildMaterializerTest, NonRootUnavailableTerrainSiblingsBecomeUpsampledLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 2, 0}), parent.children[0]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 3, 0}), parent.children[1]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 2, 1}), parent.children[2]->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 3, 1}), parent.children[3]->key);

    EXPECT_FALSE(parent.children[0]->content.upsampledFromParent);
    EXPECT_TRUE(parent.children[1]->content.upsampledFromParent);
    EXPECT_TRUE(parent.children[2]->content.upsampledFromParent);
    EXPECT_TRUE(parent.children[3]->content.upsampledFromParent);
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityUpgradeClearsStaleUpsampledMesh) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    ASSERT_TRUE(TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure));
    ASSERT_EQ(4u, parent.children.size());
    TilesetTile* upgradedChild = parent.children[1];
    ASSERT_EQ((TileKey{"Geographic-TMS", 2, 3, 0}), upgradedChild->key);
    ASSERT_TRUE(upgradedChild->content.upsampledFromParent);
    upgradedChild->content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>());
    upgradedChild->content.renderContent.setMeshReady(true);
    upgradedChild->content.renderContent.setSurfaceDrawable(true);
    upgradedChild->content.renderContent.setSurfaceSource(
        SurfaceDrawableSource::AncestorUpsample);

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure);

    EXPECT_TRUE(changed);
    EXPECT_FALSE(upgradedChild->content.upsampledFromParent);
    EXPECT_FALSE(upgradedChild->content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(upgradedChild->content.renderContent.isMeshReady());
    EXPECT_FALSE(upgradedChild->content.renderContent.isSurfaceDrawable());
    EXPECT_EQ(4u, parent.children.size());
}

TEST(TileChildMaterializerTest,
     TerrainAvailabilityMaterializationReplacesRasterDetailUpsampleKind) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 1, 0},
        Rectangle{});

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    TilesetTile* staleRasterChild =
        ensure(TileKey{"Geographic-TMS", 2, 3, 0});
    ASSERT_NE(nullptr, staleRasterChild);
    staleRasterChild->content.markRasterDetailUpsample();
    staleRasterChild->content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>());
    staleRasterChild->content.renderContent.setMeshReady(true);

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 2 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure);

    EXPECT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_TRUE(staleRasterChild->content.isTerrainAvailabilityUpsample());
    EXPECT_FALSE(staleRasterChild->content.isRasterDetailUpsample());
    EXPECT_FALSE(staleRasterChild->content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(staleRasterChild->content.renderContent.isMeshReady());
}

TEST(TileChildMaterializerTest, NonRootGeographicTerrainChildrenPreserveBounds) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTile parent(
        TileKey{"Geographic-TMS", 1, 0, 1},
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 1, 0, 1}));

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles, &scheme](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(
                    key,
                    scheme->tileToRectangle(key)))
                     .first;
        }
        return it->second.get();
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        3,
        [](const TileKey& key) {
            return key.x == 0 && key.y == 2
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        },
        ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    const TilesetTile* sw = parent.children[0];
    const TilesetTile* se = parent.children[1];
    const TilesetTile* nw = parent.children[2];
    const TilesetTile* ne = parent.children[3];
    ASSERT_NE(nullptr, sw);
    ASSERT_NE(nullptr, se);
    ASSERT_NE(nullptr, nw);
    ASSERT_NE(nullptr, ne);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 0, 2}), sw->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 1, 2}), se->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 0, 3}), nw->key);
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 1, 3}), ne->key);
    EXPECT_NEAR(-MathUtils::OnePi, sw->bounds.west(), 1e-9);
    EXPECT_NEAR(0.0, sw->bounds.south(), 1e-9);
    EXPECT_NEAR(-MathUtils::OnePi * 0.75, sw->bounds.east(), 1e-9);
    EXPECT_NEAR(MathUtils::PiOverTwo * 0.5, sw->bounds.north(), 1e-9);
    EXPECT_NEAR(-MathUtils::OnePi * 0.75, ne->bounds.west(), 1e-9);
    EXPECT_NEAR(MathUtils::PiOverTwo * 0.5, ne->bounds.south(), 1e-9);
    EXPECT_NEAR(-MathUtils::PiOverTwo, ne->bounds.east(), 1e-9);
    EXPECT_NEAR(MathUtils::PiOverTwo, ne->bounds.north(), 1e-9);
    EXPECT_FALSE(sw->content.upsampledFromParent);
    EXPECT_TRUE(se->content.upsampledFromParent);
    EXPECT_TRUE(nw->content.upsampledFromParent);
    EXPECT_TRUE(ne->content.upsampledFromParent);
}

TEST(TileChildMaterializerTest, RasterUpsampledChildrenSplitSubdivisionAndRemainStable) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;
    parent.content.renderContent.setTerrainHeightRange(-5.0, 25.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    const Rectangle subdivision =
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0);
    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            subdivision,
            200.0,
            ensure);
    const bool changedAgain =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            subdivision,
            200.0,
            ensure);

    ASSERT_TRUE(changed);
    EXPECT_FALSE(changedAgain);
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(TileRefine::Replace, parent.refine);

    EXPECT_EQ(
        Rectangle::fromDegrees(-20.0, -10.0, -10.0, 0.0),
        parent.children[0]->bounds);
    EXPECT_EQ(
        Rectangle::fromDegrees(-10.0, 0.0, 0.0, 10.0),
        parent.children[3]->bounds);

    for (TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(&parent, child->parent);
        EXPECT_TRUE(child->content.upsampledFromParent);
        EXPECT_TRUE(child->content.rasterUpsampledForMoreDetail);
        EXPECT_DOUBLE_EQ(50.0, child->geometricError);
        ASSERT_TRUE(child->boundingVolume.has_value());
        EXPECT_EQ(TileBoundingVolumeKind::Region, child->boundingVolume->kind);
        EXPECT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            -5.0,
            child->content.renderContent.terrainMinimumHeight());
        EXPECT_DOUBLE_EQ(
            25.0,
            child->content.renderContent.terrainMaximumHeight());
    }
}

TEST(TileChildMaterializerTest, RasterUpsampledTileCanContinueSubdividingForImageryDetail) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 10, 512, 512},
        Rectangle::fromDegrees(106.0, 29.0, 107.0, 30.0));
    parent.geometricError = 64.0;
    parent.content.upsampledFromParent = true;
    parent.content.rasterUpsampledForMoreDetail = true;
    parent.content.renderContent.setTerrainHeightRange(100.0, 500.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    const bool changed =
        TileChildMaterializer::materializeRasterUpsampledChildren(
            parent,
            parent.bounds,
            64.0,
            ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());
    for (TilesetTile* child : parent.children) {
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(&parent, child->parent);
        EXPECT_TRUE(child->content.upsampledFromParent);
        EXPECT_TRUE(child->content.rasterUpsampledForMoreDetail);
        EXPECT_DOUBLE_EQ(32.0, child->geometricError);
        EXPECT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_DOUBLE_EQ(
            100.0,
            child->content.renderContent.terrainMinimumHeight());
        EXPECT_DOUBLE_EQ(
            500.0,
            child->content.renderContent.terrainMaximumHeight());
    }

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        parent,
        TileRefinementAvailabilityOptions{
            true,
            false,
            false,
            false,
            true,
            18},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return false; },
        [](const TileKey&) { return TileAvailabilityState::NotAvailable; }));
}

TEST(TileChildMaterializerTest,
     RasterUpsampledChildrenUseReadyRasterBeforeGpuTexture) {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    parent.geometricError = 100.0;
    parent.content.renderContent.setTerrainHeightRange(-5.0, 25.0);
    RasterMappedToTilesetTile& mapped =
        addMoreDetailRasterMapping(parent, provider);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };

    EXPECT_TRUE(mapped.isMoreDetailAvailable());
    EXPECT_EQ(
        SurfaceRasterBindingKind::None,
        chooseSurfaceRasterBinding(&mapped).kind);
    EXPECT_TRUE(TileRasterUpsampledChildMaterializer::materialize(
        parent,
        100.0,
        ensure));
    ASSERT_EQ(4u, parent.children.size());
    EXPECT_EQ(
        Rectangle::fromDegrees(-20.0, -10.0, -10.0, 0.0),
        parent.children[0]->bounds);
    EXPECT_EQ(
        Rectangle::fromDegrees(-10.0, 0.0, 0.0, 10.0),
        parent.children[3]->bounds);

    RasterOverlayTile* readyTile = mapped.getReadyTile();
    ASSERT_NE(nullptr, readyTile);
    readyTile->setTexture(std::make_unique<DummyTexture>(4, 4));

    EXPECT_FALSE(TileRasterUpsampledChildMaterializer::materialize(
        parent,
        100.0,
        ensure));
    EXPECT_EQ(4u, parent.children.size());
}

TEST(TileChildMaterializerTest, CanRefineHonorsContentRulesBeforeTerrainSignals) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    auto noCacheKey = [](const TileKey&) { return std::string{}; };
    auto noTerrainCached = [](const std::string&) { return false; };
    auto noAvailability = [](const TileKey&) {
        return TileAvailabilityState::NotAvailable;
    };

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            true,
            false,
            false,
            false,
            4},
        noCacheKey,
        noTerrainCached,
        noAvailability));

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            true,
            false,
            true,
            4},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return true; },
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}

TEST(TileChildMaterializerTest, CanRefineUsesCachedAndAvailableTerrainSignals) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 0, 0}, Rectangle{});

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            false,
            4},
        cacheKeyFor,
        [](const std::string& cacheKey) {
            return cacheKey == "Geographic-TMS:1:0:0";
        },
        [](const TileKey&) { return TileAvailabilityState::NotAvailable; }));

    EXPECT_TRUE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            true,
            4},
        cacheKeyFor,
        [](const std::string&) { return false; },
        [](const TileKey& key) {
            return key.x == 1 && key.y == 0
                ? TileAvailabilityState::Available
                : TileAvailabilityState::NotAvailable;
        }));
}

TEST(TileChildMaterializerTest, CanRefineStopsAtMaxZoomWithoutChildrenOrTerrainSignals) {
    TilesetTile tile(TileKey{"Geographic-TMS", 4, 8, 8}, Rectangle{});

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            true,
            4},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return true; },
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}

TEST(TileChildMaterializerTest, CanRefineSkipsOutOfRangeGeographicTmsChildren) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 2, 0}, Rectangle{});
    int cacheChecks = 0;
    int availabilityChecks = 0;

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            false,
            true,
            2},
        [](const TileKey&) { return std::string{"child"}; },
        [&cacheChecks](const std::string&) {
            ++cacheChecks;
            return true;
        },
        [&availabilityChecks](const TileKey&) {
            ++availabilityChecks;
            return TileAvailabilityState::Available;
        }));
    EXPECT_EQ(0, cacheChecks);
    EXPECT_EQ(0, availabilityChecks);
}

TEST(TileChildMaterializerTest, CanRefineBlocksAvailabilityBoundaryAndTerrainUpsampledTiles) {
    TilesetTile tile(TileKey{"Geographic-TMS", 0, 0, 0}, Rectangle{});

    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            false,
            false,
            false,
            true,
            true,
            4},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return true; },
        [](const TileKey&) { return TileAvailabilityState::Available; }));

    tile.content.upsampledFromParent = true;
    tile.content.rasterUpsampledForMoreDetail = false;
    EXPECT_FALSE(TileChildMaterializer::canRefine(
        tile,
        TileRefinementAvailabilityOptions{
            true,
            true,
            false,
            false,
            true,
            4},
        [](const TileKey&) { return std::string{"child"}; },
        [](const std::string&) { return true; },
        [](const TileKey&) { return TileAvailabilityState::Available; }));
}
