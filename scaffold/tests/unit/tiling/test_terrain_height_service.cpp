#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/LoadedTerrainHeightSampler.h"
#include "earth_engine/tiling/TerrainHeightService.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "earth_engine/tiling/TilesetTileRegistry.h"

#include <memory>
#include <optional>
#include <random>
#include <vector>

using namespace earth_engine;

// === TerrainHeightService:统一采样服务 ===
//
// 覆盖两类目标:
//   1. 服务自身语义(最深档命中/同深取更高/无覆盖 nullopt/质量标签/
//      惰性重建/不变量溢出列表);
//   2. E3 对拍守卫:与旧 LoadedTerrainHeightSampler 在同一瓦片世界上
//      逐点等价(含 cell 边界点)——迁移期间旧路径就是 golden。

namespace {

constexpr const char* kScheme = "Geographic-TMS";

std::unique_ptr<DecodedHeightmap> makeUniformHeightmap(float height) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->stagedHeights = {height, height, height, height};
    heightmap->assignHeights();
    heightmap->minHeight = height;
    heightmap->maxHeight = height;
    return heightmap;
}

// 每瓦一个可区分的恒定高度:选中了哪张瓦一目了然。
float heightFor(const TileKey& key) {
    return static_cast<float>(key.z * 1000 + key.x * 10 + key.y);
}

TilesetTile* putTerrainTile(TilesetTileRegistry& registry,
                            const TileScheme& scheme,
                            const TileKey& key,
                            float height) {
    TilesetTile* tile = registry.ensureTile(key, scheme, nullptr);
    tile->content.renderContent.setTerrainRenderContent(true);
    tile->content.renderContent.setRetainedHeightmap(
        makeUniformHeightmap(height));
    return tile;
}

} // namespace

TEST(TerrainHeightServiceTest, NoCoverageReturnsNullopt) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    const TileKey coveredKey{kScheme, 1, 0, 0};
    putTerrainTile(registry, *scheme, coveredKey, 55.0f);

    const Rectangle elsewhere =
        scheme->tileToRectangle(TileKey{kScheme, 1, 1, 1});
    const auto sample = service.sample(
        (elsewhere.west() + elsewhere.east()) * 0.5,
        (elsewhere.south() + elsewhere.north()) * 0.5,
        TerrainHeightService::Interp::FullResBilinear);
    EXPECT_FALSE(sample.has_value());
}

TEST(TerrainHeightServiceTest, DeepestTileWinsAndZoomTagsQuality) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    const TileKey rootKey{kScheme, 0, 0, 0};
    const TileKey childKey{kScheme, 1, 0, 0};
    putTerrainTile(registry, *scheme, rootKey, 10.0f);
    putTerrainTile(registry, *scheme, childKey, 42.0f);

    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const auto sample = service.sample(
        (childBounds.west() + childBounds.east()) * 0.5,
        (childBounds.south() + childBounds.north()) * 0.5,
        TerrainHeightService::Interp::FullResBilinear);
    ASSERT_TRUE(sample.has_value());
    EXPECT_NEAR(42.0f, sample->height, 1e-4f);
    EXPECT_EQ(1, sample->zoom) << "zoom 是答案的质量标签,必须指向来源瓦片";
}

TEST(TerrainHeightServiceTest, AncestorFallbackTagsCoarseZoom) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    const TileKey rootKey{kScheme, 0, 0, 0};
    const TileKey childKey{kScheme, 1, 0, 0};
    putTerrainTile(registry, *scheme, rootKey, 123.0f);
    // 子瓦存在但无 heightmap(上采样/加载中)→ 回退祖先,质量标签暴露粗档。
    registry.ensureTile(childKey, *scheme, nullptr);

    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const auto sample = service.sample(
        (childBounds.west() + childBounds.east()) * 0.5,
        (childBounds.south() + childBounds.north()) * 0.5,
        TerrainHeightService::Interp::FullResBilinear);
    ASSERT_TRUE(sample.has_value());
    EXPECT_NEAR(123.0f, sample->height, 1e-4f);
    EXPECT_EQ(0, sample->zoom);
}

TEST(TerrainHeightServiceTest, IrregularBoundsTileGoesToOverflowAndStillWins) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    // 同深重叠缝(bounds 与 key 不符):cell 索引装不下这种瓦,必须进溢出
    // 列表按旧语义参与"同深取更高"。生产不变量下溢出应恒空,这里专门
    // 构造违例验证正确性不赌不变量。
    const TileKey lowerKey{kScheme, 1, 0, 0};
    const TileKey upperKey{kScheme, 1, 1, 0};
    const Rectangle bounds = scheme->tileToRectangle(lowerKey);
    putTerrainTile(registry, *scheme, lowerKey, 78.0f);

    auto irregular = std::make_unique<TilesetTile>(upperKey, bounds);
    irregular->content.renderContent.setTerrainRenderContent(true);
    irregular->content.renderContent.setRetainedHeightmap(
        makeUniformHeightmap(83.0f));
    registry.tiles()[TileCacheKey::forTile(upperKey)] = std::move(irregular);

    const auto sample = service.sample(
        bounds.west() + bounds.width() * 0.25,
        bounds.south() + bounds.height() * 0.25,
        TerrainHeightService::Interp::FullResBilinear);
    ASSERT_TRUE(sample.has_value());
    EXPECT_NEAR(83.0f, sample->height, 1e-4f) << "同深取更高须含溢出瓦";
    EXPECT_EQ(1u, service.irregularCount());
}

TEST(TerrainHeightServiceTest, RebuildIsLazyAndGenerationDriven) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    const TileKey rootKey{kScheme, 0, 0, 0};
    putTerrainTile(registry, *scheme, rootKey, 10.0f);

    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    const double lng = (bounds.west() + bounds.east()) * 0.5;
    const double lat = (bounds.south() + bounds.north()) * 0.5;
    const auto interp = TerrainHeightService::Interp::FullResBilinear;

    (void)service.sample(lng, lat, interp);
    const std::uint64_t builds0 = service.rebuildCount();
    ASSERT_GE(builds0, 1u);

    // 稳态:重复查询不许重建(E5 判据的服务侧镜像)。
    for (int i = 0; i < 100; ++i) {
        (void)service.sample(lng, lat, interp);
    }
    EXPECT_EQ(builds0, service.rebuildCount());

    // heightmap 世界变化(bump 代次)→ 恰好一次重建,且新瓦立即可见。
    const TileKey childKey{kScheme, 1, 0, 0};
    putTerrainTile(registry, *scheme, childKey, 42.0f);
    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const auto sample = service.sample(
        (childBounds.west() + childBounds.east()) * 0.5,
        (childBounds.south() + childBounds.north()) * 0.5,
        interp);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(1, sample->zoom);
    EXPECT_EQ(builds0 + 1, service.rebuildCount());
}

TEST(TerrainHeightServiceTest, SampleStatsCountHitsMissesAndZoomBuckets) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    const TileKey rootKey{kScheme, 0, 0, 0};
    putTerrainTile(registry, *scheme, rootKey, 10.0f);
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    const auto interp = TerrainHeightService::Interp::FullResBilinear;

    // 2 次命中(z0) + 1 次 miss(另一半球无瓦)。
    (void)service.sample((bounds.west() + bounds.east()) * 0.5,
                         (bounds.south() + bounds.north()) * 0.5, interp);
    (void)service.sample(bounds.west() + bounds.width() * 0.25,
                         bounds.south() + bounds.height() * 0.25, interp);
    const Rectangle other =
        scheme->tileToRectangle(TileKey{kScheme, 0, 1, 0});
    (void)service.sample((other.west() + other.east()) * 0.5,
                         (other.south() + other.north()) * 0.5, interp);

    const auto stats = service.takeSampleStats();
    EXPECT_EQ(2u, stats.hits);
    EXPECT_EQ(1u, stats.misses);
    EXPECT_EQ(2u, stats.zoomHits[0]);

    // take 即清零:下一窗口从零起算。
    const auto empty = service.takeSampleStats();
    EXPECT_EQ(0u, empty.total());
}

// === E3 对拍守卫:与旧全表扫描逐点等价 ===

namespace {

struct ParityWorld {
    std::unique_ptr<TileScheme> scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;

    ParityWorld() {
        // 多档瓦片世界:z0 全覆盖;z1/z2/z3 按确定性伪随机布点,且混入
        // "有瓦无 heightmap"(必须被跳过、回退祖先)的瓦片。
        std::mt19937 rng(20260807u);
        putTerrainTile(registry, *scheme, TileKey{kScheme, 0, 0, 0},
                       heightFor(TileKey{kScheme, 0, 0, 0}));
        putTerrainTile(registry, *scheme, TileKey{kScheme, 0, 1, 0},
                       heightFor(TileKey{kScheme, 0, 1, 0}));
        for (int z = 1; z <= 3; ++z) {
            const int countX = scheme->tileCountX(z);
            const int countY = scheme->tileCountY(z);
            for (int x = 0; x < countX; ++x) {
                for (int y = 0; y < countY; ++y) {
                    const std::uint32_t roll = rng() % 100u;
                    const TileKey key{kScheme, z, x, y};
                    if (roll < 30u) {
                        putTerrainTile(registry, *scheme, key, heightFor(key));
                    } else if (roll < 40u) {
                        registry.ensureTile(key, *scheme, nullptr);
                    }
                }
            }
        }
    }
};

void expectParityAt(const ParityWorld& world,
                    const TerrainHeightService& service,
                    double lng,
                    double lat) {
    const std::optional<float> legacy =
        LoadedTerrainHeightSampler::sampleHeightOptional(
            const_cast<TilesetTileRegistry&>(world.registry).tiles(),
            lng, lat);
    const auto sample = service.sample(
        lng, lat, TerrainHeightService::Interp::FullResBilinear);
    ASSERT_EQ(legacy.has_value(), sample.has_value())
        << "coverage 判定分歧 @ (" << lng << ", " << lat << ")";
    if (legacy) {
        EXPECT_EQ(*legacy, sample->height)
            << "选瓦分歧 @ (" << lng << ", " << lat << ")";
    }
}

} // namespace

TEST(TerrainHeightServiceParityTest, RandomPointsMatchLegacySampler) {
    ParityWorld world;
    TerrainHeightService service(world.registry, *world.scheme);

    std::mt19937 rng(987654321u);
    std::uniform_real_distribution<double> lngDist(-3.14159, 3.14159);
    std::uniform_real_distribution<double> latDist(-1.5707, 1.5707);
    for (int i = 0; i < 2000; ++i) {
        expectParityAt(world, service, lngDist(rng), latDist(rng));
    }
}

TEST(TerrainHeightServiceParityTest, TileBoundaryPointsMatchLegacySampler) {
    ParityWorld world;
    TerrainHeightService service(world.registry, *world.scheme);

    // cell 边界是新旧算法结构差异最大的地方:旧 = bounds.contains 闭区间
    // 天然双瓦命中;新 = 中心 cell + 贴边才探邻域。逐瓦四边中点 + 四角全扫。
    for (const auto& [cacheKey, tile] : world.registry.tiles()) {
        (void)cacheKey;
        const Rectangle& b = tile->bounds;
        const double midLng = (b.west() + b.east()) * 0.5;
        const double midLat = (b.south() + b.north()) * 0.5;
        const double points[][2] = {
            {b.west(), midLat},  {b.east(), midLat},
            {midLng, b.south()}, {midLng, b.north()},
            {b.west(), b.south()}, {b.east(), b.south()},
            {b.west(), b.north()}, {b.east(), b.north()},
        };
        for (const auto& p : points) {
            expectParityAt(world, service, p[0], p[1]);
        }
    }
}

TEST(TerrainHeightServiceParityTest, RenderGridConsistentPathMatchesLegacy) {
    ParityWorld world;
    TerrainHeightService service(world.registry, *world.scheme);

    // 渲染网格一致路径:两侧同样从常驻 draw 命令读档位(测试瓦无命令 →
    // 双双走 renderGridSize=0 的 coarse 回退),选瓦逻辑仍须逐点一致。
    std::mt19937 rng(13579u);
    std::uniform_real_distribution<double> lngDist(-3.14159, 3.14159);
    std::uniform_real_distribution<double> latDist(-1.5707, 1.5707);
    for (int i = 0; i < 500; ++i) {
        const double lng = lngDist(rng);
        const double lat = latDist(rng);
        LoadedTerrainAreaSampler legacyArea(
            const_cast<TilesetTileRegistry&>(world.registry).tiles(),
            Rectangle(lng - 1e-6, lat - 1e-6, lng + 1e-6, lat + 1e-6),
            /*renderGridConsistent=*/true);
        const std::optional<float> legacy = legacyArea.sample(lng, lat);
        const auto sample = service.sample(
            lng, lat, TerrainHeightService::Interp::RenderGridConsistent);
        ASSERT_EQ(legacy.has_value(), sample.has_value());
        if (legacy) {
            EXPECT_EQ(*legacy, sample->height);
        }
    }
}

// === heightRangeForArea:计划瓦片的"地面高度所在区间"从哪来 ===
//
// 背景(真机实测 2026-08-09):包围体的收紧只发生在瓦片载入**自己**的内容
// 之后,而宽视野下计划里的瓦片大量没有自己的内容 —— 它们的包围体永远停在
// 占位常量 -1000/9000。冷启动直接落在宽视野时 t0/l20,一块 tight 都没有,
// 贴地体高度整体退化成 10km。
//
// 按**矩形**问而不是按 TileKey 上溯,是因为两边未必同属一套网格(实测:
// 计划 z12/3259/1697 的 z8 祖先应是 203/106,索引里是 202/107)。

TEST(TerrainHeightServiceTest, HeightRangeForAreaUsesCoveringTile) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    const TileKey key{kScheme, 2, 1, 1};
    TilesetTile* tile = putTerrainTile(registry, *scheme, key, 42.0f);
    tile->content.renderContent.setTerrainHeightRange(120.0, 880.0);

    // area 取该瓦片矩形的正中一小块:落在这块瓦片内部,不碰边界。
    const Rectangle full = scheme->tileToRectangle(key);
    const double cx = (full.west() + full.east()) * 0.5;
    const double cy = (full.south() + full.north()) * 0.5;
    const double dx = (full.east() - full.west()) * 0.1;
    const double dy = (full.north() - full.south()) * 0.1;
    const auto range = service.heightRangeForArea(
        Rectangle(cx - dx, cy - dy, cx + dx, cy + dy));
    ASSERT_TRUE(range.has_value());
    EXPECT_DOUBLE_EQ(range->first, 120.0);
    EXPECT_DOUBLE_EQ(range->second, 880.0);
}

TEST(TerrainHeightServiceTest, HeightRangeForAreaFallsBackToFullyCoveringZoom) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    // 粗档整块覆盖;细档只有一块(部分覆盖)。部分覆盖的那档必须被跳过 ——
    // 拿它的区间会比真实地面窄,窄了贴地整片消失。
    const TileKey coarseKey{kScheme, 1, 1, 0};
    TilesetTile* coarse = putTerrainTile(registry, *scheme, coarseKey, 7.0f);
    coarse->content.renderContent.setTerrainHeightRange(-55.0, 1545.0);

    const TileKey fineKey{kScheme, 2, 2, 0};
    TilesetTile* fine = putTerrainTile(registry, *scheme, fineKey, 9.0f);
    fine->content.renderContent.setTerrainHeightRange(10.0, 20.0);

    const Rectangle coarseRect = scheme->tileToRectangle(coarseKey);
    const auto range = service.heightRangeForArea(coarseRect);
    ASSERT_TRUE(range.has_value());
    EXPECT_DOUBLE_EQ(range->first, -55.0);
    EXPECT_DOUBLE_EQ(range->second, 1545.0);
}

TEST(TerrainHeightServiceTest, HeightRangeForAreaSkipsTileWithoutMeasuredRange) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    // 索引只保证"有 heightmap";区间是另一条写入路径。缺了必须当这档没覆盖,
    // 而不是拿一对默认 0 冒充实测值。
    const TileKey coarseKey{kScheme, 0, 0, 0};
    TilesetTile* coarse = putTerrainTile(registry, *scheme, coarseKey, 3.0f);
    coarse->content.renderContent.setTerrainHeightRange(10.0, 20.0);

    const TileKey childKey{kScheme, 1, 0, 0};
    putTerrainTile(registry, *scheme, childKey, 5.0f);  // 有图,无区间

    const Rectangle childRect = scheme->tileToRectangle(childKey);
    const auto range = service.heightRangeForArea(childRect);
    ASSERT_TRUE(range.has_value());
    EXPECT_DOUBLE_EQ(range->first, 10.0);
    EXPECT_DOUBLE_EQ(range->second, 20.0);
}

TEST(TerrainHeightServiceTest, HeightRangeForAreaReturnsNulloptWithNoCoverage) {
    auto scheme = TileScheme::createGeographicTMS();
    TilesetTileRegistry registry;
    TerrainHeightService service(registry, *scheme);

    // 别处有货不算覆盖 → nullopt,调用方据此退回 loose 桶(宁可 10km 的体,
    // 也不能让路网整片消失)。
    const TileKey elsewhereKey{kScheme, 1, 1, 1};
    TilesetTile* elsewhere =
        putTerrainTile(registry, *scheme, elsewhereKey, 9.0f);
    elsewhere->content.renderContent.setTerrainHeightRange(1.0, 2.0);

    EXPECT_FALSE(service
                     .heightRangeForArea(scheme->tileToRectangle(
                         TileKey{kScheme, 1, 0, 0}))
                     .has_value());
}
