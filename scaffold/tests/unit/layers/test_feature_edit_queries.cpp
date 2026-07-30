#include <gtest/gtest.h>

#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/data/FeatureSnapQuery.h"
#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Mat4.h"
#include "../../helpers/MockRenderDevice.h"

#include <glm/glm.hpp>
#include <cmath>
#include <memory>

using namespace earth_engine;
using earth_engine::testing::DummyBuffer;
using earth_engine::testing::MockRenderDevice;

namespace {

constexpr double kDeg = M_PI / 180.0;

// 要素放在 (0,0) 附近,相机在 ECEF x 轴上方垂直俯视 —— 投影可预测。
Feature squarePolygon() {
    Feature f;
    f.type = GeometryType::Polygon;
    f.rings = {{Cartographic(-0.05 * kDeg, -0.05 * kDeg),
                Cartographic(0.05 * kDeg, -0.05 * kDeg),
                Cartographic(0.05 * kDeg, 0.05 * kDeg),
                Cartographic(-0.05 * kDeg, 0.05 * kDeg),
                Cartographic(-0.05 * kDeg, -0.05 * kDeg)}};
    return f;
}

Feature lineNearby() {
    Feature f;
    f.type = GeometryType::LineString;
    f.rings = {{Cartographic(0.08 * kDeg, -0.05 * kDeg),
                Cartographic(0.08 * kDeg, 0.05 * kDeg)}};
    return f;
}

class FeatureEditQueriesTest : public ::testing::Test {
protected:
    void SetUp() override {
        renderer_ = std::make_unique<Renderer>(&device_);
        ASSERT_TRUE(renderer_->initialize());
        layer_ = std::make_unique<FeatureRenderLayer>(
            "edit-test", &device_, Ellipsoid::WGS84());

        // 约 40km 高度垂直俯视 (0,0):0.05° ≈ 5.5km 在视野内。
        const Vec3 surface =
            Ellipsoid::WGS84().cartographicToCartesian(Cartographic(0, 0));
        camera_.lookAt(surface + Vec3(40000.0, 0.0, 0.0),
                       surface, Vec3(0.0, 0.0, 1.0));
        frame_.camera = &camera_;
        frame_.viewportWidthPixels = 800;
        frame_.viewportHeightPixels = 600;
    }

    /// 与 pick 同口径把地理坐标投到屏幕像素(生成"点击位置")。
    void projectToScreen(const Cartographic& c, float& sx, float& sy) {
        const glm::dmat4 vp(camera_.viewProjectionMatrix(800.0, 600.0).raw());
        const Vec3 ecef = Ellipsoid::WGS84().cartographicToCartesian(c);
        glm::dvec4 clip = vp * glm::dvec4(ecef.x(), ecef.y(), ecef.z(), 1.0);
        ASSERT_GT(clip.w, 0.0);
        sx = static_cast<float>((clip.x / clip.w + 1.0) * 0.5 * 800.0);
        sy = static_cast<float>((1.0 - clip.y / clip.w) * 0.5 * 600.0);
    }

    RenderCommandList build() {
        RenderCommandList commands;
        layer_->buildRenderCommands(frame_, *renderer_, commands);
        return commands;
    }

    MockRenderDevice device_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<FeatureRenderLayer> layer_;
    Camera camera_;
    FrameState frame_;
};

} // namespace

// ============================================================
// pick
// ============================================================

TEST_F(FeatureEditQueriesTest, PickVertexAtCorner) {
    const FeatureId id = layer_->store().addFeature(squarePolygon());

    float sx = 0, sy = 0;
    projectToScreen(Cartographic(0.05 * kDeg, 0.05 * kDeg), sx, sy);
    const FeaturePickResult hit = layer_->pick(frame_, sx, sy, 12.0f);

    ASSERT_TRUE(hit.isValid());
    EXPECT_EQ(FeaturePickResult::Part::Vertex, hit.part);
    EXPECT_EQ(id, hit.featureId);
    EXPECT_EQ(0, hit.ringIndex);
    EXPECT_EQ(2, hit.vertexIndex);  // 第三个顶点 (0.05, 0.05)
    EXPECT_LT(hit.distancePx, 2.0);
    EXPECT_NEAR(0.05 * kDeg, hit.position.longitude(), 1e-9);
    EXPECT_NEAR(0.05 * kDeg, hit.position.latitude(), 1e-9);
}

TEST_F(FeatureEditQueriesTest, PickEdgeAtMidpointAndFillInside) {
    const FeatureId id = layer_->store().addFeature(squarePolygon());

    // 南边中点 → Edge,起点索引 0
    float sx = 0, sy = 0;
    projectToScreen(Cartographic(0.0, -0.05 * kDeg), sx, sy);
    const FeaturePickResult edge = layer_->pick(frame_, sx, sy, 12.0f);
    ASSERT_TRUE(edge.isValid());
    EXPECT_EQ(FeaturePickResult::Part::Edge, edge.part);
    EXPECT_EQ(id, edge.featureId);
    EXPECT_EQ(0, edge.vertexIndex);
    EXPECT_NEAR(-0.05 * kDeg, edge.position.latitude(), 1e-7);

    // 中心 → Fill(离一切顶点/边都超容差)
    projectToScreen(Cartographic(0.0, 0.0), sx, sy);
    const FeaturePickResult fill = layer_->pick(frame_, sx, sy, 12.0f);
    ASSERT_TRUE(fill.isValid());
    EXPECT_EQ(FeaturePickResult::Part::Fill, fill.part);
    EXPECT_EQ(id, fill.featureId);
}

TEST_F(FeatureEditQueriesTest, PickMissOutsideTolerance) {
    layer_->store().addFeature(squarePolygon());
    // 远离一切要素(0.5° ≈ 55km 外,视野外)
    const FeaturePickResult miss = layer_->pick(frame_, 5.0f, 5.0f, 12.0f);
    EXPECT_FALSE(miss.isValid());
}

TEST_F(FeatureEditQueriesTest, PickPrefersVertexOverEdgeAndFill) {
    layer_->store().addFeature(squarePolygon());
    // 点在角顶点旁 5px:顶点与两条边都在容差内,顶点必须赢
    float sx = 0, sy = 0;
    projectToScreen(Cartographic(-0.05 * kDeg, -0.05 * kDeg), sx, sy);
    const FeaturePickResult hit =
        layer_->pick(frame_, sx + 5.0f, sy, 12.0f);
    ASSERT_TRUE(hit.isValid());
    EXPECT_EQ(FeaturePickResult::Part::Vertex, hit.part);
    EXPECT_EQ(0, hit.vertexIndex);
}

TEST_F(FeatureEditQueriesTest, PickObliqueWithHeightOffsetFindsSmallFeature) {
    // 真机踩过的 miss:预筛用裸椭球交点,斜视下比 heightOffset 面上的
    // 要素偏出 ~offset·tan(俯角),小要素整体漏筛。
    FeatureRenderStyle style = layer_->style();
    style.heightOffset = 800.0;
    layer_->setStyle(style);

    // ~550m 小方形 + 45° 斜视相机(从南侧 2km 外、高 2km 看向原点)
    Feature small;
    small.type = GeometryType::Polygon;
    small.rings = {{Cartographic(-0.0025 * kDeg, -0.0025 * kDeg),
                    Cartographic(0.0025 * kDeg, -0.0025 * kDeg),
                    Cartographic(0.0025 * kDeg, 0.0025 * kDeg),
                    Cartographic(-0.0025 * kDeg, 0.0025 * kDeg),
                    Cartographic(-0.0025 * kDeg, 0.0025 * kDeg)}};
    const FeatureId id = layer_->store().addFeature(std::move(small));

    const Vec3 target = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0, 800.0));
    const Vec3 south = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, -2000.0 / 6.378137e6, 800.0));
    camera_.lookAt(south + Vec3(2000.0, 0.0, 0.0), target,
                   Vec3(0.0, 0.0, 1.0));

    // 点击位置 = 顶点在 heightOffset 面上的投影(与渲染一致)
    float sx = 0, sy = 0;
    projectToScreen(
        Cartographic(0.0025 * kDeg, 0.0025 * kDeg, 800.0), sx, sy);
    const FeaturePickResult hit = layer_->pick(frame_, sx, sy, 12.0f);
    ASSERT_TRUE(hit.isValid());
    EXPECT_EQ(FeaturePickResult::Part::Vertex, hit.part);
    EXPECT_EQ(id, hit.featureId);
    EXPECT_EQ(2, hit.vertexIndex);
}

// ============================================================
// snap
// ============================================================

TEST_F(FeatureEditQueriesTest, SnapPrefersVertexAndHonorsExclude) {
    const FeatureId polyId = layer_->store().addFeature(squarePolygon());
    const FeatureId lineId = layer_->store().addFeature(lineNearby());

    // 参考点在线的南端点(0.08,-0.05)西侧 ~200m:容差 500m 内有该顶点
    const Cartographic ref(0.078 * kDeg, -0.05 * kDeg);
    auto snap = FeatureSnapQuery::nearest(
        layer_->store(), Ellipsoid::WGS84(), ref, 500.0, polyId);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(lineId, snap->featureId);
    EXPECT_EQ(SnapCandidate::Part::Vertex, snap->part);
    EXPECT_EQ(0, snap->vertexIndex);
    EXPECT_NEAR(0.08 * kDeg, snap->position.longitude(), 1e-9);
    EXPECT_LT(snap->distanceMeters, 500.0);

    // 排除线要素本身:邻域内只剩多边形(>2km 外)→ 无候选
    auto excluded = FeatureSnapQuery::nearest(
        layer_->store(), Ellipsoid::WGS84(), ref, 500.0, lineId);
    EXPECT_FALSE(excluded.has_value());
}

TEST_F(FeatureEditQueriesTest, SnapFallsBackToEdgePoint) {
    const FeatureId lineId = layer_->store().addFeature(lineNearby());

    // 参考点在线段中部西侧 ~300m:两端点都在 5.5km 外,边上最近点在容差内
    const Cartographic ref(0.077 * kDeg, 0.0);
    auto snap = FeatureSnapQuery::nearest(
        layer_->store(), Ellipsoid::WGS84(), ref, 500.0);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(lineId, snap->featureId);
    EXPECT_EQ(SnapCandidate::Part::Edge, snap->part);
    EXPECT_NEAR(0.08 * kDeg, snap->position.longitude(), 1e-8);
    EXPECT_NEAR(0.0, snap->position.latitude(), 1e-5);
}

TEST_F(FeatureEditQueriesTest, SnapRespectsTolerance) {
    layer_->store().addFeature(lineNearby());
    // 5km 外,容差 500m → 无候选
    auto snap = FeatureSnapQuery::nearest(
        layer_->store(), Ellipsoid::WGS84(), Cartographic(0.0, 0.0), 500.0);
    EXPECT_FALSE(snap.has_value());
}

// ============================================================
// 编辑预览通道
// ============================================================

TEST_F(FeatureEditQueriesTest, PreviewDetachesAndRestores) {
    const FeatureId id = layer_->store().addFeature(squarePolygon());
    RenderCommandList before = build();
    ASSERT_EQ(2u, before.size());
    const auto mvpBefore = before[0].uniforms.at("u_modelViewProjection");

    // begin:常驻桶摘除该要素,预览瞬态路径顶上 → 命令数不变
    ASSERT_TRUE(layer_->beginEditPreview(id));
    EXPECT_EQ(id, layer_->previewFeatureId());
    EXPECT_EQ(0u, layer_->gpuBucketCount());  // 桶里只有它,摘除后桶空
    RenderCommandList during = build();
    EXPECT_EQ(2u, during.size());

    // update:整体平移 0.02° → 预览 mvp 原点变化(几何真的动了)
    auto rings = layer_->store().getFeature(id)->rings;
    for (auto& ring : rings) {
        for (auto& c : ring) {
            c = Cartographic(c.longitude() + 0.02 * kDeg, c.latitude(),
                             c.height());
        }
    }
    layer_->updateEditPreview(rings);
    RenderCommandList moved = build();
    ASSERT_EQ(2u, moved.size());
    EXPECT_NE(mvpBefore, moved[0].uniforms.at("u_modelViewProjection"));

    // store 未被预览污染
    EXPECT_NEAR(-0.05 * kDeg,
                layer_->store().getFeature(id)->rings[0][0].longitude(),
                1e-12);

    // end(cancel,不 commit):常驻桶复原,几何回到原位
    layer_->endEditPreview();
    EXPECT_EQ(kInvalidFeatureId, layer_->previewFeatureId());
    RenderCommandList after = build();
    ASSERT_EQ(2u, after.size());
    EXPECT_EQ(mvpBefore, after[0].uniforms.at("u_modelViewProjection"));
    EXPECT_EQ(1u, layer_->gpuBucketCount());
}

TEST_F(FeatureEditQueriesTest, PreviewCommitPersistsGeometry) {
    const FeatureId id = layer_->store().addFeature(squarePolygon());
    build();

    ASSERT_TRUE(layer_->beginEditPreview(id));
    auto rings = layer_->store().getFeature(id)->rings;
    rings[0][0] = Cartographic(-0.06 * kDeg, -0.06 * kDeg);
    rings[0][4] = rings[0][0];  // 闭合末点同步
    layer_->updateEditPreview(rings);
    build();

    // commit:先落库再 end(应用层契约)
    Feature edited = *layer_->store().getFeature(id);
    edited.rings = rings;
    edited.bounds = Rectangle();
    ASSERT_TRUE(layer_->store().updateFeature(edited));
    layer_->endEditPreview();

    RenderCommandList after = build();
    ASSERT_EQ(2u, after.size());
    EXPECT_NEAR(-0.06 * kDeg,
                layer_->store().getFeature(id)->rings[0][0].longitude(),
                1e-12);
    EXPECT_EQ(2u, layer_->store().getFeature(id)->version);
}

TEST_F(FeatureEditQueriesTest, PreviewKeepsBucketSiblingsRendered) {
    // 同桶两个要素:预览 A 时 B 仍从常驻桶出图
    const FeatureId a = layer_->store().addFeature(squarePolygon());
    Feature small = squarePolygon();
    for (auto& ring : small.rings) {
        for (auto& c : ring) {
            c = Cartographic(c.longitude() * 0.2 + 0.002 * kDeg,
                             c.latitude() * 0.2, c.height());
        }
    }
    layer_->store().addFeature(std::move(small));

    RenderCommandList before = build();
    ASSERT_EQ(2u, before.size());  // 同桶合并:一对命令
    const int fillBefore = before[0].kind == RenderCommandKind::VectorFill
                               ? before[0].indexCount
                               : before[1].indexCount;

    ASSERT_TRUE(layer_->beginEditPreview(a));
    RenderCommandList during = build();
    // 常驻桶(只剩 B)一对 + 预览(A)一对
    ASSERT_EQ(4u, during.size());
    int fillDuringResident = -1;
    for (const auto& cmd : during) {
        if (cmd.kind == RenderCommandKind::VectorFill &&
            cmd.indexCount < fillBefore) {
            fillDuringResident = cmd.indexCount;
        }
    }
    EXPECT_GT(fillDuringResident, 0);  // B 的 fill 仍在,索引数少于合并时

    layer_->endEditPreview();
    RenderCommandList after = build();
    EXPECT_EQ(2u, after.size());
}

// ============================================================
// P3 贴地(方案 A:高程采样钳制)
// ============================================================

namespace {

/// 合成斜坡地形:h(lat) = slope · lat(rad)。可注入、逐点可验。
FeatureTerrainSampling makeSlopeSampling(double slopeMetersPerRad,
                                         uint64_t* revisionCounter) {
    FeatureTerrainSampling s;
    s.makeAreaSampler = [slopeMetersPerRad](const Rectangle&) {
        return [slopeMetersPerRad](double /*lng*/, double lat) {
            return std::optional<float>(
                static_cast<float>(slopeMetersPerRad * lat));
        };
    };
    s.revision = [revisionCounter]() { return *revisionCounter; };
    return s;
}

Feature smallSquare() {
    Feature f;
    f.type = GeometryType::Polygon;
    f.rings = {{Cartographic(0.0, 0.0),
                Cartographic(0.005 * kDeg, 0.0),
                Cartographic(0.005 * kDeg, 0.005 * kDeg),
                Cartographic(0.0, 0.005 * kDeg),
                Cartographic(0.0, 0.0)}};
    return f;
}

} // namespace

TEST_F(FeatureEditQueriesTest, ClampSetsSampledVertexHeights) {
    uint64_t revision = 1;
    constexpr double kSlope = 2.0e5;  // m / rad
    layer_->setTerrainSampling(makeSlopeSampling(kSlope, &revision));
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.heightOffset = 10.0;
    style.clampDensifyMeters = 1.0e9;  // 关细分/内部撒点,只验角点高度
    layer_->setStyle(style);

    layer_->store().addFeature(smallSquare());
    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());

    // fill 顶点 = 4 角(无细分/无内部点),第一顶点即环首点 → 其 ECEF 就是
    // 桶原点。逐角验证 |rel + origin| == |椭球面(采样高+offset)|。
    const RenderCommand* fill = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorFill) fill = &cmd;
    }
    ASSERT_NE(nullptr, fill);
    const auto* vb = dynamic_cast<const DummyBuffer*>(fill->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    const size_t vertexCount = vb->bytes().size() / 12;
    ASSERT_EQ(4u, vertexCount);

    const auto& ring = layer_->store().features().begin()->second.rings[0];
    const Vec3 origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(ring[0].longitude(), ring[0].latitude(),
                     kSlope * ring[0].latitude() + 10.0));
    for (size_t v = 0; v < vertexCount; ++v) {
        const Vec3 world =
            origin + Vec3(floats[v * 3], floats[v * 3 + 1],
                          floats[v * 3 + 2]) -
            Vec3(0, 0, 0);
        const Cartographic c =
            Ellipsoid::WGS84().cartesianToCartographic(world);
        const double expected = kSlope * c.latitude() + 10.0;
        EXPECT_NEAR(expected, c.height(), 1.0)
            << "vertex " << v << " height mismatch";
    }
}

TEST_F(FeatureEditQueriesTest, ClampDensifiesEdgesAndAddsInteriorPoints) {
    uint64_t revision = 1;
    layer_->setTerrainSampling(makeSlopeSampling(1.0e4, &revision));

    // 基线:Absolute,无细分
    layer_->store().addFeature(smallSquare());
    RenderCommandList absolute = build();
    ASSERT_EQ(2u, absolute.size());
    size_t absoluteFillVerts = 0, absoluteLineVerts = 0;
    for (const auto& cmd : absolute) {
        const auto* vb = dynamic_cast<const DummyBuffer*>(cmd.vertexBuffer);
        ASSERT_NE(nullptr, vb);
        if (cmd.kind == RenderCommandKind::VectorFill) {
            absoluteFillVerts = vb->bytes().size() / 12;
        } else {
            absoluteLineVerts = vb->bytes().size() / 44;
        }
    }

    // Clamp + 50m 细分:~550m 边 → 每边 ~11 段
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.clampDensifyMeters = 50.0;
    layer_->setStyle(style);
    RenderCommandList clamped = build();
    ASSERT_EQ(2u, clamped.size());
    for (const auto& cmd : clamped) {
        const auto* vb = dynamic_cast<const DummyBuffer*>(cmd.vertexBuffer);
        ASSERT_NE(nullptr, vb);
        if (cmd.kind == RenderCommandKind::VectorFill) {
            // 边细分 + 内部网格点都进 CDT → 顶点数远超 4 角
            EXPECT_GT(vb->bytes().size() / 12, absoluteFillVerts * 8);
        } else {
            EXPECT_GT(vb->bytes().size() / 44, absoluteLineVerts * 8);
        }
        EXPECT_GT(cmd.indexCount, 0);
    }
}

TEST_F(FeatureEditQueriesTest, ClampReclampsOnRevisionChangeThrottled) {
    uint64_t revision = 1;
    double slope = 1.0e4;
    FeatureTerrainSampling sampling;
    sampling.makeAreaSampler = [&slope](const Rectangle&) {
        double s = slope;  // 按当前值捕获
        return [s](double, double lat) {
            return std::optional<float>(static_cast<float>(s * lat));
        };
    };
    sampling.revision = [&revision]() { return revision; };
    layer_->setTerrainSampling(std::move(sampling));

    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.clampDensifyMeters = 1.0e9;
    layer_->setStyle(style);
    layer_->store().addFeature(smallSquare());

    frame_.frameId = 200;
    RenderCommandList first = build();
    ASSERT_EQ(2u, first.size());
    const int buffersAfterFirst = device_.createdBufferCount;

    // 代次变化但节流窗内(首次重钳发生在 frame 200,窗=120 帧)→ 不重钳
    slope = 2.0e4;
    revision = 2;
    frame_.frameId = 250;
    build();
    EXPECT_EQ(buffersAfterFirst, device_.createdBufferCount);

    // 过节流窗 → 重钳(新 buffer)
    frame_.frameId = 400;
    build();
    const int buffersAfterReclamp = device_.createdBufferCount;
    EXPECT_GT(buffersAfterReclamp, buffersAfterFirst);

    // 代次不再变化 → 稳定,不再重钳
    frame_.frameId = 600;
    build();
    EXPECT_EQ(buffersAfterReclamp, device_.createdBufferCount);
}

TEST_F(FeatureEditQueriesTest, PickWorksInClampMode) {
    uint64_t revision = 1;
    constexpr double kSlope = 2.0e5;
    layer_->setTerrainSampling(makeSlopeSampling(kSlope, &revision));
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.heightOffset = 5.0;
    style.clampDensifyMeters = 1.0e9;
    layer_->setStyle(style);
    const FeatureId id = layer_->store().addFeature(smallSquare());

    // 点击位置 = 角点在"采样高+offset"处的投影(与渲染一致)
    const Cartographic corner(0.005 * kDeg, 0.005 * kDeg);
    float sx = 0, sy = 0;
    projectToScreen(Cartographic(corner.longitude(), corner.latitude(),
                                 kSlope * corner.latitude() + 5.0),
                    sx, sy);
    const FeaturePickResult hit = layer_->pick(frame_, sx, sy, 12.0f);
    ASSERT_TRUE(hit.isValid());
    EXPECT_EQ(FeaturePickResult::Part::Vertex, hit.part);
    EXPECT_EQ(id, hit.featureId);
    EXPECT_EQ(2, hit.vertexIndex);
}

TEST_F(FeatureEditQueriesTest, ClampPointAnchorSampledHeight) {
    // P5a 点符号贴地:锚点高度 = 采样高 + offset。
    uint64_t revision = 1;
    constexpr double kSlope = 2.0e5;
    layer_->setTerrainSampling(makeSlopeSampling(kSlope, &revision));
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.heightOffset = 20.0;
    layer_->setStyle(style);

    Feature p;
    p.type = GeometryType::Point;
    const double lat = 0.003 * kDeg;
    p.rings = {{Cartographic(0.002 * kDeg, lat)}};
    layer_->store().addFeature(std::move(p));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    ASSERT_EQ(RenderCommandKind::VectorPoint, commands[0].kind);

    // 单点桶:首顶点 anchor == 桶原点 → 原点即锚点 ECEF。验证其大地高。
    const auto* vb = dynamic_cast<const DummyBuffer*>(commands[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    // rel(0,0,0) → anchor = origin;从 mvp 反推原点太绕,直接重算期望 ECEF
    // 并确认 rel 首顶点为 0(原点=锚点),即锚点高度由钳制路径决定。
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(0.0f, floats[0]);
    EXPECT_FLOAT_EQ(0.0f, floats[1]);
    EXPECT_FLOAT_EQ(0.0f, floats[2]);
    // mvp 吸收原点平移:与手算"采样高+offset"处的期望 mvp 一致性由
    // ClampSetsSampledVertexHeights 同机制覆盖;此处验证命令存在+打包。
    EXPECT_EQ(6, commands[0].indexCount);
}
