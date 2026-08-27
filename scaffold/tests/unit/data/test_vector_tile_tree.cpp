#include "earth_engine/data/VectorTileTree.h"
#include "earth_engine/data/AmapTileManifest.h"

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

TEST(VectorTileTree, SnapsToSupportedDataZoom) {
    VectorTileTree::Options opt;
    opt.minZoom = 12;
    opt.maxZoom = 14;
    opt.maxTilesPerView = 256;
    opt.supportedZooms = {12, 14};
    VectorTileTree tree(opt);

    // canonical z13 是高德空档，必须直接选 z12，不发 z13 请求。
    auto z13 = tree.update(rectDeg(106.45, 29.60, 106.46, 29.61),
                           heightForZoom(13));
    ASSERT_FALSE(z13.requestTiles.empty());
    for (const TileKey& key : z13.requestTiles) EXPECT_EQ(key.z, 12);

    // 更近时仍能进入高德有效的 z14 档位。
    VectorTileTree fine(opt);
    auto z14 = fine.update(rectDeg(106.45, 29.60, 106.46, 29.61),
                           heightForZoom(14));
    ASSERT_FALSE(z14.requestTiles.empty());
    for (const TileKey& key : z14.requestTiles) EXPECT_EQ(key.z, 14);
}

TEST(VectorTileTree, AmapGlobalLodUsesCoarseTiersInsteadOfCenterIsland) {
    VectorTileTree::Options opt;
    opt.minZoom = 3;
    opt.maxZoom = 14;
    opt.maxTilesPerView = 256;
    opt.supportedZooms = {3, 6, 8, 10, 12, 14};
    opt.dataZoomForCanonicalZoom = [](int z) { return amapDataZoom(z); };
    opt.scheme = TileScheme::createAmapGeographic();
    VectorTileTree tree(opt);

    // 整个可见世界在远景必须使用 z3（8×8=64），完整覆盖而不是
    // 固定 z10/z12/z14 后只截中心最近 256 瓦形成“重庆孤岛”。
    const auto world = tree.update(rectDeg(-179.9, -89.9, 179.9, 89.9),
                                   4.0e7);
    EXPECT_EQ(3, world.selectedZoom);
    EXPECT_EQ(64, world.desiredTileCount);
    EXPECT_EQ(64u, world.desiredTiles.size());
    EXPECT_LE(world.desiredTiles.size(),
              static_cast<size_t>(opt.maxTilesPerView));

    bool hasWesternHemisphere = false;
    bool hasEasternHemisphere = false;
    for (const TileKey& key : world.desiredTiles) {
        EXPECT_EQ(3, key.z);
        hasWesternHemisphere = hasWesternHemisphere || key.x < 4;
        hasEasternHemisphere = hasEasternHemisphere || key.x >= 4;
    }
    EXPECT_TRUE(hasWesternHemisphere);
    EXPECT_TRUE(hasEasternHemisphere);

    // 两个容易被简单 +1 bias 提前切细的边界必须严格遵守高德表。
    VectorTileTree z11Tree(opt);
    const auto z11 = z11Tree.update(rectDeg(106.0, 29.0, 106.1, 29.1),
                                    heightForZoom(11));
    EXPECT_EQ(10, z11.selectedZoom);
    VectorTileTree z13Tree(opt);
    const auto z13 = z13Tree.update(rectDeg(106.0, 29.0, 106.1, 29.1),
                                    heightForZoom(13));
    EXPECT_EQ(12, z13.selectedZoom);

    // 中国全国级视野同样应自动选择能在预算内完整覆盖的粗档，而不是
    // 在 z10 固定档截一块中心区域。
    VectorTileTree china(opt);
    const auto nationwide = china.update(rectDeg(73.0, 18.0, 135.0, 54.0),
                                         heightForZoom(8));
    EXPECT_LT(nationwide.selectedZoom, 10);
    EXPECT_EQ(nationwide.desiredTiles.size(),
              static_cast<size_t>(nationwide.desiredTileCount));
    EXPECT_LE(nationwide.desiredTiles.size(),
              static_cast<size_t>(opt.maxTilesPerView));
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

TEST(VectorTileTree, TemporaryFailureRetriesOnlyAfterDeadline) {
    double nowMs = 1000.0;
    VectorTileTree::Options opt;
    opt.nowMs = [&]() { return nowMs; };
    VectorTileTree tree(opt);
    const Rectangle view = rectDeg(1, 1, 2, 2);
    auto initial = tree.update(view, heightForZoom(4));
    ASSERT_FALSE(initial.requestTiles.empty());
    const TileKey key = initial.requestTiles.front();

    tree.markFailedUntil(key, 1500.0);
    auto before = tree.update(view, heightForZoom(4));
    EXPECT_EQ(std::find(before.requestTiles.begin(), before.requestTiles.end(),
                        key),
              before.requestTiles.end());
    EXPECT_GT(tree.failedCount(), 0u);

    nowMs = 1500.0;
    auto due = tree.update(view, heightForZoom(4));
    EXPECT_NE(std::find(due.requestTiles.begin(), due.requestTiles.end(), key),
              due.requestTiles.end());
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

    // 逐理想瓦独立回退:灌一块细瓦 → 它自己上屏(内容最大化),其余理想瓦
    // 继续由粗祖先顶住。粗细并存 = 同一 POI 画两遍,对**只喂符号**的这棵树
    // 是可忍受轻伪影;换成"全有全无"消重叠会整支回滚到很粗祖先,内容量掉
    // 一个数量级(真机 z11→z8,42 顶 312)。取舍记在 B.5,别再翻回去。
    TileKey child = fine.requestTiles[0];
    tree.provide(child, emptyTile());
    auto mixed = tree.update(rectDeg(1, 1, 40, 40), heightForZoom(4));
    ASSERT_FALSE(mixed.renderTiles.empty());
    EXPECT_TRUE(std::find(mixed.renderTiles.begin(), mixed.renderTiles.end(),
                          child) != mixed.renderTiles.end())
        << "已加载的细瓦必须上屏,不得因兄弟未到而作废";
    // 先粗后细的确定性顺序
    for (size_t i = 1; i < mixed.renderTiles.size(); ++i) {
        EXPECT_LE(mixed.renderTiles[i - 1].z, mixed.renderTiles[i].z);
    }
    // 祖先去重:多个理想瓦共享同一祖先时只出现一次
    std::vector<TileKey> uniq = mixed.renderTiles;
    std::sort(uniq.begin(), uniq.end(), [](const TileKey& a, const TileKey& b) {
        if (a.z != b.z) return a.z < b.z;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    EXPECT_EQ(std::unique(uniq.begin(), uniq.end()) - uniq.begin(),
              static_cast<long>(mixed.renderTiles.size()));
}

TEST(VectorTileTree, GeometryReplaceKeepsParentUntilAllFineTilesLoad) {
    VectorTileTree::Options opt;
    opt.minZoom = 12;
    opt.maxZoom = 14;
    opt.maxTilesPerView = 256;
    opt.supportedZooms = {12, 14};
    opt.refinement =
        VectorTileTree::RefinementPolicy::GeometryReplace;
    VectorTileTree tree(opt);
    const Rectangle view = rectDeg(106.45, 29.45, 106.55, 29.55);

    auto coarse = tree.update(view, heightForZoom(12));
    ASSERT_FALSE(coarse.requestTiles.empty());
    for (const TileKey& key : coarse.requestTiles) {
        tree.provide(key, emptyTile());
    }
    coarse = tree.update(view, heightForZoom(12));
    ASSERT_FALSE(coarse.renderTiles.empty());
    for (const TileKey& key : coarse.renderTiles) EXPECT_EQ(key.z, 12);

    auto fine = tree.update(view, heightForZoom(14));
    ASSERT_GE(fine.requestTiles.size(), 2u);
    const TileKey firstFine = fine.requestTiles.front();
    tree.provide(firstFine, emptyTile());

    auto partial = tree.update(view, heightForZoom(14));
    ASSERT_FALSE(partial.renderTiles.empty());
    EXPECT_EQ(std::find(partial.renderTiles.begin(), partial.renderTiles.end(),
                        firstFine),
              partial.renderTiles.end())
        << "GeometryReplace 不得让部分细瓦与父瓦提前同框";
    for (const TileKey& key : partial.renderTiles) EXPECT_EQ(key.z, 12);

    for (size_t i = 1; i < fine.requestTiles.size(); ++i) {
        tree.provide(fine.requestTiles[i], emptyTile());
    }
    auto settled = tree.update(view, heightForZoom(14));
    ASSERT_FALSE(settled.renderTiles.empty());
    for (const TileKey& key : settled.renderTiles) EXPECT_EQ(key.z, 14);
}

TEST(VectorTileTree, GeometryReplaceRenderSetHasNoAncestorPairs) {
    VectorTileTree::Options opt;
    opt.minZoom = 10;
    opt.maxZoom = 14;
    opt.maxTilesPerView = 256;
    opt.refinement =
        VectorTileTree::RefinementPolicy::GeometryReplace;
    VectorTileTree tree(opt);
    const Rectangle view = rectDeg(106.45, 29.45, 106.55, 29.55);

    auto coarse = tree.update(view, heightForZoom(10));
    for (const TileKey& key : coarse.requestTiles) {
        tree.provide(key, emptyTile());
    }
    tree.update(view, heightForZoom(10));

    auto fine = tree.update(view, heightForZoom(14));
    ASSERT_GE(fine.requestTiles.size(), 3u);
    for (size_t i = 0; i < fine.requestTiles.size() / 2; ++i) {
        tree.provide(fine.requestTiles[i], emptyTile());
    }
    const auto partial = tree.update(view, heightForZoom(14));
    auto isAncestorOf = [](const TileKey& a, const TileKey& b) {
        if (a.schemeId != b.schemeId || a.z >= b.z) return false;
        const int d = b.z - a.z;
        return (b.x >> d) == a.x && (b.y >> d) == a.y;
    };
    for (const TileKey& a : partial.renderTiles) {
        for (const TileKey& b : partial.renderTiles) {
            EXPECT_FALSE(isAncestorOf(a, b))
                << a << " 与 " << b << " 违反 GeometryReplace 父子互斥";
        }
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

TEST(VectorTileTree, FixedZoomOversizedViewKeepsCenterBounded) {
    VectorTileTree::Options opt;
    opt.minZoom = 14;
    opt.maxZoom = 14;
    opt.maxTilesPerView = 8;
    opt.maxPendingRequests = 4;
    VectorTileTree tree(opt);
    const Rectangle view = rectDeg(100, 20, 112, 36);
    const TileKey center = tree.scheme().positionToTile(
        deg(106.0), deg(28.0), 14);

    auto result = tree.update(view, heightForZoom(14));
    ASSERT_EQ(4u, result.requestTiles.size());
    EXPECT_GT(result.desiredTileCount, 400000)
        << "测试视野应足够大，才能守住固定档位有界扫描";
    EXPECT_LT(result.scannedTileCount, 64u)
        << "最近 8 瓦不得再全量扫描百万级矩形";
    EXPECT_EQ(4u, tree.pendingCount());
    for (const TileKey& key : result.requestTiles) {
        EXPECT_EQ(14, key.z);
    }
    const TileKey first = result.requestTiles.front();
    const long long firstDx = first.x - center.x;
    const long long firstDy = first.y - center.y;
    EXPECT_LE(firstDx * firstDx + firstDy * firstDy, 1);

    // Repeated updates must not leak the rest of the huge fixed-z view into
    // pending/network work while the selected center set is in flight.
    auto again = tree.update(view, heightForZoom(14));
    EXPECT_TRUE(again.requestTiles.empty());
    EXPECT_EQ(4u, tree.pendingCount());

    // 快速平移时旧视野 pending 必须从树侧取消，新视野立即获得请求预算；
    // 否则一个慢请求批次会长期饿死当前视野。迟到响应仍可进 LRU。
    auto shifted = tree.update(rectDeg(110, 20, 122, 36),
                               heightForZoom(14));
    EXPECT_EQ(4u, shifted.requestTiles.size());
    EXPECT_EQ(4u, tree.pendingCount());

    // 旧视野迟到响应不会挤掉或重复当前视野工作。
    tree.provide(result.requestTiles.front(), emptyTile());
    auto current = tree.update(rectDeg(110, 20, 122, 36),
                               heightForZoom(14));
    EXPECT_TRUE(current.requestTiles.empty());
    EXPECT_EQ(4u, tree.pendingCount());
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

// ---------------------------------------------------------------------------
// V24:换代抖动(2026-08-15 排查,同日 R* 根修)
//
// 排查期这里钉的是缺陷行为(②③⑤);R* 落地后已翻转为钉正确行为。
// 缺陷① zoom 无迟滞仍保留原样 —— R* 之后翻档不再产生空窗(存货直接顶),
// 迟滞从"必需"降为"省重算",暂不做。
// ---------------------------------------------------------------------------

/// 缺陷①(仍在,已降级):zoom 选择无迟滞,视高抖 ±0.5% 就来回翻档。
TEST(VectorTileTree, ZoomHasNoHysteresisAtBoundary) {
    VectorTileTree::Options opt;
    // z14 的边界高度:log2(4e7/h) == 14 → h = 4e7/2^14
    const double hBoundary = 4.0e7 / std::pow(2.0, 14.0);
    int flips = 0;
    int prev = VectorTileTree::zoomForCameraHeight(hBoundary * 1.005, opt);
    // 相机在边界附近做 20 次 ±0.5% 的微小起伏(真实缩放/地形起伏都会这样)
    for (int i = 0; i < 20; ++i) {
        const double h = hBoundary * ((i % 2 == 0) ? 0.995 : 1.005);
        const int z = VectorTileTree::zoomForCameraHeight(h, opt);
        if (z != prev) ++flips;
        prev = z;
    }
    // 1% 的高度抖动 → 20 次全翻档。无死区。
    EXPECT_EQ(flips, 20);
}

/// R* 消缺陷②:拉远跳档(z14→z11,级差 3)时,粗瓦没到,已加载的细瓦
/// 后代整片顶住 —— renderTiles 非空、全 z14、且粗理想瓦照常请求(过渡态
/// 必须收敛到目标层,否则 4^3 倍 draw 白付)。
TEST(VectorTileTree, ZoomOutFallsBackToLoadedDescendants) {
    VectorTileTree::Options opt;
    opt.minZoom = 0;
    opt.maxZoom = 14;
    VectorTileTree tree(opt);
    const Rectangle view = rectDeg(106.50, 29.50, 106.53, 29.53);

    // 先在 z14 把细瓦喂满
    VectorTileTree::UpdateResult fine = tree.update(view, heightForZoom(14));
    ASSERT_FALSE(fine.requestTiles.empty());
    for (const TileKey& k : fine.requestTiles) tree.provide(k, emptyTile());
    fine = tree.update(view, heightForZoom(14));
    EXPECT_FALSE(fine.renderTiles.empty());

    // 拉远到 z11:粗瓦没加载过,细瓦后代顶住,不空窗
    VectorTileTree::UpdateResult coarse =
        tree.update(view, heightForZoom(11));
    ASSERT_FALSE(coarse.renderTiles.empty()) << "后代回退失效 = 空窗回归";
    for (const TileKey& k : coarse.renderTiles) {
        EXPECT_EQ(k.z, 14);
    }
    ASSERT_FALSE(coarse.requestTiles.empty());
    for (const TileKey& k : coarse.requestTiles) {
        EXPECT_EQ(k.z, 11) << "理想层缺瓦必须照常请求(收敛义务)";
    }

    // 粗理想瓦到齐 → 换代为 z11,细瓦退场(全有全无,无重叠)
    for (const TileKey& k : coarse.requestTiles) tree.provide(k, emptyTile());
    VectorTileTree::UpdateResult settled =
        tree.update(view, heightForZoom(11));
    ASSERT_FALSE(settled.renderTiles.empty());
    for (const TileKey& k : settled.renderTiles) {
        EXPECT_EQ(k.z, 11);
    }
}

/// R* 消缺陷③的验证配对:**视口内**的存货(等 quad 凑齐的细瓦、顶班的
/// 后代)在 resolve 沿途被 touch 保活,预算压力下不被淘汰。
TEST(VectorTileTree, StandinTilesAreRetainedUnderBudgetPressure) {
    VectorTileTree::Options opt;
    opt.minZoom = 0;
    opt.maxZoom = 14;
    opt.maxCachedTiles = 2;   // 预算远小于存货量
    VectorTileTree tree(opt);
    const Rectangle view = rectDeg(106.50, 29.50, 106.53, 29.53);

    // z14 喂满后拉远到 z11:细瓦是唯一存货,正在顶班
    VectorTileTree::UpdateResult fine = tree.update(view, heightForZoom(14));
    for (const TileKey& k : fine.requestTiles) tree.provide(k, emptyTile());
    tree.update(view, heightForZoom(14));
    VectorTileTree::UpdateResult coarse =
        tree.update(view, heightForZoom(11));
    ASSERT_FALSE(coarse.renderTiles.empty());

    // 连续多帧更新(不喂粗瓦):顶班细瓦必须一直活着,不然空窗+重拉
    for (int i = 0; i < 3; ++i) {
        VectorTileTree::UpdateResult r = tree.update(view, heightForZoom(11));
        ASSERT_FALSE(r.renderTiles.empty()) << "第 " << i << " 帧空窗";
        for (const TileKey& k : r.renderTiles) {
            EXPECT_NE(tree.loadedTile(k), nullptr);
        }
    }
}

/// 出视口的瓦片仍走正常 LRU 淘汰(这是**期望行为**,不是缺陷③——
/// ③指的是视口内存货被淘汰,由上一条守卫)。
TEST(VectorTileTree, TilesLeavingRenderSetAreImmediatelyEvictable) {
    VectorTileTree::Options opt;
    opt.minZoom = 0;
    opt.maxZoom = 14;
    opt.maxCachedTiles = 2;   // 压小预算,让淘汰立刻可见
    VectorTileTree tree(opt);

    const double h = heightForZoom(12);
    const Rectangle viewA = rectDeg(106.50, 29.50, 106.52, 29.52);

    VectorTileTree::UpdateResult a = tree.update(viewA, h);
    for (const TileKey& k : a.requestTiles) tree.provide(k, emptyTile());
    a = tree.update(viewA, h);
    ASSERT_FALSE(a.renderTiles.empty());
    const TileKey wasRendering = a.renderTiles.front();

    // 相机连续平移到 6 个互不重叠的位置并喂满 → 必然超预算
    for (int i = 1; i <= 6; ++i) {
        const double lng = 106.50 + 0.30 * i;
        const Rectangle v = rectDeg(lng, 29.50, lng + 0.02, 29.52);
        VectorTileTree::UpdateResult b = tree.update(v, h);
        for (const TileKey& k : b.requestTiles) tree.provide(k, emptyTile());
        tree.update(v, h);
    }
    EXPECT_EQ(tree.loadedTile(wasRendering), nullptr)
        << "若这条红了 = 出视口瓦也被保活了,LRU 失效会撑爆预算";
}

/// 过渡态不变量(2026-08-15 修订):**无空洞**且**已加载的细瓦必上屏**。
/// 不再要求无重叠 —— 本树只喂 POI 符号,祖先与细瓦同框 = 同一点画两遍
/// (轻伪影),而消重叠要付"整支回滚到很粗祖先"的代价(见 B.5)。
TEST(VectorTileTree, RenderTilesCoverWithoutHolesAndKeepLoadedFine) {
    VectorTileTree::Options opt;
    opt.minZoom = 0;
    opt.maxZoom = 14;
    VectorTileTree tree(opt);
    const Rectangle view = rectDeg(106.45, 29.45, 106.55, 29.55);

    // 先在 z12 喂满(父辈),保证祖先回退有存货
    VectorTileTree::UpdateResult coarse = tree.update(view, heightForZoom(12));
    for (const TileKey& k : coarse.requestTiles) tree.provide(k, emptyTile());
    tree.update(view, heightForZoom(12));

    VectorTileTree::UpdateResult f13 = tree.update(view, heightForZoom(13));
    ASSERT_GE(f13.requestTiles.size(), 2u);
    const std::vector<TileKey> ideal = f13.requestTiles;
    auto isAncestorOf = [](const TileKey& a, const TileKey& b) {
        if (a.z >= b.z) return false;
        const int d = b.z - a.z;
        return (b.x >> d) == a.x && (b.y >> d) == a.y;
    };
    std::vector<TileKey> fed;
    for (size_t feed = 0; feed < 3; ++feed) {
        const size_t lo = feed * ideal.size() / 3;
        const size_t hi = (feed + 1) * ideal.size() / 3;
        for (size_t i = lo; i < hi; ++i) {
            tree.provide(ideal[i], emptyTile());
            fed.push_back(ideal[i]);
        }
        VectorTileTree::UpdateResult r = tree.update(view, heightForZoom(13));
        ASSERT_FALSE(r.renderTiles.empty());
        // 无空洞:每个理想格都被某瓦覆盖(自身/祖先/后代)
        for (const TileKey& cell : ideal) {
            bool covered = false;
            for (const TileKey& t : r.renderTiles) {
                if (t == cell || isAncestorOf(t, cell) ||
                    isAncestorOf(cell, t)) { covered = true; break; }
            }
            EXPECT_TRUE(covered) << cell << " 无覆盖(空洞)";
        }
        // 已加载的细瓦一块都不许丢
        for (const TileKey& k : fed) {
            EXPECT_TRUE(std::find(r.renderTiles.begin(), r.renderTiles.end(),
                                  k) != r.renderTiles.end())
                << k << " 已加载却未上屏(内容损失)";
        }
    }
}

/// 回归守卫(2026-08-15,用户报"点全部消失"的那条):quad 里缺一块时,
/// **已加载的兄弟必须全部上屏**,不得整支回滚到粗祖先。
TEST(VectorTileTree, IncompleteQuadKeepsLoadedSiblings) {
    VectorTileTree::Options opt;
    opt.minZoom = 0;
    opt.maxZoom = 14;
    VectorTileTree tree(opt);
    const Rectangle view = rectDeg(106.45, 29.45, 106.55, 29.55);

    VectorTileTree::UpdateResult a11 = tree.update(view, heightForZoom(11));
    for (const TileKey& k : a11.requestTiles) tree.provide(k, emptyTile());
    tree.update(view, heightForZoom(11));

    VectorTileTree::UpdateResult f13 = tree.update(view, heightForZoom(13));
    ASSERT_GE(f13.requestTiles.size(), 4u);
    for (size_t i = 0; i + 1 < f13.requestTiles.size(); ++i) {
        tree.provide(f13.requestTiles[i], emptyTile());
    }
    VectorTileTree::UpdateResult r = tree.update(view, heightForZoom(13));

    size_t fine = 0;
    for (const TileKey& k : r.renderTiles) {
        if (k.z == 13) ++fine;
    }
    EXPECT_EQ(fine, f13.requestTiles.size() - 1)
        << "已加载细瓦 " << (f13.requestTiles.size() - 1)
        << " 块,上屏 " << fine << " 块 —— 整支回滚回归了";
}
