#include <gtest/gtest.h>

#include "earth_engine/providers/VectorDrapeImageryProvider.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Gcj02CoordinateTransform.h"

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;

/// 最小 pbf 编码(只够造一个单层单多边形瓦片)。
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

/// 以 (cx,cy) 为中心、half 为半边长的方形 polygon(extent 4096,可越界)。
std::vector<uint8_t> makeSquarePolygon(const std::string& layerName, int cx,
                                       int cy, int half) {
    Pbf geom;
    geom.varint(9);  // MoveTo ×1
    geom.varint(zigzag(cx - half)); geom.varint(zigzag(cy - half));
    geom.varint((3 << 3) | 2);  // LineTo ×3
    geom.varint(zigzag(2 * half)); geom.varint(zigzag(0));
    geom.varint(zigzag(0)); geom.varint(zigzag(2 * half));
    geom.varint(zigzag(-2 * half)); geom.varint(zigzag(0));
    geom.varint(15);  // ClosePath

    Pbf feature;
    feature.varintField(1, 7);  // id
    feature.varintField(3, 3);  // type Polygon
    feature.bytesField(4, geom.bytes);

    Pbf layer;
    layer.varintField(15, 2);  // version
    layer.stringField(1, layerName);
    layer.bytesField(2, feature.bytes);
    layer.varintField(5, 4096);  // extent

    Pbf tile;
    tile.bytesField(3, layer.bytes);
    return tile.bytes;
}

/// 覆盖瓦片左半边的矩形。
std::vector<uint8_t> makeHalfTilePolygon(const std::string& layerName) {
    return makeSquarePolygon(layerName, 1024, 2048, 1024);
}

VectorDrapeImageryProvider::Options optionsFor(const std::string& layerName) {
    VectorRasterLayerPaint paint;
    paint.layer = layerName;
    paint.fillColor = {255, 0, 0, 255};

    VectorDrapeImageryProvider::Options opt;
    opt.tileSize = 32;
    opt.advertisedMaxZoom = 18;
    opt.dataMaxZoom = 14;
    opt.style.layers = {paint};
    return opt;
}

struct FakeFetch {
    std::vector<uint8_t> body;
    int statusCode = 200;
    int calls = 0;
    std::vector<TileKey> keys;
    bool deferred = false;
    std::vector<std::pair<TileKey, VectorDrapeImageryProvider::FetchCallback>>
        pending;

    VectorDrapeImageryProvider::FetchFn fn() {
        return [this](const TileKey& key,
                      VectorDrapeImageryProvider::FetchCallback cb) {
            ++calls;
            keys.push_back(key);
            if (deferred) {
                pending.emplace_back(key, std::move(cb));
            } else {
                cb(statusCode, body);
            }
        };
    }
};

int alphaAt(const DecodedImage& img, int x, int y) {
    return img.pixels[(static_cast<size_t>(y) * img.width + x) * 4 + 3];
}

bool anyOpaquePixel(const DecodedImage& img) {
    for (size_t i = 3; i < img.pixels.size(); i += 4) {
        if (img.pixels[i] != 0) return true;
    }
    return false;
}

std::unique_ptr<DecodedImage> request(VectorDrapeImageryProvider& provider,
                                      const TileKey& key,
                                      bool* calledOut = nullptr) {
    std::unique_ptr<DecodedImage> got;
    bool called = false;
    provider.requestTile(key, CancellationToken(),
                         [&](const TileKey&,
                             std::unique_ptr<DecodedImage> img) {
                             called = true;
                             got = std::move(img);
                         });
    if (calledOut) *calledOut = called;
    return got;
}

double unitXFromLng(double lngRad) { return lngRad / (2.0 * kPi) + 0.5; }
double unitYFromLat(double latRad) {
    return 0.5 - std::log(std::tan(kPi / 4.0 + latRad / 2.0)) / (2.0 * kPi);
}

} // namespace

// 支点验证:矢量瓦片经 provider 变成一张 RGBA 影像,格式与真影像无区别。
TEST(VectorDrapeProviderTest, ProducesRgbaImageFromVectorTile) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    VectorDrapeImageryProvider provider(optionsFor("L"), fetch.fn());

    auto got = request(provider, TileKey{"XYZ-WebMercator", 12, 1, 1});
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->width, 32);
    EXPECT_EQ(got->channels, 4);
    EXPECT_EQ(alphaAt(*got, 8, 16), 255) << "左半应被填充";
    EXPECT_EQ(alphaAt(*got, 24, 16), 0) << "右半应透明";
    ASSERT_EQ(fetch.keys.size(), 1u);
    EXPECT_EQ(fetch.keys[0].z, 12) << "z≤dataMaxZoom 直取同级";
}

// 空瓦片(样式层不存在)必须回全透明图,不是 nullptr。
TEST(VectorDrapeProviderTest, EmptyTileYieldsTransparentImageNotNull) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("other");
    VectorDrapeImageryProvider provider(optionsFor("L"), fetch.fn());

    bool called = false;
    auto got = request(provider, TileKey{"XYZ-WebMercator", 12, 1, 1},
                       &called);
    ASSERT_TRUE(called);
    ASSERT_NE(got, nullptr) << "空结果 ≠ 失败";
    EXPECT_FALSE(anyOpaquePixel(*got));
}

// **与 E4 版语义相反**:fetch 失败也回全透明图。页存储的 assembler 收不到
// 某源就永不 complete,合成缓冲不释放 —— server 未启动时会按页数滞留内存。
TEST(VectorDrapeProviderTest, FetchFailureYieldsTransparentImageNotNull) {
    FakeFetch fetch;
    fetch.statusCode = 404;
    VectorDrapeImageryProvider provider(optionsFor("L"), fetch.fn());

    bool called = false;
    auto got = request(provider, TileKey{"XYZ-WebMercator", 12, 1, 1},
                       &called);
    ASSERT_TRUE(called);
    ASSERT_NE(got, nullptr) << "失败必须降级成透明图,不能卡死页合成";
    EXPECT_FALSE(anyOpaquePixel(*got));
}

// 取消后仍要回调(上层按「回调必到」管理在途计数)。
TEST(VectorDrapeProviderTest, CancelledRequestStillInvokesCallback) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    VectorDrapeImageryProvider provider(optionsFor("L"), fetch.fn());

    CancellationToken token;
    token.cancel();
    bool called = false;
    std::unique_ptr<DecodedImage> got;
    provider.requestTile(TileKey{"XYZ-WebMercator", 12, 1, 1}, token,
                         [&](const TileKey&,
                             std::unique_ptr<DecodedImage> img) {
                             called = true;
                             got = std::move(img);
                         });
    EXPECT_TRUE(called) << "取消也必须回调";
    EXPECT_EQ(got, nullptr);
}

// overzoom 核心:深于 dataMaxZoom 的页请求取祖先瓦、只画子矩形。
// 数据瓦 (14,5,5) 左半有 polygon → z16 子页 (16,20,20) 在其内=全填,
// (16,23,20) 在其外=全空;fetch 只该收到 z14 祖先 key。
TEST(VectorDrapeProviderTest, OverzoomPaintsAncestorSubRect) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    VectorDrapeImageryProvider provider(optionsFor("L"), fetch.fn());

    auto nw = request(provider, TileKey{"XYZ-WebMercator", 16, 20, 21});
    ASSERT_NE(nw, nullptr);
    EXPECT_EQ(alphaAt(*nw, 16, 16), 255) << "西侧子页应全填";
    ASSERT_GE(fetch.keys.size(), 1u);
    EXPECT_EQ(fetch.keys[0].z, 14) << "应取 dataMaxZoom 祖先";
    EXPECT_EQ(fetch.keys[0].x, 5);
    EXPECT_EQ(fetch.keys[0].y, 5);

    auto ne = request(provider, TileKey{"XYZ-WebMercator", 16, 23, 21});
    ASSERT_NE(ne, nullptr);
    EXPECT_FALSE(anyOpaquePixel(*ne)) << "东侧子页应全空";
}

// 样式 zoom 用页 zoom 而非数据瓦 z:building minZoom=13 这类门槛跟屏幕
// 清晰度走,数据被钳到 z14 不应把 z16 页判成 z14。
TEST(VectorDrapeProviderTest, StyleZoomUsesPageZoomNotDataZoom) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    auto opt = optionsFor("L");
    opt.style.layers[0].minZoom = 15;  // 深于数据上限的门槛
    VectorDrapeImageryProvider provider(opt, fetch.fn());

    auto z16 = request(provider, TileKey{"XYZ-WebMercator", 16, 20, 21});
    ASSERT_NE(z16, nullptr);
    EXPECT_TRUE(anyOpaquePixel(*z16)) << "styleZoom=16 ≥ minZoom=15 应画";

    auto z12 = request(provider, TileKey{"XYZ-WebMercator", 12, 1, 1});
    ASSERT_NE(z12, nullptr);
    EXPECT_FALSE(anyOpaquePixel(*z12)) << "styleZoom=12 < 15 整层跳过";
}

// 解码缓存:同一祖先瓦被多个子页请求只 fetch 一次(overzoom 常态)。
TEST(VectorDrapeProviderTest, CacheAvoidsRefetchAcrossPages) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    VectorDrapeImageryProvider provider(optionsFor("L"), fetch.fn());

    request(provider, TileKey{"XYZ-WebMercator", 16, 20, 20});
    request(provider, TileKey{"XYZ-WebMercator", 16, 21, 21});
    request(provider, TileKey{"XYZ-WebMercator", 16, 22, 20});
    EXPECT_EQ(fetch.calls, 1) << "同一 z14 祖先只该拉一次";
    const auto stats = provider.cacheStats();
    EXPECT_EQ(stats.fetches, 1u);
    EXPECT_EQ(stats.hits, 2u);
}

// 在途合并:第一片还在拉时来了同祖先的第二个请求,不重复 fetch,
// 到达后两个请求都完成。
TEST(VectorDrapeProviderTest, InflightRequestsMergeIntoSingleFetch) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    fetch.deferred = true;
    VectorDrapeImageryProvider provider(optionsFor("L"), fetch.fn());

    std::unique_ptr<DecodedImage> gotA;
    std::unique_ptr<DecodedImage> gotB;
    provider.requestTile(TileKey{"XYZ-WebMercator", 16, 20, 21},
                         CancellationToken(),
                         [&](const TileKey&,
                             std::unique_ptr<DecodedImage> img) {
                             gotA = std::move(img);
                         });
    provider.requestTile(TileKey{"XYZ-WebMercator", 16, 21, 21},
                         CancellationToken(),
                         [&](const TileKey&,
                             std::unique_ptr<DecodedImage> img) {
                             gotB = std::move(img);
                         });
    EXPECT_EQ(fetch.calls, 1) << "在途中的同瓦请求应搭车";
    EXPECT_EQ(gotA, nullptr) << "未到达前不该回调";

    ASSERT_EQ(fetch.pending.size(), 1u);
    fetch.pending[0].second(200, fetch.body);
    ASSERT_NE(gotA, nullptr);
    ASSERT_NE(gotB, nullptr);
    EXPECT_TRUE(anyOpaquePixel(*gotA));
    EXPECT_TRUE(anyOpaquePixel(*gotB));
}

// GCJ 源网格方向判据:页网格按高德(GCJ)建 → 含 fromWgs84(p) 的页,开
// gcj02SourceGrid 后应画出 WGS84 点 p 处的要素;不开则矩形停在 GCJ 数值
// 位置,距 p ~500m(重庆),远超 ±50m 的要素与 z18 页宽 → 全空。
// 平移方向做反的话两个断言都会翻。
TEST(VectorDrapeProviderTest, Gcj02GridSelectsWgs84ShiftedContent) {
    const Cartographic p = Cartographic::fromDegrees(106.55, 29.56);
    const double u = unitXFromLng(p.longitude());
    const double v = unitYFromLat(p.latitude());
    const int dataZ = 14;
    const int n14 = 1 << dataZ;
    const int tx = static_cast<int>(std::floor(u * n14));
    const int ty = static_cast<int>(std::floor(v * n14));
    // p 在数据瓦内的 local 坐标(extent 4096),要素 = p 为中心 ±50m
    // (z14 瓦 ~9km → 50m ≈ 23 units)。
    const int lx = static_cast<int>((u * n14 - tx) * 4096.0);
    const int ly = static_cast<int>((v * n14 - ty) * 4096.0);

    FakeFetch fetch;
    fetch.body = makeSquarePolygon("L", lx, ly, 23);

    // 页:含 GCJ(p) 的 z18 瓦(高德网格里 p 的地物所在的页)。
    const Cartographic gcj = Gcj02CoordinateTransform::fromWgs84(p);
    const int n18 = 1 << 18;
    const int kx = static_cast<int>(
        std::floor(unitXFromLng(gcj.longitude()) * n18));
    const int ky = static_cast<int>(
        std::floor(unitYFromLat(gcj.latitude()) * n18));
    const TileKey pageKey{"XYZ-WebMercator", 18, kx, ky};

    auto optOn = optionsFor("L");
    optOn.gcj02SourceGrid = true;
    VectorDrapeImageryProvider on(optOn, fetch.fn());
    auto gotOn = request(on, pageKey);
    ASSERT_NE(gotOn, nullptr);
    EXPECT_TRUE(anyOpaquePixel(*gotOn))
        << "GCJ 页平移回 WGS84 后应含 p 处要素";

    FakeFetch fetch2;
    fetch2.body = fetch.body;
    auto optOff = optionsFor("L");
    optOff.gcj02SourceGrid = false;
    VectorDrapeImageryProvider off(optOff, fetch2.fn());
    auto gotOff = request(off, pageKey);
    ASSERT_NE(gotOff, nullptr);
    EXPECT_FALSE(anyOpaquePixel(*gotOff))
        << "不平移时矩形停在 GCJ 数值位置,应不含 p 处要素";
}
