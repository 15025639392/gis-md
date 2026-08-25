#include <gtest/gtest.h>
#include <zlib.h>

#include "earth_engine/data/AmapVectorTile.h"
#include "earth_engine/data/AmapGeometry.h"

#include <cstring>
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

std::vector<uint8_t> makeBlobLayer(int layerType, const std::vector<uint8_t>& cg) {
    std::vector<uint8_t> content;
    putBytesField(content, 1, cg);
    std::vector<uint8_t> layer;
    putVarintField(layer, 1, 14);
    putVarintField(layer, 2, 13038);
    putVarintField(layer, 3, 6785);
    putVarintField(layer, 4, layerType);
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

std::vector<uint8_t> makeRegionTile(int classCode = 0) {
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
    return makeBlobLayer(2, cg);
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
    FILE* diag = std::fopen("/tmp/amap_poi.txt", "w");
    ASSERT_NE(nullptr, diag);
    for (const auto& p : parts) {
        if (p.type != 0) continue;
        sawType0 = true;
        for (const auto& feat : p.features) {
            ++poiCount;
            if (feat.name.empty() || feat.rings.empty()) {
                std::fprintf(diag, "POI cc=%d subKey=%d name=<empty> "
                                   "rings=%zu\n",
                             feat.classCode, feat.subKey, feat.rings.size());
                continue;
            }
            std::fprintf(diag,
                         "POI cc=%d subKey=%d name=%s rank=%d "
                         "anchor=(%.0f,%.0f)\n",
                         feat.classCode, feat.subKey, feat.name.c_str(),
                         feat.rank, feat.rings[0][0].first,
                         feat.rings[0][0].second);
            if (poiCount >= 5) break;
        }
        if (poiCount >= 5) break;
    }
    std::fclose(diag);
    EXPECT_TRUE(sawType0) << "POI tile should contain type-0 label layer";
    EXPECT_GT(poiCount, 0u) << "type-0 layer should have POI features";
}
