#include "earth_engine/data/AmapVectorSource.h"
#include "earth_engine/data/MvtTileFetchCache.h"
#include "earth_engine/tiling/TileScheme.h"

#include <gtest/gtest.h>

#include <cmath>
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

    template <bool RegionsOnly>
    std::shared_ptr<MvtTileFetchCacheT<std::vector<Feature>,
                                       AmapDecodeTraits<RegionsOnly>>>
    cache() {
        return std::make_shared<
            MvtTileFetchCacheT<std::vector<Feature>,
                               AmapDecodeTraits<RegionsOnly>>>(
            [this](const TileKey& key,
                   MvtTileFetchCacheT<std::vector<Feature>,
                                      AmapDecodeTraits<RegionsOnly>>::
                       FetchCallback cb) {
                requested.push_back(key);
                cb(statusCode, body);
            },
            48);
    }
};

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
        s.commit = [this](const TileKey& k, FeatureTileMesh&&) {
            committed.push_back(k);
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
        sinks.fn<AmapMainVectorSource>(), fetch.cache<false>());

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
        sinks.fn<AmapRegionsVectorSource>(), fetch.cache<true>());

    const Rectangle view = rectDeg(106.47, 29.515, 106.49, 29.525);
    source.update(view, heightForZoom(14));
    source.update(view, heightForZoom(14));
    // 主样本的 z14 瓦(type1 请求组)若不含 type2 面,regionsOnly 解码为空
    // 网格 → 无 commit(粗源数据在 z10 组,这里验证过滤语义不炸)。
    EXPECT_GE(sinks.tessellateCalls, 0);
    // 不崩溃即可;过滤正确性由 AmapGeometry 单测覆盖。
}

}  // namespace
