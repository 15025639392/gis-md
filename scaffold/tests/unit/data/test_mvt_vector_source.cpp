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

/// 假网络:记录请求,立即成功回调
struct FakeFetch {
    std::vector<TileKey> requested;
    std::vector<uint8_t> body;
    int statusCode = 200;

    MvtVectorSource::FetchFn fn() {
        return [this](const TileKey& key, MvtVectorSource::FetchCallback cb) {
            requested.push_back(key);
            cb(statusCode, body);
        };
    }
};

MvtVectorSource::Options optionsForTest() {
    MvtVectorSource::Options opt;
    opt.tree.maxTilesPerView = 64;
    return opt;
}

TEST(MvtVectorSource, FetchDecodeActivatePipeline) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    FeatureStore store;
    MvtVectorSource source(optionsForTest(), store, fetch.fn());

    Rectangle view = rectDeg(1, 1, 40, 40);
    source.update(view, heightForZoom(2));
    ASSERT_FALSE(fetch.requested.empty());
    EXPECT_EQ(source.store().size(), 0u);  // 解码在收件箱,尚未激活

    source.update(view, heightForZoom(2));
    EXPECT_GT(source.activeTileCount(), 0u);
    ASSERT_GT(source.store().size(), 0u);

    // 要素带属性 + mvt_layer 注入(点在瓦片中心,可能落在视口外,
    // 查询窗用全球)
    auto ids = source.store().queryVisible(rectDeg(-179, -85, 179, 85));
    ASSERT_FALSE(ids.empty());
    const Feature* f = source.store().getFeature(ids[0]);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->properties.at("kind"), "poi");
    EXPECT_EQ(f->properties.at("mvt_layer"), "pois");
    EXPECT_EQ(f->sourceId, "42");
}

TEST(MvtVectorSource, FailedFetchMarksFailedNoRerequest) {
    FakeFetch fetch;
    fetch.statusCode = 404;
    FeatureStore store;
    MvtVectorSource source(optionsForTest(), store, fetch.fn());

    Rectangle view = rectDeg(1, 1, 40, 40);
    source.update(view, heightForZoom(2));
    size_t firstBatch = fetch.requested.size();
    ASSERT_GT(firstBatch, 0u);

    source.update(view, heightForZoom(2));
    EXPECT_EQ(fetch.requested.size(), firstBatch);  // 不重复请求
    EXPECT_GT(source.tree().failedCount(), 0u);
    EXPECT_EQ(source.store().size(), 0u);
}

TEST(MvtVectorSource, ZoomChangeSwapsActiveTilesNoLeftovers) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    FeatureStore store;
    MvtVectorSource source(optionsForTest(), store, fetch.fn());

    Rectangle view = rectDeg(1, 1, 40, 40);
    source.update(view, heightForZoom(2));
    source.update(view, heightForZoom(2));
    size_t activeAtZ2 = source.activeTileCount();
    ASSERT_GT(activeAtZ2, 0u);
    size_t storeAtZ2 = source.store().size();

    // 拉近两档:旧 zoom 激活集必须整体换成新 zoom
    source.update(view, heightForZoom(4));
    source.update(view, heightForZoom(4));
    // 所有激活瓦片都在新 zoom(祖先回退已被新瓦片替换,因为假网络即时返回)
    EXPECT_GT(source.activeTileCount(), 0u);
    // store 数与激活瓦片一致:每瓦片 1 要素
    EXPECT_EQ(source.store().size(), source.activeTileCount());
    // 防泄漏:回到 z2 视角再看,store 不应累积
    source.update(view, heightForZoom(2));
    source.update(view, heightForZoom(2));
    EXPECT_EQ(source.store().size(), source.activeTileCount());
    EXPECT_LE(source.store().size(), std::max(storeAtZ2, activeAtZ2) * 2);
}

TEST(MvtVectorSource, ActivationBudgetSpreadsAcrossUpdates) {
    FakeFetch fetch;
    fetch.body = makePointTile("pois");
    MvtVectorSource::Options opt = optionsForTest();
    opt.maxActivationFeaturesPerUpdate = 2;  // 每瓦片 1 要素 → 每帧至多 2 瓦
    FeatureStore store;
    MvtVectorSource source(opt, store, fetch.fn());

    Rectangle view = rectDeg(-80, -40, 80, 40);  // z2 多瓦片视口
    source.update(view, heightForZoom(2));       // 发请求(假网络即回)
    source.update(view, heightForZoom(2));       // 第一批激活(预算 2)
    size_t after1 = source.activeTileCount();
    EXPECT_GT(after1, 0u);
    EXPECT_LE(after1, 2u);

    // 反复 update 直到激活完;每帧增量 ≤ 预算,最终全量激活
    size_t prev = after1;
    for (int i = 0; i < 32 && source.tree().pendingCount() == 0; ++i) {
        source.update(view, heightForZoom(2));
        size_t now = source.activeTileCount();
        EXPECT_LE(now - prev, 2u);
        if (now == prev && now == source.store().size()) {
            break;
        }
        prev = now;
    }
    EXPECT_GE(source.activeTileCount(), 4u);  // 视口至少 2×2 瓦全部激活
    EXPECT_EQ(source.store().size(), source.activeTileCount());
}

TEST(MvtVectorSource, IncludeLayersFilters) {
    FakeFetch fetch;
    fetch.body = makePointTile("water");
    MvtVectorSource::Options opt = optionsForTest();
    opt.includeLayers = {"roads"};  // 瓦片只有 water 层 → 全被滤掉
    FeatureStore store;
    MvtVectorSource source(opt, store, fetch.fn());

    Rectangle view = rectDeg(1, 1, 40, 40);
    source.update(view, heightForZoom(2));
    source.update(view, heightForZoom(2));
    EXPECT_GT(source.activeTileCount(), 0u);  // 瓦片激活了
    EXPECT_EQ(source.store().size(), 0u);     // 但没有要素通过过滤
}

} // namespace
