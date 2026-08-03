#include "earth_engine/data/MvtDecoder.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstring>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

// ---------------------------------------------------------------------------
// 测试侧最小 protobuf 编码器:独立实现,与被测解码器无共享代码,
// 构成编码/解码双向对拍。几何示例取自 MVT spec 2.1 §4.3.5 官方样例。
// ---------------------------------------------------------------------------

struct PbfWriter {
    std::vector<uint8_t> bytes;

    void varint(uint64_t v) {
        while (v >= 0x80) {
            bytes.push_back(static_cast<uint8_t>(v) | 0x80);
            v >>= 7;
        }
        bytes.push_back(static_cast<uint8_t>(v));
    }

    void tag(uint32_t field, uint32_t wire) { varint((field << 3) | wire); }

    void varintField(uint32_t field, uint64_t v) {
        tag(field, 0);
        varint(v);
    }

    void stringField(uint32_t field, const std::string& s) {
        tag(field, 2);
        varint(s.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
    }

    void bytesField(uint32_t field, const std::vector<uint8_t>& sub) {
        tag(field, 2);
        varint(sub.size());
        bytes.insert(bytes.end(), sub.begin(), sub.end());
    }

    void packedUint32Field(uint32_t field, const std::vector<uint32_t>& vs) {
        PbfWriter payload;
        for (uint32_t v : vs) {
            payload.varint(v);
        }
        bytesField(field, payload.bytes);
    }

    void fixed32Field(uint32_t field, uint32_t v) {
        tag(field, 5);
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
        }
    }

    void fixed64Field(uint32_t field, uint64_t v) {
        tag(field, 1);
        for (int i = 0; i < 8; ++i) {
            bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
        }
    }
};

uint32_t zigzag(int32_t v) {
    return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}

/// 把路径编码成 MVT 几何命令流(delta+zigzag,游标跨路径持续)。
std::vector<uint32_t> encodeGeometry(
    const std::vector<std::vector<MvtPoint>>& paths, MvtGeomType type) {
    std::vector<uint32_t> geom;
    int32_t x = 0;
    int32_t y = 0;
    for (const auto& path : paths) {
        if (type == MvtGeomType::Point) {
            geom.push_back(1u | (static_cast<uint32_t>(path.size()) << 3));
            for (const MvtPoint& p : path) {
                geom.push_back(zigzag(p.x - x));
                geom.push_back(zigzag(p.y - y));
                x = p.x;
                y = p.y;
            }
        } else {
            geom.push_back(1u | (1u << 3));  // MoveTo count=1
            geom.push_back(zigzag(path[0].x - x));
            geom.push_back(zigzag(path[0].y - y));
            x = path[0].x;
            y = path[0].y;
            geom.push_back(2u | (static_cast<uint32_t>(path.size() - 1) << 3));
            for (size_t i = 1; i < path.size(); ++i) {
                geom.push_back(zigzag(path[i].x - x));
                geom.push_back(zigzag(path[i].y - y));
                x = path[i].x;
                y = path[i].y;
            }
            if (type == MvtGeomType::Polygon) {
                geom.push_back(7u | (1u << 3));  // ClosePath
            }
        }
    }
    return geom;
}

std::vector<uint8_t> makeFeature(uint64_t id, MvtGeomType type,
                                 const std::vector<uint32_t>& geometry,
                                 const std::vector<uint32_t>& tags = {}) {
    PbfWriter f;
    if (id != 0) {
        f.varintField(1, id);
    }
    if (!tags.empty()) {
        f.packedUint32Field(2, tags);
    }
    f.varintField(3, static_cast<uint64_t>(type));
    f.packedUint32Field(4, geometry);
    return f.bytes;
}

struct LayerSpec {
    std::string name = "layer";
    std::vector<std::vector<uint8_t>> features;
    std::vector<std::string> keys;
    std::vector<std::vector<uint8_t>> values;  // 每个是编好的 Value 子消息
    uint32_t extent = 4096;
};

std::vector<uint8_t> makeLayer(const LayerSpec& spec) {
    PbfWriter l;
    l.varintField(15, 2);  // version
    l.stringField(1, spec.name);
    for (const auto& f : spec.features) {
        l.bytesField(2, f);
    }
    for (const auto& k : spec.keys) {
        l.stringField(3, k);
    }
    for (const auto& v : spec.values) {
        l.bytesField(4, v);
    }
    if (spec.extent != 4096) {
        l.varintField(5, spec.extent);
    }
    return l.bytes;
}

std::vector<uint8_t> makeTile(const std::vector<std::vector<uint8_t>>& layers) {
    PbfWriter t;
    for (const auto& l : layers) {
        t.bytesField(3, l);
    }
    return t.bytes;
}

std::vector<uint8_t> stringValue(const std::string& s) {
    PbfWriter v;
    v.stringField(1, s);
    return v.bytes;
}

// ---------------------------------------------------------------------------
// 几何解码:MVT spec 2.1 §4.3.5 官方样例对拍
// ---------------------------------------------------------------------------

TEST(MvtDecoder, SpecExamplePoint) {
    // spec §4.3.5.1:Point(25,17) → [9, 50, 34]
    LayerSpec spec;
    spec.features.push_back(makeFeature(1, MvtGeomType::Point, {9, 50, 34}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    ASSERT_EQ(tile.layers.size(), 1u);
    ASSERT_EQ(tile.layers[0].features.size(), 1u);
    const MvtFeature& f = tile.layers[0].features[0];
    EXPECT_EQ(f.type, MvtGeomType::Point);
    ASSERT_EQ(f.paths.size(), 1u);
    ASSERT_EQ(f.paths[0].size(), 1u);
    EXPECT_EQ(f.paths[0][0], (MvtPoint{25, 17}));
}

TEST(MvtDecoder, SpecExampleMultiPoint) {
    // spec §4.3.5.2:MultiPoint(5,7),(3,2) → [17, 10, 14, 3, 9]
    LayerSpec spec;
    spec.features.push_back(makeFeature(1, MvtGeomType::Point, {17, 10, 14, 3, 9}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    const MvtFeature& f = tile.layers[0].features[0];
    ASSERT_EQ(f.paths.size(), 1u);  // 点要素全部点合并进一个 path
    ASSERT_EQ(f.paths[0].size(), 2u);
    EXPECT_EQ(f.paths[0][0], (MvtPoint{5, 7}));
    EXPECT_EQ(f.paths[0][1], (MvtPoint{3, 2}));
}

TEST(MvtDecoder, SpecExampleLineString) {
    // spec §4.3.5.3:(2,2)→(2,10)→(10,10) → [9,4,4,18,0,16,16,0]
    LayerSpec spec;
    spec.features.push_back(
        makeFeature(1, MvtGeomType::LineString, {9, 4, 4, 18, 0, 16, 16, 0}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    const MvtFeature& f = tile.layers[0].features[0];
    ASSERT_EQ(f.paths.size(), 1u);
    ASSERT_EQ(f.paths[0].size(), 3u);
    EXPECT_EQ(f.paths[0][0], (MvtPoint{2, 2}));
    EXPECT_EQ(f.paths[0][1], (MvtPoint{2, 10}));
    EXPECT_EQ(f.paths[0][2], (MvtPoint{10, 10}));
}

TEST(MvtDecoder, SpecExampleMultiLineString) {
    // spec §4.3.5.4:线1 (2,2)(2,10)(10,10);线2 (1,1)(3,5)
    // → [9,4,4,18,0,16,16,0, 9,17,17, 10,4,8](游标跨线持续累积)
    LayerSpec spec;
    spec.features.push_back(makeFeature(
        1, MvtGeomType::LineString,
        {9, 4, 4, 18, 0, 16, 16, 0, 9, 17, 17, 10, 4, 8}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    const MvtFeature& f = tile.layers[0].features[0];
    ASSERT_EQ(f.paths.size(), 2u);
    ASSERT_EQ(f.paths[0].size(), 3u);
    EXPECT_EQ(f.paths[0][2], (MvtPoint{10, 10}));
    ASSERT_EQ(f.paths[1].size(), 2u);
    EXPECT_EQ(f.paths[1][0], (MvtPoint{1, 1}));
    EXPECT_EQ(f.paths[1][1], (MvtPoint{3, 5}));
}

TEST(MvtDecoder, SpecExamplePolygon) {
    // spec §4.3.5.5:环 (3,6)(8,12)(20,34) → [9,6,12,18,10,12,24,44,15]
    // ClosePath 不重复首点
    LayerSpec spec;
    spec.features.push_back(makeFeature(
        1, MvtGeomType::Polygon, {9, 6, 12, 18, 10, 12, 24, 44, 15}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    const MvtFeature& f = tile.layers[0].features[0];
    ASSERT_EQ(f.paths.size(), 1u);
    ASSERT_EQ(f.paths[0].size(), 3u);
    EXPECT_EQ(f.paths[0][0], (MvtPoint{3, 6}));
    EXPECT_EQ(f.paths[0][1], (MvtPoint{8, 12}));
    EXPECT_EQ(f.paths[0][2], (MvtPoint{20, 34}));
}

// ---------------------------------------------------------------------------
// 编码/解码往返(测试编码器独立实现)
// ---------------------------------------------------------------------------

TEST(MvtDecoder, RoundTripPolygonWithHoleAndSecondPolygon) {
    std::vector<std::vector<MvtPoint>> rings = {
        {{0, 0}, {10, 0}, {10, 10}, {0, 10}},      // 外环 A
        {{2, 2}, {2, 8}, {8, 8}, {8, 2}},          // 孔(反向)
        {{20, 20}, {30, 20}, {30, 30}, {20, 30}},  // 外环 B(与 A 同向)
    };
    LayerSpec spec;
    spec.features.push_back(makeFeature(
        7, MvtGeomType::Polygon, encodeGeometry(rings, MvtGeomType::Polygon)));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    const MvtFeature& f = tile.layers[0].features[0];
    EXPECT_EQ(f.id, 7u);
    ASSERT_EQ(f.paths.size(), 3u);
    EXPECT_EQ(f.paths[0], rings[0]);
    EXPECT_EQ(f.paths[1], rings[1]);
    EXPECT_EQ(f.paths[2], rings[2]);

    auto polygons = classifyMvtRings(f.paths);
    ASSERT_EQ(polygons.size(), 2u);
    EXPECT_EQ(polygons[0].exterior, rings[0]);
    ASSERT_EQ(polygons[0].holes.size(), 1u);
    EXPECT_EQ(polygons[0].holes[0], rings[1]);
    EXPECT_TRUE(polygons[1].holes.empty());
    EXPECT_EQ(polygons[1].exterior, rings[2]);
}

TEST(MvtDecoder, RoundTripLargeCoordinatesAndNegativeDeltas) {
    // extent 4096 + buffer 越界坐标、负坐标(buffer 区),大 delta
    std::vector<std::vector<MvtPoint>> lines = {
        {{-128, -128}, {4223, -128}, {4223, 4223}, {-128, 4223}},
        {{2048, 2048}, {0, 4096}},
    };
    LayerSpec spec;
    spec.features.push_back(makeFeature(
        1, MvtGeomType::LineString,
        encodeGeometry(lines, MvtGeomType::LineString)));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    const MvtFeature& f = tile.layers[0].features[0];
    ASSERT_EQ(f.paths.size(), 2u);
    EXPECT_EQ(f.paths[0], lines[0]);
    EXPECT_EQ(f.paths[1], lines[1]);
}

// ---------------------------------------------------------------------------
// 属性表:Value 全类型字符串化 + tags 索引
// ---------------------------------------------------------------------------

TEST(MvtDecoder, PropertiesAllValueTypes) {
    LayerSpec spec;
    spec.keys = {"name", "height", "count", "visible", "offset", "ratio", "big"};
    PbfWriter vFloat;
    vFloat.fixed32Field(2, [] {
        float f = 1.5f;
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return bits;
    }());
    PbfWriter vDouble;
    vDouble.fixed64Field(3, [] {
        double d = -12.25;
        uint64_t bits;
        std::memcpy(&bits, &d, 8);
        return bits;
    }());
    PbfWriter vInt;
    vInt.varintField(4, static_cast<uint64_t>(int64_t{42}));
    PbfWriter vBool;
    vBool.varintField(7, 1);
    PbfWriter vSint;  // sint64 zigzag:-7 → 13
    vSint.varintField(6, 13);
    PbfWriter vUint;
    vUint.varintField(5, 9007199254740993ull);
    spec.values = {stringValue("塔楼"), vFloat.bytes, vInt.bytes,
                   vBool.bytes,        vSint.bytes,  vDouble.bytes,
                   vUint.bytes};
    spec.features.push_back(makeFeature(
        3, MvtGeomType::Point, {9, 0, 0},
        {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    const auto& props = tile.layers[0].features[0].properties;
    EXPECT_EQ(props.at("name"), "塔楼");
    EXPECT_EQ(props.at("height"), "1.5");
    EXPECT_EQ(props.at("count"), "42");
    EXPECT_EQ(props.at("visible"), "true");
    EXPECT_EQ(props.at("offset"), "-7");
    EXPECT_EQ(props.at("ratio"), "-12.25");
    EXPECT_EQ(props.at("big"), "9007199254740993");
}

TEST(MvtDecoder, TagsOutOfRangeSkippedOddTailIgnored) {
    LayerSpec spec;
    spec.keys = {"k"};
    spec.values = {stringValue("v")};
    // (0,0) 合法;(9,0) key 越界跳过;尾部落单的 0 忽略
    spec.features.push_back(
        makeFeature(1, MvtGeomType::Point, {9, 0, 0}, {0, 0, 9, 0, 0}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    const auto& props = tile.layers[0].features[0].properties;
    EXPECT_EQ(props.size(), 1u);
    EXPECT_EQ(props.at("k"), "v");
}

// ---------------------------------------------------------------------------
// Layer 元数据与字段顺序
// ---------------------------------------------------------------------------

TEST(MvtDecoder, LayerMetadataAndCustomExtent) {
    LayerSpec spec;
    spec.name = "roads";
    spec.extent = 512;
    spec.features.push_back(makeFeature(1, MvtGeomType::Point, {9, 0, 0}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    EXPECT_EQ(tile.layers[0].name, "roads");
    EXPECT_EQ(tile.layers[0].extent, 512u);
    EXPECT_EQ(tile.layers[0].version, 2u);
}

TEST(MvtDecoder, FeaturesBeforeKeysValuesInStream) {
    // makeLayer 天然把 features(field 2)写在 keys/values(field 3/4)之前,
    // 验证两遍解析:feature 的 tags 索引在 keys/values 之后仍解析正确。
    LayerSpec spec;
    spec.keys = {"kind"};
    spec.values = {stringValue("river")};
    spec.features.push_back(
        makeFeature(1, MvtGeomType::LineString, {9, 0, 0, 10, 2, 2}, {0, 0}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    EXPECT_EQ(tile.layers[0].features[0].properties.at("kind"), "river");
}

TEST(MvtDecoder, MultipleLayersAndUnknownFieldsSkipped) {
    LayerSpec a;
    a.name = "a";
    a.features.push_back(makeFeature(1, MvtGeomType::Point, {9, 0, 0}));
    LayerSpec b;
    b.name = "b";
    b.features.push_back(makeFeature(2, MvtGeomType::Point, {9, 2, 2}));

    // 瓦片层塞未知字段(field 9 varint + field 10 length-delimited)
    PbfWriter t;
    t.varintField(9, 12345);
    t.bytesField(3, makeLayer(a));
    t.stringField(10, "future-extension");
    t.bytesField(3, makeLayer(b));

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(t.bytes.data(), t.bytes.size(), tile));
    ASSERT_EQ(tile.layers.size(), 2u);
    EXPECT_EQ(tile.layers[0].name, "a");
    EXPECT_EQ(tile.layers[1].name, "b");
}

TEST(MvtDecoder, NonPackedGeometryAccepted) {
    // spec 用 packed,但 protobuf 兼容解析器必须也收非 packed 形式
    PbfWriter f;
    f.varintField(3, static_cast<uint64_t>(MvtGeomType::Point));
    for (uint32_t v : {9u, 50u, 34u}) {
        f.varintField(4, v);
    }
    LayerSpec spec;
    spec.features.push_back(f.bytes);
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    ASSERT_EQ(tile.layers[0].features.size(), 1u);
    EXPECT_EQ(tile.layers[0].features[0].paths[0][0], (MvtPoint{25, 17}));
}

// ---------------------------------------------------------------------------
// 容错与失败路径
// ---------------------------------------------------------------------------

TEST(MvtDecoder, MalformedGeometryDropsFeatureNotTile) {
    LayerSpec spec;
    // 几何命令流残缺(MoveTo 声明 1 点但只有 x)→ 只丢该 feature
    spec.features.push_back(makeFeature(1, MvtGeomType::Point, {9, 50}));
    spec.features.push_back(makeFeature(2, MvtGeomType::Point, {9, 0, 0}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    ASSERT_EQ(tile.layers[0].features.size(), 1u);
    EXPECT_EQ(tile.layers[0].features[0].id, 2u);
}

TEST(MvtDecoder, ClosePathOnLineStringDropsFeature) {
    LayerSpec spec;
    spec.features.push_back(
        makeFeature(1, MvtGeomType::LineString, {9, 0, 0, 10, 2, 2, 15}));
    auto tileBytes = makeTile({makeLayer(spec)});

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile));
    EXPECT_TRUE(tile.layers[0].features.empty());
}

TEST(MvtDecoder, TruncatedTileFails) {
    LayerSpec spec;
    spec.features.push_back(makeFeature(1, MvtGeomType::Point, {9, 50, 34}));
    auto tileBytes = makeTile({makeLayer(spec)});
    tileBytes.resize(tileBytes.size() / 2);

    MvtTile tile;
    std::string error;
    EXPECT_FALSE(decodeMvtTile(tileBytes.data(), tileBytes.size(), tile, &error));
    EXPECT_FALSE(error.empty());
}

TEST(MvtDecoder, EmptyInputFails) {
    MvtTile tile;
    EXPECT_FALSE(decodeMvtTile(nullptr, 0, tile));
    uint8_t byte = 0;
    EXPECT_FALSE(decodeMvtTile(&byte, 0, tile));
}

// ---------------------------------------------------------------------------
// gzip / zlib 压缩瓦片
// ---------------------------------------------------------------------------

std::vector<uint8_t> gzipCompress(const std::vector<uint8_t>& input) {
    z_stream strm{};
    // 16+MAX_WBITS = gzip 头
    EXPECT_EQ(deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY),
              Z_OK);
    std::vector<uint8_t> out(deflateBound(&strm, input.size()));
    strm.next_in = const_cast<Bytef*>(input.data());
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());
    EXPECT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_END);
    out.resize(out.size() - strm.avail_out);
    deflateEnd(&strm);
    return out;
}

TEST(MvtDecoder, GzipCompressedTileTransparentlyInflated) {
    LayerSpec spec;
    spec.name = "compressed";
    spec.features.push_back(makeFeature(1, MvtGeomType::Point, {9, 50, 34}));
    auto raw = makeTile({makeLayer(spec)});
    auto gz = gzipCompress(raw);
    ASSERT_EQ(gz[0], 0x1F);
    ASSERT_EQ(gz[1], 0x8B);

    MvtTile tile;
    ASSERT_TRUE(decodeMvtTile(gz.data(), gz.size(), tile));
    ASSERT_EQ(tile.layers.size(), 1u);
    EXPECT_EQ(tile.layers[0].name, "compressed");
    EXPECT_EQ(tile.layers[0].features[0].paths[0][0], (MvtPoint{25, 17}));
}

TEST(MvtDecoder, CorruptGzipFails) {
    LayerSpec spec;
    spec.features.push_back(makeFeature(1, MvtGeomType::Point, {9, 50, 34}));
    auto gz = gzipCompress(makeTile({makeLayer(spec)}));
    for (size_t i = 10; i < gz.size(); ++i) {
        gz[i] ^= 0xFF;  // 毁掉 deflate 流,保留 gzip 头
    }
    MvtTile tile;
    std::string error;
    EXPECT_FALSE(decodeMvtTile(gz.data(), gz.size(), tile, &error));
}

// ---------------------------------------------------------------------------
// classifyMvtRings 语义(对拍 @mapbox/vector-tile classifyRings)
// ---------------------------------------------------------------------------

TEST(MvtClassifyRings, WindingAgnosticBothOrientations) {
    // 同一组环,整体镜像绕向后分类结果不变(首环定义外环方向)
    std::vector<MvtPoint> outer = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    std::vector<MvtPoint> hole = {{2, 2}, {2, 8}, {8, 8}, {8, 2}};
    auto polys = classifyMvtRings({outer, hole});
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0].holes.size(), 1u);

    std::vector<MvtPoint> outerRev(outer.rbegin(), outer.rend());
    std::vector<MvtPoint> holeRev(hole.rbegin(), hole.rend());
    auto polysRev = classifyMvtRings({outerRev, holeRev});
    ASSERT_EQ(polysRev.size(), 1u);
    EXPECT_EQ(polysRev[0].holes.size(), 1u);
}

TEST(MvtClassifyRings, ZeroAreaAndDegenerateRingsSkipped) {
    std::vector<MvtPoint> outer = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    std::vector<MvtPoint> zeroArea = {{0, 0}, {5, 5}, {0, 0}};       // 面积 0
    std::vector<MvtPoint> tooShort = {{1, 1}, {2, 2}};               // <3 点
    auto polys = classifyMvtRings({zeroArea, tooShort, outer});
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0].exterior, outer);
    EXPECT_TRUE(polys[0].holes.empty());
}

TEST(MvtClassifyRings, SignedAreaSignConsistency) {
    // 同一环镜像绕向后有向面积反号,量值相同
    std::vector<MvtPoint> ring = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    std::vector<MvtPoint> reversed(ring.rbegin(), ring.rend());
    int64_t a = mvtRingSignedArea2(ring);
    int64_t b = mvtRingSignedArea2(reversed);
    EXPECT_EQ(a, -b);
    EXPECT_EQ(std::abs(a), 32);  // 4×4 方形,面积 16,×2 = 32
}

} // namespace
