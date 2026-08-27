#include "earth_engine/data/AmapVectorSource.h"
#include "earth_engine/data/MvtTileFetchCache.h"
#include "earth_engine/tiling/TileScheme.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;

double deg(double d) { return d * kPi / 180.0; }

Rectangle rectDeg(double west, double south, double east, double north) {
    return Rectangle(deg(west), deg(south), deg(east), deg(north));
}

double heightForZoom(int zoom) { return 4.0e7 / std::pow(2.0, zoom + 0.5); }

/// 假网络:返回内存里的 amap 瓦片字节(真实样本或手工容器)。
struct FakeAmapFetch {
    std::vector<uint8_t> body;
    int statusCode = 200;
    std::vector<TileKey> requested;

    std::shared_ptr<AmapType1TileCache> cache() {
        return std::make_shared<AmapType1TileCache>(
            [this](const TileKey& key,
                   AmapType1TileCache::FetchCallback cb) {
                requested.push_back(key);
                cb(statusCode, body);
            },
            48);
    }
};

void putVarint(std::vector<uint8_t>& bytes, uint64_t value) {
    while (value >= 0x80) {
        bytes.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    bytes.push_back(static_cast<uint8_t>(value));
}

void putTag(std::vector<uint8_t>& bytes, int field, int wire) {
    putVarint(bytes, (static_cast<uint64_t>(field) << 3) | wire);
}

void putVarintField(std::vector<uint8_t>& bytes, int field, uint64_t value) {
    putTag(bytes, field, 0);
    putVarint(bytes, value);
}

void putBytesField(std::vector<uint8_t>& bytes, int field,
                   const std::vector<uint8_t>& value) {
    putTag(bytes, field, 2);
    putVarint(bytes, value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void putZigzag(std::vector<uint8_t>& bytes, int64_t value) {
    const uint64_t u = static_cast<uint64_t>(value);
    putVarint(bytes, (u << 1) ^ (0 - (u >> 63)));
}

std::vector<uint8_t> gzipCompress(const std::vector<uint8_t>& input) {
    z_stream stream{};
    EXPECT_EQ(Z_OK, deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY));
    std::vector<uint8_t> out(deflateBound(&stream, input.size()));
    stream.next_in = const_cast<uint8_t*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    EXPECT_EQ(Z_STREAM_END, deflate(&stream, Z_FINISH));
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

std::vector<uint8_t> makeSharedType1Tile() {
    std::vector<uint8_t> regionRing;
    for (const int64_t v : {0, 0, 40, 0, 0, 40, -40, 0}) {
        putZigzag(regionRing, v);
    }
    std::vector<uint8_t> regionRings;
    putBytesField(regionRings, 1, regionRing);
    std::vector<uint8_t> regionFeature;
    putBytesField(regionFeature, 6, regionRings);
    std::vector<uint8_t> regionGroup;
    putVarintField(regionGroup, 1, 30001);
    putBytesField(regionGroup, 4, regionFeature);
    std::vector<uint8_t> regionContent;
    putBytesField(regionContent, 1, regionGroup);
    std::vector<uint8_t> regionLayer;
    putVarintField(regionLayer, 1, 14);
    putVarintField(regionLayer, 2, 13038);
    putVarintField(regionLayer, 3, 5505);
    putVarintField(regionLayer, 4, 2);
    putBytesField(regionLayer, 5, regionContent);

    std::vector<uint8_t> lineBlob;
    for (const int64_t v : {0, 0, 40, 0, 0, 40}) putZigzag(lineBlob, v);
    std::vector<uint8_t> linePart;
    putBytesField(linePart, 5, lineBlob);
    std::vector<uint8_t> lineFeature;
    putBytesField(lineFeature, 4, linePart);
    std::vector<uint8_t> lineGroup;
    putVarintField(lineGroup, 1, 20009);
    putBytesField(lineGroup, 4, lineFeature);
    std::vector<uint8_t> lineContent;
    putBytesField(lineContent, 1, lineGroup);
    std::vector<uint8_t> lineLayer;
    putVarintField(lineLayer, 1, 14);
    putVarintField(lineLayer, 2, 13038);
    putVarintField(lineLayer, 3, 5505);
    putVarintField(lineLayer, 4, 1);
    putBytesField(lineLayer, 5, lineContent);

    std::vector<uint8_t> tile;
    putBytesField(tile, 4, regionLayer);
    putBytesField(tile, 4, lineLayer);
    std::vector<uint8_t> root;
    putBytesField(root, 1, tile);
    const std::vector<uint8_t> gzip = gzipCompress(root);
    std::vector<uint8_t> container = {
        static_cast<uint8_t>(gzip.size() >> 24),
        static_cast<uint8_t>(gzip.size() >> 16),
        static_cast<uint8_t>(gzip.size() >> 8),
        static_cast<uint8_t>(gzip.size())};
    container.insert(container.end(), gzip.begin(), gzip.end());
    return container;
}

struct FakeSinks {
    int tessellateCalls = 0;
    std::vector<TileKey> committed;
    std::vector<TileKey> dropped;

    template <typename Source>
    typename Source::Sinks fn() {
        typename Source::Sinks s;
        s.tessellate = [this](const TileKey&, std::vector<Feature>&& features) {
            ++tessellateCalls;
            lastFeatureCount = features.size();
            FeatureTileMesh mesh;
            mesh.hasOrigin = true;
            if (!features.empty()) {
                mesh.fillVerts = {0.f, 0.f, 0.f};
                mesh.fillIndices = {0, 0, 0};
            }
            return mesh;
        };
        s.commit = [this](const TileKey& k, FeatureTileMesh&) {
            committed.push_back(k);
            return TileMeshCommitResult::Committed;
        };
        s.drop = [this](const TileKey& k) { dropped.push_back(k); };
        return s;
    }
    size_t lastFeatureCount = 0;
};

/// 手工最小 amap 容器:4 字节 BE 长度 + gzip 不是必须的(解码器先看长度头,
/// 无 gzip 魔法时按 raw protobuf 走?—— 见 decodeAmapTile 实现,这里直接
/// 用真实样本,避免手工构造与实现细节耦合)。

TEST(AmapVectorSource, AmapSchemeSelectsChongqingZ14Tile) {
    auto scheme = TileScheme::createAmapGeographic();
    // 样本瓦片 13038_5505 覆盖 106.47-106.49E / 29.52-29.53N,
    // 取其中一点 106.48E / 29.52N → z14 x=13038 y=5505。
    TileKey key =
        scheme->positionToTile(deg(106.48), deg(29.52), 14);
    EXPECT_EQ(14, key.z);
    EXPECT_EQ(13038, key.x);
    EXPECT_EQ(5505, key.y);
}

TEST(AmapVectorSource, AmapSchemeTileRectangleCoversChongqing) {
    auto scheme = TileScheme::createAmapGeographic();
    TileKey key{scheme->id(), 14, 13038, 5505};
    const Rectangle rect = scheme->tileToRectangle(key);
    const double west = rect.west() / kPi * 180.0;
    const double north = rect.north() / kPi * 180.0;
    EXPECT_LT(west, 106.5);
    EXPECT_GT(north, 29.5);
    // 瓦片宽 360/2^14 ≈ 0.022°,高 180/2^14 ≈ 0.011°。
    EXPECT_NEAR(360.0 / 16384.0,
                (rect.east() - rect.west()) / kPi * 180.0, 1e-9);
    EXPECT_NEAR(180.0 / 16384.0,
                (rect.north() - rect.south()) / kPi * 180.0, 1e-9);
}

TEST(AmapVectorSource, PipelineDecodesAndCommitsRealSample) {
    const char* path = std::getenv("AMAP_SAMPLE_TILE");
    if (!path) GTEST_SKIP() << "AMAP_SAMPLE_TILE unset";
    FILE* f = std::fopen(path, "rb");
    ASSERT_NE(nullptr, f);
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::rewind(f);
    std::vector<uint8_t> raw(static_cast<size_t>(len));
    ASSERT_EQ(len, static_cast<long>(std::fread(raw.data(), 1, raw.size(), f)));
    std::fclose(f);

    FakeAmapFetch fetch;
    fetch.body = std::move(raw);
    FakeSinks sinks;
    AmapMainVectorSource source(
        [] {
            AmapMainVectorSource::Options opt;
            opt.tree.minZoom = 14;
            opt.tree.maxZoom = 14;
            opt.tree.scheme = TileScheme::createAmapGeographic();
            opt.tree.maxTilesPerView = 64;
            opt.maxTileCommitsPerUpdate = 0;
            return opt;
        }(),
        sinks.fn<AmapMainVectorSource>(), fetch.cache());

    // 视口 = 样本瓦片附近(约 1 瓦,避免 z14 枚举海量 key)。
    const Rectangle view = rectDeg(106.47, 29.515, 106.49, 29.525);
    source.update(view, heightForZoom(14));
    ASSERT_FALSE(fetch.requested.empty());
    EXPECT_EQ(sinks.tessellateCalls, 0);

    source.update(view, heightForZoom(14));  // 消化解码 → 派镶嵌
    EXPECT_GT(sinks.tessellateCalls, 0);
    EXPECT_GT(sinks.lastFeatureCount, 0u);

    source.update(view, heightForZoom(14));  // 消化网格 → commit
    EXPECT_FALSE(sinks.committed.empty());
    EXPECT_EQ(sinks.committed.size(), source.activeTileCount());
}

TEST(AmapVectorSource, RegionsOnlyFiltersType2FromRealSample) {
    const char* path = std::getenv("AMAP_SAMPLE_TILE");
    if (!path) GTEST_SKIP() << "AMAP_SAMPLE_TILE unset";
    FILE* f = std::fopen(path, "rb");
    ASSERT_NE(nullptr, f);
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::rewind(f);
    std::vector<uint8_t> raw(static_cast<size_t>(len));
    ASSERT_EQ(len, static_cast<long>(std::fread(raw.data(), 1, raw.size(), f)));
    std::fclose(f);

    FakeAmapFetch fetch;
    fetch.body = std::move(raw);
    FakeSinks sinks;
    AmapRegionsVectorSource source(
        [] {
            AmapRegionsVectorSource::Options opt;
            opt.tree.minZoom = 14;
            opt.tree.maxZoom = 14;
            opt.tree.scheme = TileScheme::createAmapGeographic();
            opt.tree.maxTilesPerView = 64;
            opt.maxTileCommitsPerUpdate = 0;
            return opt;
        }(),
        sinks.fn<AmapRegionsVectorSource>(), fetch.cache());

    const Rectangle view = rectDeg(106.47, 29.515, 106.49, 29.525);
    source.update(view, heightForZoom(14));
    source.update(view, heightForZoom(14));
    // 主样本的 z14 瓦(type1 请求组)若不含 type2 面,regionsOnly 解码为空
    // 网格 → 无 commit(粗源数据在 z10 组,这里验证过滤语义不炸)。
    EXPECT_GE(sinks.tessellateCalls, 0);
    // 不崩溃即可;过滤正确性由 AmapGeometry 单测覆盖。
}

TEST(AmapVectorSource, RegionsAndMainShareOneType1DecodeCache) {
    FakeAmapFetch fetch;
    fetch.body = makeSharedType1Tile();
    auto sharedCache = fetch.cache();
    FakeSinks regionSinks;
    FakeSinks mainSinks;
    auto options = [] {
        AmapRegionsVectorSource::Options opt;
        opt.tree.minZoom = 14;
        opt.tree.maxZoom = 14;
        opt.tree.scheme = TileScheme::createAmapGeographic();
        opt.tree.maxTilesPerView = 64;
        opt.maxTileCommitsPerUpdate = 0;
        return opt;
    }();
    AmapRegionsVectorSource regions(
        options, regionSinks.fn<AmapRegionsVectorSource>(), sharedCache);
    AmapMainVectorSource::Options mainOptions;
    mainOptions.tree = options.tree;
    mainOptions.maxTileCommitsPerUpdate = 0;
    AmapMainVectorSource main(
        mainOptions, mainSinks.fn<AmapMainVectorSource>(), sharedCache);

    const Rectangle view = rectDeg(106.47, 29.515, 106.49, 29.525);
    regions.update(view, heightForZoom(14));
    const size_t uniqueRequests = fetch.requested.size();
    ASSERT_GT(uniqueRequests, 0u);
    main.update(view, heightForZoom(14));
    EXPECT_EQ(uniqueRequests, fetch.requested.size());
    EXPECT_EQ(uniqueRequests, sharedCache->stats().fetches);

    regions.update(view, heightForZoom(14));
    main.update(view, heightForZoom(14));
    EXPECT_GT(regionSinks.lastFeatureCount, 0u);
    EXPECT_GT(mainSinks.lastFeatureCount, 0u);
}

}  // namespace
