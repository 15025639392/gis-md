#include <gtest/gtest.h>

#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"

using namespace earth_engine;

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;

Feature makePolygon(double lonDeg, double latDeg) {
    Feature f;
    f.type = GeometryType::Polygon;
    f.rings = {{Cartographic(lonDeg * kDeg, latDeg * kDeg),
                Cartographic((lonDeg + 0.01) * kDeg, latDeg * kDeg),
                Cartographic((lonDeg + 0.01) * kDeg, (latDeg + 0.01) * kDeg),
                Cartographic(lonDeg * kDeg, (latDeg + 0.01) * kDeg)}};
    return f;
}

Feature makeLine(double lonDeg, double latDeg) {
    Feature f;
    f.type = GeometryType::LineString;
    f.rings = {{Cartographic(lonDeg * kDeg, latDeg * kDeg),
                Cartographic((lonDeg + 0.02) * kDeg, (latDeg + 0.005) * kDeg),
                Cartographic((lonDeg + 0.03) * kDeg, (latDeg + 0.02) * kDeg)}};
    return f;
}

std::vector<Feature> sampleFeatures() {
    return {makePolygon(106.5, 29.6), makeLine(106.52, 29.61),
            makePolygon(106.54, 29.62)};
}

FeatureRenderLayer::TessellationContext makeContext(
    const FeatureRenderStyle& style, GlyphAtlas* glyph, IconAtlas* icon) {
    return FeatureRenderLayer::TessellationContext{
        style, Ellipsoid::WGS84(), glyph, icon, /*stencil=*/false};
}

} // namespace

// E1 的核心不变量:**fill/line 镶嵌全程不解引用图集**。
//
// 这条是把 fill/line 搬上 worker 的全部依据 —— 图集必须在渲染线程
// (GlyphAtlas::ensureGlyph 现场栅格化 + 传纹理)。检验方式就是把图集置
// nullptr 跑一遍:若 Polygon/LineString 路径碰了图集,这里当场段错误。
//
// ⚠️ 曾试过「渲染上下文传非零假指针、与 worker 产物对拍」,那是错的:
// label 发射的判断在类型 switch **之外**,对任何要素都跑,假指针非空即
// 进 ->ready() 崩掉 —— 测的是 label 路径,不是 fill/line。
TEST(FeatureTileMeshTest, FillAndLineNeverTouchAtlases) {
    FeatureRenderStyle style;
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeContext(style, nullptr, nullptr), sampleFeatures());

    ASSERT_TRUE(mesh.hasOrigin);
    EXPECT_FALSE(mesh.fillIndices.empty()) << "polygon 应产出 fill";
    EXPECT_FALSE(mesh.lineIndices.empty()) << "polygon 外环 + line 应产出 line";
}

// MVT 要素普遍带 name 属性(路名等)。label 发射的空图集短路必须真的短路,
// 否则 worker 侧一碰带名要素就崩 —— 这是接线后最先炸的地方。
TEST(FeatureTileMeshTest, NamedFeaturesSurviveNullAtlasOnWorker) {
    FeatureRenderStyle style;
    std::vector<Feature> features = sampleFeatures();
    for (Feature& f : features) {
        f.properties["name"] = std::string("测试路");
    }
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeContext(style, nullptr, nullptr), features);
    EXPECT_FALSE(mesh.empty()) << "带 name 的要素照常产出 fill/line";
}

// 镶嵌是纯函数:同一输入两次调用逐位相等。worker 并发跑多块瓦片时,
// 结果不得依赖调用顺序或任何残留状态。
TEST(FeatureTileMeshTest, TessellationIsDeterministic) {
    FeatureRenderStyle style;
    const std::vector<Feature> features = sampleFeatures();
    const auto ctx = makeContext(style, nullptr, nullptr);

    const auto a = FeatureRenderLayer::tessellateTileMesh(ctx, features);
    const auto b = FeatureRenderLayer::tessellateTileMesh(ctx, features);
    EXPECT_EQ(a.fillVerts, b.fillVerts);
    EXPECT_EQ(a.fillIndices, b.fillIndices);
    EXPECT_EQ(a.lineVerts, b.lineVerts);
    EXPECT_EQ(a.lineIndices, b.lineIndices);
}

// 符号刀A:Point 要素在 worker 产 TileSymbolCpu 实例表,**不产任何顶点**
// (quad 定型要图集,留渲染线程准入)。锚点存大地坐标而非 ECEF:贴地的
// 地形采样也是渲染线程状态。全程零图集解引用 —— 图集传 nullptr 即验证。
TEST(FeatureTileMeshTest, PointFeaturesEmitSymbolInstancesNotGeometry) {
    FeatureRenderStyle style;
    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(106.5 * kDeg, 29.6 * kDeg)}};
    point.properties["name"] = "解放碑";
    point.properties["rank"] = "3";

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeContext(style, nullptr, nullptr), {point});

    ASSERT_EQ(1u, mesh.symbols.size());
    const TileSymbolCpu& s = mesh.symbols[0];
    EXPECT_DOUBLE_EQ(106.5 * kDeg, s.lonRad);
    EXPECT_DOUBLE_EQ(29.6 * kDeg, s.latRad);
    EXPECT_EQ("解放碑", s.name);
    EXPECT_EQ(3, s.rank);
    EXPECT_NE(0.0f, s.colorPacked) << "worker 应已求值样式色并打包";
    EXPECT_TRUE(mesh.hasOrigin) << "纯符号瓦片也要有 RTE 原点";
    EXPECT_FALSE(mesh.empty()) << "实例表非空的瓦片不得被 drop";
    EXPECT_TRUE(mesh.fillIndices.empty() && mesh.lineIndices.empty())
        << "Point 不产几何顶点(定型在准入)";
}

// 属性缺省的回落:无 rank 属性 → 默认 6(最不重要,截断时先丢);
// 无 name → 空串(刀B 文字期不出标签,但图标照常)。
TEST(FeatureTileMeshTest, SymbolInstanceDefaultsWithoutProperties) {
    FeatureRenderStyle style;
    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(106.5 * kDeg, 29.6 * kDeg)}};

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeContext(style, nullptr, nullptr), {point});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(6, mesh.symbols[0].rank);
    EXPECT_TRUE(mesh.symbols[0].name.empty());
}

// 空输入不该产出原点,commitTileMesh 会据此走 drop 分支。
TEST(FeatureTileMeshTest, EmptyInputYieldsNoOrigin) {
    FeatureRenderStyle style;
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeContext(style, nullptr, nullptr), {});
    EXPECT_FALSE(mesh.hasOrigin);
    EXPECT_TRUE(mesh.empty());
}

// ---------------------------------------------------------------------------
// 贴地(ClampToGround)走区域高度范围,零地形采样
//
// 背景:worker 拿不到地形采样器(渲染线程状态),所以 v1 的瓦片桶只能走
// Absolute。改用「区域 min/max 高度」这一对标量后,worker 能自己建 stencil
// 挤出体 —— 体只要覆盖住地形高度范围即可,stencil 是像素级判定,不需要每个
// 顶点贴合精确地面高度(同 cesium ApproximateTerrainHeights 的取舍)。
// ---------------------------------------------------------------------------

namespace {

FeatureRenderLayer::TessellationContext makeClampContext(
    const FeatureRenderStyle& style, double minH, double maxH) {
    FeatureRenderLayer::TessellationContext ctx{
        style, Ellipsoid::WGS84(), nullptr, nullptr, /*stencil=*/true};
    ctx.hasTerrainHeightRange = true;
    ctx.terrainMinHeight = minH;
    ctx.terrainMaxHeight = maxH;
    return ctx;
}

/// 顶点流是 [x,y,z, ...] 每顶点 6 float(位置 + 挤出),取位置到原点的距离
/// 范围 —— 用它反推体在径向上的跨度。
std::pair<double, double> radialSpan(const std::vector<float>& verts,
                                     const Vec3& origin) {
    double lo = 1e300, hi = -1e300;
    for (size_t i = 0; i + 2 < verts.size(); i += 6) {
        const Vec3 p(origin.x() + verts[i], origin.y() + verts[i + 1],
                     origin.z() + verts[i + 2]);
        const double r = p.length();
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    return {lo, hi};
}

} // namespace

// 核心不变量:贴地镶嵌**一次地形采样都不做**。worker 上没有采样器,所以
// 这里不是性能优化而是可行性前提 —— 之前正是因为要采样,底图贴地这条路
// 才走不通(旧 store 路径实测单帧 235s)。
TEST(FeatureTileMeshTest, ClampedTessellationNeverSamplesTerrain) {
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.fillColor = {0.2f, 0.5f, 0.9f, 0.5f};
    style.lineColor = {1.0f, 1.0f, 1.0f, 1.0f};

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeClampContext(style, /*minH=*/200.0, /*maxH=*/1500.0),
        sampleFeatures());

    // 产物落在 stencil 体里,而不是普通 fill/line 流(两条路互斥)。
    EXPECT_FALSE(mesh.fillVolumeGroups.empty() && mesh.lineVolumeGroups.empty())
        << "贴地时应产出 stencil 挤出体";
    EXPECT_TRUE(mesh.fillIndices.empty())
        << "走 stencil 就不该再产方案 A 的 fill(同内容画两遍)";
    EXPECT_TRUE(mesh.lineIndices.empty())
        << "走 stencil 就不该再产方案 A 的 line ribbon";
    EXPECT_TRUE(mesh.hasOrigin);
}

// 体必须**纵向穿透整个地形高度范围**。取窄了体埋在地下或浮在空中,stencil
// 判定不到任何地形像素 —— 现象是该瓦片的路网整片消失(不是变淡)。
TEST(FeatureTileMeshTest, ClampVolumeSpansTerrainHeightRange) {
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.lineColor = {1.0f, 1.0f, 1.0f, 1.0f};

    constexpr double kMinH = 200.0;
    constexpr double kMaxH = 1500.0;
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeClampContext(style, kMinH, kMaxH), {makeLine(106.52, 29.61)});

    ASSERT_FALSE(mesh.lineVolumeGroups.empty());
    const auto& group = mesh.lineVolumeGroups.begin()->second;
    const auto span = radialSpan(group.verts, mesh.origin);
    const double spanMeters = span.second - span.first;
    // 体高至少覆盖 (maxH - minH);margin 让它更高,但不该低于范围本身。
    EXPECT_GT(spanMeters, kMaxH - kMinH)
        << "体没穿透地形范围,该区域会整片不显示";
    // 也不该离谱地高(range + 2×margin 量级),否则是白烧 fill rate。
    EXPECT_LT(spanMeters, (kMaxH - kMinH) + 1000.0);
}

// 地形范围随瓦片变化 → 体高度跟着变。恒定体高说明范围没接进来。
TEST(FeatureTileMeshTest, FlatTerrainYieldsShorterVolumeThanMountainous) {
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.lineColor = {1.0f, 1.0f, 1.0f, 1.0f};

    const auto flat = FeatureRenderLayer::tessellateTileMesh(
        makeClampContext(style, 0.0, 10.0), {makeLine(106.52, 29.61)});
    const auto hilly = FeatureRenderLayer::tessellateTileMesh(
        makeClampContext(style, 0.0, 2000.0), {makeLine(106.52, 29.61)});

    ASSERT_FALSE(flat.lineVolumeGroups.empty());
    ASSERT_FALSE(hilly.lineVolumeGroups.empty());
    const double flatSpan = [&] {
        const auto s = radialSpan(flat.lineVolumeGroups.begin()->second.verts,
                                  flat.origin);
        return s.second - s.first;
    }();
    const double hillySpan = [&] {
        const auto s = radialSpan(hilly.lineVolumeGroups.begin()->second.verts,
                                  hilly.origin);
        return s.second - s.first;
    }();
    EXPECT_GT(hillySpan, flatSpan + 1500.0) << "体高没跟随地形范围";
}

// 不给范围 = 保持此前行为(Absolute 几何,不产体)。这条守住向后兼容:
// 既有调用方(demo 的 Absolute 底图)不该因为本次改动而变。
TEST(FeatureTileMeshTest, WithoutHeightRangeFallsBackToAbsolute) {
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::Absolute;
    style.heightOffset = 500.0;
    style.lineColor = {1.0f, 1.0f, 1.0f, 1.0f};

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeContext(style, nullptr, nullptr), {makeLine(106.52, 29.61)});
    EXPECT_TRUE(mesh.lineVolumeGroups.empty());
    EXPECT_FALSE(mesh.lineIndices.empty());
}
