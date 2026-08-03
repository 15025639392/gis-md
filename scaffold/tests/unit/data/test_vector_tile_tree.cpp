#include "earth_engine/data/VectorTileTree.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;

double deg(double d) { return d * kPi / 180.0; }

Rectangle rectDeg(double west, double south, double east, double north) {
    return Rectangle(deg(west), deg(south), deg(east), deg(north));
}

MvtTile emptyTile() { return MvtTile{}; }

/// zoom z 时高度取 4e7 / 2^z 的中点,保证 floor 后正好落在 z。
double heightForZoom(int zoom) { return 4.0e7 / std::pow(2.0, zoom + 0.5); }

// ---------------------------------------------------------------------------
// zoom 选择
// ---------------------------------------------------------------------------

TEST(VectorTileTree, ZoomForCameraHeight) {
    VectorTileTree::Options opt;
    opt.minZoom = 0;
    opt.maxZoom = 14;
    // 全球一屏(4e7m)→ z0
    EXPECT_EQ(VectorTileTree::zoomForCameraHeight(4.0e7, opt), 0);
    // 视高减半 → zoom +1
    EXPECT_EQ(VectorTileTree::zoomForCameraHeight(4.0e7 / 2.0 * 0.99, opt), 1);
    // 深缩放钳到 maxZoom
    EXPECT_EQ(VectorTileTree::zoomForCameraHeight(10.0, opt), 14);
    // 超高空钳到 minZoom;非法高度不炸
    EXPECT_EQ(VectorTileTree::zoomForCameraHeight(1.0e9, opt), 0);
    EXPECT_EQ(VectorTileTree::zoomForCameraHeight(0.0, opt), 14);
    // 偏置
    opt.zoomBias = 1.0;
    EXPECT_EQ(VectorTileTree::zoomForCameraHeight(4.0e7, opt), 1);
}

TEST(VectorTileTree, OverzoomClampsToMaxZoom) {
    VectorTileTree::Options opt;
    opt.maxZoom = 3;
    VectorTileTree tree(opt);
    auto result = tree.update(rectDeg(10, 10, 11, 11), 10.0 /* 极近 */);
    ASSERT_FALSE(result.requestTiles.empty());
    for (const TileKey& key : result.requestTiles) {
        EXPECT_EQ(key.z, 3);
    }
}

// ---------------------------------------------------------------------------
// 请求生成与 pending 语义
// ---------------------------------------------------------------------------

TEST(VectorTileTree, RequestsCoverViewAndCenterFirst) {
    VectorTileTree::Options opt;
    opt.maxTilesPerView = 256;
    VectorTileTree tree(opt);

    // z2:每瓦片 90°,视口横跨 4 瓦片(2×2)
    auto result = tree.update(rectDeg(-80, -40, 80, 40), heightForZoom(2));
    EXPECT_TRUE(result.renderTiles.empty());
    ASSERT_GE(result.requestTiles.size(), 4u);
    for (const TileKey& key : result.requestTiles) {
        EXPECT_EQ(key.z, 2);
    }
    // 中心优先:首个请求应是含视口中心 (0,0) 的瓦片
    TileKey first = result.requestTiles[0];
    const TileScheme& scheme = tree.scheme();
    TileKey center = scheme.positionToTile(0.0, 0.0, 2);
    long long d0 = static_cast<long long>(first.x - center.x) * (first.x - center.x) +
                   static_cast<long long>(first.y - center.y) * (first.y - center.y);
    for (const TileKey& key : result.requestTiles) {
        long long d = static_cast<long long>(key.x - center.x) * (key.x - center.x) +
                      static_cast<long long>(key.y - center.y) * (key.y - center.y);
        EXPECT_GE(d, d0);
    }
    EXPECT_EQ(tree.pendingCount(), result.requestTiles.size());

    // 同视口再 update:全部已 pending,不重复请求
    auto again = tree.update(rectDeg(-80, -40, 80, 40), heightForZoom(2));
    EXPECT_TRUE(again.requestTiles.empty());
}

TEST(VectorTileTree, ProvideMakesTileRenderable) {
    VectorTileTree tree;
    auto result = tree.update(rectDeg(1, 1, 2, 2), heightForZoom(4));
    ASSERT_FALSE(result.requestTiles.empty());
    TileKey key = result.requestTiles[0];

    tree.provide(key, emptyTile());
    EXPECT_EQ(tree.pendingCount(), result.requestTiles.size() - 1);
    EXPECT_NE(tree.loadedTile(key), nullptr);

    auto after = tree.update(rectDeg(1, 1, 2, 2), heightForZoom(4));
    EXPECT_TRUE(std::find(after.renderTiles.begin(), after.renderTiles.end(),
                          key) != after.renderTiles.end());
}

TEST(VectorTileTree, FailedTilesNotReRequestedUntilCleared) {
    VectorTileTree tree;
    auto result = tree.update(rectDeg(1, 1, 2, 2), heightForZoom(4));
    ASSERT_FALSE(result.requestTiles.empty());
    size_t total = result.requestTiles.size();
    TileKey key = result.requestTiles[0];

    tree.markFailed(key);
    EXPECT_EQ(tree.pendingCount(), total - 1);

    auto again = tree.update(rectDeg(1, 1, 2, 2), heightForZoom(4));
    EXPECT_TRUE(std::find(again.requestTiles.begin(), again.requestTiles.end(),
                          key) == again.requestTiles.end());

    tree.clearFailed();
    auto cleared = tree.update(rectDeg(1, 1, 2, 2), heightForZoom(4));
    EXPECT_TRUE(std::find(cleared.requestTiles.begin(),
                          cleared.requestTiles.end(),
                          key) != cleared.requestTiles.end());
}

// ---------------------------------------------------------------------------
// 祖先回退
// ---------------------------------------------------------------------------

TEST(VectorTileTree, AncestorFallbackWhileChildrenLoad) {
    VectorTileTree tree;
    // 先在低 zoom 看一眼,把粗瓦片灌进去
    auto coarse = tree.update(rectDeg(1, 1, 40, 40), heightForZoom(1));
    for (const TileKey& key : coarse.requestTiles) {
        tree.provide(key, emptyTile());
    }

    // 拉近到 z4(单瓦片 22.5°,视口跨多瓦片):细瓦片全 pending,
    // 渲染列表应是已加载的粗祖先
    auto fine = tree.update(rectDeg(1, 1, 40, 40), heightForZoom(4));
    ASSERT_FALSE(fine.requestTiles.empty());
    ASSERT_FALSE(fine.renderTiles.empty());
    for (const TileKey& key : fine.renderTiles) {
        EXPECT_EQ(key.z, 1);
    }

    // 灌一块细瓦片:渲染列表粗+细并存,粗在前
    TileKey child = fine.requestTiles[0];
    tree.provide(child, emptyTile());
    auto mixed = tree.update(rectDeg(1, 1, 40, 40), heightForZoom(4));
    ASSERT_GE(mixed.renderTiles.size(), 2u);
    EXPECT_TRUE(std::find(mixed.renderTiles.begin(), mixed.renderTiles.end(),
                          child) != mixed.renderTiles.end());
    for (size_t i = 1; i < mixed.renderTiles.size(); ++i) {
        EXPECT_LE(mixed.renderTiles[i - 1].z, mixed.renderTiles[i].z);
    }
}

// ---------------------------------------------------------------------------
// desired 闸与反经线
// ---------------------------------------------------------------------------

TEST(VectorTileTree, OversizedViewLowersZoomInsteadOfTruncating) {
    VectorTileTree::Options opt;
    opt.maxTilesPerView = 8;
    VectorTileTree tree(opt);
    // 全球视口但相机在 z6 高度:z6 全球 4096 瓦片,必须降 zoom 到 ≤8 片
    auto result = tree.update(rectDeg(-179, -80, 179, 80), heightForZoom(6));
    ASSERT_FALSE(result.requestTiles.empty());
    EXPECT_LE(result.requestTiles.size(), 8u);
    int z = result.requestTiles[0].z;
    EXPECT_LT(z, 6);
    for (const TileKey& key : result.requestTiles) {
        EXPECT_EQ(key.z, z);  // 降档后仍是单一 zoom,不是截断的碎集
    }
}

TEST(VectorTileTree, AntimeridianViewSplitsAndCoversBothSides) {
    VectorTileTree::Options opt;
    opt.maxTilesPerView = 64;
    VectorTileTree tree(opt);
    // west=170°, east=-170°(跨反经线)
    auto result = tree.update(rectDeg(170, -10, -170, 10), heightForZoom(3));
    ASSERT_FALSE(result.requestTiles.empty());
    bool hasWestSide = false;
    bool hasEastSide = false;
    int tilesX = tree.scheme().tileCountX(3);
    for (const TileKey& key : result.requestTiles) {
        if (key.x == tilesX - 1) {
            hasWestSide = true;  // 170°..180° 落在最后一列
        }
        if (key.x == 0) {
            hasEastSide = true;  // -180°..-170° 落在第一列
        }
    }
    EXPECT_TRUE(hasWestSide);
    EXPECT_TRUE(hasEastSide);
}

// ---------------------------------------------------------------------------
// LRU 淘汰
// ---------------------------------------------------------------------------

TEST(VectorTileTree, LruEvictsLeastRecentlyUsedKeepsRendered) {
    VectorTileTree::Options opt;
    opt.maxCachedTiles = 4;
    opt.maxTilesPerView = 64;
    VectorTileTree tree(opt);

    // 视口 A(z3,几块瓦片),灌满
    auto a = tree.update(rectDeg(1, 1, 40, 40), heightForZoom(3));
    for (const TileKey& key : a.requestTiles) {
        tree.provide(key, emptyTile());
    }
    size_t loadedA = tree.loadedCount();
    ASSERT_GT(loadedA, 0u);

    // 移到不相交的视口 B 并灌满:A 的瓦片不再被 touch,超预算部分淘汰
    auto b = tree.update(rectDeg(-120, -40, -80, -10), heightForZoom(3));
    for (const TileKey& key : b.requestTiles) {
        tree.provide(key, emptyTile());
    }
    // 再 update 一次触发淘汰(provide 不淘汰,淘汰在 update 末尾)
    auto bAgain = tree.update(rectDeg(-120, -40, -80, -10), heightForZoom(3));
    EXPECT_LE(tree.loadedCount(),
              std::max(opt.maxCachedTiles, bAgain.renderTiles.size()));
    // B 的瓦片(本帧在渲染)必须还在
    for (const TileKey& key : bAgain.renderTiles) {
        EXPECT_NE(tree.loadedTile(key), nullptr);
    }
}

TEST(VectorTileTree, ProvideUnrequestedKeyAccepted) {
    VectorTileTree tree;
    TileKey key{SchemeId("XYZ-WebMercator"), 2, 1, 1};
    tree.provide(key, emptyTile());
    EXPECT_NE(tree.loadedTile(key), nullptr);
    EXPECT_EQ(tree.pendingCount(), 0u);
}

} // namespace
