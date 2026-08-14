#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "earth_engine/data/MvtTileFetchCache.h"
#include "earth_engine/providers/RoadFieldSource.h"
#include "earth_engine/providers/VectorDrapeImageryProvider.h"

using namespace earth_engine;

namespace {

/// 最小 pbf:单层单 LineString(水平中线)。
struct Pbf {
    std::vector<uint8_t> bytes;
    void varint(uint64_t v) {
        while (v >= 0x80) {
            bytes.push_back(static_cast<uint8_t>(v) | 0x80);
            v >>= 7;
        }
        bytes.push_back(static_cast<uint8_t>(v));
    }
    void varintField(uint32_t f, uint64_t v) { varint((f << 3) | 0); varint(v); }
    void bytesField(uint32_t f, const std::vector<uint8_t>& sub) {
        varint((f << 3) | 2); varint(sub.size());
        bytes.insert(bytes.end(), sub.begin(), sub.end());
    }
    void stringField(uint32_t f, const std::string& s) {
        varint((f << 3) | 2); varint(s.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
};

uint32_t zigzag(int32_t v) {
    return static_cast<uint32_t>((v << 1) ^ (v >> 31));
}

std::vector<uint8_t> makeMidlineRoad(const std::string& layerName) {
    Pbf geom;
    geom.varint(9);  // MoveTo ×1
    geom.varint(zigzag(0)); geom.varint(zigzag(2048));
    geom.varint((1 << 3) | 2);  // LineTo ×1
    geom.varint(zigzag(4096)); geom.varint(zigzag(0));

    Pbf feature;
    feature.varintField(1, 7);
    feature.varintField(3, 2);  // type LineString
    feature.bytesField(4, geom.bytes);

    Pbf layer;
    layer.varintField(15, 2);
    layer.stringField(1, layerName);
    layer.bytesField(2, feature.bytes);
    layer.varintField(5, 4096);

    Pbf tile;
    tile.bytesField(3, layer.bytes);
    return tile.bytes;
}

struct FakeFetch {
    std::vector<uint8_t> body;
    int statusCode = 200;
    int calls = 0;

    MvtTileFetchCache::FetchFn fn() {
        return [this](const TileKey&, MvtTileFetchCache::FetchCallback cb) {
            ++calls;
            cb(statusCode, body);
        };
    }
};

RoadFieldSource::Options fieldOptions() {
    VectorRasterLayerPaint roads;
    roads.layer = "roads";
    roads.lineColor = {255, 255, 255, 255};
    roads.lineWidthPixels = 8.0;
    RoadFieldSource::Options opt;
    opt.fieldSize = 64;
    opt.dataMaxZoom = 14;
    opt.style.layers = {roads};
    return opt;
}

} // namespace

// 基本产出:z≤dataMax 单瓦,中线纹素有线段记录(A>0)、远处空哨兵。
TEST(RoadFieldSourceTest, ProducesFieldForRoadTile) {
    FakeFetch fetch;
    fetch.body = makeMidlineRoad("roads");
    auto cache = std::make_shared<MvtTileFetchCache>(fetch.fn(), 8);
    RoadFieldSource source(fieldOptions(), cache);

    std::vector<uint8_t> got;
    source.requestField(TileKey{"XYZ-WebMercator", 12, 1, 1},
                        CancellationToken(),
                        [&](std::vector<uint8_t> r8) { got = std::move(r8); });
    ASSERT_EQ(got.size(), 64u * 64u * 4u);
    EXPECT_NE(got[(32u * 64u + 32u) * 4u + 3u], 0) << "中线纹素 A>0";
    EXPECT_EQ(got[(8u * 64u + 32u) * 4u + 3u], 0) << "远处空哨兵";
}

// fetch 失败 → 全 0 场(空哨兵=无线,失败安全),回调必到。
TEST(RoadFieldSourceTest, FetchFailureYieldsFarField) {
    FakeFetch fetch;
    fetch.statusCode = 404;
    auto cache = std::make_shared<MvtTileFetchCache>(fetch.fn(), 8);
    RoadFieldSource source(fieldOptions(), cache);

    std::vector<uint8_t> got;
    bool called = false;
    source.requestField(TileKey{"XYZ-WebMercator", 12, 1, 1},
                        CancellationToken(),
                        [&](std::vector<uint8_t> r8) {
                            called = true;
                            got = std::move(r8);
                        });
    ASSERT_TRUE(called);
    ASSERT_EQ(got.size(), 64u * 64u * 4u);
    for (uint8_t v : got) EXPECT_EQ(v, 0);
}

// 共享缓存:面 drape 与场烘焙请求同一 z14 祖先只 fetch 一次(刀2 提炼
// MvtTileFetchCache 的存在理由)。
TEST(RoadFieldSourceTest, SharesTileCacheWithDrapeProvider) {
    FakeFetch fetch;
    fetch.body = makeMidlineRoad("roads");
    auto cache = std::make_shared<MvtTileFetchCache>(fetch.fn(), 8);

    RoadFieldSource field(fieldOptions(), cache);
    VectorDrapeImageryProvider::Options dopt;
    dopt.tileSize = 32;
    dopt.dataMaxZoom = 14;
    dopt.advertisedMaxZoom = 18;
    VectorRasterLayerPaint water;
    water.layer = "water";
    water.fillColor = {0, 0, 255, 255};
    dopt.style.layers = {water};
    VectorDrapeImageryProvider drape(dopt, cache);

    // 同一 z14 祖先(z16 子页 → (14,5,5))被两条链各请求一次。
    std::vector<uint8_t> fieldOut;
    field.requestField(TileKey{"XYZ-WebMercator", 16, 20, 21},
                       CancellationToken(),
                       [&](std::vector<uint8_t> r8) {
                           fieldOut = std::move(r8);
                       });
    std::unique_ptr<DecodedImage> drapeOut;
    drape.requestTile(TileKey{"XYZ-WebMercator", 16, 21, 21},
                      CancellationToken(),
                      [&](const TileKey&, std::unique_ptr<DecodedImage> img) {
                          drapeOut = std::move(img);
                      });
    EXPECT_EQ(fetch.calls, 1) << "两条链共享同一次 fetch+decode";
    EXPECT_EQ(cache->stats().hits, 1u);
    ASSERT_FALSE(fieldOut.empty());
    ASSERT_NE(drapeOut, nullptr);
}

// 取消仍回调(空场):页存储按回调必到记账。
TEST(RoadFieldSourceTest, CancelledStillInvokesCallback) {
    FakeFetch fetch;
    fetch.body = makeMidlineRoad("roads");
    auto cache = std::make_shared<MvtTileFetchCache>(fetch.fn(), 8);
    RoadFieldSource source(fieldOptions(), cache);

    CancellationToken token;
    token.cancel();
    bool called = false;
    source.requestField(TileKey{"XYZ-WebMercator", 12, 1, 1}, token,
                        [&](std::vector<uint8_t> r8) {
                            called = true;
                            EXPECT_EQ(r8.size(), 64u * 64u * 4u);
                        });
    EXPECT_TRUE(called);
}
