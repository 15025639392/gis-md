#include <gtest/gtest.h>

#include "earth_engine/providers/VectorImageryProvider.h"

#include <string>
#include <vector>

using namespace earth_engine;

namespace {

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

/// 覆盖瓦片左半边的矩形(extent 4096)。
std::vector<uint8_t> makeHalfTilePolygon(const std::string& layerName) {
    Pbf geom;
    geom.varint(9);                       // MoveTo ×1
    geom.varint(zigzag(0)); geom.varint(zigzag(0));
    geom.varint((3 << 3) | 2);            // LineTo ×3
    geom.varint(zigzag(2048)); geom.varint(zigzag(0));
    geom.varint(zigzag(0));    geom.varint(zigzag(4096));
    geom.varint(zigzag(-2048)); geom.varint(zigzag(0));
    geom.varint(15);                      // ClosePath

    Pbf feature;
    feature.varintField(1, 7);            // id
    feature.varintField(3, 3);            // type Polygon
    feature.bytesField(4, geom.bytes);

    Pbf layer;
    layer.varintField(15, 2);             // version
    layer.stringField(1, layerName);
    layer.bytesField(2, feature.bytes);
    layer.varintField(5, 4096);           // extent

    Pbf tile;
    tile.bytesField(3, layer.bytes);
    return tile.bytes;
}

VectorImageryProvider::Options optionsFor(const std::string& layerName) {
    VectorRasterLayerPaint paint;
    paint.layer = layerName;
    paint.fillColor = {255, 0, 0, 255};

    VectorImageryProvider::Options opt;
    opt.tileSize = 32;
    opt.maxZoom = 14;
    opt.style.layers = {paint};
    return opt;
}

struct FakeFetch {
    std::vector<uint8_t> body;
    int statusCode = 200;
    int calls = 0;

    VectorImageryProvider::FetchFn fn() {
        return [this](const TileKey&,
                      VectorImageryProvider::FetchCallback cb) {
            ++calls;
            cb(statusCode, body);
        };
    }
};

int alphaAt(const DecodedImage& img, int x, int y) {
    return img.pixels[(static_cast<size_t>(y) * img.width + x) * 4 + 3];
}

} // namespace

// 支点验证:矢量瓦片经 provider 变成一张 RGBA 影像,格式与真影像无区别 ——
// 这正是它能冒充 imagery 走完整套地形合成管线的原因。
TEST(VectorImageryProviderTest, ProducesRgbaImageFromVectorTile) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    VectorImageryProvider provider(optionsFor("L"), fetch.fn());

    std::unique_ptr<DecodedImage> got;
    provider.requestTile(TileKey{"XYZ-WebMercator", 12, 1, 1},
                         CancellationToken(),
                         [&](const TileKey&, std::unique_ptr<DecodedImage> img) {
                             got = std::move(img);
                         });
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->width, 32);
    EXPECT_EQ(got->height, 32);
    EXPECT_EQ(got->channels, 4);
    EXPECT_EQ(got->bytesPerChannel, 1);
    EXPECT_EQ(alphaAt(*got, 8, 16), 255) << "左半应被填充";
    EXPECT_EQ(alphaAt(*got, 24, 16), 0) << "右半应透明";
}

// **空瓦片必须产出全透明图而不是 nullptr**。nullptr 会被上层当成「加载失败」
// 去重试/回退祖先,而「这里确实没有路」是有效结果 —— 回退祖先会画出上一档
// 的粗路网,表现为「缩放到细档反而多出不该有的路」。
TEST(VectorImageryProviderTest, EmptyTileYieldsTransparentImageNotNull) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("other");  // 样式里那层不存在
    VectorImageryProvider provider(optionsFor("L"), fetch.fn());

    std::unique_ptr<DecodedImage> got;
    bool called = false;
    provider.requestTile(TileKey{"XYZ-WebMercator", 12, 1, 1},
                         CancellationToken(),
                         [&](const TileKey&, std::unique_ptr<DecodedImage> img) {
                             called = true;
                             got = std::move(img);
                         });
    ASSERT_TRUE(called);
    ASSERT_NE(got, nullptr) << "空结果 ≠ 失败";
    EXPECT_EQ(alphaAt(*got, 16, 16), 0);
}

// **取消后仍要回调**。上层按「回调必到」管理在途计数,静默丢弃会让瓦片
// 永远停在 pending。
TEST(VectorImageryProviderTest, CancelledRequestStillInvokesCallback) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    VectorImageryProvider provider(optionsFor("L"), fetch.fn());

    CancellationToken token;
    token.cancel();
    bool called = false;
    std::unique_ptr<DecodedImage> got;
    provider.requestTile(TileKey{"XYZ-WebMercator", 12, 1, 1}, token,
                         [&](const TileKey&, std::unique_ptr<DecodedImage> img) {
                             called = true;
                             got = std::move(img);
                         });
    EXPECT_TRUE(called) << "取消也必须回调";
    EXPECT_EQ(got, nullptr);
}

TEST(VectorImageryProviderTest, FetchFailureYieldsNullImage) {
    FakeFetch fetch;
    fetch.statusCode = 404;
    VectorImageryProvider provider(optionsFor("L"), fetch.fn());

    bool called = false;
    std::unique_ptr<DecodedImage> got;
    provider.requestTile(TileKey{"XYZ-WebMercator", 12, 1, 1},
                         CancellationToken(),
                         [&](const TileKey&, std::unique_ptr<DecodedImage> img) {
                             called = true;
                             got = std::move(img);
                         });
    EXPECT_TRUE(called);
    EXPECT_EQ(got, nullptr) << "拉取失败与「空瓦片」必须可区分";
}

// 瓦片 z 要真的传进样式求值(否则 E2 那套 zoom 分级在影像通道上失效)。
TEST(VectorImageryProviderTest, TileZoomReachesStyleEvaluation) {
    FakeFetch fetch;
    fetch.body = makeHalfTilePolygon("L");
    VectorImageryProvider::Options opt = optionsFor("L");
    opt.style.layers[0].minZoom = 10;
    opt.style.layers[0].maxZoom = 14;
    VectorImageryProvider provider(opt, fetch.fn());

    auto alphaForZoom = [&](int z) {
        std::unique_ptr<DecodedImage> got;
        provider.requestTile(TileKey{"XYZ-WebMercator", z, 1, 1},
                             CancellationToken(),
                             [&](const TileKey&,
                                 std::unique_ptr<DecodedImage> img) {
                                 got = std::move(img);
                             });
        return got ? alphaAt(*got, 8, 16) : -1;
    };
    EXPECT_EQ(alphaForZoom(12), 255) << "z12 在层区间内";
    EXPECT_EQ(alphaForZoom(5), 0) << "z5 在区间外 → 整层跳过,出空图";
}
