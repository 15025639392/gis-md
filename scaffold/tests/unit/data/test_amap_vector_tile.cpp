#include <gtest/gtest.h>
#include "../../helpers/AmapOfficialTestAdapters.h"
#include "../../helpers/AmapOfficialStyleTestAdapter.h"
#include <zlib.h>

#include "earth_engine/data/AmapVectorTile.h"
#include "earth_engine/data/AmapGeometry.h"
#include "earth_engine/data/AmapVectorSource.h"
#include "earth_engine/style/AmapClassicLabelStyleInternal.h"
#include "earth_engine/style/AmapClassicStyleInternal.h"
#include "earth_engine/style/AmapClassicRoadStyle.h"
#include "earth_engine/style/AmapClassicStyleInternal.h"

#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

using namespace earth_engine;
using namespace earth_engine::testing;

namespace {

std::filesystem::path fixturePath(const char* relative) {
    return std::filesystem::path(AMAP_TEST_FIXTURE_ROOT) / relative;
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size <= 0) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) return {};
    return bytes;
}

double officialGroup(const StyleExpression::Ptr& expression,
                     int classCode, int subKey) {
    StyleExpression::PropertyMap properties{
        {"amap_class", std::to_string(classCode)},
        {"amap_subkey", std::to_string(subKey)}};
    const auto value = expression->evaluate(&properties, 0.0);
    return value && value->kind() == StyleValue::Kind::Number
               ? value->number()
               : 0.0;
}

std::shared_ptr<const AmapDecodedTile> decodeOfficialTile(
    const std::vector<uint8_t>& bytes, std::string* error = nullptr) {
    AmapDecodedTile tile;
    if (!AmapDecodedTileDecodeTraits::decode(
            bytes.data(), bytes.size(), tile, error)) {
        return nullptr;
    }
    return std::make_shared<const AmapDecodedTile>(std::move(tile));
}

template <typename Adapter>
std::vector<Feature> officialFeatures(const std::vector<uint8_t>& bytes,
                                      Adapter adapter,
                                      std::string* error = nullptr) {
    auto tile = decodeOfficialTile(bytes, error);
    if (!tile) return {};
    return adapter(TileKey{}, std::move(tile), {}, {});
}

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

// Official type-3 BuildingSameStyle: mainKey=55001, subKey=1.
// 一个 Feature → Part { blob=三角形 zigzag 几何, height=36 }。
std::vector<uint8_t> makeBuildingTile(bool omitOfficialDefaults = false,
                                      uint64_t encodedHeight = 36) {
    std::vector<uint8_t> part;
    {
        std::vector<uint8_t> blob;
        const int64_t pts[6] = {0, 0, 10, 0, 0, 10};  // (0,0)->(10,0)->(10,10)
        for (int64_t v : pts) putZigzag(blob, v);
        putBytesField(part, 3, blob);
        if (!omitOfficialDefaults) putVarintField(part, 5, encodedHeight);
    }
    std::vector<uint8_t> feature;
    if (!omitOfficialDefaults) {
        putVarintField(feature, 3, 47);  // FeatureMulti.drawOrder
    }
    putBytesField(feature, 4, part);
    std::vector<uint8_t> cg;
    if (!omitOfficialDefaults) {
        putVarintField(cg, 1, 55001);
        putVarintField(cg, 2, 1);
        putVarintField(cg, 3, 13);
    }
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
    putVarintField(feature, 3, 89);  // RoadLineMulti.drawOrder
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
    putVarintField(feature, 4, 23);  // PolygonFeature.drawOrder
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
    if (boundaryClass == 20014) putVarintField(boundaryGroup, 2, 2);
    if (boundaryClass == 20010) putVarintField(boundaryGroup, 2, 1);
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
std::vector<uint8_t> makePoiTile(uint64_t classCode = 12024,
                                 uint64_t subKey = 5,
                                 bool languageContract = false,
                                 bool packedMii = false) {
    const std::vector<uint8_t> nameBytes = languageContract
        ? std::vector<uint8_t>{'f', 'i', 'r', 's', 't'}
        : std::vector<uint8_t>{'c', 'o', 'u', 'r', 't'};
    std::vector<uint8_t> nameBox;
    putBytesField(nameBox, 1, nameBytes);
    if (languageContract) {
        if (packedMii) {
            std::vector<uint8_t> boundaries;
            putVarint(boundaries, 2);
            putVarint(boundaries, 5);
            putBytesField(nameBox, 4, boundaries);
        } else {
            putVarintField(nameBox, 4, 2);
            putVarintField(nameBox, 4, 5);
        }
    }

    std::vector<uint8_t> coord;
    putVarint(coord, 1024);  // native 2048×1024 grid center
    putVarint(coord, 512);

    std::vector<uint8_t> label;
    putBytesField(label, 1, nameBox);
    if (languageContract) {
        std::vector<uint8_t> secondName;
        putBytesField(secondName, 1,
                      std::vector<uint8_t>{'l', 'a', 's', 't'});
        putBytesField(label, 1, secondName);
    }
    const std::vector<uint8_t> idBytes = {'A', 'B', 'C', 'D', 'E', 'F'};
    putBytesField(label, 3, idBytes);
    putBytesField(label, 4, coord);

    std::vector<uint8_t> feature;
    putVarintField(feature, 1, 14);
    putVarintField(feature, 2, 30);
    putVarintField(feature, 3, 7);
    putBytesField(feature, 4, label);

    std::vector<uint8_t> group;
    putVarintField(group, 1, classCode);
    putVarintField(group, 2, subKey);
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

std::vector<uint8_t> makeTransitPointTile(bool explicitFields) {
    std::vector<uint8_t> firstName;
    putBytesField(firstName, 1,
                  std::vector<uint8_t>{'s', 'u', 'b', 'w', 'a', 'y'});
    putVarintField(firstName, 4, 3);
    std::vector<uint8_t> secondName;
    putBytesField(secondName, 1,
                  std::vector<uint8_t>{'i', 'g', 'n', 'o', 'r', 'e'});

    std::vector<uint8_t> coord;
    putVarint(coord, 100);
    putVarint(coord, 200);
    std::vector<uint8_t> point;
    putBytesField(point, 1, firstName);
    putBytesField(point, 1, secondName);
    if (explicitFields) putVarintField(point, 2, 23);  // drawOrder
    putBytesField(point, 4, coord);
    if (explicitFields) putVarintField(point, 6, 77);  // uid

    std::vector<uint8_t> feature;
    if (explicitFields) {
        putVarintField(feature, 1, 11);
        putVarintField(feature, 2, 19);
        putVarintField(feature, 3, 456);
    }
    putBytesField(feature, 4, point);

    std::vector<uint8_t> group;
    if (explicitFields) {
        putVarintField(group, 1, 10005);
        putVarintField(group, 2, 3);
        putVarintField(group, 3, 13);
    }
    putBytesField(group, 4, feature);

    std::vector<uint8_t> content;
    putBytesField(content, 2, group);
    std::vector<uint8_t> layer;
    putVarintField(layer, 1, 14);
    putVarintField(layer, 2, 13039);
    putVarintField(layer, 3, 5497);
    putVarintField(layer, 4, 4);
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
    EXPECT_EQ(55001, f.classCode);
    EXPECT_EQ(1, f.subKey);
    EXPECT_EQ(1, f.geomType);
    EXPECT_EQ(13, f.buildingResolution);
    EXPECT_DOUBLE_EQ(36.0, f.height);
    EXPECT_TRUE(f.hasHeight);
    EXPECT_EQ(47, f.drawOrder);
    EXPECT_EQ(15, f.minZoom);
    EXPECT_EQ(30, f.maxZoom);
    ASSERT_EQ(1u, f.rings.size());
    ASSERT_EQ(3u, f.rings[0].size());
    EXPECT_DOUBLE_EQ(0.0, f.rings[0][0].first);
    EXPECT_DOUBLE_EQ(0.0, f.rings[0][0].second);
    EXPECT_DOUBLE_EQ(10.0, f.rings[0][1].first);
    EXPECT_DOUBLE_EQ(0.0, f.rings[0][1].second);
    EXPECT_DOUBLE_EQ(10.0, f.rings[0][2].first);
    EXPECT_DOUBLE_EQ(10.0, f.rings[0][2].second);
}

TEST(AmapVectorTileTest, AppliesOfficialBuildingProtobufDefaults) {
    const auto container = makeBuildingTile(true);
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    ASSERT_EQ(1u, parts[0].features.size());
    const auto& feature = parts[0].features[0];
    EXPECT_EQ(55001, feature.classCode);
    EXPECT_EQ(1, feature.subKey);
    EXPECT_EQ(12, feature.buildingResolution);
    EXPECT_EQ(15, feature.minZoom);
    EXPECT_EQ(30, feature.maxZoom);
    EXPECT_EQ(0, feature.drawOrder);
    EXPECT_TRUE(feature.hasDrawOrder);
    EXPECT_DOUBLE_EQ(6.0, feature.height);
    EXPECT_FALSE(feature.hasHeight);
}

TEST(AmapVectorTileTest, DecodesBuildingHeightWithOfficialInt32Sign) {
    const auto container = makeBuildingTile(
        false, static_cast<uint64_t>(static_cast<int64_t>(-1)));
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    ASSERT_EQ(1u, parts[0].features.size());
    const auto& feature = parts[0].features[0];
    EXPECT_TRUE(feature.hasHeight);
    EXPECT_DOUBLE_EQ(-1.0, feature.height);
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
    EXPECT_EQ(89, f.drawOrder);
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
    EXPECT_EQ(30001, f.classCode);
    EXPECT_EQ(23, f.kind);
    EXPECT_EQ(0, f.drawOrder);
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
    EXPECT_EQ("2", features[1].properties.at("amap_subkey"));
    EXPECT_EQ(features[1].properties.end(),
              features[1].properties.find("name"));
    EXPECT_EQ(features[1].properties.end(),
              features[1].properties.find("amap_rank"));
    const auto lineSelector = amapClassicLineStyleGroupExpression();
    const auto labelSelector = amapClassicLineLabelStyleGroupExpression();
    EXPECT_EQ(0.0, officialGroup(lineSelector, 20014, 2));
    EXPECT_EQ(amapClassicStyleIdentity(20014, 2),
              officialGroup(labelSelector, 20014, 2));
}

TEST(AmapVectorTileTest, MissingBoundaryClassUsesOfficialSchemaDefault) {
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

TEST(AmapVectorTileTest, CoarseRegionDrawableLineUsesOfficialTransportIdentity) {
    const auto container = makeRegionWithBoundaryTile(20010);
    std::string error;
    const auto features = officialFeatures(
        container, AmapRegionsToFeaturesForTest{}, &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(2u, features.size());
    ASSERT_EQ(GeometryType::LineString, features[1].type);
    EXPECT_EQ("20010", features[1].properties.at("amap_class"));
    EXPECT_EQ("1", features[1].properties.at("amap_subkey"));
    EXPECT_EQ(amapClassicStyleIdentity(20010, 1),
              officialGroup(amapClassicLineStyleGroupExpression(), 20010, 1));
}

TEST(AmapVectorTileTest,
     CoarseRegionZeroOutputLineIsDroppedBeforeFeatureConstruction) {
    const auto container = makeRegionWithBoundaryTile(20014);
    std::string error;
    const auto features = officialFeatures(
        container, AmapRegionsToFeaturesForTest{}, &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeometryType::Polygon, features.front().type);
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
    EXPECT_EQ("20015", features[0].properties.at("amap_class"));
    EXPECT_EQ("7", features[0].properties.at("amap_subkey"));
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

    std::vector<Feature> mainFeatures =
        officialFeatures(container, AmapMainToFeaturesForTest{});
    ASSERT_EQ(2u, mainFeatures.size());
    EXPECT_EQ(GeometryType::LineString, mainFeatures[0].type);
    EXPECT_EQ(GeometryType::Polygon, mainFeatures[1].type);

    std::vector<Feature> coarseRegions =
        officialFeatures(container, AmapRegionsToFeaturesForTest{});
    EXPECT_TRUE(coarseRegions.empty())
        << "type4 regions belong to the main group, not coarse type2 source";
}

TEST(AmapVectorTileTest, TransitPointsUseOfficialPointDefaultsAndPointScale) {
    const auto container = makeTransitPointTile(false);
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    ASSERT_EQ(1u, parts[0].features.size());
    const auto& point = parts[0].features.front();
    EXPECT_TRUE(point.pointGeometry);
    EXPECT_FALSE(point.lineGeometry);
    EXPECT_FALSE(point.polygonGeometry);
    EXPECT_EQ(12024, point.classCode);
    EXPECT_EQ(1, point.subKey);
    EXPECT_EQ(18, point.minZoom);
    EXPECT_EQ(30, point.maxZoom);
    EXPECT_EQ(0, point.rank);
    EXPECT_EQ(0, point.drawOrder);
    EXPECT_TRUE(point.hasDrawOrder);
    EXPECT_EQ(1u, point.uid);
    EXPECT_EQ("subway", point.name) << "first nameLoc owns the label";
    EXPECT_EQ((std::vector<uint32_t>{3}), point.nameSplitIndicesUtf16);
    EXPECT_DOUBLE_EQ(4.0, point.coordScale)
        << "type4 point resolution 12 must not use the type4 line scale 2";
    ASSERT_EQ(1u, point.rings.size());
    ASSERT_EQ(1u, point.rings[0].size());
    EXPECT_DOUBLE_EQ(100.0, point.rings[0][0].first);
    EXPECT_DOUBLE_EQ(200.0, point.rings[0][0].second);

    const auto features = amapDecodedPartToFeatures(parts[0], true);
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeometryType::Point, features[0].type);
    EXPECT_EQ("1", features[0].properties.at("amap_uid"));
}

TEST(AmapVectorTileTest, TransitPointsPreserveExplicitOfficialFields) {
    const auto container = makeTransitPointTile(true);
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    ASSERT_EQ(1u, parts[0].features.size());
    const auto& point = parts[0].features.front();
    EXPECT_TRUE(point.pointGeometry);
    EXPECT_EQ(10005, point.classCode);
    EXPECT_EQ(3, point.subKey);
    EXPECT_EQ(11, point.minZoom);
    EXPECT_EQ(19, point.maxZoom);
    EXPECT_EQ(456, point.rank);
    EXPECT_EQ(23, point.drawOrder);
    EXPECT_EQ(77u, point.uid);
    EXPECT_DOUBLE_EQ(2.0, point.coordScale);
    EXPECT_TRUE(isAmapClassicPoiIdentity(point.classCode, point.subKey));
}

TEST(AmapVectorTileTest, MainAndRegionSourcesPreserveOfficialSurfaceDefaults) {
    // Missing protobuf class/subkey fields resolve to the official schema
    // defaults (30001:1); this is an official identity, not an unknown-path
    // compatibility case.
    const auto defaultSurface = makeRegionTile();
    std::string error;
    std::vector<Feature> mainSurface =
        officialFeatures(defaultSurface, AmapMainToFeaturesForTest{}, &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_FALSE(mainSurface.empty());

    std::vector<Feature> regionSurface =
        officialFeatures(defaultSurface, AmapRegionsToFeaturesForTest{}, &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_FALSE(regionSurface.empty());

    const auto blocks = makeRegionTile(30002);
    std::vector<Feature> mainBlocks =
        officialFeatures(blocks, AmapMainToFeaturesForTest{}, &error);
    ASSERT_TRUE(error.empty()) << error;
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
    std::string err;
    std::vector<Feature> mainLow =
        officialFeatures(low, AmapMainToFeaturesForTest{}, &err);
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_TRUE(mainLow.empty());

    std::vector<Feature> regionsLow =
        officialFeatures(low, AmapRegionsToFeaturesForTest{}, &err);
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_FALSE(regionsLow.empty());

    const auto near = makeRegionTile(30002, 12, 3259, 1374);
    std::vector<Feature> mainNear =
        officialFeatures(near, AmapMainToFeaturesForTest{}, &err);
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_FALSE(mainNear.empty());
}

TEST(AmapVectorTileTest, MainSourceRoadKeepsGeometryWithoutLabelState) {
    auto tile = std::make_shared<AmapDecodedTile>();
    AmapDecodedLayerPart part;
    part.type = 1;
    part.z = 14;
    part.x = 13038;
    part.y = 5505;
    AmapDecodedFeature road;
    road.classCode = 20001;
    road.subKey = 1;
    road.name = "must-not-become-a-main-label";
    road.rank = 37;
    road.rings = {{{0.0, 0.0}, {100.0, 100.0}}};
    part.features.push_back(std::move(road));
    tile->parts.push_back(std::move(part));

    const auto features = AmapMainToFeaturesForTest{}(
        TileKey{}, std::move(tile), {}, {});
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeometryType::LineString, features.front().type);
    EXPECT_EQ(features.front().properties.end(),
              features.front().properties.find("name"));
    EXPECT_EQ(features.front().properties.end(),
              features.front().properties.find("rank"));
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

TEST(AmapVectorTileTest, PoiDecoderDoesNotRetainGenericBuildingPath) {
    const auto container = makeBuildingTile();
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(container.data(), container.size(), parts));
    EXPECT_TRUE(parts.empty());
}

TEST(AmapVectorTileTest, PoiDecoderUsesOnlyOfficialPointSchema) {
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
    EXPECT_EQ(0, poi.drawOrder);
    EXPECT_TRUE(poi.hasDrawOrder);
    ASSERT_EQ(1u, poi.rings.size());
    ASSERT_EQ(1u, poi.rings[0].size());
    EXPECT_DOUBLE_EQ(1024.0, poi.rings[0][0].first);
    EXPECT_DOUBLE_EQ(512.0, poi.rings[0][0].second);
}

TEST(AmapVectorTileTest, PoiDefaultsToFirstNameLocAndPreservesMiiSplitIndices) {
    const auto container = makePoiTile(12024, 5, true);
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(container.data(), container.size(), parts));
    const auto part = std::find_if(parts.begin(), parts.end(),
                                   [](const auto& value) {
                                       return value.type == 0;
                                   });
    ASSERT_NE(parts.end(), part);
    ASSERT_EQ(1u, part->features.size());
    EXPECT_EQ("first", part->features[0].name);
    EXPECT_EQ((std::vector<uint32_t>{2, 5}),
              part->features[0].nameSplitIndicesUtf16);

    const auto features = amapDecodedPartToFeatures(*part, false);
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ("first", features[0].properties.at("name"));
    EXPECT_EQ((std::vector<uint32_t>{2, 5}),
              features[0].labelSplitIndicesUtf16);
}

TEST(AmapVectorTileTest, PoiDecodesPackedMiiSplitIndices) {
    const auto container = makePoiTile(12024, 5, true, true);
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(container.data(), container.size(), parts));
    const auto part = std::find_if(parts.begin(), parts.end(),
                                   [](const auto& value) {
                                       return value.type == 0;
                                   });
    ASSERT_NE(parts.end(), part);
    ASSERT_EQ(1u, part->features.size());
    EXPECT_EQ((std::vector<uint32_t>{2, 5}),
              part->features[0].nameSplitIndicesUtf16);
}

TEST(AmapVectorTileTest, RealPoiFixturesPreserveOfficialLanguageContract) {
    const auto shanghai = readBinaryFile(
        fixturePath("cross-region/shanghai_13720_5349_14_t2.pbf"));
    ASSERT_FALSE(shanghai.empty());
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(shanghai.data(), shanghai.size(), parts));
    const AmapDecodedFeature* multiline = nullptr;
    for (const auto& part : parts) {
        for (const auto& feature : part.features) {
            if (feature.classCode == 12024 && feature.subKey == 1249 &&
                feature.name == "B-128弄39号") {
                multiline = &feature;
            }
        }
    }
    ASSERT_NE(nullptr, multiline);
    EXPECT_EQ((std::vector<uint32_t>{8, 9}),
              multiline->nameSplitIndicesUtf16);

    const auto tokyo = readBinaryFile(
        fixturePath("cross-region/tokyo_56_19_6_t2.pbf"));
    ASSERT_FALSE(tokyo.empty());
    parts.clear();
    ASSERT_TRUE(decodeAmapPoiTile(tokyo.data(), tokyo.size(), parts));
    bool foundOfficialDefault = false;
    for (const auto& part : parts) {
        for (const auto& feature : part.features) {
            if (feature.name == "东京") foundOfficialDefault = true;
            EXPECT_NE("東京", feature.name)
                << "default formatter selects nameLoc[0], not the last locale";
        }
    }
    EXPECT_TRUE(foundOfficialDefault);
}

TEST(AmapVectorTileTest, PoiDecoderRejectsOverflowIdentityWithoutAliasing) {
    const auto container = makePoiTile((uint64_t{1} << 32) + 12024,
                                       (uint64_t{1} << 32) + 5);
    std::vector<AmapDecodedLayerPart> parts;
    ASSERT_TRUE(decodeAmapPoiTile(container.data(), container.size(), parts));
    ASSERT_EQ(1u, parts.size());
    EXPECT_TRUE(parts.front().features.empty())
        << "malformed raw identity must not narrow into official 12024:5";
}

// 仓库固定官方真样本门禁；环境变量只允许开发者临时替换样本。
TEST(AmapVectorTileTest, DecodesRealSampleWhenProvided) {
    const char* path = std::getenv("AMAP_SAMPLE_TILE");
    const std::filesystem::path defaultPath = fixturePath(
        "samples/main-13038_5505_14.pbf");
    FILE* f = std::fopen(path ? path : defaultPath.c_str(), "rb");
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
    std::map<std::tuple<int, int, int, int>, size_t> distribution;
    for (const auto& p : parts) {
        std::printf("layer z=%d x=%d y=%d type=%d features=%zu\n", p.z, p.x,
                    p.y, p.type, p.features.size());
        for (const auto& feature : p.features) {
            ++distribution[{p.type, feature.classCode, feature.subKey,
                            feature.kind}];
            if (!feature.name.empty()) {
                std::printf("  named type=%d class=%d name=%s\n", p.type,
                            feature.classCode, feature.name.c_str());
            }
        }
    }
    for (const auto& [key, count] : distribution) {
        const auto [type, classCode, subKey, kind] = key;
        std::printf("main layer type=%d class=%d sub=%d kind=%d count=%zu\n",
                    type, classCode, subKey, kind, count);
    }
}

TEST(AmapVectorTileTest, CrossRegionFixturesUseOnlyOfficialStyleContracts) {
    const char* directory = std::getenv("AMAP_CROSS_REGION_FIXTURE_DIR");
    const std::filesystem::path defaultDirectory = fixturePath("cross-region");
    const std::filesystem::path fixtureDirectory =
        directory ? std::filesystem::path(directory) : defaultDirectory;

    FeatureRenderStyle surfaceStyle;
    surfaceStyle = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Regions);
    const auto lineSelector = amapClassicLineStyleGroupExpression();
    const auto labelSelector = amapClassicLineLabelStyleGroupExpression();
    std::set<std::pair<int, int>> observedLines;
    std::set<std::pair<int, int>> observedLineLabels;
    std::set<std::pair<int, int>> observedSurfaces;
    std::set<std::pair<int, int>> observedPois;
    std::set<std::pair<int, int>> rejectedPois;
    std::set<std::pair<int, int>> rejectedLines;
    size_t files = 0;
    size_t transitPoints = 0;

    for (const auto& entry : std::filesystem::directory_iterator(
             fixtureDirectory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pbf")
            continue;
        const std::string filename = entry.path().filename().string();
        const auto hasSuffix = [&](const char* suffix) {
            const size_t length = std::strlen(suffix);
            return filename.size() >= length &&
                   filename.compare(filename.size() - length, length,
                                    suffix) == 0;
        };
        const bool poi = hasSuffix("_t2.pbf");
        const bool main = hasSuffix("_t1.pbf");
        if (!poi && !main) continue;
        const auto bytes = readBinaryFile(entry.path());
        ASSERT_FALSE(bytes.empty()) << entry.path();
        std::vector<AmapDecodedLayerPart> parts;
        std::string error;
        ASSERT_TRUE((poi ? decodeAmapPoiTile : decodeAmapTile)(
            bytes.data(), bytes.size(), parts, &error))
            << entry.path() << ": " << error;
        ++files;

        for (const auto& part : parts) {
            for (const auto& feature : part.features) {
                const std::pair identity{feature.classCode, feature.subKey};
                const bool polygon = feature.polygonGeometry ||
                                     (part.type == 2 && !feature.lineGeometry) ||
                                     part.type == 3;
                const bool point = feature.pointGeometry;
                const bool line = !point &&
                                  (feature.lineGeometry ||
                                   (!feature.polygonGeometry &&
                                    (part.type == 1 || part.type == 4)));
                if (polygon) {
                    observedSurfaces.insert(identity);
                    EXPECT_NE(0.0, officialGroup(surfaceStyle.fillStyleGroupExpr,
                                                  feature.classCode,
                                                  feature.subKey))
                        << entry.path() << " surface " << feature.classCode
                        << ':' << feature.subKey;
                }
                if (line && !feature.name.empty()) {
                    observedLineLabels.insert(identity);
                    EXPECT_NE(0.0, officialGroup(labelSelector,
                                                  feature.classCode,
                                                  feature.subKey))
                        << entry.path() << " line-label " << feature.classCode
                        << ':' << feature.subKey;
                }
                if (line && !feature.roadNameGeometry) {
                    const double lineGroup = officialGroup(
                        lineSelector, feature.classCode, feature.subKey);
                    const double labelGroup = officialGroup(
                        labelSelector, feature.classCode, feature.subKey);
                    if (lineGroup != 0.0) observedLines.insert(identity);
                    if (lineGroup == 0.0 && labelGroup == 0.0)
                        rejectedLines.insert(identity);
                }
                if (point) {
                    if (part.type == 4) ++transitPoints;
                    observedPois.insert(identity);
                    if (!isAmapClassicPoiIdentity(feature.classCode,
                                                  feature.subKey)) {
                        rejectedPois.insert(identity);
                    }
                }
            }
        }
    }

    EXPECT_GE(files, 50u) << "expected the complete multi-region fixture set";
    EXPECT_FALSE(observedLines.empty());
    EXPECT_FALSE(observedLineLabels.empty());
    EXPECT_FALSE(observedSurfaces.empty());
    EXPECT_EQ((std::set<std::pair<int, int>>{{20016, 17}}), rejectedLines)
        << "official width/type-only identities must remain fail-closed";
    EXPECT_FALSE(observedPois.empty());
    EXPECT_GT(transitPoints, 0u)
        << "official TransitLayer.points/content.#2 must reach subwayLabel";
    std::printf("official contract audit files=%zu lines=%zu lineLabels=%zu "
                "surfaces=%zu pois=%zu rejectedPois=%zu\n", files,
                observedLines.size(),
                observedLineLabels.size(), observedSurfaces.size(),
                observedPois.size(), rejectedPois.size());
    EXPECT_EQ((std::set<std::pair<int, int>>{{12024, 860}, {12024, 861},
                                              {12024, 862}}),
              rejectedPois)
        << "Only payload identities absent from the current official style "
           "PBF may be rejected; newly styled classes must be generated.";
}

// 指定已知含 type4 content.#3 的真实瓦片时，验证它不再仅保留一个
// name-like payload 后丢弃，而是端到端输出 class30003/kind64 Polygon。
TEST(AmapVectorTileTest, RealType4RegionSampleProducesPolygonWhenProvided) {
    const char* path = std::getenv("AMAP_TYPE4_REGION_SAMPLE_TILE");
    const std::filesystem::path defaultPath = fixturePath(
        "samples/type4-region-13489_4559_14.pbf");
    FILE* f = std::fopen(path ? path : defaultPath.c_str(), "rb");
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

    std::vector<Feature> features =
        officialFeatures(raw, AmapMainToFeaturesForTest{}, &err);
    ASSERT_TRUE(err.empty()) << err;
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
    const std::filesystem::path defaultPath = fixturePath(
        "samples/poi-814_343_10.pbf");
    FILE* f = std::fopen(path ? path : defaultPath.c_str(), "rb");
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
    // Keep subKey in the audit distribution: classCode alone is not a
    // sufficient style key for AMap classic-normal.  This output is consumed
    // when probing signed production tiles, so retaining the complete style
    // identity avoids inferring label families from names or screenshots.
    std::map<std::tuple<int, int, int, int, int>, size_t> distribution;
    std::map<std::tuple<int, int, int, bool>, size_t> allLayerDistribution;
    for (const auto& p : parts) {
        for (const auto& feat : p.features) {
            ++allLayerDistribution[{p.type, feat.classCode, feat.subKey,
                                    !feat.name.empty()}];
        }
        if (p.type != 0) continue;
        sawType0 = true;
        for (const auto& feat : p.features) {
            ++poiCount;
            ++distribution[{feat.classCode, feat.subKey, feat.minZoom,
                            feat.maxZoom, feat.rank}];
            if (!feat.name.empty() && (feat.rank >= 15000 || feat.minZoom <= 10)) {
                std::printf(
                    "POI sample name=%s class=%d sub=%d min=%d max=%d rank=%d\n",
                    feat.name.c_str(), feat.classCode, feat.subKey,
                    feat.minZoom, feat.maxZoom, feat.rank);
            }
            // Generic decoder output must not leak into the POI stream.
            EXPECT_NE(0, feat.classCode);
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
    for (const auto& [key, count] : distribution) {
        const auto [classCode, subKey, minZoom, maxZoom, rank] = key;
        std::printf(
            "POI class=%d sub=%d min=%d max=%d rank=%d count=%zu\n",
            classCode, subKey, minZoom, maxZoom, rank, count);
    }
    for (const auto& [key, count] : allLayerDistribution) {
        const auto [type, classCode, subKey, named] = key;
        std::printf("POI layer type=%d class=%d sub=%d named=%d count=%zu\n",
                    type, classCode, subKey, named ? 1 : 0, count);
    }
}

TEST(AmapVectorTileTest, DecodesRoadNameClassWhenProvided) {
    const char* path = std::getenv("AMAP_ROADNAME_SAMPLE_TILE");
    const std::filesystem::path defaultPath = fixturePath(
        "samples/roadname-817_337_10.pbf");
    FILE* f = std::fopen(path ? path : defaultPath.c_str(), "rb");
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
    size_t names = 0;
    size_t shields = 0;
    for (const auto& part : parts) {
        for (const auto& feature : part.features) {
            if (!feature.roadNameGeometry) continue;
            ++names;
            EXPECT_EQ(2, feature.geomType);
            EXPECT_TRUE(feature.lineGeometry);
            EXPECT_FALSE(feature.name.empty());
            EXPECT_FALSE(feature.rings.empty());
            EXPECT_GT(feature.classCode, 0);
            EXPECT_TRUE(feature.roadNameGeometry);
            if (!feature.shield.empty()) {
                ++shields;
                EXPECT_GT(feature.shieldType, 0);
            }
        }
    }
    EXPECT_GT(names, 0u);
    // The default Chongqing sample need not contain a shield. Cross-region
    // fixtures below lock reachability for the official shield-bearing data.
    (void)shields;
}

TEST(AmapVectorTileTest, RealOfficialFixturePreservesRoadShieldContract) {
    const auto bytes = readBinaryFile(fixturePath(
        "cross-region/beijing_843_284_10_t2.pbf"));
    ASSERT_FALSE(bytes.empty());
    std::vector<AmapDecodedLayerPart> parts;
    std::string error;
    ASSERT_TRUE(decodeAmapPoiTile(bytes.data(), bytes.size(), parts, &error))
        << error;
    size_t shields = 0;
    for (const auto& part : parts) {
        for (const auto& feature : part.features) {
            if (feature.shield.empty()) continue;
            ++shields;
            EXPECT_TRUE(feature.roadNameGeometry);
            EXPECT_GT(feature.shieldType, 0);
            EXPECT_FALSE(feature.rings.empty());
        }
    }
    EXPECT_GT(shields, 0u);
}

// Structured dual-source diagnostic for road labels. The two paths are
// intentionally supplied separately because AMap serves visible roads and
// road-name geometry from different tile groups.
TEST(AmapVectorTileTest, DiagnosesRoadNamePathAgainstVisibleRoadWhenProvided) {
    const char* poiPath = std::getenv("AMAP_ROADNAME_SAMPLE_TILE");
    const char* roadPath = std::getenv("AMAP_MATCHING_ROAD_SAMPLE_TILE");
    const std::filesystem::path defaultPoiPath = fixturePath(
        "samples/roadname-13038_5501_14.pbf");
    const std::filesystem::path defaultRoadPath = fixturePath(
        "samples/main-13038_5501_14.pbf");
    auto read = [](const char* path) {
        FILE* f = std::fopen(path, "rb");
        EXPECT_NE(nullptr, f);
        if (!f) return std::vector<uint8_t>{};
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::rewind(f);
        std::vector<uint8_t> bytes(static_cast<size_t>(len));
        EXPECT_EQ(len, static_cast<long>(
                           std::fread(bytes.data(), 1, bytes.size(), f)));
        std::fclose(f);
        return bytes;
    };
    const auto poiBytes = read(poiPath ? poiPath : defaultPoiPath.c_str());
    const auto roadBytes = read(roadPath ? roadPath : defaultRoadPath.c_str());
    std::vector<AmapDecodedLayerPart> poiParts, roadParts;
    std::string error;
    ASSERT_TRUE(decodeAmapPoiTile(poiBytes.data(), poiBytes.size(), poiParts,
                                  &error)) << error;
    ASSERT_TRUE(decodeAmapTile(roadBytes.data(), roadBytes.size(), roadParts,
                              &error)) << error;
    std::vector<Feature> labels, roads;
    for (const auto& part : poiParts) {
        auto converted = amapDecodedPartToFeatures(part, false);
        for (auto& feature : converted) {
            if (feature.type == GeometryType::LineString &&
                feature.properties.find("name") != feature.properties.end()) {
                labels.push_back(std::move(feature));
            }
        }
    }
    for (const auto& part : roadParts) {
        auto converted = amapDecodedPartToFeatures(part, false);
        for (auto& feature : converted) {
            if (feature.type == GeometryType::LineString) {
                roads.push_back(std::move(feature));
            }
        }
    }
    ASSERT_FALSE(labels.empty());
    ASSERT_FALSE(roads.empty());
    size_t exactMatches = 0;
    size_t simplifiedPaths = 0;
    constexpr double earthRadius = 6378137.0;
    auto directedDistance = [&](const Feature& from, const Feature& to) {
        double worst = 0.0;
        for (const auto& p : from.rings.front()) {
            const double cosLat = std::cos(p.latitude());
            double nearest = std::numeric_limits<double>::infinity();
            for (size_t i = 1; i < to.rings.front().size(); ++i) {
                const auto& a = to.rings.front()[i - 1];
                const auto& b = to.rings.front()[i];
                const double ax = (a.longitude() - p.longitude()) * cosLat;
                const double ay = a.latitude() - p.latitude();
                const double bx = (b.longitude() - p.longitude()) * cosLat;
                const double by = b.latitude() - p.latitude();
                const double vx = bx - ax, vy = by - ay;
                const double vv = vx * vx + vy * vy;
                const double t = vv > 0.0
                    ? std::clamp(-(ax * vx + ay * vy) / vv, 0.0, 1.0)
                    : 0.0;
                nearest = std::min(nearest, earthRadius *
                    std::hypot(ax + vx * t, ay + vy * t));
            }
            worst = std::max(worst, nearest);
        }
        return worst;
    };
    for (const auto& label : labels) {
        const auto classIt = label.properties.find("amap_class");
        ASSERT_NE(label.properties.end(), classIt);
        const Feature* best = nullptr;
        double bestHausdorff = std::numeric_limits<double>::infinity();
        double bestLabelToRoad = bestHausdorff;
        double bestRoadToLabel = bestHausdorff;
        for (const auto& road : roads) {
            const auto roadClass = road.properties.find("amap_class");
            if (roadClass == road.properties.end() ||
                roadClass->second != classIt->second) continue;
            const double a = directedDistance(label, road);
            const double b = directedDistance(road, label);
            const double h = std::max(a, b);
            if (h < bestHausdorff) {
                best = &road;
                bestHausdorff = h;
                bestLabelToRoad = a;
                bestRoadToLabel = b;
            }
        }
        const auto name = label.properties.find("name");
        if (bestHausdorff < 0.001) ++exactMatches;
        if (label.rings.front().size() > 65) ++simplifiedPaths;
        std::printf(
            "ROAD_LABEL_MATCH name=%s class=%s points=%zu candidate=%d "
            "label_to_road_m=%.3f road_to_label_m=%.3f hausdorff_m=%.3f\n",
            name == label.properties.end() ? "" : name->second.c_str(),
            classIt->second.c_str(), label.rings.front().size(), best ? 1 : 0,
            bestLabelToRoad, bestRoadToLabel, bestHausdorff);
    }
    std::printf(
        "ROAD_LABEL_SUMMARY labels=%zu exact_same_geometry=%zu "
        "over_65_points=%zu\n",
        labels.size(), exactMatches, simplifiedPaths);
    EXPECT_GT(exactMatches, labels.size() / 2)
        << "dual-source geometry should not be assumed divergent";
}

TEST(AmapVectorTileTest, DiagnosesParentChildSurfaceCoverageWhenProvided) {
    const char* parentPath = std::getenv("AMAP_SURFACE_PARENT_TILE");
    const char* childPath = std::getenv("AMAP_SURFACE_CHILD_TILE");
    const std::filesystem::path defaultParentPath = fixturePath(
        "cross-region/shanghai_3430_1337_12_t1.pbf");
    const std::filesystem::path defaultChildPath = fixturePath(
        "cross-region/shanghai_13720_5349_14_t1.pbf");
    auto read = [](const char* path) {
        FILE* f = std::fopen(path, "rb");
        EXPECT_NE(nullptr, f);
        if (!f) return std::vector<uint8_t>{};
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::rewind(f);
        std::vector<uint8_t> bytes(static_cast<size_t>(std::max(0L, len)));
        EXPECT_EQ(len, static_cast<long>(
                           std::fread(bytes.data(), 1, bytes.size(), f)));
        std::fclose(f);
        return bytes;
    };
    auto decodeRegions = [&](const char* path) {
        const auto bytes = read(path);
        std::vector<AmapDecodedLayerPart> parts;
        std::string error;
        EXPECT_TRUE(decodeAmapTile(bytes.data(), bytes.size(), parts, &error))
            << error;
        std::vector<Feature> out;
        for (const auto& part : parts) {
            if (part.type != 2) continue;
            auto features = amapDecodedPartToFeatures(part, false);
            out.insert(out.end(), std::make_move_iterator(features.begin()),
                       std::make_move_iterator(features.end()));
        }
        return out;
    };
    const auto parent = decodeRegions(
        parentPath ? parentPath : defaultParentPath.c_str());
    const auto child = decodeRegions(
        childPath ? childPath : defaultChildPath.c_str());
    ASSERT_FALSE(parent.empty());
    ASSERT_FALSE(child.empty());

    const auto childPart = [&]() -> const Feature* {
        for (const auto& feature : child) {
            if (!feature.rings.empty() && !feature.rings.front().empty())
                return &feature;
        }
        return nullptr;
    }();
    ASSERT_NE(nullptr, childPart);
    double west = std::numeric_limits<double>::infinity();
    double east = -west, south = west, north = -west;
    for (const auto& feature : child) {
        for (const auto& ring : feature.rings) {
            for (const auto& p : ring) {
                west = std::min(west, p.longitude());
                east = std::max(east, p.longitude());
                south = std::min(south, p.latitude());
                north = std::max(north, p.latitude());
            }
        }
    }
    auto contains = [](const Feature& feature, double x, double y) {
        bool inside = false;
        for (const auto& ring : feature.rings) {
            if (ring.size() < 3) continue;
            bool ringInside = false;
            for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
                const double xi = ring[i].longitude();
                const double yi = ring[i].latitude();
                const double xj = ring[j].longitude();
                const double yj = ring[j].latitude();
                if (((yi > y) != (yj > y)) &&
                    x < (xj - xi) * (y - yi) /
                                ((yj - yi) == 0.0 ? 1e-30 : (yj - yi)) + xi)
                    ringInside = !ringInside;
            }
            inside ^= ringInside;
        }
        return inside;
    };
    auto classifies = [&](const std::vector<Feature>& features, int subKey,
                          double x, double y) {
        for (const auto& feature : features) {
            const auto cls = feature.properties.find("amap_class");
            const auto sub = feature.properties.find("amap_subkey");
            if (cls == feature.properties.end() || sub == feature.properties.end() ||
                cls->second != "30001" || sub->second != std::to_string(subKey))
                continue;
            if (contains(feature, x, y)) return true;
        }
        return false;
    };
    std::set<int> surfaceSubKeys;
    auto collectSurfaceSubKeys = [&](const std::vector<Feature>& features) {
        for (const auto& feature : features) {
            const auto cls = feature.properties.find("amap_class");
            const auto sub = feature.properties.find("amap_subkey");
            if (cls == feature.properties.end() ||
                sub == feature.properties.end() || cls->second != "30001") {
                continue;
            }
            try {
                surfaceSubKeys.insert(std::stoi(sub->second));
            } catch (...) {
            }
        }
    };
    collectSurfaceSubKeys(parent);
    collectSurfaceSubKeys(child);
    ASSERT_FALSE(surfaceSubKeys.empty());

    constexpr int grid = 256;
    for (const int subKey : surfaceSubKeys) {
        size_t pCount = 0, cCount = 0, intersection = 0, unionCount = 0;
        for (int iy = 0; iy < grid; ++iy) {
            const double y = south + (north - south) * (iy + 0.5) / grid;
            for (int ix = 0; ix < grid; ++ix) {
                const double x = west + (east - west) * (ix + 0.5) / grid;
                const bool p = classifies(parent, subKey, x, y);
                const bool c = classifies(child, subKey, x, y);
                pCount += p;
                cCount += c;
                intersection += p && c;
                unionCount += p || c;
            }
        }
        size_t parentVertices = 0, childVertices = 0;
        auto countVertices = [&](const std::vector<Feature>& features) {
            size_t count = 0;
            for (const auto& feature : features) {
                const auto cls = feature.properties.find("amap_class");
                const auto sub = feature.properties.find("amap_subkey");
                if (cls == feature.properties.end() || sub == feature.properties.end() ||
                    cls->second != "30001" || sub->second != std::to_string(subKey))
                    continue;
                for (const auto& ring : feature.rings) count += ring.size();
            }
            return count;
        };
        parentVertices = countVertices(parent);
        childVertices = countVertices(child);
        const double iou = unionCount ? static_cast<double>(intersection) / unionCount : 1.0;
        std::printf("SURFACE_COVERAGE sub=%d parent=%zu child=%zu intersection=%zu union=%zu iou=%.6f parent_vertices=%zu child_vertices=%zu\n",
                    subKey, pCount, cCount, intersection, unionCount, iou,
                    parentVertices, childVertices);
    }
}
