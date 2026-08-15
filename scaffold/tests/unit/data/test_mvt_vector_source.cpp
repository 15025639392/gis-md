#include "earth_engine/data/MvtVectorSource.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;

double deg(double d) { return d * kPi / 180.0; }

Rectangle rectDeg(double west, double south, double east, double north) {
    return Rectangle(deg(west), deg(south), deg(east), deg(north));
}

double heightForZoom(int zoom) { return 4.0e7 / std::pow(2.0, zoom + 0.5); }

// 最小 pbf 编码(仅本测试需要的子集)
struct Pbf {
    std::vector<uint8_t> bytes;
    void varint(uint64_t v) {
        while (v >= 0x80) {
            bytes.push_back(static_cast<uint8_t>(v) | 0x80);
            v >>= 7;
        }
        bytes.push_back(static_cast<uint8_t>(v));
    }
    void varintField(uint32_t field, uint64_t v) {
        varint((field << 3) | 0);
        varint(v);
    }
    void bytesField(uint32_t field, const std::vector<uint8_t>& sub) {
        varint((field << 3) | 2);
        varint(sub.size());
        bytes.insert(bytes.end(), sub.begin(), sub.end());
    }
    void stringField(uint32_t field, const std::string& s) {
        varint((field << 3) | 2);
        varint(s.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
};

/// 单图层单点要素瓦片(点在瓦片中心,带一个属性 kind=poi)
std::vector<uint8_t> makePointTile(const std::string& layerName) {
    Pbf feature;
    feature.varintField(1, 42);                    // id
    Pbf tags;
    tags.varint(0);
    tags.varint(0);
    feature.bytesField(2, tags.bytes);             // tags (0,0)
    feature.varintField(3, 1);                     // type Point
    Pbf geom;                                      // MoveTo(2048,2048)
    geom.varint(9);
    geom.varint((2048u << 1));                     // zigzag(2048)
    geom.varint((2048u << 1));
    feature.bytesField(4, geom.bytes);

    Pbf value;
    value.stringField(1, "poi");

    Pbf layer;
    layer.varintField(15, 2);
    layer.stringField(1, layerName);
    layer.bytesField(2, feature.bytes);
    layer.stringField(3, "kind");
    layer.bytesField(4, value.bytes);

    Pbf tile;
    tile.bytesField(3, layer.bytes);
    return tile.bytes;
}

/// 假网络:记录请求,立即成功回调。刀A.5 后源经 MvtTileFetchCache 获取,
/// 假网络包成 cache 注入(requested 记的是**穿透到网络**的请求 —— cache
/// 命中不计,这正是「零重拉取」断言想要的语义)。
struct FakeFetch {
    std::vector<TileKey> requested;
    std::vector<uint8_t> body;
    int statusCode = 200;

    std::shared_ptr<MvtTileFetchCache> cache() {
        return std::make_shared<MvtTileFetchCache>(
            [this](const TileKey& key, MvtTileFetchCache::FetchCallback cb) {
                requested.push_back(key);
                cb(statusCode, body);
            },
            48);
    }
};

MvtVectorSource::Options optionsForTest() {
    MvtVectorSource::Options opt;
    opt.tree.maxTilesPerView = 64;
    opt.maxTileCommitsPerUpdate = 0;  // 默认不限流,限流单独一条测
    return opt;
}

/// 假 sink:记录 worker 镶嵌调用与渲染线程 commit/drop。
/// 镶嵌返回一个恒定的非空网格 —— 本文件测的是**通路**(拉取/解码/派单/
/// commit/drop/重入),真镶嵌产物的正确性归 test_feature_tile_mesh。
struct FakeSinks {
    int tessellateCalls = 0;
    size_t lastFeatureCount = 0;
    std::vector<TileKey> committed;
    std::vector<TileKey> dropped;

    MvtVectorSource::Sinks fn() {
        MvtVectorSource::Sinks s;
        s.tessellate = [this](const TileKey&, std::vector<Feature>&& features) {
            ++tessellateCalls;
            lastFeatureCount = features.size();
            FeatureTileMesh mesh;
            mesh.hasOrigin = true;
            mesh.fillVerts = {0.f, 0.f, 0.f};
            mesh.fillIndices = {0, 0, 0};
            return mesh;
        };
        s.commit = [this](const TileKey& k, FeatureTileMesh&&) {
            committed.push_back(k);
        };
        s.drop = [this](const TileKey& k) { dropped.push_back(k); };
        return s;
    }
};

TEST(MvtVectorSource, FetchDecodeTessellateCommitPipeline) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    FakeSinks sinks;
    MvtVectorSource source(optionsForTest(), sinks.fn(), fetch.cache());

    Rectangle view = rectDeg(1, 1, 40, 40);
    source.update(view, heightForZoom(2));
    ASSERT_FALSE(fetch.requested.empty());
    EXPECT_EQ(sinks.tessellateCalls, 0);  // 解码结果还在收件箱

    source.update(view, heightForZoom(2));  // 消化解码 → 派镶嵌单
    EXPECT_GT(sinks.tessellateCalls, 0);
    EXPECT_TRUE(sinks.committed.empty());   // 镶嵌结果还在收件箱

    source.update(view, heightForZoom(2));  // 消化网格 → commit
    EXPECT_FALSE(sinks.committed.empty());
    EXPECT_GT(source.activeTileCount(), 0u);
    EXPECT_EQ(sinks.committed.size(), source.activeTileCount());
}

// E1 必须守住的性质:瓦片离开视口后网格被 drop,但解码结果留在树的 LRU,
// 重入时**只重镶嵌、不重拉取**。这条曾在改造中差点丢掉 —— 若图省事让
// worker 一步吐网格、树里只塞空占位,重入时树认为"已加载"不再请求,而
// 网格已丢,该瓦片将永远不再出现。
TEST(MvtVectorSource, ReentryRetessellatesWithoutRefetch) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    FakeSinks sinks;
    MvtVectorSource source(optionsForTest(), sinks.fn(), fetch.cache());

    Rectangle near = rectDeg(1, 1, 10, 10);
    for (int i = 0; i < 3; ++i) source.update(near, heightForZoom(2));
    const size_t fetchesAfterFirst = fetch.requested.size();
    const int tessAfterFirst = sinks.tessellateCalls;
    ASSERT_GT(source.activeTileCount(), 0u);

    // 换到别处再回来
    Rectangle far = rectDeg(-170, -40, -160, -30);
    for (int i = 0; i < 3; ++i) source.update(far, heightForZoom(2));
    EXPECT_FALSE(sinks.dropped.empty()) << "离开视口的瓦片应被 drop";

    for (int i = 0; i < 3; ++i) source.update(near, heightForZoom(2));
    EXPECT_GT(sinks.tessellateCalls, tessAfterFirst) << "重入应重镶嵌";
    // 回到原视口不该再对同一批瓦片发网络请求。
    size_t refetchOfOriginals = 0;
    for (size_t i = fetchesAfterFirst; i < fetch.requested.size(); ++i) {
        for (size_t j = 0; j < fetchesAfterFirst; ++j) {
            if (fetch.requested[i] == fetch.requested[j]) ++refetchOfOriginals;
        }
    }
    EXPECT_EQ(refetchOfOriginals, 0u) << "重入零重拉取";
}

TEST(MvtVectorSource, FailedFetchMarksFailedNoRerequest) {
    FakeFetch fetch;
    fetch.statusCode = 404;
    FakeSinks sinks;
    MvtVectorSource source(optionsForTest(), sinks.fn(), fetch.cache());

    Rectangle view = rectDeg(1, 1, 40, 40);
    source.update(view, heightForZoom(2));
    size_t firstBatch = fetch.requested.size();
    ASSERT_GT(firstBatch, 0u);

    source.update(view, heightForZoom(2));
    EXPECT_EQ(fetch.requested.size(), firstBatch);  // 不重复请求
    EXPECT_GT(source.tree().failedCount(), 0u);
    EXPECT_EQ(sinks.tessellateCalls, 0);
    EXPECT_TRUE(sinks.committed.empty());
}

TEST(MvtVectorSource, ZoomChangeSwapsTilesNoLeftovers) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    FakeSinks sinks;
    MvtVectorSource source(optionsForTest(), sinks.fn(), fetch.cache());

    Rectangle view = rectDeg(1, 1, 40, 40);
    for (int i = 0; i < 3; ++i) source.update(view, heightForZoom(2));
    const size_t activeAtZ2 = source.activeTileCount();
    ASSERT_GT(activeAtZ2, 0u);

    for (int i = 0; i < 3; ++i) source.update(view, heightForZoom(4));
    EXPECT_GT(source.activeTileCount(), 0u);
    EXPECT_FALSE(sinks.dropped.empty()) << "旧 zoom 的瓦片必须被 drop";
    // commit 与 drop 的差 = 当前驻留数,不该有泄漏。
    EXPECT_EQ(sinks.committed.size() - sinks.dropped.size(),
              source.activeTileCount());
}

// commit 限流拦的是**上传**成本(镶嵌已在 worker),与旧的激活预算语义
// 不同 —— 那个拦的是渲染线程上的镶嵌,是结构性补丁。
TEST(MvtVectorSource, CommitBudgetSpreadsAcrossUpdates) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    MvtVectorSource::Options opt = optionsForTest();
    opt.maxTileCommitsPerUpdate = 2;
    FakeSinks sinks;
    MvtVectorSource source(opt, sinks.fn(), fetch.cache());

    Rectangle view = rectDeg(-80, -40, 80, 40);  // z2 多瓦片视口
    size_t prev = 0;
    for (int i = 0; i < 40; ++i) {
        source.update(view, heightForZoom(2));
        const size_t now = source.activeTileCount();
        EXPECT_LE(now - prev, 2u) << "单帧 commit 不得超预算";
        prev = now;
    }
    EXPECT_GE(source.activeTileCount(), 4u) << "最终全部 commit 完";
    EXPECT_EQ(source.pendingCommitCount(), 0u);
}

// E2:层级 zoom 区间在**转换之前**判 —— 整层跳过时连 MVT→Feature 的转换
// 都省了(P4 实测巨瓦转换 81ms),这正是 maplibre 把 layer minzoom/maxzoom
// 放在建桶前的理由。
TEST(MvtVectorSource, LayerRuleZoomRangeSkipsWholeLayer) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    MvtVectorSource::Options opt = optionsForTest();
    SourceLayerRule rule;
    rule.layer = "pois";
    rule.minZoom = 10;   // 视口在 z2 → 整层跳过
    rule.maxZoom = 14;
    opt.layerRules = {rule};
    FakeSinks sinks;
    MvtVectorSource source(opt, sinks.fn(), fetch.cache());

    Rectangle view = rectDeg(1, 1, 40, 40);
    for (int i = 0; i < 3; ++i) source.update(view, heightForZoom(2));
    EXPECT_GT(sinks.tessellateCalls, 0) << "瓦片仍派了镶嵌单";
    EXPECT_EQ(sinks.lastFeatureCount, 0u) << "z2 不在 [10,14],整层跳过";
}

// 逐要素 filter 在 worker 上求值(StyleFilter 不可变纯函数)。
TEST(MvtVectorSource, LayerRuleFeatureFilterApplies) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");  // 要素带 kind=poi
    MvtVectorSource::Options opt = optionsForTest();
    SourceLayerRule keep;
    keep.layer = "pois";
    keep.filter = StyleFilter::compare("kind", StyleFilter::Compare::Equal,
                                       std::string("poi"));
    opt.layerRules = {keep};
    FakeSinks sinks;
    MvtVectorSource source(opt, sinks.fn(), fetch.cache());
    Rectangle view = rectDeg(1, 1, 40, 40);
    for (int i = 0; i < 3; ++i) source.update(view, heightForZoom(2));
    EXPECT_GT(sinks.lastFeatureCount, 0u) << "匹配的要素应留下";

    // 换成不匹配的谓词 → 同一批瓦片重镶后要素全被滤掉。
    SourceLayerRule drop = keep;
    drop.filter = StyleFilter::compare("kind", StyleFilter::Compare::Equal,
                                       std::string("nope"));
    const size_t fetchesBefore = fetch.requested.size();
    source.setLayerRules({drop});
    for (int i = 0; i < 3; ++i) source.update(view, heightForZoom(2));
    EXPECT_EQ(sinks.lastFeatureCount, 0u) << "新谓词下要素应被滤光";
    // 规则变更只该重镶,不该重拉网络(解码结果在树的 LRU 里)。
    EXPECT_EQ(fetch.requested.size(), fetchesBefore) << "改规则零重拉取";
}

TEST(MvtVectorSource, IncludeLayersFilters) {
    FakeFetch fetch;
    fetch.body = makePointTile("water");
    MvtVectorSource::Options opt = optionsForTest();
    opt.includeLayers = {"roads"};  // 瓦片只有 water 层 → 全被滤掉
    FakeSinks sinks;
    MvtVectorSource source(opt, sinks.fn(), fetch.cache());

    Rectangle view = rectDeg(1, 1, 40, 40);
    for (int i = 0; i < 3; ++i) source.update(view, heightForZoom(2));
    EXPECT_GT(sinks.tessellateCalls, 0) << "瓦片仍派了镶嵌单";
    EXPECT_EQ(sinks.lastFeatureCount, 0u) << "但没有要素通过过滤";
}

} // namespace

// ---------------------------------------------------------------------------
// R* 换手原子性(2026-08-15,V24 缺陷④根修)
// ---------------------------------------------------------------------------

/// LOD 换代全程(粗→细→粗)逐 update 校验两条不变量:
///   1. 无空窗:占位者在替换内容全部就绪前决不退场,active 集不空;
///   2. 无重影:active 集内任意两瓦无祖先/后代关系 —— 细瓦不提前上屏,
///      commit 替换者与 drop 占位者发生在同一次 update。
TEST(MvtVectorSource, LodSwapIsAtomicNoHoleNoOverlap) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    FakeSinks sinks;
    MvtVectorSource source(optionsForTest(), sinks.fn(), fetch.cache());
    // 视口跨 z4 的 2×2 子瓦、落在同一张 z3 父瓦内(22.5° 界在 22.5°)
    const Rectangle view = rectDeg(20, 20, 25, 25);

    std::unordered_set<TileKey> act;
    auto pumpAndCheck = [&](int zoom) {
        const size_t c0 = sinks.committed.size();
        const size_t d0 = sinks.dropped.size();
        source.update(view, heightForZoom(zoom));
        for (size_t i = c0; i < sinks.committed.size(); ++i) {
            act.insert(sinks.committed[i]);
        }
        for (size_t i = d0; i < sinks.dropped.size(); ++i) {
            act.erase(sinks.dropped[i]);
        }
        // 不变量 2:无重影
        for (const TileKey& a : act) {
            for (const TileKey& b : act) {
                if (a == b || a.z >= b.z) continue;
                const int d = b.z - a.z;
                EXPECT_FALSE((b.x >> d) == a.x && (b.y >> d) == a.y)
                    << a << " 与 " << b << " 同框(重影)";
            }
        }
    };

    // 阶段 1:z3 粗瓦上屏
    for (int i = 0; i < 6 && act.empty(); ++i) pumpAndCheck(3);
    ASSERT_FALSE(act.empty());
    for (const TileKey& k : act) ASSERT_EQ(k.z, 3);

    // 阶段 2:拉近到 z4 —— 换手全程不空窗
    for (int i = 0; i < 8; ++i) {
        pumpAndCheck(4);
        ASSERT_FALSE(act.empty()) << "细化换手第 " << i << " 次 update 空窗";
    }
    ASSERT_GE(act.size(), 2u) << "视口应跨多张 z4 子瓦";
    for (const TileKey& k : act) {
        EXPECT_EQ(k.z, 4) << "换手应已收敛到 z4";
    }

    // 阶段 3:拉回 z3(细换粗,网格已 drop 需重镶嵌)—— 同样不空窗
    for (int i = 0; i < 8; ++i) {
        pumpAndCheck(3);
        ASSERT_FALSE(act.empty()) << "粗化换手第 " << i << " 次 update 空窗";
    }
    for (const TileKey& k : act) {
        EXPECT_EQ(k.z, 3) << "换手应已收敛回 z3";
    }
}

// ---------------------------------------------------------------------------
// P2 两层缓存(2026-08-15):L1 淘汰降级到 L2 字节层,重取免网络。
// ---------------------------------------------------------------------------

TEST(MvtTileFetchCache, EvictedTileRefetchesFromRawTierNotNetwork) {
    int networkCalls = 0;
    const std::vector<uint8_t> body = makePointTile("pois");
    // L1 只放 1 张,L2 放 8 张 → 第二张必挤掉第一张,但字节仍在
    MvtTileFetchCache cache(
        [&](const TileKey& k, MvtTileFetchCache::FetchCallback cb) {
            ++networkCalls;
            cb(200, body);
        },
        /*capacity=*/1, /*rawCapacity=*/8, /*decodePool=*/nullptr);

    auto get = [&](int x) {
        std::shared_ptr<const MvtTile> got;
        cache.request(TileKey{SchemeId{}, 4, x, 1},
                      [&](std::shared_ptr<const MvtTile> t) { got = t; });
        return got;
    };
    ASSERT_NE(get(1), nullptr);
    ASSERT_NE(get(2), nullptr);   // 挤掉 x=1
    EXPECT_EQ(networkCalls, 2);

    // 再要 x=1:L1 已无,但 L2 有字节 → 重解码,**不再走网络**
    ASSERT_NE(get(1), nullptr);
    EXPECT_EQ(networkCalls, 2) << "L2 未命中 = 又去拉网络了,两层缓存失效";
    const auto st = cache.stats();
    EXPECT_EQ(st.rawHits, 1u);
    EXPECT_EQ(st.refetches, 0u) << "L2 命中不该计重复拉取";
    EXPECT_GT(st.rawBytes, 0u);
}
