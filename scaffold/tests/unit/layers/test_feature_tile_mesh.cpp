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

// v1 边界:只收 fill/line。Point 要素不该产出任何几何 —— 它走 store 路径
// (要图集)。这条钉住边界,免得后人以为瓦片桶已经全类型覆盖。
TEST(FeatureTileMeshTest, PointFeaturesProduceNoTileGeometry) {
    FeatureRenderStyle style;
    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(106.5 * kDeg, 29.6 * kDeg)}};

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeContext(style, nullptr, nullptr), {point});
    EXPECT_TRUE(mesh.empty())
        << "Point 属 store 路径(需图集),瓦片桶 v1 不收";
}

// 空输入不该产出原点,commitTileMesh 会据此走 drop 分支。
TEST(FeatureTileMeshTest, EmptyInputYieldsNoOrigin) {
    FeatureRenderStyle style;
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        makeContext(style, nullptr, nullptr), {});
    EXPECT_FALSE(mesh.hasOrigin);
    EXPECT_TRUE(mesh.empty());
}
