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

    // R* 全有全无:灌一块细瓦不够凑齐它所在的 quad → 渲染列表**保持纯粗**
    // (旧行为是粗+细并存 = 祖先与子瓦重影,已判死)
    TileKey child = fine.requestTiles[0];
    tree.provide(child, emptyTile());
    auto mixed = tree.update(rectDeg(1, 1, 40, 40), heightForZoom(4));
    ASSERT_FALSE(mixed.renderTiles.empty());
    for (const TileKey& key : mixed.renderTiles) {
        EXPECT_EQ(key.z, 1) << "quad 未凑齐时不得混入细瓦(重影)";
    }

    // 凑齐 child 所在 quad 的全部在视口兄弟 → 细瓦上屏,且其祖先退场
    // (注意:pending 的瓦不会再出现在 requestTiles,须从首轮列表取兄弟)
    const TileKey parent = child.parent();
    for (const TileKey& key : fine.requestTiles) {
        if (key.parent() == parent) tree.provide(key, emptyTile());
    }
    auto swapped = tree.update(rectDeg(1, 1, 40, 40), heightForZoom(4));
    EXPECT_TRUE(std::find(swapped.renderTiles.begin(),
                          swapped.renderTiles.end(),
                          child) != swapped.renderTiles.end());
    for (const TileKey& key : swapped.renderTiles) {
        EXPECT_FALSE(key == parent) << "子瓦上屏后祖先必须退场(否则重影)";
    }
    for (size_t i = 1; i < swapped.renderTiles.size(); ++i) {
        EXPECT_LE(swapped.renderTiles[i - 1].z, swapped.renderTiles[i].z);
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

/// R* 消缺陷⑤:renderTiles 是**精确覆盖** —— 部分到达的过渡态下,
/// 任意两瓦无祖先/后代关系(无重影),且视口内每个理想格恰被一瓦覆盖
/// (无空洞)。这是 R* 的核心不变量,任何后续改动不得破坏。
TEST(VectorTileTree, RenderTilesAreExactCoverDuringTransition) {
    VectorTileTree::Options opt;
    opt.minZoom = 0;
    opt.maxZoom = 14;
    VectorTileTree tree(opt);
    // 视口跨多张 z13 瓦,保证同一父瓦下有多个子瓦
    const Rectangle view = rectDeg(106.45, 29.45, 106.55, 29.55);

    // 先在 z12 喂满(父辈)
    VectorTileTree::UpdateResult coarse = tree.update(view, heightForZoom(12));
    for (const TileKey& k : coarse.requestTiles) tree.provide(k, emptyTile());
    tree.update(view, heightForZoom(12));

    // 到 z13:请求子瓦,只喂一半(模拟部分到达),连续三个过渡态都要满足
    VectorTileTree::UpdateResult fine = tree.update(view, heightForZoom(13));
    ASSERT_GE(fine.requestTiles.size(), 2u);
    const std::vector<TileKey> all = fine.requestTiles;
    auto isAncestorOf = [](const TileKey& a, const TileKey& b) {
        if (a.z >= b.z) return false;
        const int d = b.z - a.z;
        return (b.x >> d) == a.x && (b.y >> d) == a.y;
    };
    auto checkInvariant = [&](const std::vector<TileKey>& rt,
                              const std::vector<TileKey>& idealCells) {
        for (size_t i = 0; i < rt.size(); ++i) {
            for (size_t j = 0; j < rt.size(); ++j) {
                if (i == j) continue;
                EXPECT_FALSE(isAncestorOf(rt[i], rt[j]))
                    << rt[i] << " 与 " << rt[j] << " 重叠(重影)";
            }
        }
        for (const TileKey& cell : idealCells) {
            int covers = 0;
            for (const TileKey& r : rt) {
                if (r == cell || isAncestorOf(r, cell) ||
                    isAncestorOf(cell, r)) {
                    ++covers;
                }
            }
            EXPECT_EQ(covers, 1) << cell << " 覆盖数 " << covers
                                 << "(0=空洞,≥2=重影)";
        }
    };
    for (size_t feed = 0; feed < 3; ++feed) {
        const size_t lo = feed * all.size() / 3;
        const size_t hi = (feed + 1) * all.size() / 3;
        for (size_t i = lo; i < hi; ++i) tree.provide(all[i], emptyTile());
        VectorTileTree::UpdateResult r = tree.update(view, heightForZoom(13));
        ASSERT_FALSE(r.renderTiles.empty());
        checkInvariant(r.renderTiles, all);
    }
}
