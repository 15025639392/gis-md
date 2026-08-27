#include <gtest/gtest.h>
#include <zlib.h>

#include "earth_engine/data/AmapVectorTile.h"
#include "earth_engine/data/AmapGeometry.h"

#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace earth_engine;

namespace {

std::vector<uint8_t> gzipCompress(const std::vector<uint8_t>& input) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::vector<uint8_t> out(deflateBound(&strm, input.size()));
    strm.next_in = const_cast<uint8_t*>(input.data());
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());
    deflate(&strm, Z_FINISH);
    out.resize(strm.total_out);
    deflateEnd(&strm);
    return out;
}

void putVarint(std::vector<uint8_t>& b, uint64_t v) {
    while (v >= 0x80) {
        b.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    b.push_back(static_cast<uint8_t>(v));
}

void putTag(std::vector<uint8_t>& b, int field, int wire) {
    putVarint(b, (static_cast<uint64_t>(field) << 3) | wire);
}

void putVarintField(std::vector<uint8_t>& b, int field, uint64_t v) {
    putTag(b, field, 0);
    putVarint(b, v);
}

void putBytesField(std::vector<uint8_t>& b, int field,
                   const std::vector<uint8_t>& v) {
    putTag(b, field, 2);
    putVarint(b, v.size());
    b.insert(b.end(), v.begin(), v.end());
}

std::vector<uint8_t> makeContainer(const std::vector<uint8_t>& protobuf) {
    const auto gz = gzipCompress(protobuf);
    std::vector<uint8_t> out;
    const uint32_t n = static_cast<uint32_t>(gz.size());
    out.push_back(static_cast<uint8_t>(n >> 24));
    out.push_back(static_cast<uint8_t>(n >> 16));
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), gz.begin(), gz.end());
    return out;
}

// zigzag(v) = (v<<1) ^ (v>>63 语义:非负 → 2v)。
void putZigzag(std::vector<uint8_t>& b, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    const uint64_t z = (u << 1) ^ (0 - (u >> 63));
    putVarint(b, z);
}

// 类型 3(建筑)层:一个 ClassGroup(classCode=20009, geomType=13),
// 一个 Feature → Part { blob=三角形 zigzag 几何, height=36 }。
std::vector<uint8_t> makeBuildingTile() {
    std::vector<uint8_t> part;
    {
        std::vector<uint8_t> blob;
        const int64_t pts[6] = {0, 0, 10, 0, 0, 10};  // (0,0)->(10,0)->(10,10)
        for (int64_t v : pts) putZigzag(blob, v);
        putBytesField(part, 3, blob);
        putVarintField(part, 5, 36);
    }
    std::vector<uint8_t> feature;
    putBytesField(feature, 4, part);
    std::vector<uint8_t> cg;
    putVarintField(cg, 1, 20009);
    putVarintField(cg, 3, 13);
    putBytesField(cg, 4, feature);
    std::vector<uint8_t> content;
    putBytesField(content, 1, cg);
    std::vector<uint8_t> layer;
    putVarintField(layer, 1, 14);
    putVarintField(layer, 2, 13038);
    putVarintField(layer, 3, 6785);
    putVarintField(layer, 4, 3);
    putBytesField(layer, 5, content);
    putVarintField(layer, 8, 4);
    std::vector<uint8_t> tile;
    putVarintField(tile, 1, 14);
    putVarintField(tile, 2, 13038);
    putVarintField(tile, 3, 6785);
    putBytesField(tile, 4, layer);
    std::vector<uint8_t> root;
    putBytesField(root, 1, tile);
    const std::vector<uint8_t> ver{'v', '2'};
    putBytesField(root, 2, ver);
    return makeContainer(root);
}

std::vector<uint8_t> makeBlobLayer(int layerType,
                                   const std::vector<uint8_t>& cg,
                                   int z = 14, int x = 13038,
                                   int y = 6785) {
    std::vector<uint8_t> content;
    putBytesField(content, 1, cg);
    std::vector<uint8_t> layer;
    putVarintField(layer, 1, z);
    putVarintField(layer, 2, x);
    putVarintField(layer, 3, y);
    putVarintField(layer, 4, layerType);
    putBytesField(layer, 5, content);
    putVarintField(layer, 8, 4);
    std::vector<uint8_t> tile;
    putVarintField(tile, 1, z);
    putVarintField(tile, 2, x);
    putVarintField(tile, 3, y);
    putBytesField(tile, 4, layer);
    std::vector<uint8_t> root;
    putBytesField(root, 1, tile);
    const std::vector<uint8_t> ver{'v', '2'};
    putBytesField(root, 2, ver);
    return makeContainer(root);
}

std::vector<uint8_t> makeLineTile() {
    std::vector<uint8_t> part;
    {
        std::vector<uint8_t> blob;
        const int64_t pts[6] = {0, 0, 10, 0, 0, 10};
        for (int64_t v : pts) putZigzag(blob, v);
        putBytesField(part, 5, blob);  // 实测:type1 线 Part{blob #5}
    }
    std::vector<uint8_t> feature;
    putBytesField(feature, 4, part);
    std::vector<uint8_t> cg;
    putVarintField(cg, 1, 20009);
    putVarintField(cg, 3, 13);
    putBytesField(cg, 4, feature);
    return makeBlobLayer(1, cg);
}

std::vector<uint8_t> makeRegionTile(int classCode = 0, int z = 14,
                                    int x = 13038, int y = 6785) {
    std::vector<uint8_t> feature;
    {
        std::vector<uint8_t> rings;
        // Feature#6 的真实结构是 repeated #1 = length-delimited ring
        // blobs。每个 blob 的首点是 zigzag absolute，后续是 delta。
        std::vector<uint8_t> first;
        const int64_t firstPts[8] = {7, 8, 10, 0, 0, 10, -10, -10};
        for (int64_t v : firstPts) putZigzag(first, v);
        putBytesField(rings, 1, first);

        std::vector<uint8_t> second;
        const int64_t secondPts[6] = {30, 40, 5, 0, 0, 5};
        for (int64_t v : secondPts) putZigzag(second, v);
        putBytesField(rings, 1, second);

        putBytesField(feature, 6, rings);  // type2 Feature#6 nested message
    }
    std::vector<uint8_t> cg;
    if (classCode != 0) putVarintField(cg, 1, classCode);
    putBytesField(cg, 4, feature);
    return makeBlobLayer(2, cg, z, x, y);
}

std::vector<uint8_t> makeRegionWithBoundaryTile(int boundaryClass = 20014) {
    std::vector<uint8_t> regionFeature;
    {
        std::vector<uint8_t> rings;
        std::vector<uint8_t> blob;
        const int64_t pts[6] = {10, 10, 20, 0, 0, 20};
        for (int64_t v : pts) putZigzag(blob, v);
        putBytesField(rings, 1, blob);
        putBytesField(regionFeature, 6, rings);
    }
    std::vector<uint8_t> regionGroup;
    putVarintField(regionGroup, 1, 30001);
    putBytesField(regionGroup, 4, regionFeature);

    std::vector<uint8_t> boundaryPart;
    {
        std::vector<uint8_t> blob;
        const int64_t pts[6] = {100, 100, 30, 0, 0, 40};
        for (int64_t v : pts) putZigzag(blob, v);
        putBytesField(boundaryPart, 3, blob);
    }
    std::vector<uint8_t> boundaryFeature;
    putBytesField(boundaryFeature, 4, boundaryPart);
    std::vector<uint8_t> boundaryGroup;
    if (boundaryClass != 0) putVarintField(boundaryGroup, 1, boundaryClass);
    putBytesField(boundaryGroup, 4, boundaryFeature);

    std::vector<uint8_t> content;
    putBytesField(content, 1, regionGroup);
    putBytesField(content, 2, boundaryGroup);
    std::vector<uint8_t> layer;
    putVarintField(layer, 1, 14);
    putVarintField(layer, 2, 13038);
    putVarintField(layer, 3, 6785);
    putVarintField(layer, 4, 2);
    putBytesField(layer, 5, content);
    std::vector<uint8_t> tile;
    putBytesField(tile, 4, layer);
    std::vector<uint8_t> root;
    putBytesField(root, 1, tile);
    return makeContainer(root);
}

std::vector<uint8_t> makeDetailLineTile() {
    std::vector<uint8_t> regionFeature;
    {
        std::vector<uint8_t> rings;
        std::vector<uint8_t> blob;
        const int64_t pts[6] = {10, 10, 20, 0, 0, 20};
        for (int64_t v : pts) putZigzag(blob, v);
        putBytesField(rings, 1, blob);
        putBytesField(regionFeature, 6, rings);
    }
    std::vector<uint8_t> regionGroup;
    putVarintField(regionGroup, 1, 30001);
    putBytesField(regionGroup, 4, regionFeature);

    // Exercise the full native 16384x8192 detail grid.  A generic z14 line
    // scale would put the last point four times too far from this tile.
    std::vector<uint8_t> detailBlob;
    const int64_t detailPts[10] = {0, 0, 16384, 0, -16384, 8192, 0, -8192,
                                   16384, 8192};
    for (int64_t v : detailPts) putZigzag(detailBlob, v);
    std::vector<uint8_t> detailPart;
    putBytesField(detailPart, 3, detailBlob);
    std::vector<uint8_t> detailFeature;
    putVarintField(detailFeature, 1, 16);
    putVarintField(detailFeature, 3, 51);
    putBytesField(detailFeature, 4, detailPart);
    std::vector<uint8_t> detailGroup;
    putVarintField(detailGroup, 1, 20017);
    putVarintField(detailGroup, 3, 15);
    putBytesField(detailGroup, 4, detailFeature);

    // Keep a regular boundary line in the same container as a regression
    // guard: only 20017 gets the dedicated detail-grid scale.
    std::vector<uint8_t> ordinaryBlob;
    const int64_t ordinaryPts[6] = {0, 0, 4096, 0, -4096, 2048};
    for (int64_t v : ordinaryPts) putZigzag(ordinaryBlob, v);
    std::vector<uint8_t> ordinaryPart;
    putBytesField(ordinaryPart, 3, ordinaryBlob);
    std::vector<uint8_t> ordinaryFeature;
    putBytesField(ordinaryFeature, 4, ordinaryPart);
    std::vector<uint8_t> ordinaryGroup;
    putVarintField(ordinaryGroup, 1, 20014);
    putVarintField(ordinaryGroup, 3, 15);
    putBytesField(ordinaryGroup, 4, ordinaryFeature);

    std::vector<uint8_t> content;
    putBytesField(content, 1, regionGroup);
    putBytesField(content, 2, detailGroup);
    putBytesField(content, 2, ordinaryGroup);
    std::vector<uint8_t> layer;
    putVarintField(layer, 1, 14);
    putVarintField(layer, 2, 13038);
    putVarintField(layer, 3, 5503);
    putVarintField(layer, 4, 2);
    putBytesField(layer, 5, content);
    std::vector<uint8_t> tile;
    putBytesField(tile, 4, layer);
    std::vector<uint8_t> root;
    putBytesField(root, 1, tile);
    return makeContainer(root);
}

// type4 同层混合两套 schema：content.#1 是轨道线，content.#3 是
// class 30003/kind 64 的区域面。两者共用 line-grid scale，但几何语义
// 不能由 enclosing layer type 一刀切。
std::vector<uint8_t> makeTransitWithRegionTile() {
    std::vector<uint8_t> transitBlob;
    const int64_t transitPts[6] = {100, 200, 20, 0, 0, 30};
    for (int64_t v : transitPts) putZigzag(transitBlob, v);
    std::vector<uint8_t> transitPart;
    putBytesField(transitPart, 3, transitBlob);
    std::vector<uint8_t> transitFeature;
    putBytesField(transitFeature, 4, transitPart);
    std::vector<uint8_t> transitGroup;
    putVarintField(transitGroup, 1, 20015);
    putVarintField(transitGroup, 2, 7);   // transit line subKey
    putVarintField(transitGroup, 3, 13);  // resolution, not geomType
    putBytesField(transitGroup, 4, transitFeature);

    std::vector<uint8_t> regionBlob;
    const int64_t regionPts[8] = {
        100, 100, 100, 0, 0, 100, -100, 0};
    for (int64_t v : regionPts) putZigzag(regionBlob, v);
    std::vector<uint8_t> regionRings;
    putBytesField(regionRings, 1, regionBlob);
    std::vector<uint8_t> regionFeature;
    putVarintField(regionFeature, 1, 16);  // rank
    putVarintField(regionFeature, 4, 64);  // line-grid region kind
    putBytesField(regionFeature, 6, regionRings);
    std::vector<uint8_t> regionGroup;
    putVarintField(regionGroup, 1, 30003);
    putVarintField(regionGroup, 2, 10);  // region subKey
    putVarintField(regionGroup, 3, 13);  // resolution, not geomType
    putBytesField(regionGroup, 4, regionFeature);

    std::vector<uint8_t> content;
    putBytesField(content, 1, transitGroup);
    putBytesField(content, 3, regionGroup);
    std::vector<uint8_t> layer;
    putVarintField(layer, 1, 14);
    putVarintField(layer, 2, 13038);
    putVarintField(layer, 3, 5503);
    putVarintField(layer, 4, 4);
    putBytesField(layer, 5, content);
    std::vector<uint8_t> tile;
    putBytesField(tile, 4, layer);
    std::vector<uint8_t> root;
    putBytesField(root, 1, tile);
    return makeContainer(root);
}

// POI type0:Feature#4 label，其中 label#3 是字符串 id、label#4 是
// plain-unsigned 单点坐标。通用 decoder 会把 label 当 Part、把 id 当
// geometry blob；专项 decoder 必须替换掉那份伪 type0 结果。
std::vector<uint8_t> makePoiTile() {
    const std::vector<uint8_t> nameBytes = {'c', 'o', 'u', 'r', 't'};
    std::vector<uint8_t> nameBox;
    putBytesField(nameBox, 1, nameBytes);

    std::vector<uint8_t> coord;
    putVarint(coord, 1024);  // native 2048×1024 grid center
    putVarint(coord, 512);

    std::vector<uint8_t> label;
    putBytesField(label, 1, nameBox);
    const std::vector<uint8_t> idBytes = {'A', 'B', 'C', 'D', 'E', 'F'};
    putBytesField(label, 3, idBytes);
    putBytesField(label, 4, coord);

    std::vector<uint8_t> feature;
    putVarintField(feature, 1, 14);
    putVarintField(feature, 2, 30);
    putVarintField(feature, 3, 7);
    putBytesField(feature, 4, label);

    std::vector<uint8_t> group;
    putVarintField(group, 1, 12024);
    putVarintField(group, 2, 5);
    putBytesField(group, 4, feature);

    std::vector<uint8_t> content;
    putBytesField(content, 1, group);
    std::vector<uint8_t> layer;
    putVarintField(layer, 1, 14);
    putVarintField(layer, 2, 13039);
    putVarintField(layer, 3, 5497);
    putVarintField(layer, 4, 0);
    putBytesField(layer, 5, content);
    std::vector<uint8_t> tile;
    putBytesField(tile, 4, layer);
    std::vector<uint8_t> root;
    putBytesField(root, 1, tile);
    return makeContainer(root);
}

}  // namespace

TEST(AmapVectorTileTest, DecodesBuildingContainer) {
    const auto container = makeBuildingTile();
    std::vector<AmapDecodedLayerPart> parts;
    std::string err;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts, &err))
        << err;
    ASSERT_EQ(1u, parts.size());
    const AmapDecodedLayerPart& p = parts[0];
    EXPECT_EQ(14, p.z);
    EXPECT_EQ(13038, p.x);
    EXPECT_EQ(6785, p.y);
    EXPECT_EQ(3, p.type);
    ASSERT_EQ(1u, p.features.size());
    const AmapDecodedFeature& f = p.features[0];
    EXPECT_EQ(20009, f.classCode);
    EXPECT_EQ(13, f.geomType);
    EXPECT_DOUBLE_EQ(36.0, f.height);
    ASSERT_EQ(1u, f.rings.size());
    ASSERT_EQ(3u, f.rings[0].size());
    EXPECT_DOUBLE_EQ(0.0, f.rings[0][0].first);
    EXPECT_DOUBLE_EQ(0.0, f.rings[0][0].second);
    EXPECT_DOUBLE_EQ(10.0, f.rings[0][1].first);
    EXPECT_DOUBLE_EQ(0.0, f.rings[0][1].second);
    EXPECT_DOUBLE_EQ(10.0, f.rings[0][2].first);
    EXPECT_DOUBLE_EQ(10.0, f.rings[0][2].second);
}

TEST(AmapVectorTileTest, DecodesLineContainerWithBlobAtPart5) {
    const auto container = makeLineTile();
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    EXPECT_EQ(1, parts[0].type);
    ASSERT_EQ(1u, parts[0].features.size());
    const AmapDecodedFeature& f = parts[0].features[0];
    EXPECT_EQ(20009, f.classCode);
    ASSERT_EQ(1u, f.rings.size());
    ASSERT_EQ(3u, f.rings[0].size());
    EXPECT_DOUBLE_EQ(10.0, f.rings[0][1].first);
}

TEST(AmapVectorTileTest, DecodesRegionContainerWithFeature6Rings) {
    const auto container = makeRegionTile();
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    EXPECT_EQ(2, parts[0].type);
    ASSERT_EQ(1u, parts[0].features.size());
    const AmapDecodedFeature& f = parts[0].features[0];
    EXPECT_EQ(30001, f.classCode);  // 缺省类
    ASSERT_EQ(2u, f.rings.size());
    ASSERT_EQ(4u, f.rings[0].size());
    ASSERT_EQ(3u, f.rings[1].size());
    // #1 的 framing 不能作为坐标；首点必须保留为 geometry blob
    // 内的 absolute zigzag 坐标，且第二个 ring 必须从独立 cursor 开始。
    EXPECT_DOUBLE_EQ(7.0, f.rings[0][0].first);
    EXPECT_DOUBLE_EQ(8.0, f.rings[0][0].second);
    EXPECT_DOUBLE_EQ(7.0, f.rings[0][3].first);
    EXPECT_DOUBLE_EQ(30.0, f.rings[1][0].first);
    EXPECT_DOUBLE_EQ(40.0, f.rings[1][0].second);
}

TEST(AmapVectorTileTest, DecodesType2Content2BoundaryAsLine) {
    const auto container = makeRegionWithBoundaryTile();
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    ASSERT_EQ(2u, parts[0].features.size());

    const auto& region = parts[0].features[0];
    EXPECT_EQ(30001, region.classCode);
    EXPECT_FALSE(region.lineGeometry);

    const auto& boundary = parts[0].features[1];
    EXPECT_EQ(20014, boundary.classCode);
    EXPECT_TRUE(boundary.lineGeometry);
    EXPECT_EQ(2, boundary.geomType);
    ASSERT_EQ(1u, boundary.rings.size());
    ASSERT_EQ(3u, boundary.rings[0].size());

    const auto features = amapDecodedPartToFeatures(parts[0], false);
    ASSERT_EQ(2u, features.size());
    EXPECT_EQ(GeometryType::Polygon, features[0].type);
    EXPECT_EQ(GeometryType::LineString, features[1].type);
    EXPECT_EQ("20014", features[1].properties.at("amap_class"));
}

TEST(AmapVectorTileTest, DefaultsType2Content2BoundaryClassTo20016) {
    const auto container = makeRegionWithBoundaryTile(0);
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    ASSERT_EQ(2u, parts[0].features.size());
    const auto& boundary = parts[0].features[1];
    EXPECT_TRUE(boundary.lineGeometry);
    EXPECT_EQ(20016, boundary.classCode);
    EXPECT_EQ(2, boundary.geomType);
}

TEST(AmapVectorTileTest, Class20017UsesDedicatedDetailGridScale) {
    const auto container = makeDetailLineTile();
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    ASSERT_EQ(3u, parts[0].features.size());
    const auto& detail = parts[0].features[1];
    EXPECT_EQ(20017, detail.classCode);
    EXPECT_TRUE(detail.lineGeometry);
    EXPECT_DOUBLE_EQ(0.5, detail.coordScale);

    const auto& ordinary = parts[0].features[2];
    EXPECT_EQ(20014, ordinary.classCode);
    EXPECT_TRUE(ordinary.lineGeometry);
    EXPECT_DOUBLE_EQ(0.0, ordinary.coordScale);

    const auto features = amapDecodedPartToFeatures(parts[0], false);
    ASSERT_EQ(3u, features.size());
    ASSERT_EQ(GeometryType::LineString, features[1].type);
    ASSERT_EQ(1u, features[1].rings.size());
    ASSERT_EQ(5u, features[1].rings[0].size());
    // The native detail-grid corners map to canonical tile corners after
    // scale=.5 and the common bottom-up Y flip.
    const double expectedDetailLocal[5][2] = {
        {0.0, 4096.0}, {8192.0, 4096.0}, {0.0, 0.0},
        {0.0, 4096.0}, {8192.0, 0.0}};
    for (size_t i = 0; i < 5; ++i) {
        const Cartographic expected = amapTileLocalToLngLat(
            13038, 5503, 14, expectedDetailLocal[i][0],
            expectedDetailLocal[i][1]);
        EXPECT_NEAR(expected.longitude(),
                    features[1].rings[0][i].longitude(), 1e-12);
        EXPECT_NEAR(expected.latitude(), features[1].rings[0][i].latitude(),
                    1e-12);
    }
    ASSERT_EQ(GeometryType::LineString, features[2].type);
    ASSERT_EQ(1u, features[2].rings.size());
    ASSERT_EQ(3u, features[2].rings[0].size());
    const Cartographic ordinaryExpected =
        amapTileLocalToLngLat(13038, 5503, 14, 8192.0, 4096.0);
    EXPECT_NEAR(ordinaryExpected.longitude(),
                features[2].rings[0][1].longitude(), 1e-12);
    EXPECT_NEAR(ordinaryExpected.latitude(),
                features[2].rings[0][1].latitude(), 1e-12);
}

TEST(AmapVectorTileTest, DecodesType4TransitAndRegionWithDistinctGeometry) {
    const auto container = makeTransitWithRegionTile();
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    ASSERT_EQ(4, parts[0].type);
    ASSERT_EQ(2u, parts[0].features.size());

    const auto& transit = parts[0].features[0];
    EXPECT_EQ(20015, transit.classCode);
    EXPECT_EQ(2, transit.geomType);
    EXPECT_FALSE(transit.polygonGeometry);
    EXPECT_EQ(7, transit.subKey);
    ASSERT_EQ(1u, transit.rings.size());

    const auto& region = parts[0].features[1];
    EXPECT_EQ(30003, region.classCode);
    EXPECT_EQ(3, region.geomType);
    EXPECT_TRUE(region.polygonGeometry);
    EXPECT_EQ(10, region.subKey);
    EXPECT_EQ(16, region.rank);
    EXPECT_EQ(64, region.kind);
    ASSERT_EQ(1u, region.rings.size());
    ASSERT_EQ(4u, region.rings[0].size());

    const auto features = amapDecodedPartToFeatures(parts[0], false);
    ASSERT_EQ(2u, features.size());
    EXPECT_EQ(GeometryType::LineString, features[0].type);
    EXPECT_EQ(GeometryType::Polygon, features[1].type);
    EXPECT_EQ("30003", features[1].properties.at("amap_class"));
    EXPECT_EQ("64", features[1].properties.at("amap_kind"));
    EXPECT_EQ("10", features[1].properties.at("amap_subkey"));

    // kind 64 必须使用 z14 line-grid scale=2：raw (100,100) ->
    // canonical (200,3896)，不能按普通区域 scale=4 压到错误位置。
    const Cartographic expected =
        amapTileLocalToLngLat(13038, 5503, 14, 200.0, 3896.0);
    ASSERT_FALSE(features[1].rings.empty());
    ASSERT_FALSE(features[1].rings[0].empty());
    EXPECT_NEAR(expected.longitude(), features[1].rings[0][0].longitude(),
                1e-12);
    EXPECT_NEAR(expected.latitude(), features[1].rings[0][0].latitude(),
                1e-12);

    std::vector<Feature> mainFeatures;
    ASSERT_TRUE(amapBytesToFeatures(container.data(), container.size(), false,
                                    mainFeatures));
    ASSERT_EQ(2u, mainFeatures.size());
    EXPECT_EQ(GeometryType::LineString, mainFeatures[0].type);
    EXPECT_EQ(GeometryType::Polygon, mainFeatures[1].type);

    std::vector<Feature> coarseRegions;
    ASSERT_TRUE(amapBytesToFeatures(container.data(), container.size(), true,
                                    coarseRegions));
    EXPECT_TRUE(coarseRegions.empty())
        << "type4 regions belong to the main group, not coarse type2 source";
}

TEST(AmapVectorTileTest, MainSourceFiltersWaterButKeepsBlockRegions) {
    const auto water = makeRegionTile();  // default class 30001
    std::vector<Feature> mainWater;
    std::string error;
    ASSERT_TRUE(amapBytesToFeatures(water.data(), water.size(), false,
                                    mainWater, &error))
        << error;
    EXPECT_TRUE(mainWater.empty());

    std::vector<Feature> regionWater;
    ASSERT_TRUE(amapBytesToFeatures(water.data(), water.size(), true,
                                    regionWater, &error))
        << error;
    EXPECT_FALSE(regionWater.empty());

    const auto blocks = makeRegionTile(30002);
    std::vector<Feature> mainBlocks;
    ASSERT_TRUE(amapBytesToFeatures(blocks.data(), blocks.size(), false,
                                    mainBlocks, &error))
        << error;
    ASSERT_FALSE(mainBlocks.empty());
    for (const auto& feature : mainBlocks) {
        ASSERT_TRUE(feature.properties.count("amap_class"));
        EXPECT_EQ("30002", feature.properties.at("amap_class"));
    }
}

TEST(AmapVectorTileTest, MainSourceLeavesLowZoomRegionsToCoarseSource) {
    // 真实低档由 live smoke 覆盖；这里锁定 source 分工：main 在 z3/6/8/10
    // 不得与 regions 重复输出 type2，z12+ 才接管非水系地块。
    const auto low = makeRegionTile(30002, 10, 814, 343);
    std::vector<Feature> mainLow;
    std::string err;
    ASSERT_TRUE(amapBytesToFeatures(low.data(), low.size(), false, mainLow,
                                    &err))
        << err;
    EXPECT_TRUE(mainLow.empty());

    std::vector<Feature> regionsLow;
    ASSERT_TRUE(amapBytesToFeatures(low.data(), low.size(), true, regionsLow,
                                    &err))
        << err;
    EXPECT_FALSE(regionsLow.empty());

    const auto near = makeRegionTile(30002, 12, 3259, 1374);
    std::vector<Feature> mainNear;
    ASSERT_TRUE(amapBytesToFeatures(near.data(), near.size(), false, mainNear,
                                    &err))
        << err;
    EXPECT_FALSE(mainNear.empty());
}

TEST(AmapVectorTileTest, RejectsBadLengthHeader) {
    auto container = makeBuildingTile();
    container[0] ^= 0xff;  // 破坏大端长度头
    std::vector<AmapDecodedLayerPart> parts;
    std::string err;
    EXPECT_FALSE(decodeAmapTile(container.data(), container.size(), parts, &err));
    EXPECT_FALSE(err.empty());
}

TEST(AmapVectorTileTest, RejectsCorruptGzip) {
    auto container = makeBuildingTile();
    container[container.size() / 2] ^= 0xff;  // 破坏 deflate 流
    std::vector<AmapDecodedLayerPart> parts;
    std::string err;
    EXPECT_FALSE(decodeAmapTile(container.data(), container.size(), parts, &err));
}

TEST(AmapVectorTileTest, PoiEntrySharesContainerPath) {
    const auto container = makeBuildingTile();
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    EXPECT_EQ(3, parts[0].type);
}

TEST(AmapVectorTileTest, PoiDecoderReplacesGenericType0Garbage) {
    const auto container = makePoiTile();

    // Prove the fixture exercises the historical failure: generic decoding
    // sees label#3 id bytes as one bogus geometry ring.
    std::vector<AmapDecodedLayerPart> generic;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), generic));
    ASSERT_EQ(1u, generic.size());
    ASSERT_EQ(0, generic[0].type);
    ASSERT_FALSE(generic[0].features.empty());
    EXPECT_TRUE(generic[0].features[0].name.empty());
    // The explicit group class can survive generic decoding; the invariant
    // that proves pollution is an id-derived ring with no POI name.
    EXPECT_EQ(12024, generic[0].features[0].classCode);
    EXPECT_FALSE(generic[0].features[0].rings.empty());

    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(container.data(), container.size(), parts));
    std::vector<const AmapDecodedLayerPart*> pointParts;
    for (const auto& part : parts) {
        if (part.type == 0) pointParts.push_back(&part);
    }
    ASSERT_EQ(1u, pointParts.size());
    ASSERT_EQ(1u, pointParts[0]->features.size());
    const auto& poi = pointParts[0]->features[0];
    EXPECT_EQ(12024, poi.classCode);
    EXPECT_EQ(5, poi.subKey);
    EXPECT_EQ(1, poi.geomType);
    EXPECT_EQ("court", poi.name);
    EXPECT_EQ(7, poi.rank);
    ASSERT_EQ(1u, poi.rings.size());
    ASSERT_EQ(1u, poi.rings[0].size());
    EXPECT_DOUBLE_EQ(1024.0, poi.rings[0][0].first);
    EXPECT_DOUBLE_EQ(512.0, poi.rings[0][0].second);
}

// 真样本校准:设置 AMAP_SAMPLE_TILE=<真实瓦片路径> 时解码并打印结构。
// 仓库不携带高德数据,默认跳过。
TEST(AmapVectorTileTest, DecodesRealSampleWhenProvided) {
    const char* path = std::getenv("AMAP_SAMPLE_TILE");
    if (!path) GTEST_SKIP() << "AMAP_SAMPLE_TILE unset";
    FILE* f = std::fopen(path, "rb");
    ASSERT_NE(nullptr, f);
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::rewind(f);
    ASSERT_GT(len, 0L);
    std::vector<uint8_t> raw(static_cast<size_t>(len));
    ASSERT_EQ(len, static_cast<long>(std::fread(raw.data(), 1, raw.size(), f)));
    std::fclose(f);

    std::vector<AmapDecodedLayerPart> parts;
    std::string err;
    ASSERT_TRUE(decodeAmapTile(raw.data(), raw.size(), parts, &err)) << err;
    EXPECT_FALSE(parts.empty());
    for (const auto& p : parts) {
        std::printf("layer z=%d x=%d y=%d type=%d features=%zu\n", p.z, p.x,
                    p.y, p.type, p.features.size());
    }
}

// 指定已知含 type4 content.#3 的真实瓦片时，验证它不再仅保留一个
// name-like payload 后丢弃，而是端到端输出 class30003/kind64 Polygon。
TEST(AmapVectorTileTest, RealType4RegionSampleProducesPolygonWhenProvided) {
    const char* path = std::getenv("AMAP_TYPE4_REGION_SAMPLE_TILE");
    if (!path) GTEST_SKIP() << "AMAP_TYPE4_REGION_SAMPLE_TILE unset";
    FILE* f = std::fopen(path, "rb");
    ASSERT_NE(nullptr, f);
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::rewind(f);
    ASSERT_GT(len, 0L);
    std::vector<uint8_t> raw(static_cast<size_t>(len));
    ASSERT_EQ(len, static_cast<long>(std::fread(raw.data(), 1, raw.size(), f)));
    std::fclose(f);

    std::vector<AmapDecodedLayerPart> parts;
    std::string err;
    ASSERT_TRUE(decodeAmapTile(raw.data(), raw.size(), parts, &err)) << err;
    size_t decodedRegions = 0;
    for (const auto& part : parts) {
        if (part.type != 4) continue;
        const double n = std::exp2(part.z);
        for (const auto& feature : part.features) {
            if (feature.classCode != 30003 || feature.kind != 64) continue;
            EXPECT_TRUE(feature.polygonGeometry);
            EXPECT_EQ(3, feature.geomType);
            EXPECT_FALSE(feature.rings.empty());
            AmapDecodedLayerPart one = part;
            one.features = {feature};
            const auto polygons = amapDecodedPartToFeatures(one, false);
            ASSERT_FALSE(polygons.empty());
            for (const auto& polygon : polygons) {
                EXPECT_EQ(GeometryType::Polygon, polygon.type);
                for (const auto& ring : polygon.rings) {
                    for (const Cartographic& point : ring) {
                        const double lonDeg =
                            point.longitude() * 180.0 / 3.14159265358979323846;
                        const double latDeg =
                            point.latitude() * 180.0 / 3.14159265358979323846;
                        const double localX =
                            ((lonDeg + 180.0) / 360.0 * n - part.x) * 8192.0;
                        const double localY =
                            ((90.0 - latDeg) / 180.0 * n - part.y) * 4096.0;
                        EXPECT_GE(localX, -256.0);
                        EXPECT_LE(localX, 8192.0 + 256.0);
                        EXPECT_GE(localY, -256.0);
                        EXPECT_LE(localY, 4096.0 + 256.0);
                    }
                }
            }
            ++decodedRegions;
        }
    }
    EXPECT_GT(decodedRegions, 0u);

    std::vector<Feature> features;
    ASSERT_TRUE(amapBytesToFeatures(raw.data(), raw.size(), false, features,
                                    &err))
        << err;
    size_t outputRegions = 0;
    for (const auto& feature : features) {
        const auto cls = feature.properties.find("amap_class");
        const auto kind = feature.properties.find("amap_kind");
        if (feature.type == GeometryType::Polygon &&
            cls != feature.properties.end() && cls->second == "30003" &&
            kind != feature.properties.end() && kind->second == "64") {
            EXPECT_FALSE(feature.rings.empty());
            ++outputRegions;
        }
    }
    // One source feature may split into multiple clipped Polygon Features at
    // tile borders; the invariant is that every decoded region survives and
    // no region is silently lost.
    EXPECT_GE(outputRegions, decodedRegions);
}

// POI 真实样本:AMAP_POI_SAMPLE_TILE=<poi 瓦片> 时解码,断言 type 0
// 通用 POI 层带 name/坐标/rank。
TEST(AmapVectorTileTest, DecodesRealPoiSampleWhenProvided) {
    const char* path = std::getenv("AMAP_POI_SAMPLE_TILE");
    if (!path) GTEST_SKIP() << "AMAP_POI_SAMPLE_TILE unset";
    FILE* f = std::fopen(path, "rb");
    ASSERT_NE(nullptr, f);
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::rewind(f);
    ASSERT_GT(len, 0L);
    std::vector<uint8_t> raw(static_cast<size_t>(len));
    ASSERT_EQ(len, static_cast<long>(std::fread(raw.data(), 1, raw.size(), f)));
    std::fclose(f);

    std::vector<AmapDecodedLayerPart> parts;
    std::string err;
    ASSERT_TRUE(decodeAmapPoiTile(raw.data(), raw.size(), parts, &err)) << err;
    bool sawType0 = false;
    size_t poiCount = 0;
    for (const auto& p : parts) {
        if (p.type != 0) continue;
        sawType0 = true;
        for (const auto& feat : p.features) {
            ++poiCount;
            // Generic decoder output must not leak into the POI stream.
            EXPECT_NE(90001, feat.classCode);
            EXPECT_EQ(1, feat.geomType);
            ASSERT_EQ(1u, feat.rings.size());
            ASSERT_EQ(1u, feat.rings[0].size());
            const double maxX = p.z <= 3 ? 1024.0 : 2048.0;
            const double maxY = p.z <= 3 ? 512.0 : 1024.0;
            EXPECT_GE(feat.rings[0][0].first, 0.0);
            EXPECT_LE(feat.rings[0][0].first, maxX);
            EXPECT_GE(feat.rings[0][0].second, 0.0);
            EXPECT_LE(feat.rings[0][0].second, maxY);
        }
    }
    EXPECT_TRUE(sawType0) << "POI tile should contain type-0 label layer";
    EXPECT_GT(poiCount, 0u) << "type-0 layer should have POI features";
}
