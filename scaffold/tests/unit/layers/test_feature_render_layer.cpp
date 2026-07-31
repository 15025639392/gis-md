#include <gtest/gtest.h>

#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/renderer/IconAtlas.h"
#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/renderer/SymbolShape.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "../../helpers/MockRenderDevice.h"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

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
        // P6c 图标图集把 region 上传成败当契约(失败 = 不登记 frame)。
        device_.textureRegionUploadSucceeds = true;
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
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));

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
    EXPECT_EQ(16, fill->vertexStride);  // P6b:+color(RGBA8)
    EXPECT_EQ(48, line->vertexStride);  // P6b:+color(RGBA8)
    ASSERT_EQ(1u, line->uniforms.count("u_viewport"));
    ASSERT_EQ(1u, line->uniforms.count("u_lineWidthPx"));
    // 方形外环 4 顶点闭合 ribbon:2n=8 顶点,6·段数=24 索引
    EXPECT_EQ(24, line->indexCount);
    // 方形 CDT:4 顶点 → 2 三角形 = 6 索引
    EXPECT_EQ(6, fill->indexCount);
}

TEST_F(FeatureRenderLayerTest, LineStringEmitsOnlyLineCommand) {
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorLine, commands[0].kind);
    // open 3 顶点:2n=6 顶点,6·(n-1)=12 索引
    EXPECT_EQ(12, commands[0].indexCount);
}

TEST_F(FeatureRenderLayerTest, PointFeatureRendersBillboard) {
    // P5a/P6c:Point 要素 = billboard quad(4 顶点 6 索引,36B 顶点)。
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    layer_->store().addFeature(std::move(p));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const RenderCommand& cmd = commands[0];
    EXPECT_EQ(RenderCommandKind::VectorPoint, cmd.kind);
    EXPECT_EQ(36, cmd.vertexStride);  // P6b:+color;P6c:+uv/shape
    EXPECT_EQ(6, cmd.indexCount);
    EXPECT_EQ("color", cmd.pass);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_FALSE(cmd.depthWrite);
    EXPECT_TRUE(cmd.blend);
    ASSERT_EQ(1u, cmd.uniforms.count("u_pointSizePx"));
    ASSERT_EQ(1u, cmd.uniforms.count("u_viewport"));

    // 顶点打包:4 × (anchor rel 3f + offsetUnit 2f + uv 2f + color 4B +
    // shape 1f);首顶点 anchor = 桶原点 → rel(0,0,0),corner=(-1,-1) →
    // 默认 circle 居中锚定 → offsetUnit(-0.5,-0.5),uv 局部(-1,-1)。
    const auto* vb =
        dynamic_cast<const earth_engine::testing::DummyBuffer*>(
            cmd.vertexBuffer);
    ASSERT_NE(nullptr, vb);
    ASSERT_EQ(4u * 36u, vb->bytes().size());
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(0.0f, floats[0]);
    EXPECT_FLOAT_EQ(0.0f, floats[1]);
    EXPECT_FLOAT_EQ(0.0f, floats[2]);
    EXPECT_FLOAT_EQ(-0.5f, floats[3]);
    EXPECT_FLOAT_EQ(-0.5f, floats[4]);
    EXPECT_FLOAT_EQ(-1.0f, floats[5]);
    EXPECT_FLOAT_EQ(-1.0f, floats[6]);
    EXPECT_FLOAT_EQ(0.0f, floats[8]);  // shape = circle
}

TEST_F(FeatureRenderLayerTest, MultiplePointsShareOneCommand) {
    for (int i = 0; i < 3; ++i) {
        Feature p;
        p.type = GeometryType::Point;
        p.rings = {{Cartographic((6.0 + i * 0.001) * kDeg, 29.0 * kDeg)}};
        layer_->store().addFeature(std::move(p));
    }
    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorPoint, commands[0].kind);
    EXPECT_EQ(18, commands[0].indexCount);  // 3 quad × 6
}

TEST_F(FeatureRenderLayerTest, OutOfHorizonBucketEmitsNoCommands) {
    // 视口桶裁剪:相机(星下点 0°E/0°N,高 ~8.6e6m,地平线角 ~65°)看不到
    // 的桶不出命令。视野内 polygon 出 fill+outline 两条;150°E 的桶被裁。
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));
    layer_->store().addFeature(makePolygon(150.0, 0.0, 0.1));

    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    for (const auto& cmd : commands) {
        EXPECT_TRUE(cmd.kind == RenderCommandKind::VectorFill ||
                    cmd.kind == RenderCommandKind::VectorLine);
    }
}

TEST_F(FeatureRenderLayerTest, OversizedFeatureDrawnRegardlessOfView) {
    // 超大要素(bounds 跨 cell)归 oversized 桶,视口查询恒纳入——即便
    // 其中心在地平线外,也保守出命令(不可漏画)。
    layer_->store().addFeature(makePolygon(150.0, 0.0, 5.0));

    RenderCommandList commands = build();
    EXPECT_EQ(2u, commands.size());  // fill + outline
}

TEST_F(FeatureRenderLayerTest, SymbolDepthPushedToNearAtHighAltitude) {
    // 高空(fixture 相机大地高 ~8.6e6m > 200km 阈值):点符号命令携带
    // u_depthPushNdc > 0 —— VS 把深度顶到近平面,防 billboard 锚点常数
    // 深度被地形逐像素深度斜切/吞没(真机 2.6e7m 实测半切复现)。
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    layer_->store().addFeature(std::move(p));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    ASSERT_EQ(1u, commands[0].uniforms.count("u_depthPushNdc"));
    EXPECT_GT(commands[0].uniforms.at("u_depthPushNdc")[0], 0.9f);
}

TEST_F(FeatureRenderLayerTest, SymbolDepthTestedNormallyAtLowAltitude) {
    // 低空(5km < 200km 阈值):u_depthPushNdc = 0,保留地形遮挡语义
    // (billboard 斜视被前方地面按锚点深度遮挡是既定行为)。
    const auto eye = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 5000.0));
    camera_.lookAt(eye, Vec3::zero(), Vec3(0.0, 0.0, 1.0));

    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    layer_->store().addFeature(std::move(p));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    ASSERT_EQ(1u, commands[0].uniforms.count("u_depthPushNdc"));
    EXPECT_FLOAT_EQ(0.0f, commands[0].uniforms.at("u_depthPushNdc")[0]);
}

TEST_F(FeatureRenderLayerTest, InvisibleLayerEmitsNothing) {
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));
    layer_->setVisible(false);
    EXPECT_TRUE(build().empty());
}

// ============================================================
// 精度:顶点相对桶原点(RTE)
// ============================================================

TEST_F(FeatureRenderLayerTest, VerticesAreBucketOriginRelative) {
    // 0.1° ≈ 11km 要素:相对桶原点的顶点幅值必须在 ~10^5 m 以内,
    // 而 ECEF 绝对坐标是 ~6.4e6 m —— 判据区分两者(RTE 是否生效)。
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));
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
        layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));
    layer_->store().addFeature(makePolygon(10.0, 33.0, 0.1));

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
        layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));
    layer_->syncDirtyBuckets();
    ASSERT_EQ(1u, layer_->gpuBucketCount());

    ASSERT_TRUE(layer_->store().removeFeature(id));
    layer_->syncDirtyBuckets();
    EXPECT_EQ(0u, layer_->gpuBucketCount());
    EXPECT_TRUE(build().empty());
}

TEST_F(FeatureRenderLayerTest, SameBucketFeaturesShareOneCommandPair) {
    // 两个近邻小要素落同桶 → 仍是一对 fill/line 命令(合桶绘制)
    layer_->store().addFeature(makePolygon(6.000, 29.000, 0.002));
    layer_->store().addFeature(makePolygon(6.003, 29.003, 0.002));

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
    p.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
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

TEST_F(FeatureLabelPlacementTest, BackSideLabelBucketCulledBeforePlacement) {
    // 球背面要素(lon 180°,相机在 lon 0 上空):桶级视口裁剪先于逐标签
    // 地平线剔除——背面桶不进候选(candidates=0),标签不显示。逐标签
    // 地平线剔除路径(地平线圆内、近缘的候选)由 test_label_placement
    // 单元级覆盖。
    const FeatureId back =
        layer_->store().addFeature(makeNamedPoint(180.0, 0.0, "BACK"));

    advanceFrames(6);
    EXPECT_EQ(0, layer_->labelPlacementStats().candidates);
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

// ============================================================
// P6 stencil 终态贴地(方案 B:挤出体双 pass 分类)
// ============================================================

namespace {

/// 合成地形采样:恒定 50m(体积高度范围可手算)。
FeatureTerrainSampling makeFlatSampling(float height) {
    FeatureTerrainSampling s;
    s.makeAreaSampler = [height](const Rectangle&) {
        return [height](double, double) -> std::optional<float> {
            return height;
        };
    };
    s.revision = []() -> uint64_t { return 1; };
    return s;
}

} // namespace

TEST_F(FeatureRenderLayerTest, StencilVolumePairForClampedPolygon) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyle(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    layer_->store().addFeature(makePolygon(0.0, 0.0, 0.01));

    RenderCommandList commands = build();
    // 命令对相邻:体 pass 在前、色 pass 在后,几何同源。
    const RenderCommand* vol = nullptr;
    const RenderCommand* col = nullptr;
    for (size_t i = 0; i < commands.size(); ++i) {
        if (commands[i].kind == RenderCommandKind::VectorStencil) {
            vol = &commands[i];
            ASSERT_LT(i + 1, commands.size());
            col = &commands[i + 1];
            break;
        }
    }
    ASSERT_NE(nullptr, vol);
    ASSERT_EQ(RenderCommandKind::VectorStencil, col->kind);
    EXPECT_EQ(StencilPhase::ClassifyVolume, vol->stencilPhase);
    EXPECT_EQ(StencilPhase::ClassifyColor, col->stencilPhase);
    // 体 pass:深度测开写关、不混合、双面;色 pass:关深度测、开混合。
    EXPECT_TRUE(vol->depthTest);
    EXPECT_FALSE(vol->depthWrite);
    EXPECT_FALSE(vol->blend);
    EXPECT_FALSE(vol->cullFace);
    EXPECT_FALSE(col->depthTest);
    EXPECT_FALSE(col->depthWrite);
    EXPECT_TRUE(col->blend);
    EXPECT_EQ(vol->vertexBuffer, col->vertexBuffer);
    EXPECT_EQ(vol->indexBuffer, col->indexBuffer);
    EXPECT_EQ(12, vol->vertexStride);

    // 方形 footprint 水密体:cap 4 顶点×2 层 + 4 边墙×4 顶点 = 24 顶点;
    // 索引 = 2 cap×2 三角×3 + 4 墙×6 = 36。
    const auto* vb = dynamic_cast<const DummyBuffer*>(vol->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    EXPECT_EQ(24u * 12u, vb->bytes().size());
    EXPECT_EQ(36, vol->indexCount);

    // stencil 模式下不再产出方案 A 的采样钳制 fill。
    for (const auto& cmd : commands) {
        EXPECT_NE(RenderCommandKind::VectorFill, cmd.kind);
    }

    // 底/顶两层高差 = (50-120) 到 (50+120) = 240m(体积罩住采样面)。
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    // cap 顶点相对桶原点,还原绝对长度比较壳层半径。
    // 原点 = 首个底 cap 顶点(绝对) → rel(0)=0。比较底层与顶层首顶点。
    // 无法直接拿 origin,改比较底/顶 cap 对应顶点的 rel 差向量长度。
    double shell = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double d = static_cast<double>(floats[4 * 3 + i]) -
                         static_cast<double>(floats[0 + i]);
        shell += d * d;
    }
    EXPECT_NEAR(240.0, std::sqrt(shell), 1.0);

    // 状态校验通过(两 phase 各自规则 + order 29 在其它矢量之前)。
    const auto validation = validateMvpRenderCommands(commands, frame_.frameId);
    EXPECT_FALSE(validation.has_value())
        << (validation ? validation->message : "");
}

TEST_F(FeatureRenderLayerTest, StencilFallsBackToSamplingWithoutSupport) {
    device_.stencilClassificationSupported = false;
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyle(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    layer_->store().addFeature(makePolygon(0.0, 0.0, 0.01));

    RenderCommandList commands = build();
    bool hasFill = false;
    for (const auto& cmd : commands) {
        EXPECT_NE(RenderCommandKind::VectorStencil, cmd.kind);
        if (cmd.kind == RenderCommandKind::VectorFill) hasFill = true;
    }
    EXPECT_TRUE(hasFill);  // 回落方案 A(采样钳制 fill)
}

TEST_F(FeatureRenderLayerTest, AbsoluteModePolygonHasNoStencilVolume) {
    // Absolute 模式与 stencil 无关:普通 fill,无分类命令。
    layer_->store().addFeature(makePolygon(0.0, 0.0, 0.01));
    for (const auto& cmd : build()) {
        EXPECT_NE(RenderCommandKind::VectorStencil, cmd.kind);
    }
}

// ============================================================
// P6d stencil 贴地线(墙带体双 pass 分类)
// ============================================================

namespace {

/// 流形断言:体积网格任意无向边必须恰被 2 个三角形引用(水密;z-fail
/// 计数正确性的机器可检查前提)。
void expectWatertight(const RenderCommand& vol) {
    const auto* ib =
        dynamic_cast<const DummyBuffer*>(vol.indexBuffer);
    ASSERT_NE(nullptr, ib);
    const auto* idx = reinterpret_cast<const uint32_t*>(ib->bytes().data());
    const size_t idxCount = ib->bytes().size() / sizeof(uint32_t);
    ASSERT_EQ(static_cast<size_t>(vol.indexCount), idxCount);
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t t = 0; t + 2 < idxCount; t += 3) {
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = idx[t + e];
            const uint32_t b = idx[t + (e + 1) % 3];
            ++edges[{std::min(a, b), std::max(a, b)}];
        }
    }
    for (const auto& [edge, count] : edges) {
        EXPECT_EQ(2, count) << "edge " << edge.first << "-" << edge.second
                            << " referenced " << count << "x";
    }
}

} // namespace

TEST_F(FeatureRenderLayerTest, StencilLineVolumePairForClampedLineString) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyle(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    layer_->store().addFeature(makeLine(0.0, 0.0, 0.05));

    RenderCommandList commands = build();
    const RenderCommand* vol = nullptr;
    const RenderCommand* col = nullptr;
    for (size_t i = 0; i < commands.size(); ++i) {
        if (commands[i].kind == RenderCommandKind::VectorStencil) {
            vol = &commands[i];
            ASSERT_LT(i + 1, commands.size());
            col = &commands[i + 1];
            break;
        }
    }
    ASSERT_NE(nullptr, vol);
    ASSERT_EQ(RenderCommandKind::VectorStencil, col->kind);
    EXPECT_EQ(StencilPhase::ClassifyVolume, vol->stencilPhase);
    EXPECT_EQ(StencilPhase::ClassifyColor, col->stencilPhase);
    EXPECT_EQ(24, vol->vertexStride);  // pos(12)+extrude(12)
    EXPECT_EQ(vol->vertexBuffer, col->vertexBuffer);
    // 宽度挤出 uniform 齐备(mvp + modelView + 每米眼深半宽)。
    ASSERT_EQ(1u, vol->uniforms.count("u_modelViewProjection"));
    ASSERT_EQ(1u, vol->uniforms.count("u_modelView"));
    ASSERT_EQ(1u, vol->uniforms.count("u_halfWidthPerEyeZ"));
    // stencil 模式下不再产出方案 A 的线 ribbon。
    for (const auto& cmd : commands) {
        EXPECT_NE(RenderCommandKind::VectorLine, cmd.kind);
    }
    // 开放墙带:n 横截面 → 8(n-1) 墙三角 + 4 端 cap 三角。
    const auto* vb = dynamic_cast<const DummyBuffer*>(vol->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const size_t sections = vb->bytes().size() / (4 * 24);
    ASSERT_GE(sections, 2u);
    EXPECT_EQ(static_cast<int>(8 * (sections - 1) + 4) * 3, vol->indexCount);
    expectWatertight(*vol);

    const auto validation = validateMvpRenderCommands(commands, frame_.frameId);
    EXPECT_FALSE(validation.has_value())
        << (validation ? validation->message : "");
}

TEST_F(FeatureRenderLayerTest, ClampedPolygonOutlineBecomesClosedLineVolume) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyle(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    layer_->store().addFeature(makePolygon(0.0, 0.0, 0.01));

    RenderCommandList commands = build();
    // fill 体(stride 12)与 outline 墙带(stride 24)各一对,共 4 条。
    const RenderCommand* fillVol = nullptr;
    const RenderCommand* lineVol = nullptr;
    int stencilCount = 0;
    for (const auto& cmd : commands) {
        if (cmd.kind != RenderCommandKind::VectorStencil) continue;
        ++stencilCount;
        if (cmd.stencilPhase != StencilPhase::ClassifyVolume) continue;
        if (cmd.vertexStride == 12) fillVol = &cmd;
        if (cmd.vertexStride == 24) lineVol = &cmd;
    }
    EXPECT_EQ(4, stencilCount);
    ASSERT_NE(nullptr, fillVol);
    ASSERT_NE(nullptr, lineVol);
    // 闭合墙带:n 横截面 wrap → 8n 三角,无端 cap。
    const auto* vb = dynamic_cast<const DummyBuffer*>(lineVol->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const size_t sections = vb->bytes().size() / (4 * 24);
    ASSERT_GE(sections, 3u);
    EXPECT_EQ(static_cast<int>(8 * sections) * 3, lineVol->indexCount);
    expectWatertight(*lineVol);

    const auto validation = validateMvpRenderCommands(commands, frame_.frameId);
    EXPECT_FALSE(validation.has_value())
        << (validation ? validation->message : "");
}

TEST_F(FeatureRenderLayerTest, StencilLineDensifyDecoupledFromSchemeA) {
    // 细分解耦:方案 A 档位(8m,防扎地)不该传染 stencil 线——stencil
    // 贴地是像素级分类,细分只服务曲率/高度采样,放宽到 ≥100m。
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.clampDensifyMeters = 8.0;
    layer_->setStyle(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    // 两段各 ~5.6km:100m 细分 → ~113 横截面;8m 会是 ~1400。
    layer_->store().addFeature(makeLine(0.0, 0.0, 0.05));

    RenderCommandList commands = build();
    const RenderCommand* vol = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorStencil &&
            cmd.stencilPhase == StencilPhase::ClassifyVolume) {
            vol = &cmd;
            break;
        }
    }
    ASSERT_NE(nullptr, vol);
    const auto* vb = dynamic_cast<const DummyBuffer*>(vol->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const size_t sections = vb->bytes().size() / (4 * 24);
    EXPECT_GE(sections, 100u);
    EXPECT_LE(sections, 200u);
    expectWatertight(*vol);
}

TEST_F(FeatureRenderLayerTest, StencilLineFallsBackToRibbonWithoutSupport) {
    device_.stencilClassificationSupported = false;
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyle(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    layer_->store().addFeature(makeLine(0.0, 0.0, 0.05));

    RenderCommandList commands = build();
    bool hasLine = false;
    for (const auto& cmd : commands) {
        EXPECT_NE(RenderCommandKind::VectorStencil, cmd.kind);
        if (cmd.kind == RenderCommandKind::VectorLine) hasLine = true;
    }
    EXPECT_TRUE(hasLine);  // 回落方案 A(采样钳制 ribbon)
}

// ============================================================
// P6b 样式表达式(数据驱动顶点色 + zoom 驱动宽度/尺寸)
// ============================================================

namespace {

/// 顶点流末位 float 的 RGBA8 位模式还原(小端 R 低字节)。
std::array<int, 4> unpackVertexColor(float packed) {
    uint32_t bits;
    std::memcpy(&bits, &packed, sizeof(bits));
    return {static_cast<int>(bits & 0xFF),
            static_cast<int>((bits >> 8) & 0xFF),
            static_cast<int>((bits >> 16) & 0xFF),
            static_cast<int>((bits >> 24) & 0xFF)};
}

Feature makeKindPoint(double lonDeg, const char* kind) {
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(lonDeg * kDeg, 29.0 * kDeg)}};
    p.properties["kind"] = kind;
    return p;
}

} // namespace

TEST_F(FeatureRenderLayerTest, DataDrivenPointColorBakedPerFeature) {
    FeatureRenderStyle style = layer_->style();
    style.pointColorExpr = StyleExpression::match(
        "kind",
        {{"tower", StyleExpression::literal({1.0f, 0.0f, 0.0f, 1.0f})}},
        StyleExpression::literal({0.0f, 0.0f, 1.0f, 1.0f}));
    layer_->setStyle(style);
    layer_->store().addFeature(makeKindPoint(6.0, "tower"));
    layer_->store().addFeature(makeKindPoint(6.001, "gate"));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        commands[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    // 2 要素 × 4 顶点 × 9 float(36B):颜色在每顶点下标 7。
    ASSERT_EQ(2u * 4u * 36u, vb->bytes().size());
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    const auto c0 = unpackVertexColor(floats[7]);           // 要素1 tower
    const auto c1 = unpackVertexColor(floats[4 * 9 + 7]);   // 要素2 gate
    EXPECT_EQ((std::array<int, 4>{255, 0, 0, 255}), c0);
    EXPECT_EQ((std::array<int, 4>{0, 0, 255, 255}), c1);
    // u_color 已退役(顶点色接管)。
    EXPECT_EQ(0u, commands[0].uniforms.count("u_color"));
}

TEST_F(FeatureRenderLayerTest, ZoomDrivenLineWidthUniform) {
    FeatureRenderStyle style = layer_->style();
    style.lineWidthExpr = StyleExpression::interpolateLinear(
        StyleExpression::zoom(),
        {{0.0, StyleExpression::literal(2.0)},
         {24.0, StyleExpression::literal(26.0)}});
    layer_->setStyle(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    // 相机高 ~8.6e6m → zoom = log2(4e7/高) ≈ 2.2 → 宽度 ≈ 2 + 2.2 ≈ 4.2
    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    ASSERT_EQ(1u, commands[0].uniforms.count("u_lineWidthPx"));
    const float width = commands[0].uniforms.at("u_lineWidthPx")[0];
    EXPECT_GT(width, 2.0f);
    EXPECT_LT(width, 8.0f);
}

TEST_F(FeatureRenderLayerTest, StencilVolumesGroupedByResolvedColor) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.fillColorExpr = StyleExpression::match(
        "kind",
        {{"tower", StyleExpression::literal({1.0f, 0.0f, 0.0f, 0.5f})}},
        StyleExpression::literal({0.0f, 0.0f, 1.0f, 0.5f}));
    layer_->setStyle(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));

    Feature a = makePolygon(0.0, 0.0, 0.01);
    a.properties["kind"] = "tower";
    Feature b = makePolygon(0.02, 0.0, 0.01);
    b.properties["kind"] = "gate";
    layer_->store().addFeature(std::move(a));
    layer_->store().addFeature(std::move(b));

    // 两个解析色 → 两对 Volume/Color 命令,色 pass u_color 分别取组色。
    RenderCommandList commands = build();
    int volumePasses = 0;
    std::vector<float> reds;
    for (size_t i = 0; i < commands.size(); ++i) {
        const auto& cmd = commands[i];
        if (cmd.kind != RenderCommandKind::VectorStencil) continue;
        // 只统计 fill 挤出体(stride 12);outline 墙带(stride 24)归
        // P6d 测试段管。
        if (cmd.vertexStride != 12) continue;
        if (cmd.stencilPhase == StencilPhase::ClassifyVolume) {
            ++volumePasses;
            ASSERT_LT(i + 1, commands.size());
            const auto& col = commands[i + 1];
            ASSERT_EQ(StencilPhase::ClassifyColor, col.stencilPhase);
            reds.push_back(col.uniforms.at("u_color")[0]);
        }
    }
    EXPECT_EQ(2, volumePasses);
    ASSERT_EQ(2u, reds.size());
    // 两组色都在(要素可能分属不同桶,组序不保证)。
    std::sort(reds.begin(), reds.end());
    EXPECT_FLOAT_EQ(0.0f, reds[0]);
    EXPECT_FLOAT_EQ(1.0f, reds[1]);
}

TEST_F(FeatureRenderLayerTest, OutOfScopeExpressionsFallBackToLiterals) {
    FeatureRenderStyle style = layer_->style();
    style.pointColor = {0.0f, 1.0f, 0.0f, 1.0f};
    // 颜色引用 zoom(禁)→ 剥离降级字面量;宽度引用属性(禁)→ 同。
    style.pointColorExpr = StyleExpression::interpolateLinear(
        StyleExpression::zoom(),
        {{0.0, StyleExpression::literal({1.0f, 0.0f, 0.0f, 1.0f})},
         {24.0, StyleExpression::literal({0.0f, 0.0f, 1.0f, 1.0f})}});
    style.lineWidthExpr = StyleExpression::get("width");
    layer_->setStyle(style);
    EXPECT_EQ(nullptr, layer_->style().pointColorExpr);
    EXPECT_EQ(nullptr, layer_->style().lineWidthExpr);

    layer_->store().addFeature(makeKindPoint(6.0, "tower"));
    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        commands[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_EQ((std::array<int, 4>{0, 255, 0, 255}),
              unpackVertexColor(floats[7]));  // 字面量绿
}

TEST_F(FeatureRenderLayerTest, NoLabelWithoutFontOrName) {
    // 字体未注入:有 name 也不出标注
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    p.properties["name"] = "X";
    layer_->store().addFeature(std::move(p));
    for (const auto& cmd : build()) {
        EXPECT_NE(RenderCommandKind::VectorLabel, cmd.kind);
    }
}

// ============================================================
// P6c 图标/Marker(内置解析 SDF 形状 + 位图图集通道)
// ============================================================

namespace {

/// 纯色 RGBA8 位图(w*h*4)。
std::vector<uint8_t> solidRgba(int w, int h, uint8_t r) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 255);
    for (size_t i = 0; i < px.size(); i += 4) px[i] = r;
    return px;
}

Feature makePointAt(double lonDeg) {
    Feature p;
    p.type = GeometryType::Point;
    p.rings = {{Cartographic(lonDeg * kDeg, 29.0 * kDeg)}};
    return p;
}

} // namespace

TEST(IconAtlasTest, PacksNamedImagesAndRejectsBadInput) {
    earth_engine::testing::MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    IconAtlas atlas(&device);
    EXPECT_TRUE(atlas.empty());
    EXPECT_EQ(nullptr, atlas.texture());

    ASSERT_TRUE(atlas.addImage("a", 16, 8, solidRgba(16, 8, 200)));
    EXPECT_FALSE(atlas.empty());
    EXPECT_NE(nullptr, atlas.texture());
    EXPECT_EQ(1u, atlas.revision());

    const IconAtlas::Frame* a = atlas.frame("a");
    ASSERT_NE(nullptr, a);
    EXPECT_FLOAT_EQ(16.0f, a->widthPx);
    EXPECT_FLOAT_EQ(8.0f, a->heightPx);
    EXPECT_LT(a->u0, a->u1);
    EXPECT_LT(a->v0, a->v1);

    // 第二张 shelf 右排,不与第一张重叠。
    ASSERT_TRUE(atlas.addImage("b", 16, 8, solidRgba(16, 8, 10)));
    const IconAtlas::Frame* b = atlas.frame("b");
    ASSERT_NE(nullptr, b);
    EXPECT_GE(b->u0, a->u1);
    EXPECT_EQ(2u, atlas.revision());

    // 非法输入不改状态。
    EXPECT_FALSE(atlas.addImage("c", 4, 4, solidRgba(2, 2, 0)));  // 字节数不符
    EXPECT_FALSE(atlas.addImage("", 4, 4, solidRgba(4, 4, 0)));   // 空名
    EXPECT_FALSE(atlas.addImage("d", 0, 4, {}));                  // 尺寸非法
    EXPECT_FALSE(atlas.addImage("e", 4096, 4, solidRgba(4, 4, 0)));  // 超页
    EXPECT_EQ(nullptr, atlas.frame("c"));
    EXPECT_EQ(2u, atlas.revision());
}

TEST(IconAtlasTest, PageFullRejectIsCountedNotSilent) {
    // 溢出可观测:页满拒绝必须留下计数(此前只是静默 return false,产品上
    // 表现为"图标莫名回落圆点")。512²+2px padding = 514 格:一行只放下
    // 1 张,第 2 张换行后 514+514 超页高被拒 → 计数 1;非法输入拒绝不计入。
    earth_engine::testing::MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    IconAtlas atlas(&device);
    EXPECT_EQ(0, atlas.pageFullRejectCount());

    ASSERT_TRUE(atlas.addImage("p1", 512, 512, solidRgba(512, 512, 1)));
    EXPECT_FALSE(atlas.addImage("p2", 512, 512, solidRgba(512, 512, 2)));
    EXPECT_EQ(1, atlas.pageFullRejectCount());

    EXPECT_FALSE(atlas.addImage("", 4, 4, solidRgba(4, 4, 0)));  // 非法输入
    EXPECT_EQ(1, atlas.pageFullRejectCount());
}

TEST_F(FeatureRenderLayerTest, BuiltinShapeBakedIntoVertexShape) {
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "star";
    layer_->setStyle(style);
    layer_->store().addFeature(makePointAt(6.0));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        commands[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(static_cast<float>(static_cast<int>(SymbolShape::Star)),
                    f[8]);
    // 居中锚定:四角 offsetUnit.y 覆盖 [-0.5, 0.5]。
    EXPECT_FLOAT_EQ(-0.5f, f[4]);
    EXPECT_FLOAT_EQ(0.5f, f[2 * 9 + 4]);
}

TEST_F(FeatureRenderLayerTest, PinShapeIsBottomAnchored) {
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "pin";
    layer_->setStyle(style);
    layer_->store().addFeature(makePointAt(6.0));

    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        build()[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(static_cast<float>(static_cast<int>(SymbolShape::Pin)),
                    f[8]);
    // 底部锚定:quad 整个画在锚点上方 → offsetUnit.y ∈ [0, 1]。
    EXPECT_FLOAT_EQ(0.0f, f[4]);
    EXPECT_FLOAT_EQ(1.0f, f[2 * 9 + 4]);
}

TEST_F(FeatureRenderLayerTest, UnknownImageNameFallsBackToCircle) {
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "no-such-icon";
    layer_->setStyle(style);
    layer_->store().addFeature(makePointAt(6.0));

    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        build()[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    // 图标缺失不该让要素消失:回落 circle 仍可见。
    EXPECT_FLOAT_EQ(0.0f, f[8]);
}

TEST_F(FeatureRenderLayerTest, AtlasIconBakesUvAspectAndBindsTexture) {
    // 图集图标:宽高比 2:1 → 半宽 1.0(高恒 1);shape 为负走采样分支。
    ASSERT_TRUE(renderer_->iconAtlas()->addImage("marker", 32, 16,
                                                 solidRgba(32, 16, 128)));
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "marker";
    layer_->setStyle(style);
    layer_->store().addFeature(makePointAt(6.0));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const RenderCommand& cmd = commands[0];
    ASSERT_EQ(1u, cmd.textures.size());
    EXPECT_EQ(renderer_->iconAtlas()->texture(), cmd.textures[0]);

    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        cmd.vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    const IconAtlas::Frame* frame = renderer_->iconAtlas()->frame("marker");
    ASSERT_NE(nullptr, frame);
    EXPECT_LT(f[8], 0.0f);                 // 图集哨兵
    EXPECT_FLOAT_EQ(-1.0f, f[3]);          // 半宽 = 0.5 * (32/16)
    EXPECT_FLOAT_EQ(-0.5f, f[4]);          // 默认居中
    EXPECT_FLOAT_EQ(frame->u0, f[5]);      // 左下角 → u0
    EXPECT_FLOAT_EQ(frame->v1, f[6]);      // 屏幕下边 → 纹理 v1
    EXPECT_FLOAT_EQ(frame->v0, f[2 * 9 + 6]);  // 屏幕上边 → 纹理 v0
}

TEST_F(FeatureRenderLayerTest, IconInjectedAfterBucketBuildTriggersRebuild) {
    // 图标常晚于要素导入:先建桶(此时名字查不到 → circle),再注入图标,
    // 下一帧应重镶成图集通道。
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "late";
    layer_->setStyle(style);
    layer_->store().addFeature(makePointAt(6.0));
    {
        const auto* vb =
            dynamic_cast<const earth_engine::testing::DummyBuffer*>(
                build()[0].vertexBuffer);
        ASSERT_NE(nullptr, vb);
        const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
        EXPECT_FLOAT_EQ(0.0f, f[8]);  // 尚无图标 → circle
    }
    ASSERT_TRUE(renderer_->iconAtlas()->addImage("late", 8, 8,
                                                 solidRgba(8, 8, 7)));
    {
        const auto* vb =
            dynamic_cast<const earth_engine::testing::DummyBuffer*>(
                build()[0].vertexBuffer);
        ASSERT_NE(nullptr, vb);
        const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
        EXPECT_LT(f[8], 0.0f);  // 重镶后走图集
    }
}

TEST_F(FeatureRenderLayerTest, DataDrivenImageExpressionPerFeature) {
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "circle";
    style.pointImageExpr = StyleExpression::match(
        "kind", {{"tower", StyleExpression::literalString("triangle")}},
        StyleExpression::literalString("square"));
    layer_->setStyle(style);
    layer_->store().addFeature(makeKindPoint(6.0, "tower"));
    layer_->store().addFeature(makeKindPoint(6.001, "gate"));

    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        build()[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(static_cast<float>(static_cast<int>(SymbolShape::Triangle)),
                    f[8]);
    EXPECT_FLOAT_EQ(static_cast<float>(static_cast<int>(SymbolShape::Square)),
                    f[4 * 9 + 8]);
}

TEST_F(FeatureRenderLayerTest, ZoomDrivenImageExpressionStripped) {
    // 图形名 = 数据驱动语义:引用 zoom 越界 → 剥离降级字面量。
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "square";
    style.pointImageExpr = StyleExpression::interpolateLinear(
        StyleExpression::zoom(),
        {{0.0, StyleExpression::literalString("star")},
         {24.0, StyleExpression::literalString("pin")}});
    layer_->setStyle(style);
    EXPECT_EQ(nullptr, layer_->style().pointImageExpr);

    layer_->store().addFeature(makePointAt(6.0));
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        build()[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(static_cast<float>(static_cast<int>(SymbolShape::Square)),
                    f[8]);
}
