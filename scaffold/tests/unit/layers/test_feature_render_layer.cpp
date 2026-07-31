#include <gtest/gtest.h>

#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "../../helpers/MockRenderDevice.h"

#include <cmath>
#include <cstring>
#include <memory>

using namespace earth_engine;
using earth_engine::testing::DummyBuffer;
using earth_engine::testing::MockRenderDevice;

namespace {

constexpr double kDeg = M_PI / 180.0;

Feature makePolygon(double lonDeg, double latDeg, double sizeDeg) {
    const double w = lonDeg * kDeg;
    const double s = latDeg * kDeg;
    const double e = (lonDeg + sizeDeg) * kDeg;
    const double n = (latDeg + sizeDeg) * kDeg;
    Feature f;
    f.type = GeometryType::Polygon;
    f.rings = {{Cartographic(w, s), Cartographic(e, s), Cartographic(e, n),
                Cartographic(w, n), Cartographic(w, s)}};
    return f;
}

Feature makeLine(double lonDeg, double latDeg, double spanDeg) {
    Feature f;
    f.type = GeometryType::LineString;
    f.rings = {{Cartographic(lonDeg * kDeg, latDeg * kDeg),
                Cartographic((lonDeg + spanDeg) * kDeg, latDeg * kDeg),
                Cartographic((lonDeg + spanDeg) * kDeg,
                             (latDeg + spanDeg) * kDeg)}};
    return f;
}

/// 测试夹具:MockRenderDevice + Renderer(initialize 建 shader)+ 相机帧。
class FeatureRenderLayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        renderer_ = std::make_unique<Renderer>(&device_);
        ASSERT_TRUE(renderer_->initialize());
        layer_ = std::make_unique<FeatureRenderLayer>(
            "test-features", &device_, Ellipsoid::WGS84());

        camera_.lookAt(Vec3(1.5e7, 0.0, 0.0), Vec3(0.0, 0.0, 0.0),
                       Vec3(0.0, 0.0, 1.0));
        frame_.camera = &camera_;
        frame_.frameId = 7;
        frame_.viewportWidthPixels = 800;
        frame_.viewportHeightPixels = 600;
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
// 命令生成契约
// ============================================================

TEST_F(FeatureRenderLayerTest, PolygonEmitsFillAndOutlineCommands) {
    layer_->store().addFeature(makePolygon(106.0, 29.0, 0.1));

    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());

    const RenderCommand* fill = nullptr;
    const RenderCommand* line = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorFill) fill = &cmd;
        if (cmd.kind == RenderCommandKind::VectorLine) line = &cmd;
    }
    ASSERT_NE(nullptr, fill);
    ASSERT_NE(nullptr, line);

    // 固定状态契约(与 validateMvpRenderCommands 的 VectorFill/Line 分支一致)
    for (const RenderCommand* cmd : {fill, line}) {
        EXPECT_EQ("color", cmd->pass);
        EXPECT_TRUE(cmd->depthTest);
        EXPECT_FALSE(cmd->depthWrite);
        EXPECT_FALSE(cmd->cullFace);
        EXPECT_TRUE(cmd->blend);
        EXPECT_EQ(RenderCommand::IndexType::UInt32, cmd->indexType);
        EXPECT_NE(nullptr, cmd->shader);
        EXPECT_NE(nullptr, cmd->vertexBuffer);
        EXPECT_NE(nullptr, cmd->indexBuffer);
        EXPECT_GT(cmd->indexCount, 0);
        EXPECT_EQ(7u, cmd->frameId);
        ASSERT_EQ(1u, cmd->uniforms.count("u_modelViewProjection"));
    }
    EXPECT_EQ(12, fill->vertexStride);
    EXPECT_EQ(44, line->vertexStride);
    ASSERT_EQ(1u, line->uniforms.count("u_viewport"));
    ASSERT_EQ(1u, line->uniforms.count("u_lineWidthPx"));
    // 方形外环 4 顶点闭合 ribbon:2n=8 顶点,6·段数=24 索引
    EXPECT_EQ(24, line->indexCount);
    // 方形 CDT:4 顶点 → 2 三角形 = 6 索引
    EXPECT_EQ(6, fill->indexCount);
}

TEST_F(FeatureRenderLayerTest, LineStringEmitsOnlyLineCommand) {
    layer_->store().addFeature(makeLine(106.0, 29.0, 0.05));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorLine, commands[0].kind);
    // open 3 顶点:2n=6 顶点,6·(n-1)=12 索引
    EXPECT_EQ(12, commands[0].indexCount);
}

TEST_F(FeatureRenderLayerTest, PointFeatureRendersBillboard) {
    // P5a:Point 要素 = billboard quad(4 顶点 6 索引,20B 顶点)。
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(106.0 * kDeg, 29.0 * kDeg)}};
    layer_->store().addFeature(std::move(p));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const RenderCommand& cmd = commands[0];
    EXPECT_EQ(RenderCommandKind::VectorPoint, cmd.kind);
    EXPECT_EQ(20, cmd.vertexStride);
    EXPECT_EQ(6, cmd.indexCount);
    EXPECT_EQ("color", cmd.pass);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_FALSE(cmd.depthWrite);
    EXPECT_TRUE(cmd.blend);
    ASSERT_EQ(1u, cmd.uniforms.count("u_pointSizePx"));
    ASSERT_EQ(1u, cmd.uniforms.count("u_viewport"));

    // 顶点打包:4 × (anchor rel 3f + corner 2f);首顶点 anchor = 桶原点
    // → rel(0,0,0),corner=(-1,-1)。
    const auto* vb =
        dynamic_cast<const earth_engine::testing::DummyBuffer*>(
            cmd.vertexBuffer);
    ASSERT_NE(nullptr, vb);
    ASSERT_EQ(4u * 20u, vb->bytes().size());
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(0.0f, floats[0]);
    EXPECT_FLOAT_EQ(0.0f, floats[1]);
    EXPECT_FLOAT_EQ(0.0f, floats[2]);
    EXPECT_FLOAT_EQ(-1.0f, floats[3]);
    EXPECT_FLOAT_EQ(-1.0f, floats[4]);
}

TEST_F(FeatureRenderLayerTest, MultiplePointsShareOneCommand) {
    for (int i = 0; i < 3; ++i) {
        Feature p;
        p.type = GeometryType::Point;
        p.rings = {{Cartographic((106.0 + i * 0.001) * kDeg, 29.0 * kDeg)}};
        layer_->store().addFeature(std::move(p));
    }
    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorPoint, commands[0].kind);
    EXPECT_EQ(18, commands[0].indexCount);  // 3 quad × 6
}

TEST_F(FeatureRenderLayerTest, InvisibleLayerEmitsNothing) {
    layer_->store().addFeature(makePolygon(106.0, 29.0, 0.1));
    layer_->setVisible(false);
    EXPECT_TRUE(build().empty());
}

// ============================================================
// 精度:顶点相对桶原点(RTE)
// ============================================================

TEST_F(FeatureRenderLayerTest, VerticesAreBucketOriginRelative) {
    // 0.1° ≈ 11km 要素:相对桶原点的顶点幅值必须在 ~10^5 m 以内,
    // 而 ECEF 绝对坐标是 ~6.4e6 m —— 判据区分两者(RTE 是否生效)。
    layer_->store().addFeature(makePolygon(106.0, 29.0, 0.1));
    RenderCommandList commands = build();
    ASSERT_FALSE(commands.empty());

    for (const auto& cmd : commands) {
        const auto* vb = dynamic_cast<const DummyBuffer*>(cmd.vertexBuffer);
        ASSERT_NE(nullptr, vb);
        const auto& bytes = vb->bytes();
        ASSERT_FALSE(bytes.empty());
        const int strideFloats = cmd.vertexStride / 4;
        const auto* floats = reinterpret_cast<const float*>(bytes.data());
        const size_t vertexCount = bytes.size() / cmd.vertexStride;
        // fill 前 3 float 是 pos;line 前 9 float 是 pos/prev/next,全部应
        // 是原点相对小量
        const int posFloats =
            cmd.kind == RenderCommandKind::VectorLine ? 9 : 3;
        for (size_t v = 0; v < vertexCount; ++v) {
            for (int i = 0; i < posFloats; ++i) {
                const float value = floats[v * strideFloats + i];
                EXPECT_LT(std::fabs(value), 1.0e6f)
                    << "vertex " << v << " component " << i
                    << " looks like absolute ECEF (RTE broken)";
            }
        }
    }

    // mvp 必须吸收原点平移:与直接 viewProj(float) 不同
    const auto& mvpU = commands[0].uniforms.at("u_modelViewProjection");
    ASSERT_EQ(16u, mvpU.size());
}

// ============================================================
// 脏桶增量重镶
// ============================================================

TEST_F(FeatureRenderLayerTest, DirtyBucketRebuildIsIncremental) {
    // 两个远隔要素 → 两个桶(cell 0.02rad,隔 >2° 必不同桶)
    const FeatureId idA =
        layer_->store().addFeature(makePolygon(106.0, 29.0, 0.1));
    layer_->store().addFeature(makePolygon(110.0, 33.0, 0.1));

    EXPECT_EQ(2, layer_->syncDirtyBuckets());
    EXPECT_EQ(2u, layer_->gpuBucketCount());
    const int buffersAfterInitial = device_.createdBufferCount;

    // 无脏区 → 不重镶不建 buffer
    EXPECT_EQ(0, layer_->syncDirtyBuckets());
    EXPECT_EQ(buffersAfterInitial, device_.createdBufferCount);

    // 编辑 A → 只有 A 的桶重镶(4 buffer:fill vb/ib + line vb/ib)
    Feature edited = *layer_->store().getFeature(idA);
    edited.bounds = Rectangle();  // 让 store 重算 bounds
    for (auto& ring : edited.rings) {
        for (auto& c : ring) {
            c = Cartographic(c.longitude() + 0.001, c.latitude(), c.height());
        }
    }
    ASSERT_TRUE(layer_->store().updateFeature(edited));
    EXPECT_EQ(1, layer_->syncDirtyBuckets());
    EXPECT_EQ(buffersAfterInitial + 4, device_.createdBufferCount);
    EXPECT_EQ(2u, layer_->gpuBucketCount());
}

TEST_F(FeatureRenderLayerTest, RemovingLastFeatureDropsBucket) {
    const FeatureId id =
        layer_->store().addFeature(makePolygon(106.0, 29.0, 0.1));
    layer_->syncDirtyBuckets();
    ASSERT_EQ(1u, layer_->gpuBucketCount());

    ASSERT_TRUE(layer_->store().removeFeature(id));
    layer_->syncDirtyBuckets();
    EXPECT_EQ(0u, layer_->gpuBucketCount());
    EXPECT_TRUE(build().empty());
}

TEST_F(FeatureRenderLayerTest, SameBucketFeaturesShareOneCommandPair) {
    // 两个近邻小要素落同桶 → 仍是一对 fill/line 命令(合桶绘制)
    layer_->store().addFeature(makePolygon(106.000, 29.000, 0.002));
    layer_->store().addFeature(makePolygon(106.003, 29.003, 0.002));

    RenderCommandList commands = build();
    EXPECT_EQ(1u, layer_->gpuBucketCount());
    ASSERT_EQ(2u, commands.size());
    // 两方形 fill 合并:2×6=12 索引
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorFill) {
            EXPECT_EQ(12, cmd.indexCount);
        }
    }
}

// ============================================================
// P5b 文字标注(SDF 字形图集 + VectorLabel)
// ============================================================

#include "earth_engine/renderer/GlyphAtlas.h"
#include <fstream>

namespace {

// host 侧真字体(TrueType glyf):按候选表读第一个能被 stbtt 解析的。
std::vector<uint8_t> loadHostFont() {
    const char* candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Geneva.ttf",
    };
    for (const char* path : candidates) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        if (!bytes.empty()) return bytes;
    }
    return {};
}

} // namespace

TEST(GlyphAtlasTest, DecodeUtf8MixedText) {
    const auto cps = GlyphAtlas::decodeUtf8("A中\xF0\x9F\x99\x82");
    ASSERT_EQ(3u, cps.size());
    EXPECT_EQ(0x41u, cps[0]);
    EXPECT_EQ(0x4E2Du, cps[1]);
    EXPECT_EQ(0x1F642u, cps[2]);
}

TEST(GlyphAtlasTest, RasterizesAndPacksGlyphs) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";

    earth_engine::testing::MockRenderDevice device;
    GlyphAtlas atlas(&device);
    if (!atlas.setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    ASSERT_TRUE(atlas.ready());
    EXPECT_GT(atlas.ascent(), 0.0f);

    const GlyphAtlas::Glyph* a = atlas.ensureGlyph('A');
    const GlyphAtlas::Glyph* b = atlas.ensureGlyph('B');
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);
    EXPECT_TRUE(a->hasBitmap);
    EXPECT_GT(a->advance, 0.0f);
    EXPECT_GT(a->width, 0.0f);
    // uv 合法且 A/B 不重叠(shelf 打包同一行左右排)
    EXPECT_GE(a->u0, 0.0f);
    EXPECT_LE(a->u1, 1.0f);
    EXPECT_LT(a->u0, a->u1);
    EXPECT_LE(a->u1, b->u0 + 1e-6f);
    // 空格:无位图只前进
    const GlyphAtlas::Glyph* space = atlas.ensureGlyph(' ');
    ASSERT_NE(nullptr, space);
    EXPECT_FALSE(space->hasBitmap);
    EXPECT_GT(space->advance, 0.0f);
    // 重复取:同一实例(缓存)
    EXPECT_EQ(a, atlas.ensureGlyph('A'));
}

TEST_F(FeatureRenderLayerTest, LabelCommandForNamedFeature) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }

    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(106.0 * kDeg, 29.0 * kDeg)}};
    p.properties["name"] = "AB";
    layer_->store().addFeature(std::move(p));

    RenderCommandList commands = build();
    const RenderCommand* label = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorLabel) label = &cmd;
    }
    ASSERT_NE(nullptr, label);
    EXPECT_EQ(32, label->vertexStride);  // P5c:+opacity(4)
    EXPECT_EQ(12, label->indexCount);  // 2 字形 × 6
    ASSERT_EQ(1u, label->textures.size());
    EXPECT_NE(nullptr, label->textures[0]);
    ASSERT_EQ(1u, label->uniforms.count("u_sdfEdge"));
    ASSERT_EQ(1u, label->uniforms.count("u_sdfHaloDelta"));
    EXPECT_EQ("color", label->pass);
    EXPECT_TRUE(label->blend);

    // 顶点打包:2 字形 × 4 顶点 × 32B;offsetPx 水平居中(首字形 x < 0)
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        label->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    ASSERT_EQ(2u * 4u * 32u, vb->bytes().size());
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_LT(floats[3], 0.0f);  // 首顶点 offsetPx.x 在锚点左侧
}

// ============================================================
// P5c 标签避让 placement(集成:镶嵌登记 → 逐帧避让 → opacity 重传)
// ============================================================

namespace {

/// 相机正对的地表点(相机在 +x 轴 1.5e7,lon=0/lat=0 即正面)。
Feature makeNamedPoint(double lonDeg, double latDeg, const char* name) {
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(lonDeg * kDeg, latDeg * kDeg)}};
    p.properties["name"] = name;
    return p;
}

} // namespace

/// 夹具扩展:注入字体 + 多帧推进(fade 收敛)。
class FeatureLabelPlacementTest : public FeatureRenderLayerTest {
protected:
    void SetUp() override {
        FeatureRenderLayerTest::SetUp();
        std::vector<uint8_t> font = loadHostFont();
        if (font.empty()) GTEST_SKIP() << "no host font available";
        if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
            GTEST_SKIP() << "host font not stbtt-parsable";
        }
        frame_.deltaSeconds = 0.1;
    }

    /// 推进 n 帧(每帧 0.1s;fade 0.3s,4 帧内必收敛)。
    void advanceFrames(int n) {
        for (int i = 0; i < n; ++i) {
            RenderCommandList commands;
            layer_->buildRenderCommands(frame_, *renderer_, commands);
            ++frame_.frameId;
        }
    }
};

TEST_F(FeatureLabelPlacementTest, OverlappingLabelsKeepOnlyOne) {
    // 两个几乎同点的标注要素:placement 只留一个,另一个 opacity 0。
    const FeatureId a =
        layer_->store().addFeature(makeNamedPoint(0.0, 0.0, "AAAA"));
    const FeatureId b =
        layer_->store().addFeature(makeNamedPoint(0.0001, 0.0, "BBBB"));

    advanceFrames(6);
    const auto& stats = layer_->labelPlacementStats();
    EXPECT_EQ(2, stats.candidates);
    EXPECT_EQ(1, stats.placed);
    EXPECT_EQ(1, stats.collided);
    // 距离几乎同,tie-break featureId 小者赢,且收敛到全显/全隐。
    EXPECT_FLOAT_EQ(1.0f, layer_->labelOpacityForFeature(a));
    EXPECT_FLOAT_EQ(0.0f, layer_->labelOpacityForFeature(b));
}

TEST_F(FeatureLabelPlacementTest, SeparatedLabelsBothPlaced) {
    // 拉开的两点(球面 5°≈550km,屏幕上远离):都显示。
    const FeatureId a =
        layer_->store().addFeature(makeNamedPoint(0.0, 5.0, "AAAA"));
    const FeatureId b =
        layer_->store().addFeature(makeNamedPoint(0.0, -5.0, "BBBB"));

    advanceFrames(6);
    EXPECT_EQ(2, layer_->labelPlacementStats().placed);
    EXPECT_FLOAT_EQ(1.0f, layer_->labelOpacityForFeature(a));
    EXPECT_FLOAT_EQ(1.0f, layer_->labelOpacityForFeature(b));
}

TEST_F(FeatureLabelPlacementTest, PriorityFeatureWinsCollision) {
    // 编辑联动:选中要素提权,重叠时后加入的选中者赢。
    const FeatureId a =
        layer_->store().addFeature(makeNamedPoint(0.0, 0.0, "AAAA"));
    const FeatureId b =
        layer_->store().addFeature(makeNamedPoint(0.0001, 0.0, "BBBB"));
    layer_->setLabelPriorityFeature(b);

    advanceFrames(6);
    EXPECT_FLOAT_EQ(0.0f, layer_->labelOpacityForFeature(a));
    EXPECT_FLOAT_EQ(1.0f, layer_->labelOpacityForFeature(b));
}

TEST_F(FeatureLabelPlacementTest, BackSideLabelHorizonCulled) {
    // 球背面要素(lon 180°,相机在 lon 0 上空):椭球地平线遮挡剔除。
    const FeatureId back =
        layer_->store().addFeature(makeNamedPoint(180.0, 0.0, "BACK"));

    advanceFrames(6);
    EXPECT_GE(layer_->labelPlacementStats().culledHorizon, 1);
    EXPECT_FLOAT_EQ(0.0f, layer_->labelOpacityForFeature(back));
}

TEST_F(FeatureLabelPlacementTest, FadeIsGradualAndUploadsStopWhenSettled) {
    const FeatureId a =
        layer_->store().addFeature(makeNamedPoint(0.0, 0.0, "AAAA"));

    // 首帧后 opacity 应在 (0,1) 之间(0.1s / 0.3s fade)。
    advanceFrames(1);
    const float first = layer_->labelOpacityForFeature(a);
    EXPECT_GT(first, 0.0f);
    EXPECT_LT(first, 1.0f);

    advanceFrames(5);
    EXPECT_FLOAT_EQ(1.0f, layer_->labelOpacityForFeature(a));

    // 收敛后不再重传顶点流。
    const int settled = device_.updatedBufferCount;
    advanceFrames(3);
    EXPECT_EQ(settled, device_.updatedBufferCount);

    // 顶点流里 opacity 分量(offset.z,每 8 float 下标 5)已写 1。
    RenderCommandList commands;
    layer_->buildRenderCommands(frame_, *renderer_, commands);
    const RenderCommand* label = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorLabel) label = &cmd;
    }
    ASSERT_NE(nullptr, label);
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        label->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    const size_t count = vb->bytes().size() / sizeof(float);
    for (size_t i = 5; i < count; i += 8) {
        EXPECT_FLOAT_EQ(1.0f, floats[i]);
    }
}

TEST_F(FeatureLabelPlacementTest, BucketRebuildResyncsSettledOpacity) {
    // 回归锁死(真机踩坑):桶重镶(setStyle/贴地 revision)把顶点流 opacity
    // 重置 0,而 placement fade 已收敛"无变化"——若同步依赖变化位早退,
    // 重镶后标签集体隐形。正确行为 = 按 appliedOpacity 偏差回写。
    const FeatureId a =
        layer_->store().addFeature(makeNamedPoint(0.0, 0.0, "AAAA"));
    advanceFrames(6);
    EXPECT_FLOAT_EQ(1.0f, layer_->labelOpacityForFeature(a));

    // setStyle 触发全桶重镶(顶点流 opacity 归 0)。
    layer_->setStyle(layer_->style());
    advanceFrames(1);

    RenderCommandList commands;
    layer_->buildRenderCommands(frame_, *renderer_, commands);
    const RenderCommand* label = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorLabel) label = &cmd;
    }
    ASSERT_NE(nullptr, label);
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        label->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    const size_t count = vb->bytes().size() / sizeof(float);
    for (size_t i = 5; i < count; i += 8) {
        EXPECT_FLOAT_EQ(1.0f, floats[i]);
    }
}

TEST_F(FeatureRenderLayerTest, NoLabelWithoutFontOrName) {
    // 字体未注入:有 name 也不出标注
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(106.0 * kDeg, 29.0 * kDeg)}};
    p.properties["name"] = "X";
    layer_->store().addFeature(std::move(p));
    for (const auto& cmd : build()) {
        EXPECT_NE(RenderCommandKind::VectorLabel, cmd.kind);
    }
}
