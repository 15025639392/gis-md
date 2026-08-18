#include <gtest/gtest.h>

#include "MinimalGlobeDemoConfig.h"
#include "earth_engine/data/StyleFilter.h"
#include "earth_engine/data/VectorRasterStyle.h"

#include <string>
#include <utility>

// 建筑/次级路 zoom+距离分级的意图锁定。测的是**真配置函数**
// (makeMvtDrapeStyle / makeMvtRoadFieldStyle),不复制分级逻辑 —— 配置一旦
// 改错(门槛挪位、类型漏列、catch-all 覆盖面变),这里就红。距离经页 zoom
// 隐含,故 zoom 轴即距离轴。

using earth_engine::StyleFilter;
using earth_engine::VectorRasterStyle;
using earth_engine::minimal_globe_demo::makeMvtDrapeStyle;
using earth_engine::minimal_globe_demo::makeMvtRoadFieldStyle;

namespace {

StyleFilter::PropertyMap props(
    std::initializer_list<std::pair<const std::string, std::string>> init) {
    return StyleFilter::PropertyMap(init);
}

// 该层在给定 zoom 下是否会绘制此要素:zoom 落在 [minZoom,maxZoom] 且
// filter 命中(空 filter = 全收)。
bool paintDraws(const earth_engine::VectorRasterLayerPaint& paint,
                const StyleFilter::PropertyMap& p, double zoom) {
    if (zoom < paint.minZoom || zoom > paint.maxZoom) return false;
    return !paint.filter || paint.filter->matches(&p, zoom);
}

// 整套样式里任一 paint 会画到此要素即视为可见(paint 之间是并集)。
bool visible(const VectorRasterStyle& style,
             const StyleFilter::PropertyMap& p, double zoom) {
    for (const auto& paint : style.layers) {
        if (paintDraws(paint, p, zoom)) return true;
    }
    return false;
}

const earth_engine::VectorRasterLayerPaint& layerNamed(
    const VectorRasterStyle& style, const std::string& name) {
    for (const auto& paint : style.layers) {
        if (paint.layer == name) return paint;
    }
    ADD_FAILURE() << "layer not found: " << name;
    static earth_engine::VectorRasterLayerPaint kEmpty;
    return kEmpty;
}

} // namespace

// ── 建筑分级 ────────────────────────────────────────────────────────────
// 数据只有 building 类型 + 可选 name(无体量字段),按"是否地标"代理体量。

TEST(MvtBasemapGrading, BuildingGenericHiddenUntilNear) {
    const auto style = makeMvtDrapeStyle();
    const auto& building = layerNamed(style, "building");
    const auto generic = props({{"building", "yes"}});
    // z13-14 中距:generic 民房不出。
    EXPECT_FALSE(paintDraws(building, generic, 13.0));
    EXPECT_FALSE(paintDraws(building, generic, 14.0));
    // z>=15 近景:全部铺满。
    EXPECT_TRUE(paintDraws(building, generic, 15.0));
    EXPECT_TRUE(paintDraws(building, generic, 16.0));
}

TEST(MvtBasemapGrading, BuildingLandmarkTypeShowsAtMidDistance) {
    const auto style = makeMvtDrapeStyle();
    const auto& building = layerNamed(style, "building");
    // 地标类型 z13 就出(代理大体量)。
    for (const char* t : {"commercial", "hospital", "school", "stadium",
                          "apartments", "university"}) {
        const auto p = props({{"building", t}});
        EXPECT_TRUE(paintDraws(building, p, 13.0)) << "type=" << t;
    }
}

TEST(MvtBasemapGrading, BuildingNamedShowsAtMidDistance) {
    const auto style = makeMvtDrapeStyle();
    const auto& building = layerNamed(style, "building");
    // 命名建筑即便 generic 类型也算地标,z13 出。
    const auto named = props({{"building", "yes"}, {"name", "某大厦"}});
    EXPECT_TRUE(paintDraws(building, named, 13.0));
}

TEST(MvtBasemapGrading, BuildingFloorBelowZoom13) {
    const auto style = makeMvtDrapeStyle();
    const auto& building = layerNamed(style, "building");
    // minZoom=13 地板:更远整层不烘,连地标/命名建筑都不出。
    const auto landmark = props({{"building", "hospital"}, {"name", "X"}});
    EXPECT_FALSE(paintDraws(building, landmark, 12.0));
}

// ── 道路分级(含次级路细分)──────────────────────────────────────────────

namespace {
bool roadVisible(const VectorRasterStyle& style, const char* highway,
                 double zoom) {
    const auto p = props({{"highway", highway}});
    return visible(style, p, zoom);
}
} // namespace

TEST(MvtBasemapGrading, TrunkAlwaysPrimaryFromZoom10) {
    const auto style = makeMvtRoadFieldStyle();
    EXPECT_TRUE(roadVisible(style, "motorway", 8.0));
    EXPECT_TRUE(roadVisible(style, "trunk", 8.0));
    EXPECT_FALSE(roadVisible(style, "primary", 8.0));
    EXPECT_TRUE(roadVisible(style, "primary", 10.0));
}

TEST(MvtBasemapGrading, SecondaryTertiaryTieredEntry) {
    const auto style = makeMvtRoadFieldStyle();
    // secondary: z12 进。
    EXPECT_FALSE(roadVisible(style, "secondary", 11.0));
    EXPECT_TRUE(roadVisible(style, "secondary", 12.0));
    // tertiary: z13 进。
    EXPECT_FALSE(roadVisible(style, "tertiary", 12.0));
    EXPECT_TRUE(roadVisible(style, "tertiary", 13.0));
}

TEST(MvtBasemapGrading, ResidentialSplitFromMinorAtZoom14) {
    const auto style = makeMvtRoadFieldStyle();
    // residential/unclassified/living_street: z14 进(次级路细分的中近档)。
    for (const char* h : {"residential", "unclassified", "living_street"}) {
        EXPECT_FALSE(roadVisible(style, h, 13.0)) << "highway=" << h;
        EXPECT_TRUE(roadVisible(style, h, 14.0)) << "highway=" << h;
    }
}

TEST(MvtBasemapGrading, MinorRoadsOnlyAtNearest) {
    const auto style = makeMvtRoadFieldStyle();
    // service/track/footway 等最末梢: z14 仍不出,z15 catch-all 才放开。
    for (const char* h : {"service", "track", "footway", "path"}) {
        EXPECT_FALSE(roadVisible(style, h, 14.0)) << "highway=" << h;
        EXPECT_TRUE(roadVisible(style, h, 15.0)) << "highway=" << h;
    }
}

// ---- V26 尾项:sources.json 解析契约(fail-loud) ----
// 四态:全量覆盖 / 部分覆盖(缺键=内置)/ 未知键整份拒收 / 坏 JSON 拒收。
// 拒收时 out 必须不动(半应用的源配置比没配置更难排查)。

using earth_engine::minimal_globe_demo::DemoSourceOverrides;
using earth_engine::minimal_globe_demo::parseDemoSourceOverrides;
using earth_engine::minimal_globe_demo::makeDefaultDemoSceneConfig;

TEST(DemoSourceOverridesParse, FullAndPartial) {
    DemoSourceOverrides ov;
    std::string err;
    ASSERT_TRUE(parseDemoSourceOverrides(
        R"({"mvtUrlTemplate":"http://h/{z}/{x}/{y}.pbf",)"
        R"("imageryUrlTemplate":"http://i/{x}/{y}/{z}",)"
        R"("terrainUrlTemplate":"http://t/{z}/{x}/{y}.png"})",
        ov, err)) << err;
    EXPECT_EQ("http://h/{z}/{x}/{y}.pbf", ov.mvtUrlTemplate);
    EXPECT_EQ("http://i/{x}/{y}/{z}", ov.imageryUrlTemplate);
    EXPECT_EQ("http://t/{z}/{x}/{y}.png", ov.terrainUrlTemplate);

    DemoSourceOverrides partial;
    ASSERT_TRUE(parseDemoSourceOverrides(
        R"({"mvtUrlTemplate":"http://only-mvt/{z}/{x}/{y}.pbf"})", partial,
        err)) << err;
    EXPECT_EQ("http://only-mvt/{z}/{x}/{y}.pbf", partial.mvtUrlTemplate);
    EXPECT_TRUE(partial.imageryUrlTemplate.empty());
    EXPECT_TRUE(partial.terrainUrlTemplate.empty());
}

TEST(DemoSourceOverridesParse, FailLoudLeavesOutUntouched) {
    DemoSourceOverrides ov;
    ov.mvtUrlTemplate = "sentinel";  // 拒收时必须原样留住
    std::string err;
    // 未知键(zoom 参数不归文档)整份拒收。
    EXPECT_FALSE(parseDemoSourceOverrides(
        R"({"mvtUrlTemplate":"http://x/","mvtMaxZoom":14})", ov, err));
    EXPECT_NE(std::string::npos, err.find("mvtMaxZoom")) << err;
    EXPECT_EQ("sentinel", ov.mvtUrlTemplate);
    // 非字符串值拒收。
    EXPECT_FALSE(
        parseDemoSourceOverrides(R"({"mvtUrlTemplate":42})", ov, err));
    EXPECT_EQ("sentinel", ov.mvtUrlTemplate);
    // 坏 JSON / 非对象拒收。
    EXPECT_FALSE(parseDemoSourceOverrides("not json", ov, err));
    EXPECT_FALSE(parseDemoSourceOverrides(R"(["array"])", ov, err));
    EXPECT_EQ("sentinel", ov.mvtUrlTemplate);
}

TEST(DemoSourceOverridesParse, FactoryAppliesUrlOnly) {
    // 工厂只换 URL,tileSize/zoom 等源参数不随文档动(外置源须与编译期
    // 分支同构)。空字段 = 内置不动。
    const auto base = makeDefaultDemoSceneConfig();
    DemoSourceOverrides ov;
    ov.terrainUrlTemplate = "http://alt-terrain/{z}/{x}/{y}.png";
    const auto cfg = makeDefaultDemoSceneConfig(&ov);
    EXPECT_EQ("http://alt-terrain/{z}/{x}/{y}.png", cfg.terrain.urlTemplate);
    EXPECT_EQ(base.terrain.tileSize, cfg.terrain.tileSize);
    EXPECT_EQ(base.terrain.minimumZoom, cfg.terrain.minimumZoom);
    EXPECT_EQ(base.terrain.maximumZoom, cfg.terrain.maximumZoom);
    ASSERT_EQ(base.rasterOverlays.size(), cfg.rasterOverlays.size());
    if (!cfg.rasterOverlays.empty()) {
        EXPECT_EQ(base.rasterOverlays[0].urlTemplate,
                  cfg.rasterOverlays[0].urlTemplate)
            << "imagery 未给 override 不该变";
    }
}
