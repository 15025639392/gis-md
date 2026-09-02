#include <gtest/gtest.h>
#include "../../helpers/AmapOfficialStyleTestAdapter.h"

#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/Engine.h"
#include "earth_engine/core/async/AsyncSystem.h"
#include "earth_engine/renderer/IconAtlas.h"
#include "earth_engine/renderer/GlyphAtlas.h"
#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/renderer/SymbolShape.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/style/AmapClassicStyleInternal.h"
#include "earth_engine/style/AmapClassicRoadStyle.h"
#include "earth_engine/style/AmapClassicLabelStyleInternal.h"
#include "earth_engine/style/AmapClassicRuntime.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TerrainHeightService.h"
#include "earth_engine/tiling/TileRenderContentState.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "../../helpers/MockRenderDevice.h"
#include "../../helpers/MockPlatformBridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>

using namespace earth_engine;
using earth_engine::testing::DummyBuffer;
using earth_engine::testing::MockRenderDevice;
using earth_engine::testing::MockPlatformBridge;

namespace {

constexpr double kDeg = M_PI / 180.0;
std::vector<uint8_t> loadHostFont();

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

void addOfficialMetadata(Feature& feature, const char* classCode,
                         const char* subKey, const char* drawOrder = "1",
                         const char* rank = "1") {
    feature.properties["amap_class"] = classCode;
    feature.properties["amap_subkey"] = subKey;
    feature.properties["amap_draworder"] = drawOrder;
    feature.properties["amap_minzoom"] = "3";
    feature.properties["amap_maxzoom"] = "20";
    feature.properties["amap_rank"] = rank;
}

void installTestOfficialLabelStyle(FeatureRenderStyle& style,
                                   int styleGroup = 1) {
    style.labelStyleGroupPropertyA.clear();
    style.labelStyleGroupPropertyB.clear();
    style.labelStyleGroupByProperty.clear();
    style.labelStyleGroupExpr =
        StyleExpression::literal(static_cast<double>(styleGroup));
    style.labelSizeExprByStyleGroup[styleGroup] =
        StyleExpression::literal(20.0);
    style.labelColorExprByStyleGroup[styleGroup] = StyleExpression::literal(
        std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
    style.labelHaloColorExprByStyleGroup[styleGroup] =
        StyleExpression::literal(
            std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});
    style.labelHaloWidthExprByStyleGroup[styleGroup] =
        StyleExpression::literal(1.0);
}

TileSymbolCpu::GenericVisualPayload& genericVisual(TileSymbolCpu& symbol) {
    if (!symbol.genericVisual) symbol.genericVisual.emplace();
    return *symbol.genericVisual;
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
        // 命令/桶契约测例锁精确拓扑,不是地球网格。生产默认 400m,
        // densify 由 GlobeFillDensifySplitsLargePolygon 单独打开。
        FeatureRenderStyle style = layer_->style();
        style.globeFillMaxEdgeMeters = 0.0;
        layer_->setStyleForContractTest(style);

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
    // 本测例验证「fill + outline」双命令拓扑:显式打开描边(生产默认关)。
    FeatureRenderStyle style = layer_->style();
    style.fillOutlineEnabled = true;
    layer_->setStyleForContractTest(style);
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
        EXPECT_TRUE(cmd->hasVectorUniforms);
        EXPECT_TRUE(cmd->uniforms.empty());
    }
    EXPECT_EQ(16, fill->vertexStride);  // P6b:+color(RGBA8)
    EXPECT_EQ(48, line->vertexStride);  // P6b:+color(RGBA8)
    EXPECT_FLOAT_EQ(800.0f, line->vectorUniforms.viewport[0]);
    EXPECT_FLOAT_EQ(600.0f, line->vectorUniforms.viewport[1]);
    EXPECT_FLOAT_EQ(layer_->style().lineWidthPx,
                    line->vectorUniforms.lineWidthPx);
    // 方形外环 4 顶点闭合 ribbon:2n=8 顶点,6·段数=24 索引
    EXPECT_EQ(24, line->indexCount);
    // 方形 CDT:4 顶点 → 2 三角形 = 6 索引
    EXPECT_EQ(6, fill->indexCount);
}

TEST_F(FeatureRenderLayerTest, PolygonDefaultEmitsFillOnly) {
    // 高德复刻默认不描面外环(裁剪后外环含瓦片角点,描边会甩出灰射线);
    // 默认样式下多边形只出 fill 命令。
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorFill, commands[0].kind);
}

TEST_F(FeatureRenderLayerTest, PrepareActivateDecouplesGpuFromVisibility) {
    // 增量 2:prepareTile 建 GPU 但不可见;activatePreparedTile 翻转可见且
    // 不再建 GPU(原子 flip 廉价化);abandonPreparedTile 清理未激活预建。
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(),
        std::vector<Feature>{makePolygon(6.0, 29.0, 0.01)});
    const TileKey key{SchemeId("XYZ-WebMercator"), 10, 100, 200};
    const int buffersBefore = device_.createdBufferCount;
    ASSERT_TRUE(layer_->prepareTile(key, mesh));
    EXPECT_EQ(1u, layer_->preparedTileCount());
    EXPECT_EQ(0u, layer_->tileMeshCount());  // 未可见
    EXPECT_GT(device_.createdBufferCount, buffersBefore);  // GPU 已建
    const int buffersAfterPrepare = device_.createdBufferCount;
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->activatePreparedTile(key));
    EXPECT_EQ(0u, layer_->preparedTileCount());
    EXPECT_EQ(1u, layer_->tileMeshCount());  // 已可见
    EXPECT_EQ(buffersAfterPrepare, device_.createdBufferCount);  // 无新 GPU
    // abandon 清理一块未激活预建桶
    auto mesh2 = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(),
        std::vector<Feature>{makePolygon(6.0, 29.0, 0.01)});
    const TileKey key2{SchemeId("XYZ-WebMercator"), 10, 100, 201};
    ASSERT_TRUE(layer_->prepareTile(key2, mesh2));
    EXPECT_EQ(1u, layer_->preparedTileCount());
    layer_->abandonPreparedTile(key2);
    EXPECT_EQ(0u, layer_->preparedTileCount());
}

TEST_F(FeatureRenderLayerTest, DefaultPaintOrderPreservesFillBeforePoint) {
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.01));
    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(6.005 * kDeg, 29.005 * kDeg)}};
    layer_->store().addFeature(std::move(point));

    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    sortMvpRenderCommands(commands);
    EXPECT_EQ(RenderCommandKind::VectorFill, commands[0].kind);
    EXPECT_EQ(RenderCommandKind::VectorPoint, commands[1].kind);
    EXPECT_EQ(kDefaultVectorPaintOrder, commands[0].vectorPaintOrder);
    EXPECT_EQ(kDefaultVectorPaintOrder, commands[1].vectorPaintOrder);
}

TEST_F(FeatureRenderLayerTest, TilePaintRangesShareOneBufferPair) {
    FeatureRenderStyle style = layer_->style();
    style.buildingExtrusion = false;
    style.paintOrderExpr = StyleExpression::match(
        "surface",
        {{"green", StyleExpression::literal(20.0)},
         {"land", StyleExpression::literal(30.0)},
         {"water", StyleExpression::literal(50.0)}},
        StyleExpression::literal(0.0));
    layer_->setStyleForContractTest(style);

    std::vector<Feature> features;
    for (const char* surface : {"water", "green", "land"}) {
        Feature f = makePolygon(6.0, 29.0, 0.01);
        f.properties["surface"] = surface;
        features.push_back(std::move(f));
    }
    const int buffersBefore = device_.createdBufferCount;
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), features);
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
        std::move(mesh));
    EXPECT_EQ(buffersBefore + 2, device_.createdBufferCount)
        << "多个 ordinal 仍只上传一份 fill VBO/IBO";

    RenderCommandList commands = build();
    ASSERT_EQ(3u, commands.size());
    sortMvpRenderCommands(commands);
    EXPECT_EQ(20, commands[0].vectorPaintOrder);
    EXPECT_EQ(30, commands[1].vectorPaintOrder);
    EXPECT_EQ(50, commands[2].vectorPaintOrder);
    EXPECT_EQ(commands[0].vertexBuffer, commands[1].vertexBuffer);
    EXPECT_EQ(commands[1].vertexBuffer, commands[2].vertexBuffer);
    EXPECT_EQ(commands[0].indexBuffer, commands[1].indexBuffer);
    EXPECT_LT(commands[0].indexOffset, commands[1].indexOffset);
    EXPECT_LT(commands[1].indexOffset, commands[2].indexOffset);
}

TEST_F(FeatureRenderLayerTest,
       AmapSurfaceColorLateBindingReusesGpuGeometryAcrossDisplayZoom) {
    FeatureRenderStyle style = layer_->style();
    style.fillColor = {0.0f, 0.0f, 0.0f, 0.0f};
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    layer_->setStyleForContractTest(style);

    Feature sportsGround = makePolygon(0.0, 0.0, 0.01);
    sportsGround.properties = {{"amap_class", "30002"},
                               {"amap_subkey", "19"},
                               {"amap_draworder", "73"},
                               {"amap_minzoom", "14"},
                               {"amap_maxzoom", "16"}};
    const int buffersBefore = device_.createdBufferCount;
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {sportsGround});
    ASSERT_EQ(1u, mesh.fillRanges.size());
    EXPECT_EQ(73, mesh.fillRanges[0].paintOrder);
    EXPECT_EQ(30002019, mesh.fillRanges[0].styleGroup);
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
                  std::move(mesh)));
    EXPECT_EQ(buffersBefore + 2, device_.createdBufferCount);
    const int buffersAfterCommit = device_.createdBufferCount;
    const int updatesAfterCommit = device_.updatedBufferCount;

    const double radius = Ellipsoid::WGS84().radii().x();
    auto setZoom = [&](double zoom) {
        const double height = 4.0e7 / std::pow(2.0, zoom);
        camera_.lookAt(Vec3(radius + height, 0.0, 0.0),
                       Vec3(radius, 0.0, 0.0), Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
    };
    auto onlyFill = [&]() -> RenderCommand {
        auto commands = build();
        auto it = std::find_if(commands.begin(), commands.end(), [](const auto& cmd) {
            return cmd.kind == RenderCommandKind::VectorFill;
        });
        EXPECT_NE(commands.end(), it);
        return it == commands.end() ? RenderCommand{} : *it;
    };

    setZoom(14.79);
    const RenderCommand green = onlyFill();
    EXPECT_EQ(73, green.vectorPaintOrder);
    EXPECT_EQ((std::array<float, 4>{0xb4 / 255.0f, 0xeb / 255.0f,
                                    0xaf / 255.0f, 1.0f}),
              green.vectorUniforms.color);

    // ECEF -> cartographic height round-trip may land a few ulps below the
    // requested camera zoom; step safely across the exact .8 boundary whose
    // expression-level contract is covered by AmapClassicSurfaceStyleTest.
    setZoom(14.801);
    const RenderCommand turquoise = onlyFill();
    EXPECT_EQ(73, turquoise.vectorPaintOrder);
    EXPECT_EQ((std::array<float, 4>{0x79 / 255.0f, 0xd5 / 255.0f,
                                    0xc0 / 255.0f, 1.0f}),
              turquoise.vectorUniforms.color);
    EXPECT_EQ(green.vertexBuffer, turquoise.vertexBuffer);
    EXPECT_EQ(green.indexBuffer, turquoise.indexBuffer);
    EXPECT_EQ(buffersAfterCommit, device_.createdBufferCount);
    EXPECT_EQ(updatesAfterCommit, device_.updatedBufferCount);
}

TEST_F(FeatureRenderLayerTest,
       AmapSurfaceUnknownIdentityAndOverlayZoomGapEmitNoFillCommand) {
    FeatureRenderStyle style = layer_->style();
    style.fillColor = {1.0f, 0.0f, 1.0f, 1.0f};
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    layer_->setStyleForContractTest(style);

    Feature unknown = makePolygon(0.0, 0.0, 0.01);
    unknown.properties = {{"amap_class", "30002"},
                          {"amap_subkey", "999"},
                          {"amap_draworder", "73"},
                          {"amap_minzoom", "14"},
                          {"amap_maxzoom", "16"}};
    Feature overlay = makePolygon(0.02, 0.0, 0.01);
    overlay.properties = {{"amap_class", "30003"},
                          {"amap_subkey", "3"},
                          {"amap_draworder", "74"},
                          {"amap_minzoom", "14"},
                          {"amap_maxzoom", "16"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {unknown, overlay});
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
                  std::move(mesh)));

    const double radius = Ellipsoid::WGS84().radii().x();
    auto setZoom = [&](double zoom) {
        const double height = 4.0e7 / std::pow(2.0, zoom);
        camera_.lookAt(Vec3(radius + height, 0.0, 0.0),
                       Vec3(radius, 0.0, 0.0), Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
    };
    auto fillOrders = [&]() {
        std::vector<int> out;
        for (const auto& cmd : build()) {
            if (cmd.kind == RenderCommandKind::VectorFill)
                out.push_back(cmd.vectorPaintOrder);
        }
        return out;
    };

    setZoom(15.79);
    EXPECT_TRUE(fillOrders().empty());
    setZoom(15.801);
    EXPECT_EQ((std::vector<int>{74}), fillOrders());
}

TEST_F(FeatureRenderLayerTest,
       EveryOfficialSurfaceIdentityReachesMatchingFinalFillCommand) {
    const auto records = amapClassicSurfaceRecordsForTest();
    ASSERT_EQ(351u, records.size());

    struct Identity {
        int classCode = 0;
        int subKey = 0;
        int paintOrder = 0;
    };
    std::vector<Identity> identities;
    std::map<std::pair<int, int>, int> paintOrderByIdentity;
    std::vector<Feature> features;
    for (const auto& record : records) {
        const std::pair<int, int> key{record.classCode, record.subKey};
        if (paintOrderByIdentity.count(key)) continue;
        const int paintOrder = 1000 + static_cast<int>(identities.size());
        paintOrderByIdentity.emplace(key, paintOrder);
        identities.push_back({record.classCode, record.subKey, paintOrder});
        const int index = static_cast<int>(identities.size()) - 1;
        const double x = (index % 16) * 0.0002;
        const double y = (index / 16) * 0.0002;
        Feature feature = makePolygon(x, y, 0.0001);
        feature.properties = {
            {"amap_class", std::to_string(record.classCode)},
            {"amap_subkey", std::to_string(record.subKey)},
            {"amap_draworder", std::to_string(paintOrder)},
            {"amap_minzoom", "1"}, {"amap_maxzoom", "30"}};
        features.push_back(std::move(feature));
    }
    ASSERT_EQ(256u, identities.size());

    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    style.globeFillMaxEdgeMeters = 0.0;
    layer_->setStyleForContractTest(style);
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), features);
    ASSERT_EQ(identities.size(), mesh.fillRanges.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                  std::move(mesh)));

    const double radius = Ellipsoid::WGS84().radii().x();
    for (int displayZoom = 1; displayZoom <= 24; ++displayZoom) {
        const double height = 4.0e7 / std::pow(2.0, displayZoom);
        camera_.lookAt(Vec3(radius + height, 0.0, 0.0),
                       Vec3(radius, 0.0, 0.0), Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
        const auto commands = build();
        std::map<int, std::array<float, 4>> actual;
        for (const auto& command : commands) {
            if (command.kind == RenderCommandKind::VectorFill) {
                actual.emplace(command.vectorPaintOrder,
                               command.vectorUniforms.color);
            }
        }
        for (const auto& identity : identities) {
            std::array<float, 4> expected{};
            const int providerZoom = displayZoom + 1;
            for (const auto& record : records) {
                if (record.classCode == identity.classCode &&
                    record.subKey == identity.subKey &&
                    record.minZoom <= providerZoom &&
                    providerZoom <= record.maxZoom) {
                    expected = record.color;
                }
            }
            const auto found = actual.find(identity.paintOrder);
            if (expected[3] <= 0.0f) {
                EXPECT_EQ(actual.end(), found)
                    << identity.classCode << ':' << identity.subKey
                    << " must be absent at display zoom " << displayZoom;
            } else {
                ASSERT_NE(actual.end(), found)
                    << identity.classCode << ':' << identity.subKey
                    << " missing final fill at display zoom " << displayZoom;
                EXPECT_EQ(expected, found->second)
                    << identity.classCode << ':' << identity.subKey
                    << " final command color mismatch at display zoom "
                    << displayZoom;
            }
        }
    }
}

TEST_F(FeatureRenderLayerTest, TileCommitReportsRetryableGpuFailure) {
    Feature polygon = makePolygon(6.0, 29.0, 0.01);
    FeatureTileMesh mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {polygon});
    const TileKey key{SchemeId("XYZ-WebMercator"), 10, 100, 200};
    device_.failBufferCreationAtAttempt = device_.bufferCreationAttempts + 1;

    EXPECT_EQ(TileMeshCommitResult::RetryableFailure,
              layer_->commitTileMesh(key, mesh));
    EXPECT_TRUE(build().empty()) << "上传失败不能留下半张瓦片";

    device_.failBufferCreationAtAttempt = -1;
    EXPECT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(key, mesh))
        << "同一 CPU mesh 必须可在后续帧重试";
    EXPECT_FALSE(build().empty());
}

TEST_F(FeatureRenderLayerTest, EmptyTileCommitIsTerminalSuccess) {
    FeatureTileMesh mesh;
    const TileKey key{SchemeId("XYZ-WebMercator"), 10, 100, 200};
    EXPECT_EQ(TileMeshCommitResult::EmptyTerminal,
              layer_->commitTileMesh(key, mesh));
}

TEST_F(FeatureRenderLayerTest, RangeLessTileAbiIsRejectedInsteadOfStyleGroupZero) {
    Feature polygon = makePolygon(6.0, 29.0, 0.01);
    Feature line = makeLine(6.0, 29.0, 0.01);
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {polygon, line});
    ASSERT_FALSE(mesh.fillIndices.empty());
    ASSERT_FALSE(mesh.lineIndices.empty());
    ASSERT_FALSE(mesh.fillRanges.empty());
    ASSERT_FALSE(mesh.lineRanges.empty());
    mesh.fillRanges.clear();
    mesh.lineRanges.clear();

    EXPECT_EQ(TileMeshCommitResult::EmptyTerminal,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200}, mesh));
    EXPECT_TRUE(build().empty());
}

TEST_F(FeatureRenderLayerTest, GlobalPaintOrderSeparatesFillAndLineAcrossTiles) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::match(
        "family", {{"surface", StyleExpression::literal(30.0)},
                    {"road", StyleExpression::literal(80.0)}},
        StyleExpression::literal(0.0));
    layer_->setStyleForContractTest(style);

    for (int x : {101, 100}) {
        Feature polygon = makePolygon(6.0 + x * 0.001, 29.0, 0.01);
        polygon.properties["family"] = "surface";
        Feature line = makeLine(6.0 + x * 0.001, 29.0, 0.01);
        line.properties["family"] = "road";
        auto mesh = FeatureRenderLayer::tessellateTileMesh(
            layer_->workerTessellationContext(), {line, polygon});
        layer_->commitTileMesh(
            TileKey{SchemeId("XYZ-WebMercator"), 10, x, 200},
            std::move(mesh));
    }

    RenderCommandList commands = build();
    ASSERT_EQ(4u, commands.size());
    EXPECT_TRUE(mvpRenderCommandsNeedSort(commands))
        << "tile A 的 line 位于 tile B fill 前时 fast-path 必须识别";
    sortMvpRenderCommands(commands);
    EXPECT_EQ(RenderCommandKind::VectorFill, commands[0].kind);
    EXPECT_EQ(RenderCommandKind::VectorFill, commands[1].kind);
    EXPECT_EQ(RenderCommandKind::VectorLine, commands[2].kind);
    EXPECT_EQ(RenderCommandKind::VectorLine, commands[3].kind);
    EXPECT_FALSE(validateMvpRenderCommands(commands, frame_.frameId).has_value());
}

TEST_F(FeatureRenderLayerTest, MaxZoomGatesCoarseLodLayer) {
    // LOD 粗源近景让位:zoom > maxZoom 时整层不发命令(主源细面承接)。
    FeatureRenderStyle style = layer_->style();
    style.maxZoom = 11.5;
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));

    // 相机 ~3.2km 高(zoom≈13.6 > 11.5):粗源被门控,无命令。
    const auto surface =
        Ellipsoid::WGS84().cartographicToCartesian(Cartographic(0, 0));
    camera_.lookAt(surface * 1.0005, Vec3(0.0, 0.0, 0.0),
                   Vec3(0.0, 0.0, 1.0));
    RenderCommandList commands = build();
    EXPECT_TRUE(commands.empty());

    // 放远相机(恢复 fixture 高空位姿,zoom≈2):恢复渲染。
    camera_.lookAt(Vec3(1.5e7, 0.0, 0.0), Vec3(0.0, 0.0, 0.0),
                   Vec3(0.0, 0.0, 1.0));
    commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorFill, commands[0].kind);
}

TEST_F(FeatureRenderLayerTest, GlobeFillDensifySplitsLargePolygon) {
    // 0.1° ≈ 11km 方形,默认 400m 网格必须把 2 三角拆开,否则斜视近裁
    // 会把 ECEF 大三角裁成射线(VectorFill 水系/绿地的根因)。
    FeatureRenderStyle style = layer_->style();
    style.globeFillMaxEdgeMeters = 400.0;
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.1));
    RenderCommandList commands = build();
    const RenderCommand* fill = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorFill) fill = &cmd;
    }
    ASSERT_NE(nullptr, fill);
    EXPECT_GT(fill->indexCount, 6);
    EXPECT_EQ(0, fill->indexCount % 3);
}

TEST_F(FeatureRenderLayerTest, LineStringEmitsOnlyLineCommand) {
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorLine, commands[0].kind);
    // open 3 顶点:2n=6 顶点,6·(n-1)=12 索引
    EXPECT_EQ(12, commands[0].indexCount);
}

TEST_F(FeatureRenderLayerTest,
       AmapRoadNameLineProducesLabelOnlyCandidateWithoutLineGeometry) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style.labelSizePx = 21.0f;
    style.labelSizeExpr = StyleExpression::match(
        "amap_class", {{"20001", StyleExpression::literal(25.0)}},
        StyleExpression::literal(18.0));
    style.labelOffsetPx = 0.0f;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    constexpr int kRoadLabel = amapClassicStyleIdentity(20001, 1);
    style.labelStyleGroupExpr = StyleExpression::literal(kRoadLabel);
    style.labelSizeExprByStyleGroup[kRoadLabel] =
        StyleExpression::literal(18.0);
    style.labelColorExprByStyleGroup[kRoadLabel] =
        StyleExpression::literal(std::array<float, 4>{0, 0, 0, 1});
    style.labelHaloColorExprByStyleGroup[kRoadLabel] =
        StyleExpression::literal(std::array<float, 4>{1, 1, 1, 1});
    style.labelHaloWidthExprByStyleGroup[kRoadLabel] =
        StyleExpression::literal(1.0);
    layer_->setStyleForContractTest(style);

    Feature roadName = makeLine(106.4, 29.5, 0.02);
    roadName.properties["amap_class"] = "20001";
    roadName.properties["amap_draworder"] = "82";
    roadName.properties["name"] = "成渝环线高速";
    roadName.properties["amap_rank"] = "-7";
    roadName.properties["amap_minzoom"] = "10";
    roadName.properties["amap_maxzoom"] = "16";

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {roadName});
    EXPECT_TRUE(mesh.lineIndices.empty());
    EXPECT_TRUE(mesh.lineRanges.empty());
    ASSERT_EQ(1u, mesh.symbols.size());
    const TileSymbolCpu& label = mesh.symbols.front();
    EXPECT_FALSE(label.genericVisual.has_value());
    EXPECT_EQ("成渝环线高速", label.name);
    EXPECT_EQ(7, label.rank);
    EXPECT_EQ(9, label.minZoom);
    EXPECT_EQ(16, label.maxZoom);
    EXPECT_EQ(82, label.paintOrder);
    EXPECT_FLOAT_EQ(18.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, label.labelStyleGroup, 12.0,
                               0.0f));
    EXPECT_EQ(0u, label.labelRepeatGroup);
    EXPECT_FLOAT_EQ(0.0f, label.labelRepeatDistancePx);
    const float expectedAngle = static_cast<float>(
        std::atan2(0.01, 0.01 * std::cos(29.01 * kDeg)));
    EXPECT_NEAR(expectedAngle, label.labelAngleRad, 0.01f);
    ASSERT_EQ(3u, label.labelPathCartographic.size());
    EXPECT_LT(label.labelPathCartographic.front()[0],
              label.labelPathCartographic.back()[0]);
}

TEST_F(FeatureRenderLayerTest, RoadLabelPathNormalizesReverseGeometryReadable) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    style.labelStyleGroupExpr = StyleExpression::literal(
        amapClassicStyleIdentity(20001, 1));
    layer_->setStyleForContractTest(style);
    Feature road;
    road.type = GeometryType::LineString;
    road.rings = {{Cartographic(106.44 * kDeg, 29.52 * kDeg),
                   Cartographic(106.42 * kDeg, 29.51 * kDeg),
                   Cartographic(106.40 * kDeg, 29.50 * kDeg)}};
    road.properties["amap_class"] = "20001";
    road.properties["name"] = "反向道路";
    addOfficialMetadata(road, "20001", "1");

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {road});
    ASSERT_EQ(1u, mesh.symbols.size());
    const auto& path = mesh.symbols.front().labelPathCartographic;
    ASSERT_EQ(3u, path.size());
    EXPECT_LT(path.front()[0], path.back()[0]);
    EXPECT_GT(std::cos(mesh.symbols.front().labelAngleRad), 0.0f);
}

TEST_F(FeatureRenderLayerTest,
       RoadLabelPathPreservesEveryOfficialProviderPoint) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    style.labelStyleGroupExpr = StyleExpression::literal(
        amapClassicStyleIdentity(20001, 1));
    layer_->setStyleForContractTest(style);
    Feature road;
    road.type = GeometryType::LineString;
    constexpr size_t kPointCount = 130;
    road.rings.emplace_back();
    road.rings.front().reserve(kPointCount);
    for (size_t i = 0; i < kPointCount; ++i) {
        const double lat = i == 65 ? 29.51 : 29.50;
        road.rings.front().emplace_back((106.40 + i * 0.0001) * kDeg,
                                        lat * kDeg);
    }
    road.properties["amap_class"] = "20001";
    road.properties["name"] = "密集弯点道路";
    addOfficialMetadata(road, "20001", "1");

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {road});
    ASSERT_EQ(1u, mesh.symbols.size());
    const auto& path = mesh.symbols.front().labelPathCartographic;
    ASSERT_EQ(kPointCount, path.size());
    const double apexLat = 29.51 * kDeg;
    EXPECT_NE(path.end(),
              std::find_if(path.begin(), path.end(), [&](const auto& p) {
                  return std::abs(p[1] - apexLat) < 1e-12;
              }))
        << "bounded simplification must not cut across a locally dense bend";
}

TEST_F(FeatureRenderLayerTest,
       OfficialRoadLabelSamplesTerrainAtEveryProviderPathPoint) {
    FeatureRenderLayer layer("official-road-label-terrain", &device_,
                             Ellipsoid::WGS84());
    layer.installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    auto sampled = std::make_shared<std::vector<std::array<double, 2>>>();
    FeatureTerrainSampling sampling;
    sampling.makeAreaSampler = [sampled](const Rectangle&) {
        return [sampled](double lon, double lat) -> std::optional<float> {
            sampled->push_back({lon, lat});
            return static_cast<float>(100.0 + lon * 10.0 + lat * 20.0);
        };
    };
    sampling.revision = []() -> uint64_t { return 1; };
    layer.setTerrainSampling(std::move(sampling));

    Feature road;
    road.type = GeometryType::LineString;
    road.rings = {{Cartographic(106.52 * kDeg, 29.54 * kDeg),
                   Cartographic(106.55 * kDeg, 29.56 * kDeg),
                   Cartographic(106.59 * kDeg, 29.59 * kDeg)}};
    road.properties["amap_class"] = "20001";
    road.properties["name"] = "逐点贴地路名";
    addOfficialMetadata(road, "20001", "1");
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer.workerTessellationContext(), {road});
    ASSERT_EQ(1u, mesh.symbols.size());
    ASSERT_EQ(3u, mesh.symbols.front().labelPathCartographic.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer.commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 14, 13038, 5501},
                  std::move(mesh)));

    for (const Cartographic& expected : road.rings.front()) {
        EXPECT_NE(sampled->end(),
                  std::find_if(sampled->begin(), sampled->end(),
                               [&](const auto& point) {
                                   return std::abs(point[0] -
                                                   expected.longitude()) <
                                              1e-7 &&
                                          std::abs(point[1] -
                                                   expected.latitude()) <
                                              1e-7;
                               }))
            << "each official path point must own its terrain height; the "
               "anchor height cannot be copied over the whole road";
    }
}

TEST_F(FeatureRenderLayerTest,
       AlongRoadLabelRebakesAfterProjectedPathMovesBeyondThreshold) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    FeatureRenderStyle style = layer_->style();
    style.labelSizePx = 18.0f;
    style.labelOffsetPx = 0.0f;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    constexpr int kRoadLabel = amapClassicStyleIdentity(20001, 1);
    style.labelStyleGroupExpr = StyleExpression::literal(kRoadLabel);
    style.labelSizeExprByStyleGroup[kRoadLabel] =
        StyleExpression::literal(18.0);
    style.labelColorExprByStyleGroup[kRoadLabel] =
        StyleExpression::literal(std::array<float, 4>{0, 0, 0, 1});
    style.labelHaloColorExprByStyleGroup[kRoadLabel] =
        StyleExpression::literal(std::array<float, 4>{1, 1, 1, 1});
    style.labelHaloWidthExprByStyleGroup[kRoadLabel] =
        StyleExpression::literal(1.0);
    layer_->setStyleForContractTest(style);

    Feature road;
    road.type = GeometryType::LineString;
    road.rings = {{Cartographic(106.52 * kDeg, 29.54 * kDeg),
                   Cartographic(106.55 * kDeg, 29.56 * kDeg),
                   Cartographic(106.59 * kDeg, 29.59 * kDeg)}};
    road.properties["amap_class"] = "20001";
    road.properties["name"] = "AB";
    addOfficialMetadata(road, "20001", "1");
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {road});
    ASSERT_EQ(1u, mesh.symbols.size());
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 14, 13038, 5501},
        std::move(mesh));

    const Ellipsoid ellipsoid = Ellipsoid::WGS84();
    const Vec3 target = ellipsoid.cartographicToCartesian(
        Cartographic(106.55 * kDeg, 29.56 * kDeg));
    const Vec3 up = target.normalized();
    camera_.lookAt(target + up * 8000.0, target, Vec3(0, 0, 1));
    RenderCommandList first = build();
    ASSERT_TRUE(std::any_of(first.begin(), first.end(), [](const auto& cmd) {
        return cmd.kind == RenderCommandKind::VectorLabel;
    }));
    const int buffersAfterFirstBake = device_.createdBufferCount;

    camera_.lookAt(target + up * 8000.0 + Vec3(0, 3000, 0), target,
                   Vec3(0, 0, 1));
    ++frame_.frameId;
    RenderCommandList moved = build();
    EXPECT_GT(device_.createdBufferCount, buffersAfterFirstBake)
        << "camera-dependent screen-arc geometry must refresh after >2px drift";
    EXPECT_TRUE(std::any_of(moved.begin(), moved.end(), [](const auto& cmd) {
        return cmd.kind == RenderCommandKind::VectorLabel;
    }));
}

TEST_F(FeatureRenderLayerTest,
       LabelPaintOrderCanDifferWithoutSplittingPointGeometry) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(100.0);
    style.labelPaintOrderExpr = StyleExpression::match(
        "amap_class", {{"10002", StyleExpression::literal(101.0)}},
        StyleExpression::literal(100.0));
    layer_->setStyleForContractTest(style);

    Feature admin;
    admin.type = GeometryType::Point;
    admin.rings = {{Cartographic(106.5 * kDeg, 29.6 * kDeg)}};
    admin.properties["amap_class"] = "10002";
    admin.properties["name"] = "沙坪坝区";

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {admin});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(100, mesh.symbols.front().paintOrder);
    EXPECT_EQ(101, mesh.symbols.front().labelPaintOrder);
}

TEST_F(FeatureRenderLayerTest,
       LabelStyleGroupIsIndependentFromProviderDrawOrder) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style.labelPaintOrderExpr = StyleExpression::get("amap_draworder");
    style.labelStyleGroupExpr = StyleExpression::match(
        "amap_class",
        {{"20001", StyleExpression::literal(
                       static_cast<double>(amapClassicStyleIdentity(20001, 1)))}},
        StyleExpression::literal(0.0));
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);

    Feature roadName = makeLine(106.4, 29.5, 0.02);
    roadName.properties = {{"amap_class", "20001"},
                           {"amap_subkey", "1"},
                           {"amap_draworder", "37"},
                           {"amap_minzoom", "3"},
                           {"amap_maxzoom", "20"},
                           {"amap_rank", "1"},
                           {"name", "成渝高速"}};
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {roadName});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(37, mesh.symbols.front().labelPaintOrder);
    EXPECT_EQ(amapClassicStyleIdentity(20001, 1),
              mesh.symbols.front().labelStyleGroup);
}

TEST_F(FeatureRenderLayerTest,
       OfficialPoiIdentityRejectsUnknownWithoutUsingProviderOrder) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style.labelPaintOrderExpr = style.paintOrderExpr;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);

    Feature matched;
    matched.type = GeometryType::Point;
    matched.rings = {{Cartographic(106.5 * kDeg, 29.6 * kDeg)}};
    matched.properties["amap_class"] = "12024";
    matched.properties["amap_subkey"] = "1178";
    matched.properties["amap_draworder"] = "9137";
    matched.properties["amap_rank"] = "1";
    matched.properties["amap_minzoom"] = "3";
    matched.properties["amap_maxzoom"] = "20";
    matched.properties["name"] = "matched";

    Feature fallback = matched;
    fallback.rings = {{Cartographic(106.6 * kDeg, 29.6 * kDeg)}};
    fallback.properties["amap_subkey"] = "193";
    fallback.properties["name"] = "fallback";

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {matched, fallback});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(9137, mesh.symbols[0].paintOrder);
    EXPECT_EQ(9137, mesh.symbols[0].labelPaintOrder);
    EXPECT_EQ(amapClassicLabelIdentity(12024, 1178),
              mesh.symbols[0].labelStyleGroup);
    EXPECT_EQ("matched", mesh.symbols[0].name);
    EXPECT_EQ(-1, mesh.symbols[0].rank);
}

TEST_F(FeatureRenderLayerTest,
       OfficialPoiRejectsGenericRankAliasWithoutOfficialRank) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);

    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(106.5 * kDeg, 29.6 * kDeg)}};
    point.properties = {{"amap_class", "12024"},
                        {"amap_subkey", "1178"},
                        {"amap_draworder", "9137"},
                        {"rank", "-1"},
                        {"name", "legacy-alias"}};

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {point});
    EXPECT_TRUE(mesh.symbols.empty());
}

TEST_F(FeatureRenderLayerTest,
       StrictOfficialDrawOrderRejectsMissingAndMalformedButAcceptsZero) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);

    Feature missing;
    missing.type = GeometryType::Point;
    missing.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    addOfficialMetadata(missing, "12024", "1");
    missing.properties.erase("amap_draworder");
    Feature malformed = missing;
    malformed.rings = {{Cartographic(6.1 * kDeg, 29.0 * kDeg)}};
    malformed.properties["amap_draworder"] = "not-a-number";
    Feature legalZero = missing;
    legalZero.rings = {{Cartographic(6.2 * kDeg, 29.0 * kDeg)}};
    legalZero.properties["amap_draworder"] = "0";

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(),
        {missing, malformed, legalZero});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(0, mesh.symbols.front().paintOrder);
}

TEST_F(FeatureRenderLayerTest,
       OfficialStoreRejectsMissingAndMalformedDrawOrderBeforeGeometryUpload) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Regions);
    layer_->setStyleForContractTest(style);

    auto surface = [](double longitude, const char* drawOrder) {
        Feature feature = makePolygon(longitude, 29.0, 0.01);
        feature.properties = {{"amap_class", "30001"},
                              {"amap_subkey", "1"},
                              {"amap_minzoom", "2"},
                              {"amap_maxzoom", "20"}};
        if (drawOrder) feature.properties["amap_draworder"] = drawOrder;
        return feature;
    };
    layer_->store().addFeature(surface(6.0, nullptr));
    layer_->store().addFeature(surface(6.02, "not-an-integer"));
    layer_->store().addFeature(surface(6.04, "73"));

    const RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorFill, commands.front().kind);
    EXPECT_EQ(73, commands.front().vectorPaintOrder);
}

TEST_F(FeatureRenderLayerTest,
       OfficialLabelOrderIgnoresCallerExpressionAfterInstaller) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    style.labelPaintOrderExpr = StyleExpression::literal(-999.0);
    layer_->setStyleForContractTest(style);

    Feature roadName = makeLine(106.4, 29.5, 0.02);
    roadName.properties = {{"amap_class", "20001"},
                           {"amap_subkey", "1"},
                           {"amap_draworder", "37"},
                           {"amap_minzoom", "3"},
                           {"amap_maxzoom", "20"},
                           {"amap_rank", "1"},
                           {"name", "official-order"}};
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {roadName});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(37, mesh.symbols.front().paintOrder);
    EXPECT_EQ(37, mesh.symbols.front().labelPaintOrder);
}

TEST_F(FeatureRenderLayerTest,
       OfficialPointStoreFailsClosedInsteadOfUsingGenericCircleContract) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(106.5 * kDeg, 29.6 * kDeg)}};
    poi.properties = {{"amap_class", "12024"},
                      {"amap_subkey", "1178"},
                      {"amap_draworder", "9137"},
                      {"amap_rank", "1"},
                      {"amap_minzoom", "3"},
                      {"amap_maxzoom", "20"},
                      {"name", "official-poi"}};
    layer_->store().addFeature(std::move(poi));
    EXPECT_TRUE(build().empty());
}

TEST_F(FeatureRenderLayerTest,
       GenericSetterRejectsIncomingAndReplacementOfficialContracts) {
    FeatureRenderStyle incomingOfficial = layer_->style();
    incomingOfficial = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Regions);
    layer_->setStyle(incomingOfficial);
    EXPECT_FALSE(layer_->style().usesOfficialProviderContract());

    layer_->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);
    ASSERT_TRUE(layer_->style().usesOfficialProviderContract());
    ASSERT_TRUE(layer_->hasSealedOfficialProfile());

    FeatureRenderStyle mutated = layer_->style();
    mutated.fillColor = {1, 0, 0, 1};
    mutated.fillColorExprByStyleGroup.clear();
    layer_->setStyle(mutated);
    EXPECT_TRUE(layer_->style().usesOfficialProviderContract());
    EXPECT_FALSE(layer_->style().fillColorExprByStyleGroup.empty());
    EXPECT_NE((std::array<float, 4>{1, 0, 0, 1}),
              layer_->style().fillColor);

    FeatureRenderStyle generic;
    layer_->setStyle(generic);
    EXPECT_TRUE(layer_->style().usesOfficialProviderContract());
}

TEST_F(FeatureRenderLayerTest,
       OfficialCasingDoesNotFallBackToGenericLayerWidth) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    const int identity = amapClassicStyleIdentity(20001, 1);
    ASSERT_TRUE(style.lineCasingStyleGroups.count(identity));
    style.lineCasingWidthExprByStyleGroup.erase(identity);
    style.lineCasingExtraWidthPx = 99.0f;
    style.lineCasingWidthRatio = 8.0f;
    layer_->setStyleForContractTest(style);

    Feature road = makeLine(6.0, 29.0, 0.02);
    addOfficialMetadata(road, "20001", "1", "90");
    layer_->store().addFeature(std::move(road));
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.0)),
                   surface, Vec3(0.0, 0.0, 1.0));

    const RenderCommandList commands = build();
    for (const auto& command : commands) {
        EXPECT_NE(0, command.vectorPaintSubOrder);
    }
}

TEST_F(FeatureRenderLayerTest,
       MalformedOfficialCasingDoesNotConsumeGenericFallbackScalars) {
    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    const int identity = amapClassicStyleIdentity(20001, 1);
    ASSERT_TRUE(style.lineCasingStyleGroups.count(identity));
    style.lineCasingWidthExprByStyleGroup[identity] =
        StyleExpression::literal(
            std::numeric_limits<double>::quiet_NaN());
    style.lineCasingExtraWidthPx = 999.0f;
    style.lineCasingWidthRatio = 999.0f;
    layer_->setStyleForContractTest(style);

    Feature road = makeLine(6.0, 29.0, 0.02);
    addOfficialMetadata(road, "20001", "1", "90");
    layer_->store().addFeature(std::move(road));
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.0)),
                   surface, Vec3(0.0, 0.0, 1.0));

    const RenderCommandList commands = build();
    ASSERT_FALSE(commands.empty());
    for (const auto& command : commands) {
        EXPECT_NE(0, command.vectorPaintSubOrder)
            << "malformed official casing must fail closed";
        EXPECT_LT(command.vectorUniforms.lineWidthPx, 100.0f)
            << "generic fallback scalars must not contaminate official width";
    }
}

TEST_F(FeatureRenderLayerTest,
       OfficialFreshResetAndContinuationReachFinalRoadCommands) {
    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    layer_->setStyleForContractTest(style);
    frame_.devicePixelRatio = 1.0f;

    Feature road = makeLine(6.0, 29.0, 0.02);
    addOfficialMetadata(road, "20023", "6", "90");
    layer_->store().addFeature(std::move(road));
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    const Vec3 radial = surface.normalized();
    const auto atDisplayZoom = [&](double zoom) {
        camera_.lookAt(surface + radial * (4.0e7 / std::pow(2.0, zoom)),
                       surface, Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
        RenderCommandList commands = build();
        commands.erase(std::remove_if(
            commands.begin(), commands.end(), [](const RenderCommand& cmd) {
                return cmd.kind != RenderCommandKind::VectorLine;
            }), commands.end());
        return commands;
    };

    // Provider zoom 8 continues the original casing-only field set from the
    // fresh zoom-6 record. It must reach one red, 1 CSS-pixel casing command.
    const RenderCommandList inherited = atDisplayZoom(7.0);
    ASSERT_EQ(1u, inherited.size());
    EXPECT_EQ(0, inherited[0].vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(1.0f, inherited[0].vectorUniforms.lineWidthPx);
    EXPECT_EQ((std::array<float, 4>{0xe6 / 255.0f, 0x37 / 255.0f,
                                    0x19 / 255.0f, 1.0f}),
              inherited[0].vectorUniforms.color);

    // Provider zoom 9 is both continuation and fresh. Its omitted casing
    // width/color reset the inherited pass; only the new 1px gray center may
    // survive. Keeping the old red casing here would violate field 10.
    const RenderCommandList reset = atDisplayZoom(8.0);
    ASSERT_EQ(1u, reset.size());
    EXPECT_EQ(1, reset[0].vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(1.0f, reset[0].vectorUniforms.lineWidthPx);
    EXPECT_EQ((std::array<float, 4>{0xda / 255.0f, 0xda / 255.0f,
                                    0xda / 255.0f, 1.0f}),
              reset[0].vectorUniforms.color);

    // Provider zoom 10 is a continuation record that explicitly restores
    // casing width/color. Both independent final commands must return.
    const RenderCommandList restored = atDisplayZoom(9.0);
    ASSERT_EQ(2u, restored.size());
    EXPECT_EQ(0, restored[0].vectorPaintSubOrder);
    EXPECT_EQ(1, restored[1].vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(3.0f, restored[0].vectorUniforms.lineWidthPx);
    EXPECT_FLOAT_EQ(2.0f, restored[1].vectorUniforms.lineWidthPx);
    EXPECT_EQ((std::array<float, 4>{0xe6 / 255.0f, 0x37 / 255.0f,
                                    0x19 / 255.0f, 1.0f}),
              restored[0].vectorUniforms.color);
}

TEST_F(FeatureRenderLayerTest, ClosedOfficialProfileIsSealed) {
    auto sealed = std::make_unique<FeatureRenderLayer>(
        "sealed-official", &device_, Ellipsoid::WGS84());
    sealed->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);
    EXPECT_TRUE(sealed->hasSealedOfficialProfile());
    EXPECT_TRUE(sealed->style().usesOfficialProviderContract());
    EXPECT_EQ(FeatureAltitudeMode::ClampToGround,
              sealed->style().altitudeMode)
        << "official visual identity stays sealed while scene terrain owns "
           "spatial placement";
    EXPECT_DOUBLE_EQ(0.0, sealed->style().heightOffset)
        << "official geometry must not inherit a local z-fighting offset";

}

TEST_F(FeatureRenderLayerTest,
       OfficialProfilesUseOneTerrainAdaptivePlacementContract) {
    for (const auto profile : {
             FeatureRenderLayer::AmapClassicProfile::Main,
             FeatureRenderLayer::AmapClassicProfile::Regions,
             FeatureRenderLayer::AmapClassicProfile::Poi}) {
        const FeatureRenderStyle style =
            earth_engine::testing::amapOfficialStyleForTest(profile);
        EXPECT_TRUE(style.usesOfficialProviderContract());
        EXPECT_EQ(FeatureAltitudeMode::ClampToGround, style.altitudeMode);
        EXPECT_DOUBLE_EQ(0.0, style.heightOffset);
        EXPECT_TRUE(style.terrainClampRibbon)
            << "official roads must preserve a reclamp source instead of "
               "falling into the generic stencil-volume path";
    }
}

TEST_F(FeatureRenderLayerTest,
       OfficialRoadTilePreservesTerrainReclampSource) {
    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    FeatureRenderLayer::TessellationContext ctx{
        style, Ellipsoid::WGS84(), nullptr, nullptr,
        /*supportsStencilClassification=*/true};
    ctx.hasTerrainHeightRange = true;
    ctx.terrainMinHeight = 500.0;
    ctx.terrainMaxHeight = 2000.0;

    Feature road = makeLine(6.0, 29.0, 0.02);
    road.properties = {{"amap_class", "20001"},
                       {"amap_subkey", "1"},
                       {"amap_draworder", "90"},
                       {"amap_minzoom", "3"},
                       {"amap_maxzoom", "20"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(ctx, {road});

    EXPECT_TRUE(mesh.lineVolumeGroups.empty());
    ASSERT_FALSE(mesh.lineVerts.empty());
    ASSERT_FALSE(mesh.lineIndices.empty());
    ASSERT_FALSE(mesh.lineClampSource.empty());
    EXPECT_EQ(mesh.lineClampSource.size(), mesh.lineVerts.size() / 12 * 9);

    const float* vertex = mesh.lineVerts.data();
    const Vec3 relative(vertex[0], vertex[1], vertex[2]);
    const Cartographic position = Ellipsoid::WGS84().cartesianToCartographic(
        mesh.origin + relative);
    EXPECT_NEAR(position.height(), 0.0, 0.05)
        << "worker output is an ellipsoid placeholder; commit/reclamp owns "
           "the authoritative render-grid terrain height";
}

TEST_F(FeatureRenderLayerTest,
       ClosedOfficialProfilesRejectOutOfContractGeometryBeforeStyling) {
    auto makeOfficialPoint = [] {
        Feature point;
        point.type = GeometryType::Point;
        point.rings = {{Cartographic(106.5 * kDeg, 29.6 * kDeg)}};
        addOfficialMetadata(point, "12024", "1178", "90");
        point.properties["name"] = "must-not-use-generic-point";
        return point;
    };

    for (const auto profile : {
             FeatureRenderLayer::AmapClassicProfile::Main,
             FeatureRenderLayer::AmapClassicProfile::Regions}) {
        auto sealed = std::make_unique<FeatureRenderLayer>(
            "geometry-gate", &device_, Ellipsoid::WGS84());
        sealed->installAmapClassicProfile(profile);
        const auto mesh = FeatureRenderLayer::tessellateTileMesh(
            sealed->workerTessellationContext(), {makeOfficialPoint()});
        EXPECT_TRUE(mesh.symbols.empty());
        EXPECT_TRUE(mesh.fillIndices.empty());
        EXPECT_TRUE(mesh.lineIndices.empty());

        sealed->store().addFeature(makeOfficialPoint());
        RenderCommandList commands;
        sealed->buildRenderCommands(frame_, *renderer_, commands);
        EXPECT_TRUE(commands.empty());
    }

    auto poi = std::make_unique<FeatureRenderLayer>(
        "poi-geometry-gate", &device_, Ellipsoid::WGS84());
    poi->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    Feature polygon = makePolygon(106.4, 29.5, 0.01);
    addOfficialMetadata(polygon, "30001", "2", "10");
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        poi->workerTessellationContext(), {polygon});
    EXPECT_TRUE(mesh.fillIndices.empty());
    EXPECT_TRUE(mesh.lineIndices.empty());
    EXPECT_TRUE(mesh.symbols.empty());

    poi->store().addFeature(std::move(polygon));
    RenderCommandList commands;
    poi->buildRenderCommands(frame_, *renderer_, commands);
    EXPECT_TRUE(commands.empty());
}

TEST_F(FeatureRenderLayerTest,
       OfficialGeometryDoesNotBakeGenericFillOrLineColors) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Regions);
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    // Deliberately poison every generic color entry. Official tessellation
    // must retain only styleGroup identity; these values cannot become a
    // hidden fallback in the CPU/GPU payload.
    style.fillColor = {1, 0, 1, 1};
    style.fillColorExpr = StyleExpression::literal(style.fillColor);
    style.lineColor = {0, 1, 0, 1};
    style.lineColorExpr = StyleExpression::literal(style.lineColor);
    style.lineColorProperty = "generic_color";
    style.lineColorByProperty["poison"] = {1, 1, 0, 1};
    layer_->setStyleForContractTest(style);

    Feature surface = makePolygon(106.4, 29.5, 0.01);
    addOfficialMetadata(surface, "30001", "2", "10");
    Feature road = makeLine(106.4, 29.5, 0.01);
    addOfficialMetadata(road, "20001", "1", "20");
    road.properties["generic_color"] = "poison";

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {surface, road});
    ASSERT_FALSE(mesh.fillVerts.empty());
    ASSERT_FALSE(mesh.lineVerts.empty());

    uint32_t fillPacked = 1;
    uint32_t linePacked = 1;
    std::memcpy(&fillPacked, &mesh.fillVerts[3], sizeof(fillPacked));
    std::memcpy(&linePacked, &mesh.lineVerts[11], sizeof(linePacked));
    EXPECT_EQ(0u, fillPacked);
    EXPECT_EQ(0u, linePacked);
    ASSERT_EQ(1u, mesh.fillRanges.size());
    ASSERT_EQ(1u, mesh.lineRanges.size());
    EXPECT_NE(0, mesh.fillRanges.front().styleGroup);
    EXPECT_NE(0, mesh.lineRanges.front().styleGroup);
}

TEST_F(FeatureRenderLayerTest,
       SealedOfficialCommitRejectsGenericCrossProfilePayloads) {
    FeatureRenderStyle generic;
    FeatureRenderLayer::TessellationContext genericCtx{
        generic, Ellipsoid::WGS84()};

    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(106.4 * kDeg, 29.5 * kDeg)}};
    auto pointMesh = FeatureRenderLayer::tessellateTileMesh(
        genericCtx, {point});
    ASSERT_FALSE(pointMesh.symbols.empty());
    layer_->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);
    EXPECT_EQ(TileMeshCommitResult::EmptyTerminal,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                  std::move(pointMesh)));
    EXPECT_TRUE(build().empty());

    auto poiLayer = std::make_unique<FeatureRenderLayer>(
        "sealed-poi-commit", &device_, Ellipsoid::WGS84());
    Feature polygon = makePolygon(106.4, 29.5, 0.01);
    auto fillMesh = FeatureRenderLayer::tessellateTileMesh(
        genericCtx, {polygon});
    ASSERT_FALSE(fillMesh.fillIndices.empty());
    poiLayer->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    EXPECT_EQ(TileMeshCommitResult::EmptyTerminal,
              poiLayer->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 101, 200},
                  std::move(fillMesh)));
    RenderCommandList poiCommands;
    poiLayer->buildRenderCommands(frame_, *renderer_, poiCommands);
    EXPECT_TRUE(poiCommands.empty());

    Feature officialPoint;
    officialPoint.type = GeometryType::Point;
    officialPoint.rings = {{Cartographic(106.4 * kDeg, 29.5 * kDeg)}};
    addOfficialMetadata(officialPoint, "12024", "1", "10");
    auto officialMesh = FeatureRenderLayer::tessellateTileMesh(
        poiLayer->workerTessellationContext(), {officialPoint});
    ASSERT_EQ(1u, officialMesh.symbols.size());
    genericVisual(officialMesh.symbols.front()).colorPacked = 1.0f;
    EXPECT_EQ(TileMeshCommitResult::EmptyTerminal,
              poiLayer->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 102, 200},
                  std::move(officialMesh)));
}

TEST_F(FeatureRenderLayerTest,
       SealedSurfaceStoreNeverProducesGenericNameLabels) {
    layer_->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);
    Feature road = makeLine(106.4, 29.5, 0.01);
    addOfficialMetadata(road, "20001", "1", "90");
    road.properties["name"] = "must-not-be-generic-label";
    road.properties["rank"] = "1";
    layer_->store().addFeature(std::move(road));

    const auto commands = build();
    EXPECT_TRUE(std::none_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorLabel;
    }));
}

TEST_F(FeatureRenderLayerTest,
       SealedOfficialProfilesRejectEditableStoreProvenance) {
    layer_->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);
    Feature polygon = makePolygon(106.4, 29.5, 0.01);
    addOfficialMetadata(polygon, "30001", "1", "10");
    layer_->store().addFeature(std::move(polygon));

    Feature road = makeLine(106.4, 29.5, 0.01);
    addOfficialMetadata(road, "20001", "1", "20");
    layer_->store().addFeature(std::move(road));

    EXPECT_TRUE(build().empty());
}

TEST_F(FeatureRenderLayerTest,
       OfficialRailwayLabelPayloadDoesNotDuplicateMainLineGeometry) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style.labelPaintOrderExpr = style.paintOrderExpr;
    style.lineStyleGroupExpr = amapClassicLineStyleGroupExpression();
    style.labelStyleGroupExpr = amapClassicLineLabelStyleGroupExpression();
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    style.labelSizePx = 99.0f;
    style.labelSizeExpr = StyleExpression::literal(88.0);
    style.labelOffsetPx = 77.0f;
    style.labelOffsetExpr = StyleExpression::literal(66.0);
    layer_->setStyleForContractTest(style);

    Feature railway = makeLine(106.4, 29.5, 0.02);
    railway.properties["amap_class"] = "20010";
    railway.properties["amap_subkey"] = "2";
    railway.properties["amap_draworder"] = "62";
    railway.properties["amap_minzoom"] = "3";
    railway.properties["amap_maxzoom"] = "20";
    railway.properties["name"] = "成渝铁路";
    railway.properties["amap_rank"] = "1";
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {railway});
    EXPECT_TRUE(mesh.lineIndices.empty());
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(62, mesh.symbols.front().labelPaintOrder);
    EXPECT_EQ(amapClassicStyleIdentity(20010, 2),
              mesh.symbols.front().labelStyleGroup);
    EXPECT_FALSE(mesh.symbols.front().genericVisual.has_value());
    EXPECT_GT(FeatureRenderLayer::resolvedLabelSizePx(
                  style, mesh.symbols.front().labelStyleGroup,
                  8.0, 0.0f),
              0.0f);
}

TEST_F(FeatureRenderLayerTest,
       OfficialGuideSubKey2EmitsLabelWithoutLineGeometryOrUpload) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style.labelPaintOrderExpr = style.paintOrderExpr;
    style.lineStyleGroupExpr = amapClassicLineStyleGroupExpression();
    style.labelStyleGroupExpr = amapClassicLineLabelStyleGroupExpression();
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);

    Feature guide = makeLine(106.4, 29.5, 0.02);
    guide.properties = {{"amap_class", "20014"},
                        {"amap_subkey", "2"},
                        {"amap_draworder", "65"},
                        {"amap_minzoom", "3"},
                        {"amap_maxzoom", "20"},
                        {"amap_rank", "1"},
                        {"name", "官方引导线"}};
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {guide});
    EXPECT_TRUE(mesh.lineVerts.empty());
    EXPECT_TRUE(mesh.lineIndices.empty());
    EXPECT_TRUE(mesh.lineRanges.empty());
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(amapClassicStyleIdentity(20014, 2),
              mesh.symbols.front().labelStyleGroup);
    EXPECT_FLOAT_EQ(11.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, mesh.symbols.front().labelStyleGroup,
                               8.0, 0.0f));
    EXPECT_EQ(8, FeatureRenderLayer::effectiveLabelMinZoom(
                     style, mesh.symbols.front().labelStyleGroup, 0));
}

TEST_F(FeatureRenderLayerTest,
       OfficialConstructionLabelPayloadDoesNotDuplicateMainStroke) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style.labelPaintOrderExpr = style.paintOrderExpr;
    style.lineStyleGroupExpr = amapClassicLineStyleGroupExpression();
    style.labelStyleGroupExpr = amapClassicLineLabelStyleGroupExpression();
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);

    Feature construction = makeLine(106.4, 29.5, 0.02);
    construction.properties = {{"amap_class", "20019"},
                               {"amap_subkey", "1"},
                               {"amap_draworder", "90"},
                               {"amap_minzoom", "3"},
                               {"amap_maxzoom", "20"},
                               {"amap_rank", "1"},
                               {"name", "在建道路"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {construction});
    EXPECT_TRUE(mesh.lineIndices.empty());
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(amapClassicStyleIdentity(20019, 1),
              mesh.symbols.front().labelStyleGroup);
    EXPECT_FLOAT_EQ(10.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, mesh.symbols.front().labelStyleGroup,
                               14.0, 0.0f));

}

TEST(FeatureRenderStyleContractTest, RoadNameZoomCurveUsesDisplayZoom) {
    FeatureRenderStyle style;
    style.labelMinZoomByStyleGroup[82] = 9;
    style.labelSizeExprByStyleGroup[82] = StyleExpression::interpolateLinear(
        StyleExpression::zoom(),
        {{9.0, StyleExpression::literal(11.0)},
         {10.0, StyleExpression::literal(12.0)}});
    EXPECT_EQ(9, FeatureRenderLayer::effectiveLabelMinZoom(style, 82, 0));
    EXPECT_EQ(11, FeatureRenderLayer::effectiveLabelMinZoom(style, 82, 11));
    EXPECT_FLOAT_EQ(11.0f,
                    FeatureRenderLayer::resolvedLabelSizePx(style, 82, 9.0,
                                                            28.0f));
    EXPECT_FLOAT_EQ(11.5f,
                    FeatureRenderLayer::resolvedLabelSizePx(style, 82, 9.5,
                                                            28.0f));
}

TEST(FeatureRenderStyleContractTest, LabelHaloCurveUsesDisplayZoom) {
    FeatureRenderStyle style;
    style.labelHaloColor = {0, 0, 0, 0};
    style.labelHaloColorExprByStyleGroup[50005] = StyleExpression::step(
        StyleExpression::zoom(),
        {{4.0, StyleExpression::literal(
                   std::array<float, 4>{1, 1, 1, 0.8f})},
         {5.0, StyleExpression::literal(
                   std::array<float, 4>{1, 1, 1, 1})}});
    EXPECT_FLOAT_EQ(0.8f, FeatureRenderLayer::resolvedLabelHaloColor(
                              style, 50005, 4.0)[3]);
    EXPECT_FLOAT_EQ(1.0f, FeatureRenderLayer::resolvedLabelHaloColor(
                              style, 50005, 5.0)[3]);
}

TEST(FeatureRenderStyleContractTest,
     OfficialHaloNeverFallsBackToGenericOrFixedColor) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    ASSERT_FALSE(style.labelHaloWidthExprByStyleGroup.empty());
    const int kDistrict = style.labelHaloWidthExprByStyleGroup.begin()->first;
    style.labelHaloColor = {1, 0, 0, 1};
    style.labelHaloColorByStyleGroup[kDistrict] = {0, 1, 0, 1};
    style.labelHaloColorExprByStyleGroup[kDistrict] =
        StyleExpression::literal(42.0);
    EXPECT_EQ((std::array<float, 4>{0, 0, 0, 0}),
              FeatureRenderLayer::resolvedLabelHaloColor(
                  style, kDistrict, 12.0));
    style.labelHaloColorExprByStyleGroup.erase(kDistrict);
    EXPECT_EQ((std::array<float, 4>{0, 0, 0, 0}),
              FeatureRenderLayer::resolvedLabelHaloColor(
                  style, kDistrict, 12.0));
}

TEST(FeatureRenderStyleContractTest,
     OfficialHaloWidthNeverFallsBackToGenericScalar) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    ASSERT_FALSE(style.labelHaloWidthExprByStyleGroup.empty());
    const int kDistrict = style.labelHaloWidthExprByStyleGroup.begin()->first;
    style.labelHaloPx = 99.0f;
    EXPECT_FLOAT_EQ(1.0f, FeatureRenderLayer::resolvedLabelHaloWidthPx(
                              style, kDistrict, 12.0));
    style.labelHaloWidthExprByStyleGroup[kDistrict] =
        StyleExpression::literal(2.5);
    EXPECT_FLOAT_EQ(2.5f, FeatureRenderLayer::resolvedLabelHaloWidthPx(
                              style, kDistrict, 12.0));
    style.labelHaloWidthExprByStyleGroup[kDistrict] =
        StyleExpression::literal(-1.0);
    EXPECT_LT(FeatureRenderLayer::resolvedLabelHaloWidthPx(
                  style, kDistrict, 12.0),
              0.0f);
    style.labelHaloWidthExprByStyleGroup.erase(kDistrict);
    EXPECT_LT(FeatureRenderLayer::resolvedLabelHaloWidthPx(
                  style, kDistrict, 12.0),
              0.0f);
}

TEST(FeatureRenderStyleContractTest,
     RoadNameColorCurveUsesStyleGroupAndDisplayZoom) {
    FeatureRenderStyle style;
    style.labelColor = {1.0f, 0.0f, 0.0f, 1.0f};
    style.labelColorExprByStyleGroup[82] =
        StyleExpression::interpolateLinear(
            StyleExpression::zoom(),
            {{9.0, StyleExpression::literal(
                       std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f})},
             {17.0, StyleExpression::literal(
                        std::array<float, 4>{0.8f, 0.4f, 0.2f, 1.0f})}});

    const auto mid = FeatureRenderLayer::resolvedLabelColor(style, 82, 13.0);
    EXPECT_NEAR(0.4f, mid[0], 1e-6f);
    EXPECT_NEAR(0.2f, mid[1], 1e-6f);
    EXPECT_NEAR(0.1f, mid[2], 1e-6f);
    EXPECT_FLOAT_EQ(1.0f, mid[3]);
    EXPECT_EQ(style.labelColor,
              FeatureRenderLayer::resolvedLabelColor(style, 79, 13.0));
}

TEST_F(FeatureRenderLayerTest, NormalNamedRoadKeepsLineGeometry) {
    Feature road = makeLine(106.4, 29.5, 0.2);
    road.properties["amap_class"] = "20001";
    road.properties["name"] = "真实道路名";

    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {road});
    EXPECT_FALSE(mesh.lineIndices.empty());
    EXPECT_TRUE(mesh.symbols.empty());
}

TEST_F(FeatureRenderLayerTest,
       RequiredOfficialLabelIdentityRejectsUnknownRoadBeforeSymbolWork) {
    FeatureRenderStyle style = layer_->style();
    style.labelProperty = "name";
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    style.lineStyleGroupExpr = StyleExpression::match(
        "amap_class",
        {{"20010", StyleExpression::match(
                       "amap_subkey",
                       {{"1", StyleExpression::literal(20010001.0)}},
                       StyleExpression::literal(0.0))}},
        StyleExpression::literal(0.0));
    style.labelStyleGroupExpr = StyleExpression::match(
        "amap_class",
        {{"20010", StyleExpression::match(
                       "amap_subkey",
                       {{"1", StyleExpression::literal(20010001.0)}},
                       StyleExpression::literal(0.0))}},
        StyleExpression::literal(0.0));

    Feature unknown;
    unknown.type = GeometryType::LineString;
    unknown.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg),
                      Cartographic(6.1 * kDeg, 29.0 * kDeg)}};
    unknown.properties = {{"name", "unknown railway"},
                          {"amap_class", "20010"},
                          {"amap_subkey", "999"},
                          {"amap_draworder", "9137"}};
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        FeatureRenderLayer::TessellationContext{
            style, Ellipsoid::WGS84(), nullptr, nullptr, false, 10.0},
        {unknown});
    EXPECT_TRUE(mesh.symbols.empty());
    EXPECT_TRUE(mesh.lineIndices.empty())
        << "unknown official tuple must fail before line tessellation/upload";
}

TEST_F(FeatureRenderLayerTest,
       ProviderPointWorkerDoesNotCarryGenericVisualFallbackState) {
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "circle";
    style.pointColor = {1.0f, 0.0f, 0.0f, 1.0f};
    style.labelOffsetPx = 27.0f;
    style.pointStylePropertyA = "amap_class";
    style.pointStylePropertyB = "amap_subkey";
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    installTestOfficialLabelStyle(style);
    style.pointStyleResolver = [](const std::string&, const std::string&,
                                  const std::string&, double, float) {
        return FeatureRenderStyle::ResolvedPointStyle{};
    };
    layer_->setStyleForContractTest(style);

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties["amap_draworder"] = "1";
    poi.properties["amap_rank"] = "1";
    poi.properties = {{"name", "official"},
                      {"amap_class", "12024"},
                      {"amap_subkey", "1"}};
    addOfficialMetadata(poi, "12024", "1");
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_FALSE(mesh.symbols[0].genericVisual.has_value());
}

TEST_F(FeatureRenderLayerTest, ProviderRoadNameKeepsProviderSelectedBentPath) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    style.labelStyleGroupExpr = StyleExpression::literal(
        amapClassicStyleIdentity(20001, 1));
    layer_->setStyleForContractTest(style);
    Feature road;
    road.type = GeometryType::LineString;
    road.rings = {{Cartographic(106.4 * kDeg, 29.5 * kDeg),
                   Cartographic(106.41 * kDeg, 29.5 * kDeg),
                   Cartographic(106.41 * kDeg, 29.51 * kDeg)}};
    road.properties["amap_class"] = "20001";
    road.properties["name"] = "急弯道路";
    addOfficialMetadata(road, "20001", "1");
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {road});
    EXPECT_EQ(1u, mesh.symbols.size());
    EXPECT_TRUE(mesh.lineIndices.empty());
}

// 海拔着色轨迹(2026-08-23):逐顶点椭球高 → a_color 线性渐变,复用既有
// VectorLine48 布局(shader 无改动);lengthSoFar 原样携带(dash 语义不变)。
TEST_F(FeatureRenderLayerTest, LineHeightGradientBakesPerVertexColors) {
    Feature line;
    line.type = GeometryType::LineString;
    line.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0),
                   Cartographic(6.02 * kDeg, 29.02 * kDeg, 1500.0),
                   Cartographic(6.04 * kDeg, 29.04 * kDeg, 3000.0)}};
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::Absolute;
    style.lineColorGradientByHeight = true;
    style.lineColorGradientHeightMinMeters = 0.0f;
    style.lineColorGradientHeightMaxMeters = 3000.0f;
    style.lineColorGradientLow = {0.10f, 0.55f, 0.25f, 0.95f};
    style.lineColorGradientHigh = {0.90f, 0.15f, 0.15f, 0.95f};
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(std::move(line));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    ASSERT_EQ(RenderCommandKind::VectorLine, commands[0].kind);
    const auto* vb = dynamic_cast<const DummyBuffer*>(commands[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    EXPECT_EQ(48, commands[0].vertexStride);
    // 3 折线点 × 2 ribbon 顶点 × 12 float(48B)。
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    ASSERT_EQ(3u * 2u * 12u, vb->bytes().size() / sizeof(float));

    auto unpack = [](float packed) {
        uint32_t u = 0;
        std::memcpy(&u, &packed, sizeof(u));
        return std::array<float, 4>{
            ((u >> 0) & 0xFF) / 255.0f, ((u >> 8) & 0xFF) / 255.0f,
            ((u >> 16) & 0xFF) / 255.0f, ((u >> 24) & 0xFF) / 255.0f};
    };
    auto vertexColor = [&](size_t polylineIndex) {
        return unpack(floats[polylineIndex * 2 * 12 + 11]);
    };
    const auto c0 = vertexColor(0);
    const auto c1 = vertexColor(1);
    const auto c2 = vertexColor(2);
    // 0m → low(绿);3000m → high(红);1500m → 严格中点。
    EXPECT_NEAR(c0[0], 0.10f, 0.02f);
    EXPECT_NEAR(c0[1], 0.55f, 0.02f);
    EXPECT_NEAR(c0[2], 0.25f, 0.02f);
    EXPECT_NEAR(c2[0], 0.90f, 0.02f);
    EXPECT_NEAR(c2[1], 0.15f, 0.02f);
    EXPECT_NEAR(c2[2], 0.15f, 0.02f);
    EXPECT_GT(c1[0], c0[0] + 0.05f);
    EXPECT_LT(c1[0], c2[0] - 0.05f);
    EXPECT_LT(c1[1], c0[1] - 0.05f);
    EXPECT_GT(c1[1], c2[1] + 0.05f);

    // lengthSoFar 原样携带:首点 0、末点全长、递增(与 shader dash 契约不变)。
    const float l0 = floats[0 * 2 * 12 + 10];
    const float l1 = floats[1 * 2 * 12 + 10];
    const float l2 = floats[2 * 2 * 12 + 10];
    EXPECT_FLOAT_EQ(0.0f, l0);
    EXPECT_GT(l1, l0);
    EXPECT_GT(l2, l1);
}

// 渐变开关关闭 → 整线统一字面量色(默认行为不回退)。
TEST_F(FeatureRenderLayerTest, LineHeightGradientOffUsesUniformColor) {
    Feature road = makeLine(6.0, 29.0, 0.01);
    road.properties = {{"amap_draworder", "1"},
                       {"amap_minzoom", "3"},
                       {"amap_maxzoom", "20"}};
    layer_->store().addFeature(std::move(road));
    FeatureRenderStyle style;
    style.lineColor = {1.0f, 0.5f, 0.25f, 0.9f};
    layer_->setStyleForContractTest(style);

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    ASSERT_EQ(RenderCommandKind::VectorLine, commands[0].kind);
    const auto* vb = dynamic_cast<const DummyBuffer*>(commands[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    const size_t vertexCount = vb->bytes().size() / commands[0].vertexStride;
    ASSERT_EQ(6u, vertexCount);  // open 3 点 ribbon
    for (size_t v = 0; v < vertexCount; ++v) {
        EXPECT_FLOAT_EQ(floats[0 * 12 + 11], floats[v * 12 + 11])
            << "vertex " << v << " 偏离统一线色";
    }
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
    EXPECT_EQ(layer_->style().paintOrder, cmd.vectorPaintOrder);
    EXPECT_EQ(36, cmd.vertexStride);  // P6b:+color;P6c:+uv/shape
    EXPECT_EQ(6, cmd.indexCount);
    EXPECT_EQ("color", cmd.pass);
    // 符号不做硬件逐像素深度测试:billboard 四角共用锚点深度,逐像素比对
    // 只会把 quad 切一块(那道切口是不存在的形状边界)。遮挡改由锚点判定
    // (u_terrainOcclusion/u_symbolOcclusion)整符号决定。
    EXPECT_FALSE(cmd.depthTest);
    EXPECT_FALSE(cmd.depthWrite);
    EXPECT_TRUE(cmd.blend);
    ASSERT_TRUE(cmd.hasVectorUniforms);
    EXPECT_TRUE(cmd.uniforms.empty());
    EXPECT_FLOAT_EQ(layer_->presentationPolicy().symbolOccludedMinOpacity,
                    cmd.vectorUniforms.symbolOcclusion[1]);
    EXPECT_FLOAT_EQ(layer_->style().pointSizePx,
                    cmd.vectorUniforms.pointSizePx);
    EXPECT_FLOAT_EQ(800.0f, cmd.vectorUniforms.viewport[0]);
    EXPECT_FLOAT_EQ(600.0f, cmd.vectorUniforms.viewport[1]);
    // T2 不变量:**没有图标图集时也要占位**,深度纹理恒落 textures[1]。
    // 后端按下标 1:1 绑纹理单元,下标随图集有无浮动会把深度绑到图集的
    // 采样器上 —— 表现为图标被一张深度图替换,极难从现象反推。
    ASSERT_EQ(2u, cmd.textures.size());
    EXPECT_EQ(nullptr, cmd.textures[0]);  // 本例无图集
    EXPECT_EQ(nullptr, cmd.textures[1]);  // host 无深度通路
    EXPECT_FLOAT_EQ(0.0f, cmd.vectorUniforms.terrainOcclusion[0]);

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

TEST_F(FeatureRenderLayerTest,
       OfficialSymbolsIgnoreGenericPresentationPolicy) {
    build();
    std::vector<uint8_t> frame(4u * 4u * 4u, 255);
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "official-presentation-policy", 4, 4, frame));
    auto style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    style.pointStyleResolver = [](const std::string&, const std::string&,
                                  const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle out;
        out.enabled = true;
        out.image = "official-presentation-policy";
        out.sizePx = 16.0f;
        out.labelLayout.emplace();
        out.labelLayout->iconWidthPx = 16.0f;
        out.labelLayout->iconHeightPx = 16.0f;
        return out;
    };
    layer_->setStyleForContractTest(style);
    layer_->setPresentationPolicy({1.0f, 0.75f});

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties = {{"amap_class", "12024"},
                      {"amap_subkey", "1178"},
                      {"amap_draworder", "9137"},
                      {"amap_rank", "1"},
                      {"amap_minzoom", "3"},
                      {"amap_maxzoom", "20"},
                      {"name", "official"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 14, 13038, 5501},
                  std::move(mesh)));

    const RenderCommandList commands = build();
    ASSERT_FALSE(commands.empty());
    for (const RenderCommand& command : commands) {
        if (command.kind != RenderCommandKind::VectorPoint &&
            command.kind != RenderCommandKind::VectorLabel) {
            continue;
        }
        ASSERT_TRUE(command.hasVectorUniforms);
        EXPECT_FLOAT_EQ(0.0f, command.vectorUniforms.depthPushNdc);
        EXPECT_FLOAT_EQ(0.0f, command.vectorUniforms.symbolOcclusion[1]);
    }
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

TEST_F(FeatureRenderLayerTest, SurfaceRebindRebuildsEachStoreBucketOnce) {
    Feature a;
    a.type = GeometryType::Point;
    a.rings = {{Cartographic(6.005 * kDeg, 29.005 * kDeg)}};
    layer_->store().addFeature(a);
    Feature b = a;
    b.rings = {{Cartographic(6.006 * kDeg, 29.006 * kDeg)}};
    layer_->store().addFeature(std::move(b));

    const int beforeInitialBuild = device_.createdBufferCount;
    build();
    const int initialBucketBuffers =
        device_.createdBufferCount - beforeInitialBuild;
    ASSERT_GT(initialBucketBuffers, 0);

    layer_->setRenderDevice(nullptr);
    const int beforeRebind = device_.createdBufferCount;
    layer_->setRenderDevice(&device_);
    EXPECT_EQ(device_.createdBufferCount - beforeRebind, initialBucketBuffers)
        << "surface recreation must rebuild a shared bucket once, not once "
           "per feature";
}

TEST_F(FeatureRenderLayerTest, PointPaintOrderExprSplitsCommands) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::match(
        "priority",
        {{"low", StyleExpression::literal(20.0)},
         {"high", StyleExpression::literal(100.0)}},
        StyleExpression::literal(0.0));
    layer_->setStyleForContractTest(style);

    Feature low;
    low.type = GeometryType::Point;
    low.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    low.properties["priority"] = "low";
    Feature high;
    high.type = GeometryType::Point;
    high.rings = {{Cartographic(6.01 * kDeg, 29.0 * kDeg)}};
    high.properties["priority"] = "high";
    layer_->store().addFeature(std::move(high));
    layer_->store().addFeature(std::move(low));

    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    sortMvpRenderCommands(commands);
    EXPECT_EQ(RenderCommandKind::VectorPoint, commands[0].kind);
    EXPECT_EQ(RenderCommandKind::VectorPoint, commands[1].kind);
    EXPECT_EQ(20, commands[0].vectorPaintOrder);
    EXPECT_EQ(100, commands[1].vectorPaintOrder);
    EXPECT_EQ(6, commands[0].indexCount);
    EXPECT_EQ(6, commands[1].indexCount);
    EXPECT_EQ(commands[0].vertexBuffer, commands[1].vertexBuffer);
    EXPECT_EQ(commands[0].indexBuffer, commands[1].indexBuffer);
    EXPECT_EQ(0, commands[0].indexOffset);
    EXPECT_EQ(6, commands[1].indexOffset);
}

namespace {
std::vector<uint8_t> loadHostFont();
}

TEST_F(FeatureRenderLayerTest, LabelPaintOrderExprSplitsCommands) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::match(
        "priority",
        {{"low", StyleExpression::literal(20.0)},
         {"high", StyleExpression::literal(100.0)}},
        StyleExpression::literal(0.0));
    layer_->setStyleForContractTest(style);

    Feature low;
    low.type = GeometryType::Point;
    low.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    low.properties["priority"] = "low";
    low.properties["name"] = "A";
    Feature high;
    high.type = GeometryType::Point;
    high.rings = {{Cartographic(6.01 * kDeg, 29.0 * kDeg)}};
    high.properties["priority"] = "high";
    high.properties["name"] = "B";
    layer_->store().addFeature(std::move(high));
    layer_->store().addFeature(std::move(low));

    RenderCommandList commands = build();
    std::vector<const RenderCommand*> labels;
    for (const auto& command : commands) {
        if (command.kind == RenderCommandKind::VectorLabel) {
            labels.push_back(&command);
        }
    }
    ASSERT_EQ(2u, labels.size());
    sortMvpRenderCommands(commands);
    labels.clear();
    for (const auto& command : commands) {
        if (command.kind == RenderCommandKind::VectorLabel) {
            labels.push_back(&command);
        }
    }
    ASSERT_EQ(2u, labels.size());
    EXPECT_EQ(20, labels[0]->vectorPaintOrder);
    EXPECT_EQ(100, labels[1]->vectorPaintOrder);
    EXPECT_EQ(6, labels[0]->indexCount);
    EXPECT_EQ(6, labels[1]->indexCount);
    EXPECT_EQ(0, labels[0]->indexOffset);
    EXPECT_EQ(6, labels[1]->indexOffset);
}

// 符号刀A:瓦片实例表在准入(commitTileMesh)定型成 billboard quad,
// 走与 store 路径同一条 VectorPoint 命令层。纯符号瓦片(无 fill/line)
// 也必须出命令 —— empty() 若不认 symbols,这里整瓦被 drop。
TEST_F(FeatureRenderLayerTest, TileSymbolsRenderAfterCommit) {
    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    TileSymbolCpu s;
    s.lonRad = 6.0 * kDeg;
    s.latRad = 29.0 * kDeg;
    genericVisual(s).colorPacked = 1.0f;
    mesh.symbols.push_back(s);

    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorPoint, commands[0].kind);
    EXPECT_EQ(36, commands[0].vertexStride);
    EXPECT_EQ(6, commands[0].indexCount);
    // 锚点 = 桶原点 → 首顶点 rel(0,0,0)(与 store 路径同一打包契约)。
    const auto* vb = dynamic_cast<const DummyBuffer*>(commands[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(0.0f, floats[0]);
    EXPECT_FLOAT_EQ(0.0f, floats[1]);
    EXPECT_FLOAT_EQ(0.0f, floats[2]);
}

TEST_F(FeatureRenderLayerTest,
       LateBoundProviderSymbolStaysAtomicUntilExactFrameArrives) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();  // Cache glyph/icon atlas pointers before tile commit.

    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    // Inject generic presentation scalars only after the official profile is
    // installed. They must not influence provider-owned CSS geometry or the
    // binary Support.scale uniform.
    style.pointSizePx = 999.0f;
    style.pointSizeExpr = StyleExpression::literal(777.0);
    style.scaleStylePixelsByDevicePixelRatio = false;
    style.pointStylePropertyA = "provider_class";
    style.pointStylePropertyB = "provider_subkey";
    style.pointIdentityValidator = [](const std::string& cls,
                                      const std::string& sub) {
        return cls == "10002" && sub == "5";
    };
    installTestOfficialLabelStyle(style);
    style.pointStyleResolver = [](const std::string& classValue,
                                  const std::string& subKeyValue,
                                  const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle resolved;
        if (classValue == "10002" && subKeyValue == "5") {
            resolved.enabled = true;
            resolved.image = "provider-icons-1-107";
            resolved.sizePx = 21.0f;
            resolved.color = {1, 1, 1, 1};
            resolved.labelLayout.emplace();
            resolved.labelLayout->iconWidthPx = 21.0f;
            resolved.labelLayout->iconHeightPx = 21.0f;
            resolved.labelLayout->iconAnchorXPx = 10.0f;
            resolved.labelLayout->iconAnchorYPx = 10.0f;
        }
        return resolved;
    };
    layer_->setStyleForContractTest(style);
    frame_.devicePixelRatio = 2.0f;
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.0)),
                   surface, Vec3(0.0, 0.0, 1.0));

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties["name"] = "AB";
    poi.properties["provider_class"] = "10002";
    poi.properties["provider_subkey"] = "5";
    addOfficialMetadata(poi, "10002", "5");
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ("10002", mesh.symbols[0].pointStyleKeyA);
    EXPECT_EQ("5", mesh.symbols[0].pointStyleKeyB);
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    RenderCommandList commands = build();
    EXPECT_FALSE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorPoint;
    })) << "provider-owned missing frame must not fabricate a circle";
    EXPECT_FALSE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorLabel;
    })) << "provider icon and label must remain atomic while artwork loads";

    std::vector<uint8_t> officialFrame(64u * 64u * 4u, 255);
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "provider-icons-1-107", 64, 64, officialFrame));
    commands = build();
    const auto point = std::find_if(commands.begin(), commands.end(),
                                    [](const auto& c) {
        return c.kind == RenderCommandKind::VectorPoint;
    });
    ASSERT_NE(commands.end(), point);
    EXPECT_TRUE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorLabel;
    }));
    EXPECT_FLOAT_EQ(2.0f, point->vectorUniforms.pointSizePx);
    const auto* vb = dynamic_cast<const DummyBuffer*>(point->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_LT(f[8], 0.0f);
    EXPECT_FLOAT_EQ(-10.0f, f[3]);
    EXPECT_FLOAT_EQ(-11.0f, f[4])
        << "provider quad consumes the official 10/10 CSS-px anchor";
    EXPECT_FLOAT_EQ(11.0f, f[2 * 9 + 3]);
    EXPECT_FLOAT_EQ(10.0f, f[2 * 9 + 4]);
    EXPECT_FLOAT_EQ(42.0f,
                    (f[2 * 9 + 3] - f[3]) *
                        point->vectorUniforms.pointSizePx)
        << "21 CSS px artwork scales once to 42 physical px at DPR2";
    EXPECT_FLOAT_EQ(42.0f,
                    (f[2 * 9 + 4] - f[4]) *
                        point->vectorUniforms.pointSizePx);
}

TEST_F(FeatureRenderLayerTest,
       OfficialBottomCenterAnchorDrivesQuadAtDevicePixelRatioOnce) {
    build();
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "official-bottom-center", 128, 128,
        std::vector<uint8_t>(128u * 128u * 4u, 255)));
    FeatureRenderStyle style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    style.pointIdentityValidator = [](const std::string& cls,
                                      const std::string& sub) {
        return cls == "10007" && sub == "190";
    };
    style.pointStyleResolver = [](const std::string&, const std::string&,
                                  const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle out;
        out.enabled = true;
        out.image = "official-bottom-center";
        out.sizePx = 51.0f;
        out.labelLayout.emplace();
        out.labelLayout->iconWidthPx = 51.0f;
        out.labelLayout->iconHeightPx = 51.0f;
        out.labelLayout->iconAnchor =
            FeatureRenderStyle::ProviderLabelLayout::IconAnchor::BottomCenter;
        return out;
    };
    layer_->setStyleForContractTest(style);
    frame_.devicePixelRatio = 2.0f;

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    addOfficialMetadata(poi, "10007", "190");
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
        std::move(mesh));

    const RenderCommandList commands = build();
    const auto point = std::find_if(commands.begin(), commands.end(),
                                    [](const auto& command) {
        return command.kind == RenderCommandKind::VectorPoint;
    });
    ASSERT_NE(commands.end(), point);
    const auto* vb = dynamic_cast<const DummyBuffer*>(point->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* vertices =
        reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(-25.5f, vertices[3]);
    EXPECT_FLOAT_EQ(0.0f, vertices[4]);
    EXPECT_FLOAT_EQ(25.5f, vertices[2 * 9 + 3]);
    EXPECT_FLOAT_EQ(51.0f, vertices[2 * 9 + 4]);
    EXPECT_FLOAT_EQ(2.0f, point->vectorUniforms.pointSizePx);
    EXPECT_FLOAT_EQ(-51.0f,
                    vertices[3] * point->vectorUniforms.pointSizePx);
    EXPECT_FLOAT_EQ(102.0f,
                    vertices[2 * 9 + 4] *
                        point->vectorUniforms.pointSizePx);
}

TEST_F(FeatureRenderLayerTest,
       LateBoundUnknownIdentityNeverFallsBackToSyntheticShape) {
    FeatureRenderStyle style = layer_->style();
    style.pointStylePropertyA = "provider_class";
    style.pointStylePropertyB = "provider_subkey";
    style.pointIdentityValidator = [](const std::string& cls,
                                      const std::string& sub) {
        return cls == "12024" && sub == "1230";
    };
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    installTestOfficialLabelStyle(style);
    style.pointStyleResolver = [](const std::string&, const std::string&,
                                  const std::string&, double, float) {
        return FeatureRenderStyle::ResolvedPointStyle{};
    };
    layer_->setStyleForContractTest(style);

    Feature unknown;
    unknown.type = GeometryType::Point;
    unknown.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    unknown.properties["provider_class"] = "unknown";
    unknown.properties["provider_subkey"] = "unknown";
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {unknown});
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    const RenderCommandList commands = build();
    EXPECT_TRUE(commands.empty());
}

TEST_F(FeatureRenderLayerTest,
       OfficialIconOnlyIdentitySkipsGlyphAndLabelContracts) {
    build();
    std::vector<uint8_t> frame(64u * 64u * 4u, 255);
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "official-icon-only", 64, 64, frame));

    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    style.pointStylePropertyA = "amap_class";
    style.pointStylePropertyB = "amap_subkey";
    style.pointStyleResolver = [](const std::string& classValue,
                                  const std::string& subKeyValue,
                                  const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle out;
        if (classValue == "12024" && subKeyValue == "854") {
            out.enabled = true;
            out.image = "official-icon-only";
            out.sizePx = 21.0f;
            out.labelLayout.emplace();
            out.labelLayout->iconWidthPx = 21.0f;
            out.labelLayout->iconHeightPx = 21.0f;
            out.labelLayout->iconAnchorXPx = 10.0f;
            out.labelLayout->iconAnchorYPx = 10.0f;
        }
        return out;
    };
    layer_->setStyleForContractTest(style);

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties = {{"name", "icon-only"},
                      {"amap_class", "12024"},
                      {"amap_subkey", "854"}};
    addOfficialMetadata(poi, "12024", "854");
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(0, mesh.symbols.front().labelStyleGroup);
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    const auto commands = build();
    EXPECT_TRUE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorPoint;
    }));
    EXPECT_FALSE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorLabel;
    }));
}

TEST_F(FeatureRenderLayerTest,
       OfficialMultilineLayoutUsesPerLineWidthAndBoSpacing) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();

    FeatureRenderStyle style = layer_->style();
    style.labelSizePx = 20.0f;
    style.pointStylePropertyA = "provider_class";
    style.pointStylePropertyB = "provider_subkey";
    style.pointIdentityValidator = [](const std::string& cls,
                                      const std::string& sub) {
        return cls == "x" && sub == "y";
    };
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    style.labelSizePx = 20.0f;
    style.pointStylePropertyA = "provider_class";
    style.pointStylePropertyB = "provider_subkey";
    style.pointIdentityValidator = [](const std::string& cls,
                                      const std::string& sub) {
        return cls == "x" && sub == "y";
    };
    installTestOfficialLabelStyle(style);
    style.pointStyleResolver = [](const std::string&, const std::string&,
                                  const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle out;
        out.labelLayout.emplace();
        out.labelLayout->direction =
            FeatureRenderStyle::LabelDirection::Center;
        return out;
    };
    layer_->setStyleForContractTest(style);

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties = {{"name", "A\nBB"},
                      {"provider_class", "x"},
                      {"provider_subkey", "y"}};
    addOfficialMetadata(poi, "12024", "1");
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));
    frame_.devicePixelRatio = 2.0f;
    const auto commands = build();
    const auto label = std::find_if(commands.begin(), commands.end(),
                                    [](const auto& command) {
        return command.kind == RenderCommandKind::VectorLabel;
    });
    ASSERT_NE(commands.end(), label);
    EXPECT_EQ(18, label->indexCount) << "newline is layout, not a glyph";
    const auto* vb = dynamic_cast<const DummyBuffer*>(label->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* v = reinterpret_cast<const float*>(vb->bytes().data());
    constexpr size_t stride = 11;
    const float firstLineLeft = v[6];
    const float firstLineRight = v[1 * stride + 6];
    const float secondLineLeft = v[4 * stride + 6];
    const float secondGlyphRight = v[9 * stride + 6];
    EXPECT_NEAR((firstLineLeft + firstLineRight) * 0.5f,
                (secondLineLeft + secondGlyphRight) * 0.5f, 1.0f)
        << "XV centers each line using its measured width";
    EXPECT_NEAR(46.0f, v[7] - v[4 * stride + 7], 1.0f)
        << "BO advances each line by (fontSize + 3 CSS px) * DPR";
}

TEST_F(FeatureRenderLayerTest,
       OfficialLabelCommandConsumesRuntimeSdfFragmentContract) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();

    FeatureRenderStyle style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    installTestOfficialLabelStyle(style);
    style.labelSizeExprByStyleGroup[1] = StyleExpression::literal(24.0);
    style.pointStyleResolver = [](const std::string&, const std::string&,
                                  const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle out;
        out.labelLayout.emplace();
        return out;
    };
    layer_->setStyleForContractTest(style);

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties = {{"name", "AB"}};
    addOfficialMetadata(poi, "12024", "1");
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    frame_.devicePixelRatio = 1.0f;
    const auto commandsDpr1 = build();
    const auto dpr1 = std::find_if(commandsDpr1.begin(), commandsDpr1.end(),
                                   [](const auto& command) {
        return command.kind == RenderCommandKind::VectorLabel;
    });
    ASSERT_NE(commandsDpr1.end(), dpr1);
    constexpr float edge = 205.0f / 256.0f;
    constexpr float strokeWidth = 1.0f;
    EXPECT_NEAR(edge, dpr1->vectorUniforms.sdfEdge, 1e-6f);
    EXPECT_NEAR(edge - edge * (1.0f - strokeWidth / 10.1f),
                dpr1->vectorUniforms.sdfHaloDelta, 1e-6f);
    EXPECT_NEAR(1.4142f * 1.5f / 24.0f,
                dpr1->vectorUniforms.sdfGamma, 1e-6f);

    frame_.devicePixelRatio = 2.0f;
    const auto commandsDpr2 = build();
    const auto dpr2 = std::find_if(commandsDpr2.begin(), commandsDpr2.end(),
                                   [](const auto& command) {
        return command.kind == RenderCommandKind::VectorLabel;
    });
    ASSERT_NE(commandsDpr2.end(), dpr2);
    const float buffer = edge + 1.5f / 256.0f;
    const float borderBuffer = edge * (1.0f - 2.0f / 10.1f);
    EXPECT_NEAR(buffer, dpr2->vectorUniforms.sdfEdge, 1e-6f);
    EXPECT_NEAR(buffer - borderBuffer,
                dpr2->vectorUniforms.sdfHaloDelta, 1e-6f);
    EXPECT_NEAR(1.4142f * 1.7f / 24.0f,
                dpr2->vectorUniforms.sdfGamma, 1e-6f);
}

TEST_F(FeatureRenderLayerTest,
       OfficialRoadLabelIdentityReachesFinalSdfColorsAndHalo) {
    GlyphAtlas* glyphAtlas = renderer_->glyphAtlas();
    ASSERT_NE(nullptr, glyphAtlas);
    glyphAtlas->activateAmapOfficialProviderForTest([](uint32_t) {});
    std::vector<uint8_t> glyphPixels(64u * 32u, 127);
    const std::vector<GlyphAtlas::ProviderGlyph> glyphs = {
        {'A', 22, 22, 1, -2, 24, 0, 0},
        {'B', 22, 22, 1, -2, 24, 32, 0},
    };
    ASSERT_TRUE(glyphAtlas->installAmapOfficialGlyphBatchForTest(
        64, 32, glyphPixels, glyphs));
    build();
    FeatureRenderStyle style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);

    Feature road;
    road.type = GeometryType::LineString;
    road.rings = {{Cartographic(106.52 * kDeg, 29.54 * kDeg),
                   Cartographic(106.55 * kDeg, 29.56 * kDeg),
                   Cartographic(106.59 * kDeg, 29.59 * kDeg)}};
    road.properties["amap_class"] = "20001";
    road.properties["amap_subkey"] = "1";
    road.properties["amap_draworder"] = "82";
    road.properties["amap_minzoom"] = "10";
    road.properties["amap_maxzoom"] = "30";
    road.properties["amap_rank"] = "1";
    road.properties["name"] = "AB";
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {road});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(amapClassicStyleIdentity(20001, 1),
              mesh.symbols.front().labelStyleGroup);
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 14, 13038, 5501},
                  std::move(mesh)));

    const Vec3 target = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(106.55 * kDeg, 29.56 * kDeg));
    const Vec3 radial = target.normalized();
    constexpr double displayZoom = 10.0;
    const double cameraHeight = 4.0e7 / std::pow(2.0, displayZoom);
    camera_.lookAt(target + radial * cameraHeight, target,
                   Vec3(0.0, 0.0, 1.0));
    ++frame_.frameId;
    frame_.devicePixelRatio = 1.0f;
    RenderCommandList commands;
    for (int i = 0; i < 12; ++i) {
        commands = build();
        if (std::any_of(commands.begin(), commands.end(), [](const auto& c) {
                return c.kind == RenderCommandKind::VectorLabel;
            })) break;
        ++frame_.frameId;
    }
    const auto label = std::find_if(
        commands.begin(), commands.end(), [](const auto& command) {
            return command.kind == RenderCommandKind::VectorLabel;
        });
    ASSERT_NE(commands.end(), label);
    // Official road-label style group 20001:1 at display zoom 10 consumes
    // provider zoom 11 records: #606066 text and opaque white casing.
    EXPECT_NEAR(0x60 / 255.0f, label->vectorUniforms.color[0], 1e-6f);
    EXPECT_NEAR(0x60 / 255.0f, label->vectorUniforms.color[1], 1e-6f);
    EXPECT_NEAR(0x66 / 255.0f, label->vectorUniforms.color[2], 1e-6f);
    EXPECT_FLOAT_EQ(1.0f, label->vectorUniforms.color[3]);
    EXPECT_FLOAT_EQ(1.0f, label->vectorUniforms.haloColor[0]);
    EXPECT_FLOAT_EQ(1.0f, label->vectorUniforms.haloColor[1]);
    EXPECT_FLOAT_EQ(1.0f, label->vectorUniforms.haloColor[2]);
    EXPECT_FLOAT_EQ(1.0f, label->vectorUniforms.haloColor[3]);
    constexpr float labelSizeCssPx = 11.0f;
    constexpr float haloWidthCssPx = 1.0f;
    constexpr float baseEdge = 205.0f / 256.0f;
    EXPECT_FLOAT_EQ(labelSizeCssPx, FeatureRenderLayer::resolvedLabelSizePx(
                                          style,
                                          amapClassicStyleIdentity(20001, 1),
                                          displayZoom, 0.0f));
    EXPECT_FLOAT_EQ(haloWidthCssPx,
                    FeatureRenderLayer::resolvedLabelHaloWidthPx(
                        style, amapClassicStyleIdentity(20001, 1),
                        displayZoom));
    EXPECT_NEAR(baseEdge, label->vectorUniforms.sdfEdge, 1e-6f);
    EXPECT_NEAR(baseEdge -
                    baseEdge * (1.0f - haloWidthCssPx / 10.1f),
                label->vectorUniforms.sdfHaloDelta, 1e-6f);
    EXPECT_NEAR(1.4142f * 1.5f / labelSizeCssPx,
                label->vectorUniforms.sdfGamma, 1e-6f);

    frame_.devicePixelRatio = 2.0f;
    ++frame_.frameId;
    RenderCommandList commandsDpr2;
    for (int i = 0; i < 12; ++i) {
        commandsDpr2 = build();
        if (std::any_of(commandsDpr2.begin(), commandsDpr2.end(),
                        [](const auto& c) {
                            return c.kind == RenderCommandKind::VectorLabel;
                        })) {
            break;
        }
        ++frame_.frameId;
    }
    const auto labelDpr2 = std::find_if(
        commandsDpr2.begin(), commandsDpr2.end(), [](const auto& command) {
            return command.kind == RenderCommandKind::VectorLabel;
        });
    ASSERT_NE(commandsDpr2.end(), labelDpr2);
    const float dpr2Edge = baseEdge + 1.5f / 256.0f;
    const float dpr2Border =
        baseEdge * (1.0f - 2.0f * haloWidthCssPx / 10.1f);
    EXPECT_NEAR(dpr2Edge, labelDpr2->vectorUniforms.sdfEdge, 1e-6f);
    EXPECT_NEAR(dpr2Edge - dpr2Border,
                labelDpr2->vectorUniforms.sdfHaloDelta, 1e-6f);
    EXPECT_NEAR(1.4142f * 1.7f / labelSizeCssPx,
                labelDpr2->vectorUniforms.sdfGamma, 1e-6f);
}

TEST_F(FeatureRenderLayerTest,
       OfficialPoiLabelSizeTextAndHaloTransitionsReachFinalSdfCommand) {
    GlyphAtlas* glyphAtlas = renderer_->glyphAtlas();
    ASSERT_NE(nullptr, glyphAtlas);
    glyphAtlas->activateAmapOfficialProviderForTest([](uint32_t) {});
    std::vector<uint8_t> glyphPixels(64u * 32u, 127);
    const std::vector<GlyphAtlas::ProviderGlyph> glyphs = {
        {'A', 22, 22, 1, -2, 24, 0, 0},
        {'B', 22, 22, 1, -2, 24, 32, 0},
    };
    ASSERT_TRUE(glyphAtlas->installAmapOfficialGlyphBatchForTest(
        64, 32, glyphPixels, glyphs));
    build();

    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);
    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties = {{"name", "AB"},
                      {"amap_class", "10002"},
                      {"amap_subkey", "36"}};
    addOfficialMetadata(poi, "10002", "36", "90");
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    ASSERT_EQ(amapClassicLabelIdentity(10002, 36),
              mesh.symbols.front().labelStyleGroup);
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 6, 100, 200},
                  std::move(mesh)));

    const Vec3 target = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    const Vec3 radial = target.normalized();
    frame_.devicePixelRatio = 1.0f;
    const auto atDisplayZoom = [&](double displayZoom) {
        camera_.lookAt(target + radial *
                           (4.0e7 / std::pow(2.0, displayZoom)),
                       target, Vec3(0.0, 0.0, 1.0));
        RenderCommandList commands;
        for (int i = 0; i < 8; ++i) {
            ++frame_.frameId;
            commands = build();
            if (std::any_of(commands.begin(), commands.end(),
                            [](const auto& command) {
                                return command.kind ==
                                       RenderCommandKind::VectorLabel;
                            })) break;
        }
        const auto label = std::find_if(
            commands.begin(), commands.end(), [](const auto& command) {
                return command.kind == RenderCommandKind::VectorLabel;
            });
        EXPECT_NE(commands.end(), label);
        return label == commands.end() ? RenderCommand{} : *label;
    };

    const int identity = amapClassicLabelIdentity(10002, 36);
    const RenderCommand providerZoom4 = atDisplayZoom(3.0);
    EXPECT_FLOAT_EQ(10.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, identity, 3.0, 0.0f));
    EXPECT_EQ((std::array<float, 4>{0x82 / 255.0f, 0x8e / 255.0f,
                                    0x97 / 255.0f, 1.0f}),
              providerZoom4.vectorUniforms.color);
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 0xcc / 255.0f}),
              providerZoom4.vectorUniforms.haloColor);
    EXPECT_NEAR(1.4142f * 1.5f / 10.0f,
                providerZoom4.vectorUniforms.sdfGamma, 1e-6f);

    // Provider zoom 5 changes only field 1, proving the final SDF size/gamma
    // transition without conflating it with a color transition.
    const RenderCommand providerZoom5 = atDisplayZoom(4.0);
    EXPECT_FLOAT_EQ(11.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, identity, 4.0, 0.0f));
    EXPECT_EQ(providerZoom4.vectorUniforms.color,
              providerZoom5.vectorUniforms.color);
    EXPECT_EQ(providerZoom4.vectorUniforms.haloColor,
              providerZoom5.vectorUniforms.haloColor);
    EXPECT_NEAR(1.4142f * 1.5f / 11.0f,
                providerZoom5.vectorUniforms.sdfGamma, 1e-6f);

    // Provider zoom 6 keeps 11px but changes fields 2/3. Both colors must
    // reach the same final command without a host-font or generic fallback.
    const RenderCommand providerZoom6 = atDisplayZoom(5.0);
    EXPECT_FLOAT_EQ(11.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, identity, 5.0, 0.0f));
    EXPECT_EQ((std::array<float, 4>{0x55 / 255.0f, 0x55 / 255.0f,
                                    0x55 / 255.0f, 1.0f}),
              providerZoom6.vectorUniforms.color);
    // Official label field 2 final text-color consumer.
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              providerZoom6.vectorUniforms.haloColor);
    // Official label field 3 final halo-color consumer.
    EXPECT_NEAR(providerZoom5.vectorUniforms.sdfGamma,
                providerZoom6.vectorUniforms.sdfGamma, 1e-6f);
}


TEST_F(FeatureRenderLayerTest,
       OfficialRoadLabelSizeTextAndHaloTransitionsUseProviderGlyphs) {
    GlyphAtlas* glyphAtlas = renderer_->glyphAtlas();
    ASSERT_NE(nullptr, glyphAtlas);
    glyphAtlas->activateAmapOfficialProviderForTest([](uint32_t) {});
    std::vector<uint8_t> glyphPixels(64u * 32u, 127);
    const std::vector<GlyphAtlas::ProviderGlyph> glyphs = {
        {'A', 22, 22, 1, -2, 24, 0, 0},
        {'B', 22, 22, 1, -2, 24, 32, 0},
    };
    ASSERT_TRUE(glyphAtlas->installAmapOfficialGlyphBatchForTest(
        64, 32, glyphPixels, glyphs));
    build();

    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);
    Feature road;
    road.type = GeometryType::LineString;
    road.rings = {{Cartographic(106.52 * kDeg, 29.54 * kDeg),
                   Cartographic(106.55 * kDeg, 29.56 * kDeg),
                   Cartographic(106.59 * kDeg, 29.59 * kDeg)}};
    road.properties = {{"amap_class", "20026"},
                       {"amap_subkey", "1"},
                       {"amap_draworder", "82"},
                       {"amap_minzoom", "10"},
                       {"amap_maxzoom", "30"},
                       {"amap_rank", "1"},
                       {"name", "AB"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {road});
    ASSERT_EQ(1u, mesh.symbols.size());
    const int identity = amapClassicStyleIdentity(20026, 1);
    ASSERT_EQ(identity, mesh.symbols.front().labelStyleGroup);
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 18, 13038, 5501},
                  std::move(mesh)));

    const Vec3 target = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(106.55 * kDeg, 29.56 * kDeg));
    const Vec3 radial = target.normalized();
    frame_.devicePixelRatio = 1.0f;
    const auto atDisplayZoom = [&](double displayZoom) {
        camera_.lookAt(target + radial *
                           (4.0e7 / std::pow(2.0, displayZoom)),
                       target, Vec3(0.0, 0.0, 1.0));
        RenderCommandList commands;
        for (int i = 0; i < 12; ++i) {
            ++frame_.frameId;
            commands = build();
            if (std::any_of(commands.begin(), commands.end(),
                            [](const auto& command) {
                                return command.kind ==
                                       RenderCommandKind::VectorLabel;
                            })) break;
        }
        const auto label = std::find_if(
            commands.begin(), commands.end(), [](const auto& command) {
                return command.kind == RenderCommandKind::VectorLabel;
            });
        EXPECT_NE(commands.end(), label);
        return label == commands.end() ? RenderCommand{} : *label;
    };

    const RenderCommand providerZoom17 = atDisplayZoom(16.0);
    EXPECT_FLOAT_EQ(12.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, identity, 16.0, 0.0f));
    EXPECT_EQ((std::array<float, 4>{0x83 / 255.0f, 0x8a / 255.0f,
                                    0x9c / 255.0f, 1.0f}),
              providerZoom17.vectorUniforms.color);
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 0xd8 / 255.0f}),
              providerZoom17.vectorUniforms.haloColor);

    // Official road label fields 8/9 change together at provider zoom 18
    // while field 7 remains 12px, so the final uniform transition is isolated.
    const RenderCommand providerZoom18 = atDisplayZoom(17.0);
    EXPECT_FLOAT_EQ(12.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, identity, 17.0, 0.0f));
    // Official road label field 8 final text-color consumer.
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              providerZoom18.vectorUniforms.color);
    // Official road label field 9 final halo-color consumer.
    EXPECT_EQ((std::array<float, 4>{0x5d / 255.0f, 0x60 / 255.0f,
                                    0x65 / 255.0f, 1.0f}),
              providerZoom18.vectorUniforms.haloColor);

    // Official road label field 7 final size consumer: provider zoom 19
    // changes only the size to 13px and therefore changes final SDF gamma.
    const RenderCommand providerZoom19 = atDisplayZoom(18.0);
    EXPECT_FLOAT_EQ(13.0f, FeatureRenderLayer::resolvedLabelSizePx(
                               style, identity, 18.0, 0.0f));
    EXPECT_EQ(providerZoom18.vectorUniforms.color,
              providerZoom19.vectorUniforms.color);
    EXPECT_EQ(providerZoom18.vectorUniforms.haloColor,
              providerZoom19.vectorUniforms.haloColor);
    EXPECT_NEAR(1.4142f * 1.5f / 13.0f,
                providerZoom19.vectorUniforms.sdfGamma, 1e-6f);
}

TEST_F(FeatureRenderLayerTest,
       OfficialDynamicTextBackgroundUsesMeasuredSharedPlacementGeometry) {
    const AmapClassicPoiIconStyle dynamic =
        resolveAmapClassicPoiDynamicBackgroundStyle(12024, 1230, 20.0);
    ASSERT_TRUE(dynamic.enabled);
    EXPECT_EQ(4, dynamic.atlas);          // Official label field 11.
    EXPECT_EQ(73, dynamic.iconIndex);     // Official label field 12.
    EXPECT_EQ(64, dynamic.cellWidth);     // Official label field 13.
    EXPECT_EQ(64, dynamic.cellHeight);    // Official label field 14.
    EXPECT_EQ(512, dynamic.atlasWidth);   // Official label field 15.

    MockPlatformBridge bridge;
    DecodedImage atlasImage;
    atlasImage.width = 512;
    atlasImage.height = 1024;
    atlasImage.channels = 4;
    atlasImage.pixels.resize(512u * 1024u * 4u);
    for (int y = 0; y < atlasImage.height; ++y) {
        for (int x = 0; x < atlasImage.width; ++x) {
            const size_t offset =
                (static_cast<size_t>(y) * atlasImage.width + x) * 4u;
            atlasImage.pixels[offset] = static_cast<uint8_t>(x & 0xff);
            atlasImage.pixels[offset + 1] = static_cast<uint8_t>(y & 0xff);
            atlasImage.pixels[offset + 2] =
                static_cast<uint8_t>((x / 64) | ((y / 64) << 4));
            atlasImage.pixels[offset + 3] = 255;
        }
    }
    bridge.setDecodedImage(std::move(atlasImage));
    Engine engine(&device_);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    auto pool = std::make_shared<ThreadPool>(1);
    const AmapClassicRuntime* runtime = engine.installAmapClassicRuntime(
        bridge, pool, pool, pool, {});
    ASSERT_NE(nullptr, runtime);
    Renderer* officialRenderer = const_cast<Renderer*>(engine.renderer());
    ASSERT_NE(nullptr, officialRenderer);

    FeatureRenderLayer officialLayer(
        "dynamic-background-official", &device_, Ellipsoid::WGS84());
    FrameState officialFrame = frame_;
    Camera officialCamera;
    officialFrame.camera = &officialCamera;
    auto buildOfficial = [&]() {
        RenderCommandList commands;
        officialLayer.buildRenderCommands(
            officialFrame, *officialRenderer, commands);
        return commands;
    };

    GlyphAtlas* glyphAtlas = officialRenderer->glyphAtlas();
    ASSERT_NE(nullptr, glyphAtlas);
    glyphAtlas->activateAmapOfficialProviderForTest([](uint32_t) {});
    std::vector<uint8_t> glyphPixels(64u * 32u, 127);
    const std::vector<GlyphAtlas::ProviderGlyph> glyphs = {
        {'A', 22, 22, 1, -2, 24, 0, 0},
        {'B', 22, 22, 1, -2, 24, 32, 0},
    };
    ASSERT_TRUE(glyphAtlas->installAmapOfficialGlyphBatchForTest(
        64, 32, glyphPixels, glyphs));
    buildOfficial();

    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    officialLayer.setStyleForContractTest(style);

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties = {{"name", "AB"},
                      {"amap_class", "12024"},
                      {"amap_subkey", "1230"}};
    // Official Language.Mii=[1,2] makes u6t=[0,1,2]: two measured lines
    // without changing the visible provider name or cross-tile identity.
    poi.labelSplitIndicesUtf16 = {1, 2};
    addOfficialMetadata(poi, "12024", "1230");
    poi.properties["amap_maxzoom"] = "30";
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        officialLayer.workerTessellationContext(), {poi});
    officialLayer.commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
        std::move(mesh));
    const Vec3 target = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    officialCamera.lookAt(target + target.normalized() *
                             (4.0e7 / std::pow(2.0, 20.0)),
                          target, Vec3(0.0, 0.0, 1.0));
    ++officialFrame.frameId;
    ASSERT_EQ(nullptr,
              officialRenderer->iconAtlas()->frame("amap-icons-4-73"));
    const auto missingFrameCommands = buildOfficial();
    EXPECT_FALSE(std::any_of(
        missingFrameCommands.begin(), missingFrameCommands.end(),
        [&](const auto& command) {
            return command.kind == RenderCommandKind::VectorLabel &&
                   command.shader == officialRenderer->vectorLabelShader();
        })) << "dynamic text must remain atomic while its exact official "
               "background frame is absent";
    EXPECT_FALSE(std::any_of(
        missingFrameCommands.begin(), missingFrameCommands.end(),
        [&](const auto& command) {
            return command.kind == RenderCommandKind::VectorLabel &&
                   command.shader ==
                       officialRenderer->vectorLabelBackgroundShader();
        })) << "missing official frame must not emit a background command";
    ASSERT_TRUE(const_cast<AmapClassicRuntime*>(runtime)
                    ->installAtlasForContractTest(
                        4, {0x89, 0x50, 0x4e, 0x47}));
    const IconAtlas::Frame* officialIconFrame =
        officialRenderer->iconAtlas()->frame("amap-icons-4-73");
    ASSERT_NE(nullptr, officialIconFrame);
    EXPECT_FLOAT_EQ(64.0f, officialIconFrame->widthPx);
    EXPECT_FLOAT_EQ(64.0f, officialIconFrame->heightPx);
    const auto exactIndex73Payload = std::find_if(
        device_.textureRegionPayloads.begin(),
        device_.textureRegionPayloads.end(), [](const auto& pixels) {
            constexpr size_t kCellBytes = 64u * 64u * 4u;
            if (pixels.size() != kCellBytes) return false;
            // atlas4 has eight 64px columns. Official one-based index 73 is
            // zero-based cell 72: x=0, y=9*64=576. Check both corners so a
            // neighbouring cell or an off-by-one source rectangle fails.
            return pixels[0] == 0 && pixels[1] == 64 && pixels[2] == 0x90 &&
                   pixels[3] == 255 &&
                   pixels[kCellBytes - 4] == 63 &&
                   pixels[kCellBytes - 3] == 127 &&
                   pixels[kCellBytes - 2] == 0x90 &&
                   pixels[kCellBytes - 1] == 255;
        });
    ASSERT_NE(device_.textureRegionPayloads.end(), exactIndex73Payload)
        << "alternate index/cell/atlas dimensions must crop source cell "
           "(0,576)-(63,639), not merely register the expected frame name";
    struct Snapshot {
        std::array<float, 4> visible{};
        FeatureRenderLayer::LabelCollisionBoundsForTest collision;
    };
    auto snapshot = [&]() -> Snapshot {
        const auto commands = buildOfficial();
        const auto background = std::find_if(
            commands.begin(), commands.end(), [&](const auto& command) {
                return command.kind == RenderCommandKind::VectorLabel &&
                       command.shader ==
                           officialRenderer->vectorLabelBackgroundShader();
            });
        const auto text = std::find_if(
            commands.begin(), commands.end(), [&](const auto& command) {
                return command.kind == RenderCommandKind::VectorLabel &&
                       command.shader == officialRenderer->vectorLabelShader();
            });
        EXPECT_NE(commands.end(), background);
        EXPECT_NE(commands.end(), text);
        if (background == commands.end() || text == commands.end()) return {};
        EXPECT_EQ(6, background->indexCount);
        EXPECT_EQ(2, background->vectorPaintSubOrder);
        EXPECT_EQ(3, text->vectorPaintSubOrder);
        EXPECT_EQ(background->vertexBuffer, text->vertexBuffer)
            << "background and glyphs must share placement opacity vertices";
        EXPECT_GE(background->textures.size(), 1u);
        if (!background->textures.empty()) {
            EXPECT_EQ(officialRenderer->iconAtlas()->texture(),
                      background->textures[0]);
        }
        const auto* vb =
            dynamic_cast<const DummyBuffer*>(background->vertexBuffer);
        EXPECT_NE(nullptr, vb);
        if (!vb) return {};
        const auto* values =
            reinterpret_cast<const float*>(vb->bytes().data());
        constexpr size_t stride = 11;
        EXPECT_FLOAT_EQ(officialIconFrame->u0, values[9]);
        EXPECT_FLOAT_EQ(officialIconFrame->v1, values[10]);
        EXPECT_FLOAT_EQ(officialIconFrame->u1, values[2 * stride + 9]);
        EXPECT_FLOAT_EQ(officialIconFrame->v0, values[2 * stride + 10]);
        EXPECT_FLOAT_EQ(values[6], values[3 * stride + 6]);
        EXPECT_FLOAT_EQ(values[stride + 6], values[2 * stride + 6]);
        EXPECT_FLOAT_EQ(values[7], values[stride + 7]);
        EXPECT_FLOAT_EQ(values[2 * stride + 7], values[3 * stride + 7]);
        const auto collision =
            officialLayer.firstTileLabelCollisionBoundsForTest();
        EXPECT_TRUE(collision.has_value());
        Snapshot out;
        out.visible = {values[6], values[7], values[stride + 6],
                       values[2 * stride + 7]};
        if (collision) out.collision = *collision;
        return out;
    };

    officialFrame.devicePixelRatio = 1.0f;
    const Snapshot dpr1 = snapshot();
    constexpr float officialLabelSize = 9.0f;
    constexpr float officialGlyphAdvance = 25.0f;
    constexpr float metricHeight = 24.0f;
    const float expectedWidth =
        officialGlyphAdvance * officialLabelSize / metricHeight + 4.0f;
    const float expectedHeight = 2.0f * (officialLabelSize + 4.0f) + 4.0f;
    EXPECT_FLOAT_EQ(-10.0f, dpr1.visible[0]);
    EXPECT_FLOAT_EQ(-20.0f, dpr1.visible[1]);
    EXPECT_FLOAT_EQ(-10.0f + expectedWidth, dpr1.visible[2]);
    EXPECT_FLOAT_EQ(-20.0f + expectedHeight, dpr1.visible[3]);
    EXPECT_FLOAT_EQ(expectedHeight,
                    dpr1.visible[3] - dpr1.visible[1]);
    EXPECT_NE(16.0f, expectedWidth)
        << "field-16 alternateScale is transient; final width is measured";
    EXPECT_NE(16.0f, expectedHeight)
        << "field-16 alternateScale must not survive as fixed geometry";
    ASSERT_TRUE(dpr1.collision.hasSecondary);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(dpr1.visible[i], dpr1.collision.secondary[i])
            << "official icon collision uses the exact icon frame";
    }

    officialFrame.devicePixelRatio = 2.0f;
    ++officialFrame.frameId;
    const Snapshot dpr2 = snapshot();
    ASSERT_TRUE(dpr2.collision.hasSecondary);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(dpr1.visible[i] * 2.0f, dpr2.visible[i], 1e-4f);
        EXPECT_NEAR(dpr2.visible[i], dpr2.collision.secondary[i], 1e-4f)
            << "DPR scales icon geometry once without collision inflation";
    }
}

TEST_F(FeatureRenderLayerTest,
       OfficialUtf16SplitIndicesRejectSurrogateSplitAndAcceptAstralEnd) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    FeatureRenderStyle style = layer_->style();
    installTestOfficialLabelStyle(style);
    layer_->setStyleForContractTest(style);

    struct SplitSnapshot {
        bool committed = false;
        int indexCount = 0;
        float firstGlyphTop = 0.0f;
        float lastGlyphTop = 0.0f;
    };
    auto commitsLabel = [&](const std::string& name,
                            std::vector<uint32_t> splitIndices, int x) {
        Feature poi;
        poi.type = GeometryType::Point;
        poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
        poi.properties = {{"name", name},
                          {"rank", "1"}, {"minzoom", "0"},
                          {"maxzoom", "30"}};
        poi.labelSplitIndicesUtf16 = std::move(splitIndices);
        auto mesh = FeatureRenderLayer::tessellateTileMesh(
            layer_->workerTessellationContext(), {poi});
        layer_->commitTileMesh(
            TileKey{SchemeId("XYZ-WebMercator"), 10, x, 200},
            std::move(mesh));
        const auto commands = build();
        const auto label = std::find_if(commands.begin(), commands.end(),
                                        [](const auto& command) {
            return command.kind == RenderCommandKind::VectorLabel;
        });
        if (label == commands.end()) return SplitSnapshot{};
        SplitSnapshot out;
        out.committed = true;
        out.indexCount = label->indexCount;
        const auto* vb = dynamic_cast<const DummyBuffer*>(label->vertexBuffer);
        if (vb && label->indexCount >= 12) {
            constexpr size_t stride = 11;
            const auto* values =
                reinterpret_cast<const float*>(vb->bytes().data());
            const size_t glyphCount = static_cast<size_t>(label->indexCount / 6);
            out.firstGlyphTop = values[7];
            out.lastGlyphTop = values[(glyphCount - 1) * 4 * stride + 7];
        }
        return out;
    };

    const std::string astral = "A\xF0\x9F\x98\x80" "B";
    EXPECT_FALSE(commitsLabel(astral, {2, 4}, 100).committed)
        << "a UTF-16 boundary inside an astral surrogate pair fails closed";
    layer_ = std::make_unique<FeatureRenderLayer>(
        "test-features-2", &device_, Ellipsoid::WGS84());
    layer_->setStyleForContractTest(style);
    EXPECT_TRUE(commitsLabel(astral, {3, 4}, 101).committed);
    layer_ = std::make_unique<FeatureRenderLayer>(
        "test-features-3", &device_, Ellipsoid::WGS84());
    layer_->setStyleForContractTest(style);
    const SplitSnapshot open = commitsLabel(astral, {3}, 102);
    ASSERT_TRUE(open.committed)
        << "official Mii is an open split list; the remaining suffix is a "
           "final line";
    EXPECT_GE(open.indexCount, 12)
        << "the guaranteed A/B glyphs on both sides of the astral scalar draw";
    EXPECT_GT(open.firstGlyphTop, open.lastGlyphTop)
        << "the B suffix must occupy the second line, not remain on line one";
    layer_ = std::make_unique<FeatureRenderLayer>(
        "test-features-4", &device_, Ellipsoid::WGS84());
    layer_->setStyleForContractTest(style);
    EXPECT_FALSE(commitsLabel(astral, {5}, 103).committed)
        << "a split beyond the UTF-16 string remains fail-closed";

    layer_ = std::make_unique<FeatureRenderLayer>(
        "test-features-5", &device_, Ellipsoid::WGS84());
    layer_->setStyleForContractTest(style);
    const SplitSnapshot splitTrimmed =
        commitsLabel("A \xE3\x80\x80", {1}, 104);
    ASSERT_TRUE(splitTrimmed.committed);
    EXPECT_EQ(6, splitTrimmed.indexCount)
        << "non-empty Mii applies only the final JavaScript /\\s+$/ trim";
}

TEST_F(FeatureRenderLayerTest, TileSymbolPaintOrderSplitsPointCommands) {
    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    TileSymbolCpu high;
    high.paintOrder = 100;
    high.lonRad = 6.01 * kDeg;
    high.latRad = 29.0 * kDeg;
    genericVisual(high).colorPacked = 1.0f;
    TileSymbolCpu low;
    low.paintOrder = 20;
    low.lonRad = 6.0 * kDeg;
    low.latRad = 29.0 * kDeg;
    genericVisual(low).colorPacked = 1.0f;
    mesh.symbols = {high, low};

    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));
    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    sortMvpRenderCommands(commands);
    EXPECT_EQ(20, commands[0].vectorPaintOrder);
    EXPECT_EQ(100, commands[1].vectorPaintOrder);
    EXPECT_EQ(0, commands[0].indexOffset);
    EXPECT_EQ(6, commands[1].indexOffset);
    EXPECT_EQ(commands[0].vertexBuffer, commands[1].vertexBuffer);
    EXPECT_EQ(commands[0].indexBuffer, commands[1].indexBuffer);
}

TEST_F(FeatureRenderLayerTest, TileSymbolCarriesAmapZoomWindow) {
    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties["name"] = "POI";
    poi.properties["rank"] = "-42";
    poi.properties["amap_minzoom"] = "15";
    poi.properties["amap_maxzoom"] = "21";

    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(-42, mesh.symbols[0].rank);
    EXPECT_EQ(14, mesh.symbols[0].minZoom);
    EXPECT_EQ(21, mesh.symbols[0].maxZoom);
}

TEST_F(FeatureRenderLayerTest, SingleZoomAmapWindowRemainsOneDisplayZoom) {
    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties["name"] = "PROVINCE";
    poi.properties["amap_minzoom"] = "10";
    poi.properties["amap_maxzoom"] = "10";

    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(9, mesh.symbols[0].minZoom);
    EXPECT_EQ(10, mesh.symbols[0].maxZoom);
}

TEST_F(FeatureRenderLayerTest, TileSymbolCarriesDataDrivenLabelSize) {
    FeatureRenderStyle style = layer_->style();
    style.labelSizePx = 23.0f;
    style.labelSizeExpr = StyleExpression::match(
        "amap_class",
        {{"10002", StyleExpression::literal(32.0)}},
        StyleExpression::literal(23.0));
    layer_->setStyleForContractTest(style);

    Feature city;
    city.type = GeometryType::Point;
    city.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    city.properties["name"] = "City";
    city.properties["amap_class"] = "10002";
    Feature poi = city;
    poi.properties["amap_class"] = "12024";

    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {city, poi});
    ASSERT_EQ(2u, mesh.symbols.size());
    ASSERT_TRUE(mesh.symbols[0].genericVisual);
    ASSERT_TRUE(mesh.symbols[1].genericVisual);
    EXPECT_FLOAT_EQ(32.0f, mesh.symbols[0].genericVisual->labelSizePx);
    EXPECT_FLOAT_EQ(23.0f, mesh.symbols[1].genericVisual->labelSizePx);
}

TEST_F(FeatureRenderLayerTest, TileSymbolCarriesDataDrivenLabelOffset) {
    FeatureRenderStyle style = layer_->style();
    style.labelOffsetPx = 13.0f;
    style.labelOffsetExpr = StyleExpression::match(
        "amap_class",
        {{"10002", StyleExpression::literal(0.0)}},
        StyleExpression::literal(13.0));
    layer_->setStyleForContractTest(style);

    Feature city;
    city.type = GeometryType::Point;
    city.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    city.properties["name"] = "City";
    city.properties["amap_class"] = "10002";
    Feature poi = city;
    poi.properties["amap_class"] = "12024";

    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {city, poi});
    ASSERT_EQ(2u, mesh.symbols.size());
    ASSERT_TRUE(mesh.symbols[0].genericVisual);
    ASSERT_TRUE(mesh.symbols[1].genericVisual);
    EXPECT_FLOAT_EQ(0.0f, mesh.symbols[0].genericVisual->labelOffsetPx);
    EXPECT_FLOAT_EQ(13.0f, mesh.symbols[1].genericVisual->labelOffsetPx);
}

TEST_F(FeatureRenderLayerTest, TransparentPoiPointBecomesLabelOnlySource) {
    FeatureRenderStyle style = layer_->style();
    style.pointColorExpr = StyleExpression::literal(
        std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    style.labelOffsetExpr = StyleExpression::literal(0.0);
    layer_->setStyleForContractTest(style);

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties["name"] = "Official text only";
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    ASSERT_TRUE(mesh.symbols[0].genericVisual);
    EXPECT_FALSE(mesh.symbols[0].genericVisual->iconEnabled);
    EXPECT_FLOAT_EQ(0.0f, mesh.symbols[0].genericVisual->labelOffsetPx);
}

TEST_F(FeatureRenderLayerTest, MissingOrMalformedZoomWindowStaysVisible) {
    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};

    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(0, mesh.symbols[0].minZoom);
    EXPECT_EQ(30, mesh.symbols[0].maxZoom);

    poi.properties["amap_minzoom"] = "bogus";
    poi.properties["amap_maxzoom"] = "-2";
    mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(0, mesh.symbols[0].minZoom);
    EXPECT_EQ(30, mesh.symbols[0].maxZoom);
}

TEST_F(FeatureRenderLayerTest,
       StrictOfficialZoomWindowRejectsMissingPartialAndMalformed) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    installTestOfficialLabelStyle(style);
    layer_->setStyleForContractTest(style);

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg)}};
    poi.properties["amap_draworder"] = "1";
    poi.properties["amap_rank"] = "1";
    poi.properties["amap_class"] = "10002";
    poi.properties["amap_subkey"] = "37";
    EXPECT_TRUE(FeatureRenderLayer::tessellateTileMesh(
                    layer_->workerTessellationContext(), {poi})
                    .symbols.empty());

    poi.properties["amap_minzoom"] = "15";
    EXPECT_TRUE(FeatureRenderLayer::tessellateTileMesh(
                    layer_->workerTessellationContext(), {poi})
                    .symbols.empty());
    poi.properties["amap_maxzoom"] = "bogus";
    EXPECT_TRUE(FeatureRenderLayer::tessellateTileMesh(
                    layer_->workerTessellationContext(), {poi})
                    .symbols.empty());

    poi.properties["amap_maxzoom"] = "21";
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    EXPECT_EQ(14, mesh.symbols.front().minZoom);
    EXPECT_EQ(21, mesh.symbols.front().maxZoom);
}

TEST_F(FeatureRenderLayerTest,
       StrictOfficialZoomWindowRejectsFillAndLineBeforeGeometry) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Regions);
    style.fillStyleGroupExpr = StyleExpression::literal(1.0);
    layer_->setStyleForContractTest(style);

    Feature polygon;
    polygon.type = GeometryType::Polygon;
    polygon.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg),
                      Cartographic(6.01 * kDeg, 29.0 * kDeg),
                      Cartographic(6.01 * kDeg, 29.01 * kDeg)}};
    Feature line;
    line.type = GeometryType::LineString;
    line.rings = {{Cartographic(6.0 * kDeg, 29.0 * kDeg),
                   Cartographic(6.01 * kDeg, 29.01 * kDeg)}};
    polygon.properties["amap_draworder"] = "1";
    line.properties["amap_draworder"] = "1";
    polygon.properties["amap_class"] = "30001";
    polygon.properties["amap_subkey"] = "1";
    line.properties["amap_class"] = "20001";
    line.properties["amap_subkey"] = "1";

    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {polygon, line});
    EXPECT_TRUE(mesh.fillIndices.empty());
    EXPECT_TRUE(mesh.lineIndices.empty());

    polygon.properties["amap_minzoom"] = "15";
    polygon.properties["amap_maxzoom"] = "21";
    line.properties["amap_minzoom"] = "15";
    line.properties["amap_maxzoom"] = "bogus";
    mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {polygon, line});
    EXPECT_FALSE(mesh.fillIndices.empty());
    EXPECT_TRUE(mesh.lineIndices.empty());

    line.properties["amap_maxzoom"] = "21";
    mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {polygon, line});
    EXPECT_FALSE(mesh.fillIndices.empty());
    EXPECT_FALSE(mesh.lineIndices.empty());
}

TEST_F(FeatureRenderLayerTest, TileSymbolZoomWindowGatesPointAndLabel) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();  // 缓存图集指针

    constexpr double lon = 6.0 * kDeg;
    constexpr double lat = 29.0 * kDeg;
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(lon, lat));
    const Vec3 radial = surface.normalized();
    auto setHeight = [&](double heightMeters) {
        camera_.lookAt(surface + radial * heightMeters, surface,
                       Vec3(0.0, 0.0, 1.0));
    };

    FeatureTileMesh mesh;
    mesh.origin = surface;
    mesh.hasOrigin = true;
    TileSymbolCpu far;
    far.lonRad = lon;
    far.latRad = lat;
    genericVisual(far).colorPacked = 1.0f;
    far.name = "FAR";
    far.minZoom = 0;
    far.maxZoom = 14;
    TileSymbolCpu near = far;
    near.lonRad += 0.001 * kDeg;
    near.name = "NEAR";
    near.minZoom = 15;
    near.maxZoom = 30;

    const uint64_t farId = layer_->crossTileIdFor(
        far.name, far.lonRad, far.latRad, 14);
    const uint64_t nearId = layer_->crossTileIdFor(
        near.name, near.lonRad, near.latRad, 14);
    mesh.symbols = {far, near};
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
                           std::move(mesh));

    // 3000m:zoom≈13.7 → 整数档 13，只显示远景符号/标签。
    setHeight(3000.0);
    frame_.deltaSeconds = 0.35;
    RenderCommandList commands = build();
    int pointCommands = 0;
    int labelCommands = 0;
    int labelIndexCount = 0;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorPoint) ++pointCommands;
        if (cmd.kind == RenderCommandKind::VectorLabel) {
            ++labelCommands;
            labelIndexCount += cmd.indexCount;
        }
    }
    EXPECT_EQ(1, pointCommands);
    EXPECT_EQ(1, labelCommands);
    EXPECT_EQ(18, labelIndexCount);  // "FAR" only, 3 glyph quads
    EXPECT_FLOAT_EQ(1.0f, layer_->labelOpacityForFeature(farId));
    EXPECT_FLOAT_EQ(0.0f, layer_->labelOpacityForFeature(nearId));

    // 1000m:zoom≈15.3 → 整数档 15。跨档必须绕过 300ms placement 节流，
    // 同一帧切换到近景符号/标签，远景项立即退出候选集。
    setHeight(1000.0);
    frame_.deltaSeconds = 0.35;
    commands = build();
    pointCommands = 0;
    labelCommands = 0;
    labelIndexCount = 0;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorPoint) ++pointCommands;
        if (cmd.kind == RenderCommandKind::VectorLabel) {
            ++labelCommands;
            labelIndexCount += cmd.indexCount;
        }
    }
    EXPECT_EQ(1, pointCommands);
    EXPECT_EQ(1, labelCommands);
    EXPECT_EQ(24, labelIndexCount);  // "NEAR" only, 4 glyph quads
    EXPECT_FLOAT_EQ(0.0f, layer_->labelOpacityForFeature(farId));
    EXPECT_FLOAT_EQ(1.0f, layer_->labelOpacityForFeature(nearId));
    EXPECT_EQ(1, layer_->labelPlacementStats().candidates);
}

TEST_F(FeatureRenderLayerTest, TileLabelsBakeOnlyCurrentZoomWindowAndRebakeOnChange) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();  // 缓存 glyph atlas

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    mesh.hasOrigin = true;
    TileSymbolCpu coarse;
    coarse.lonRad = 0.0;
    coarse.latRad = 0.0;
    genericVisual(coarse).colorPacked = 1.0f;
    coarse.name = "A";
    coarse.minZoom = 0;
    coarse.maxZoom = 3;
    TileSymbolCpu fine = coarse;
    fine.name = "B";
    fine.minZoom = 3;
    fine.maxZoom = 30;
    mesh.symbols = {coarse, fine};
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
        std::move(mesh));

    // fixture 高空 viewZoom≈2：只允许 coarse 窗口参与字形/quad 烘焙。
    build();
    EXPECT_TRUE(renderer_->glyphAtlas()->hasGlyph('A'));
    EXPECT_FALSE(renderer_->glyphAtlas()->hasGlyph('B'))
        << "未来 zoom 不可见标签不得提前占 worker/图集/完整帧";
    EXPECT_EQ(1, layer_->labelPlacementStats().candidates);

    // viewZoom≈4，跨整数窗口后必须从保留源增量重烘 fine 标签。
    const double radius = Ellipsoid::WGS84().radii().x();
    camera_.lookAt(Vec3(radius + 2.5e6, 0.0, 0.0), Vec3::zero(),
                   Vec3(0.0, 0.0, 1.0));
    ++frame_.frameId;
    build();
    EXPECT_TRUE(renderer_->glyphAtlas()->hasGlyph('B'))
        << "跨 zoom 后不能因上一窗口 settled 而永久漏标";
    EXPECT_EQ(1, layer_->labelPlacementStats().candidates);
}

TEST_F(FeatureRenderLayerTest, TileSymbolCapacityPreservesIndependentZoomWindows) {
    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    mesh.hasOrigin = true;
    for (int i = 0; i < 128; ++i) {
        TileSymbolCpu fine;
        fine.lonRad = i * 1e-7;
        fine.latRad = 0.0;
        genericVisual(fine).colorPacked = 1.0f;
        fine.rank = 1;
        fine.minZoom = 18;
        fine.maxZoom = 30;
        mesh.symbols.push_back(fine);
    }
    TileSymbolCpu coarse;
    coarse.lonRad = -1e-5;
    coarse.latRad = 0.0;
    genericVisual(coarse).colorPacked = 1.0f;
    coarse.rank = 9;
    coarse.minZoom = 0;
    coarse.maxZoom = 18;
    mesh.symbols.push_back(coarse);
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
        std::move(mesh));

    RenderCommandList commands = build();
    int coarseIndices = 0;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorPoint) {
            coarseIndices += cmd.indexCount;
        }
    }
    EXPECT_EQ(6, coarseIndices)
        << "fine 档 top-N 不能在 commit 时永久挤掉 coarse 档唯一符号";

    const double radius = Ellipsoid::WGS84().radii().x();
    const double height = 4.0e7 / std::exp2(18.25);
    camera_.lookAt(Vec3(radius + height, 0.0, 0.0), Vec3::zero(),
                   Vec3(0.0, 0.0, 1.0));
    ++frame_.frameId;
    commands = build();
    int fineIndices = 0;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorPoint) {
            fineIndices += cmd.indexCount;
        }
    }
    EXPECT_EQ(128 * 6, fineIndices);
}

TEST_F(FeatureRenderLayerTest, TileLabelBakeGpuFailureRetriesWithoutZoomChange) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    mesh.hasOrigin = true;
    TileSymbolCpu symbol;
    symbol.lonRad = 0.0;
    symbol.latRad = 0.0;
    genericVisual(symbol).colorPacked = 1.0f;
    symbol.name = "AB";
    mesh.symbols.push_back(symbol);
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
        std::move(mesh));

    // 精确命中下一次 Vertex 创建（此时 point buffers 已在 commit 中稳定，
    // build 的下一次 Vertex 即 label VBO）。不要依赖全局 buffer 创建序号；
    // 该序号会随符号重建策略变化，无法表达本测试真正要验证的故障边界。
    device_.failNextBufferCreationOfType = BufferDesc::Type::Vertex;
    RenderCommandList commands = build();
    EXPECT_TRUE(layer_->hasPendingLabelWork());
    EXPECT_FALSE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorLabel;
    }));

    device_.failNextBufferCreationOfType.reset();
    ++frame_.frameId;
    commands = build();
    EXPECT_TRUE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorLabel;
    })) << "稳定 camera 下的瞬时 label GPU 失败必须跨帧自愈";
}

TEST_F(FeatureRenderLayerTest, PendingLabelTicketReleasesOutsideLayerZoomRange) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    FeatureRenderStyle style = layer_->style();
    style.maxZoom = 3.0;
    layer_->setStyleForContractTest(style);

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    mesh.hasOrigin = true;
    TileSymbolCpu symbol;
    symbol.lonRad = 0.0;
    symbol.latRad = 0.0;
    genericVisual(symbol).colorPacked = 1.0f;
    symbol.name = "AB";
    mesh.symbols.push_back(symbol);
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
        std::move(mesh));

    const int before =
        WorkLedger::shared().outstandingForLabel("labelConverge");
    build();
    EXPECT_GT(WorkLedger::shared().outstandingForLabel("labelConverge"),
              before);

    const double radius = Ellipsoid::WGS84().radii().x();
    camera_.lookAt(Vec3(radius + 2.0e6, 0.0, 0.0), Vec3::zero(),
                   Vec3(0.0, 0.0, 1.0));
    ++frame_.frameId;
    build();
    EXPECT_EQ(before,
              WorkLedger::shared().outstandingForLabel("labelConverge"));
    EXPECT_FALSE(layer_->hasPendingLabelWork());
}

TEST_F(FeatureRenderLayerTest, BusyLayerHiddenReleasesLabelTicketImmediately) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(0.0, 0.0)}};
    point.properties["name"] = "AB";
    layer_->store().addFeature(std::move(point));

    const int before =
        WorkLedger::shared().outstandingForLabel("labelConverge");
    build();
    EXPECT_GT(WorkLedger::shared().outstandingForLabel("labelConverge"),
              before);
    layer_->setVisible(false);
    EXPECT_EQ(before,
              WorkLedger::shared().outstandingForLabel("labelConverge"));
}

TEST_F(FeatureRenderLayerTest, SameActiveZoomWindowDoesNotRebuildTileLabels) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    mesh.hasOrigin = true;
    TileSymbolCpu symbol;
    symbol.lonRad = 0.0;
    symbol.latRad = 0.0;
    genericVisual(symbol).colorPacked = 1.0f;
    symbol.name = "AB";
    symbol.minZoom = 18;
    symbol.maxZoom = 30;
    mesh.symbols.push_back(symbol);
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
        std::move(mesh));

    const double radius = Ellipsoid::WGS84().radii().x();
    auto setZoom = [&](double zoom) {
        const double height = 4.0e7 / std::exp2(zoom);
        camera_.lookAt(Vec3(radius + height, 0.0, 0.0), Vec3::zero(),
                       Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
    };
    setZoom(18.25);
    build();
    const int buffersAt18 = device_.createdBufferCount;
    setZoom(19.25);
    build();
    EXPECT_EQ(buffersAt18, device_.createdBufferCount)
        << "[18,30) active 集未变时跨整数 zoom 不应销毁重建 point/label VBO";
}

// rank 升序截断:超过单瓦上限时留 rank 最小(最重要)的那批。这是
// placement 预算刀之前的容量闸 —— 上限本身是实现细节,契约是「重要的
// 活下来 + 总量被钉住」。
TEST_F(FeatureRenderLayerTest, TileSymbolsCappedByRankAscending) {
    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    constexpr int kOverfill = 400;  // > 单瓦上限
    for (int i = 0; i < kOverfill; ++i) {
        TileSymbolCpu s;
        s.lonRad = (6.0 + i * 1e-5) * kDeg;
        s.latRad = 29.0 * kDeg;
        genericVisual(s).colorPacked = 1.0f;
        // 前 8 个 rank=1(必须活),其余 rank=9(截断候选)。
        s.rank = i < 8 ? 1 : 9;
        mesh.symbols.push_back(s);
    }
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const int quadCount = commands[0].indexCount / 6;
    EXPECT_LT(quadCount, kOverfill) << "单瓦符号数没有上限,容量闸失效";
    EXPECT_GE(quadCount, 8) << "截断把高重要度符号也丢了";
}

TEST_F(FeatureRenderLayerTest, TileSymbolsUseSmallerBudgetAtBroadZoom) {
    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    mesh.hasOrigin = true;
    for (int i = 0; i < 80; ++i) {
        TileSymbolCpu s;
        s.lonRad = i * 1e-5;
        s.latRad = 0.0;
        s.rank = i;
        s.minZoom = 0;
        s.maxZoom = 30;
        genericVisual(s).colorPacked = 1.0f;
        mesh.symbols.push_back(s);
    }
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
        std::move(mesh));
    // Fixture camera is a broad view (zoom bucket <= 11): 16 candidates,
    // while near-view tests retain the existing 128-symbol ceiling.
    RenderCommandList commands = build();
    int quads = 0;
    for (const auto& command : commands) {
        if (command.kind == RenderCommandKind::VectorPoint) {
            quads += command.indexCount / 6;
        }
    }
    EXPECT_LE(quads, 16);
    EXPECT_GT(quads, 0);
}

TEST_F(FeatureRenderLayerTest,
       OfficialProviderSymbolsDoNotUseGenericPerTileBudget) {
    build();
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "official-budget-icon", 4, 4,
        std::vector<uint8_t>(4u * 4u * 4u, 255)));
    FeatureRenderStyle style = layer_->style();
    style.pointStylePropertyA = "amap_class";
    style.pointStylePropertyB = "amap_subkey";
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    style.pointStyleResolver = [](const std::string&, const std::string&,
                                  const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle result;
        result.enabled = true;
        result.image = "official-budget-icon";
        result.labelLayout.emplace();
        result.labelLayout->iconWidthPx = 4.0f;
        result.labelLayout->iconHeightPx = 4.0f;
        result.labelLayout->iconAnchorXPx = 2.0f;
        result.labelLayout->iconAnchorYPx = 2.0f;
        return result;
    };
    layer_->setStyleForContractTest(style);

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    mesh.hasOrigin = true;
    constexpr int kOfficialCount = 160;
    for (int i = 0; i < kOfficialCount; ++i) {
        TileSymbolCpu symbol;
        symbol.lonRad = i * 1e-6;
        symbol.latRad = 0.0;
        symbol.rank = i;
        symbol.minZoom = 0;
        symbol.maxZoom = 30;
        symbol.pointStyleKeyA = "12024";
        symbol.pointStyleKeyB = std::to_string(i + 1);
        mesh.symbols.push_back(std::move(symbol));
    }
    layer_->commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
        std::move(mesh));

    const RenderCommandList commands = build();
    int quads = 0;
    for (const auto& command : commands) {
        if (command.kind == RenderCommandKind::VectorPoint) {
            quads += command.indexCount / 6;
        }
    }
    EXPECT_EQ(kOfficialCount, quads)
        << "generic engine budget must not discard official provider records";
}

TEST_F(FeatureRenderLayerTest,
       OfficialInsertionOrderIsGlobalAcrossTilesAndSurvivesRebuild) {
    build();
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "official-order-icon", 4, 4,
        std::vector<uint8_t>(4u * 4u * 4u, 255)));
    FeatureRenderStyle style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    installTestOfficialLabelStyle(style);
    style.pointIdentityValidator = [](const std::string&,
                                      const std::string&) { return true; };
    style.pointStyleResolver = [](const std::string&, const std::string&,
                                  const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle result;
        result.enabled = true;
        result.image = "official-order-icon";
        result.labelLayout.emplace();
        result.labelLayout->iconWidthPx = 4.0f;
        result.labelLayout->iconHeightPx = 4.0f;
        result.labelLayout->iconAnchorXPx = 2.0f;
        result.labelLayout->iconAnchorYPx = 2.0f;
        return result;
    };
    layer_->setStyleForContractTest(style);

    const TileKey firstKey{SchemeId("XYZ-WebMercator"), 10, 100, 200};
    const TileKey secondKey{SchemeId("XYZ-WebMercator"), 10, 101, 200};
    const auto makeMesh = [](const char* name, double lon) {
        FeatureTileMesh mesh;
        mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic(lon, 0.0));
        mesh.hasOrigin = true;
        TileSymbolCpu symbol;
        symbol.lonRad = lon;
        symbol.latRad = 0.0;
        symbol.name = name;
        symbol.rank = -1;
        symbol.minZoom = 0;
        symbol.maxZoom = 30;
        symbol.labelStyleGroup = 1;
        symbol.pointStyleKeyA = "12024";
        symbol.pointStyleKeyB = "1";
        mesh.symbols.push_back(std::move(symbol));
        return mesh;
    };

    layer_->commitTileMesh(firstKey, makeMesh("first", 0.0));
    layer_->commitTileMesh(secondKey, makeMesh("second", 1e-5));
    const auto first =
        layer_->officialTileLabelInsertionOrderForTest(firstKey, "first");
    const auto second =
        layer_->officialTileLabelInsertionOrderForTest(secondKey, "second");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_GT(*second, *first)
        << "cross-tile order must follow admission, not unordered_map order";

    build();  // zoom/terrain-safe symbol rebuild must retain the same stamp.
    EXPECT_EQ(first, layer_->officialTileLabelInsertionOrderForTest(
                         firstKey, "first"));
    EXPECT_EQ(second, layer_->officialTileLabelInsertionOrderForTest(
                          secondKey, "second"));

    // A provider tile replacement constructs a new official label object and
    // therefore receives a new worker-stamp-equivalent id. This is different
    // from terrain/zoom/glyph rebuilds, which must preserve the existing id.
    layer_->commitTileMesh(firstKey, makeMesh("first", 0.0));
    const auto recommitted =
        layer_->officialTileLabelInsertionOrderForTest(firstKey, "first");
    ASSERT_TRUE(recommitted.has_value());
    EXPECT_GT(*recommitted, *second);
}

// 符号刀C:跨瓦稳定 ID。同一 POI 在不同 zoom 瓦片里的 MVT 量化坐标略异,
// 容差(两代中较粗 zoom 的量化格)内必须继承同一 id —— placement 的
// fade/避让账本按 id 记,id 断了 = 换代闪烁重淡入。
TEST_F(FeatureRenderLayerTest, CrossTileIdInheritedAcrossZoomQuantization) {
    const double lon = 106.55 * kDeg;
    const double lat = 29.56 * kDeg;
    const uint64_t idAtZ12 = layer_->crossTileIdFor("解放碑", lon, lat, 12);
    // z14 版坐标偏 z12 量化格的半格(~1.9e-6 rad):同一 POI 的典型代际差。
    const double halfCellZ12 = 0.5 * 6.283185307179586 / (4096.0 * 4096.0);
    const uint64_t idAtZ14 =
        layer_->crossTileIdFor("解放碑", lon + halfCellZ12, lat, 14);
    EXPECT_EQ(idAtZ12, idAtZ14) << "代际量化差内未继承 id";

    // 同名但相距远(>> 容差):不同 POI,不得误并。
    const uint64_t idFar =
        layer_->crossTileIdFor("解放碑", lon + 0.01, lat, 14);
    EXPECT_NE(idAtZ12, idFar);

    // 同坐标不同名:不同 id。
    const uint64_t idOther = layer_->crossTileIdFor("洪崖洞", lon, lat, 14);
    EXPECT_NE(idAtZ12, idOther);

    // 细 zoom 匹配后锚点参考升级 → 再来一个 z14 精确坐标仍命中。
    const uint64_t idAgain =
        layer_->crossTileIdFor("解放碑", lon + halfCellZ12, lat, 14);
    EXPECT_EQ(idAtZ14, idAgain);
}

// V29 刀2:同 pass 认领集 1:1 —— 同名多实例(路名多段)各持独立 id、换代
// 各继承各的;双认领会让 fade 互踩 + 锚点参考被来回拉扯(设计文档 §4b)。
TEST_F(FeatureRenderLayerTest, CrossTileClaimPreventsDoubleAssignment) {
    const double lon = 106.55 * kDeg;
    const double lat = 29.56 * kDeg;
    // 两实例相距 ~3m(1e-7 rad·倍率)< 刀1 扩窗(z14 ≈ ±9.5m):无认领集
    // 时第二个必误匹配第一个的 entry。
    const double sep = 5e-7;  // ≈3.2m

    // pass1(z13 commit):同名两实例 → 认领集下各自新建,id 不同。
    std::unordered_set<uint64_t> pass1;
    const uint64_t a13 = layer_->crossTileIdFor("民族路", lon, lat, 13, &pass1);
    const uint64_t b13 =
        layer_->crossTileIdFor("民族路", lon + sep, lat, 13, &pass1);
    EXPECT_NE(a13, b13) << "同 pass 同名两实例不得共 id(误并)";

    // pass2(z14 换代 commit,锚点各偏 ~1m):各继承各的,id 集合相等。
    std::unordered_set<uint64_t> pass2;
    const double drift = 1.5e-7;  // ≈1m,两窗都吸得住
    const uint64_t a14 =
        layer_->crossTileIdFor("民族路", lon + drift, lat, 14, &pass2);
    const uint64_t b14 =
        layer_->crossTileIdFor("民族路", lon + sep + drift, lat, 14, &pass2);
    EXPECT_NE(a14, b14) << "换代后仍不得共 id";
    EXPECT_TRUE((a14 == a13 && b14 == b13) || (a14 == b13 && b14 == a13))
        << "换代应继承既有两 id(1:1),不得新建";
}

// V29 刀1:窗扩到 1/256 瓦(maplibre 4px 等效)—— 换代锚点米级漂移
// (线标注弧长中点随瓦片切分挪)必须吸得住;远距同名仍不得误并。
TEST_F(FeatureRenderLayerTest, CrossTileWindowAbsorbsGenerationDrift) {
    const double lon = 106.55 * kDeg;
    const double lat = 29.56 * kDeg;
    const uint64_t id13 = layer_->crossTileIdFor("嘉陵江滨江路", lon, lat, 13);
    // 漂移 ~5m(7.85e-7 rad):旧窗(z13 ≈ ±1.8m)吸不住 = 真机 22% 断链
    // 的机制;新窗(z13 ≈ ±19m)必须吸住。
    const uint64_t id14 =
        layer_->crossTileIdFor("嘉陵江滨江路", lon + 7.85e-7, lat, 14);
    EXPECT_EQ(id13, id14) << "米级换代漂移必须继承(刀1 判据)";
    // 远距(>窗)同名 = 真不同实例,不得误并。z13 窗 ≈ 3e-6 rad,取 1e-4。
    const uint64_t idFar =
        layer_->crossTileIdFor("嘉陵江滨江路", lon + 1e-4, lat, 14);
    EXPECT_NE(id13, idFar);
}

TEST_F(FeatureRenderLayerTest, OutOfHorizonBucketEmitsNoCommands) {
    // 视口桶裁剪:相机(星下点 0°E/0°N,高 ~8.6e6m,地平线角 ~65°)看不到
    // 的桶不出命令。视野内 polygon 出 fill+outline 两条;150°E 的桶被裁。
    FeatureRenderStyle style = layer_->style();
    style.fillOutlineEnabled = true;
    layer_->setStyleForContractTest(style);
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
    FeatureRenderStyle style = layer_->style();
    style.fillOutlineEnabled = true;
    layer_->setStyleForContractTest(style);
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
    ASSERT_TRUE(commands[0].hasVectorUniforms);
    EXPECT_GT(commands[0].vectorUniforms.depthPushNdc, 0.9f);
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
    ASSERT_TRUE(commands[0].hasVectorUniforms);
    EXPECT_FLOAT_EQ(0.0f, commands[0].vectorUniforms.depthPushNdc);
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
    ASSERT_TRUE(commands[0].hasVectorUniforms);
    EXPECT_TRUE(commands[0].uniforms.empty());
    const auto& mvpU = commands[0].vectorUniforms.modelViewProjection;
    ASSERT_EQ(16u, mvpU.size());
}

// ============================================================
// 脏桶增量重镶
// ============================================================

TEST_F(FeatureRenderLayerTest, DirtyBucketRebuildIsIncremental) {
    // 两个远隔要素 → 两个桶(cell 0.02rad,隔 >2° 必不同桶)
    FeatureRenderStyle style = layer_->style();
    style.fillOutlineEnabled = true;
    layer_->setStyleForContractTest(style);
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
    FeatureRenderStyle style = layer_->style();
    style.fillOutlineEnabled = true;
    layer_->setStyleForContractTest(style);
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

TEST(GlyphAtlasTest, AmapProviderUsesOfficialMetricsAndBatchedRevision) {
    MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    GlyphAtlas atlas(&device);
    std::vector<uint32_t> demanded;
    atlas.activateAmapOfficialProviderForTest(
        [&](uint32_t cp) { demanded.push_back(cp); });
    ASSERT_TRUE(atlas.ready());
    ASSERT_NE(nullptr, atlas.texture());
    atlas.beginFrameGlyphBudget(1, 4.0);
    atlas.beginFrameGlyphBudget(2, 4.0);
    EXPECT_EQ(24.0f, atlas.metricPixelHeight());
    EXPECT_EQ(GlyphAtlas::BudgetedGlyphResult::Deferred,
              atlas.ensureGlyphBudgeted(37325));
    EXPECT_EQ(GlyphAtlas::BudgetedGlyphResult::Deferred,
              atlas.ensureGlyphBudgeted(37325));
    ASSERT_EQ(1u, demanded.size());
    EXPECT_EQ(37325u, demanded.front());

    const uint64_t before = atlas.revision();
    std::vector<uint8_t> pixels(64 * 32, 127);
    std::vector<GlyphAtlas::ProviderGlyph> glyphs = {
        {24198, 22, 22, 1, -2, 24, 0, 0},
        {37325, 22, 21, 1, -2, 24, 32, 0},
    };
    ASSERT_TRUE(atlas.installAmapOfficialGlyphBatchForTest(
        64, 32, pixels, glyphs));
    EXPECT_EQ(before + 1, atlas.revision());
    const auto* chong = atlas.ensureGlyph(37325);
    const auto* qing = atlas.ensureGlyph(24198);
    ASSERT_NE(nullptr, chong);
    ASSERT_NE(nullptr, qing);
    EXPECT_FLOAT_EQ(25.0f, chong->advance);
    EXPECT_FLOAT_EQ(25.0f, qing->advance);
    EXPECT_FLOAT_EQ(1.0f, chong->offsetX);
    EXPECT_FLOAT_EQ(2.0f, chong->offsetY);
    EXPECT_FLOAT_EQ(50.0f, chong->advance + qing->advance);
}

TEST(GlyphAtlasTest, RasterizesAndPacksGlyphs) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    std::vector<uint8_t> replacementFont = font;

    earth_engine::testing::MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    GlyphAtlas atlas(&device);
    EXPECT_EQ(0u, atlas.revision());
    if (!atlas.setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    EXPECT_EQ(1u, atlas.revision());
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

    ASSERT_TRUE(atlas.setFontData(std::move(replacementFont)));
    EXPECT_EQ(2u, atlas.revision())
        << "ready→ready 换字体也必须通知已烘焙标签失效";
}

TEST(GlyphAtlasTest, AmapLetterSpacingExpandsOnlyInterGlyphGaps) {
    EXPECT_FLOAT_EQ(0.0f,
                    FeatureRenderLayer::labelLetterSpacingAdvancePx(
                        1, 0.02f, 20.0f));
    EXPECT_NEAR(1.2f,
                FeatureRenderLayer::labelLetterSpacingAdvancePx(
                    4, 0.02f, 20.0f),
                1e-6f);
    EXPECT_FLOAT_EQ(0.0f,
                    FeatureRenderLayer::labelLetterSpacingAdvancePx(
                        4, -0.02f, 20.0f));
}

TEST(GlyphAtlasTest, ClearFontInvalidatesOfficialRuntimeResidue) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    earth_engine::testing::MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    GlyphAtlas atlas(&device);
    if (!atlas.setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    ASSERT_NE(nullptr, atlas.ensureGlyph('A'));
    const uint64_t before = atlas.revision();
    atlas.clearFontData();
    EXPECT_FALSE(atlas.ready());
    EXPECT_EQ(0u, atlas.residentGlyphCount());
    EXPECT_EQ(nullptr, atlas.ensureGlyph('A'));
    EXPECT_GT(atlas.revision(), before);
}

TEST(GlyphAtlasTest, BudgetIsSharedOncePerRenderFrame) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";

    earth_engine::testing::MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    GlyphAtlas atlas(&device);
    if (!atlas.setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }

    // 0ms 预算仍放行全局第一个缺字形，防复杂字形永久饥饿；同 frameId
    // 再 begin（模拟第二个 FeatureRenderLayer）不得重置预算。
    atlas.beginFrameGlyphBudget(7, 0.0);
    EXPECT_EQ(GlyphAtlas::BudgetedGlyphResult::Ready,
              atlas.ensureGlyphBudgeted('A'));
    atlas.beginFrameGlyphBudget(7, 1000.0);
    EXPECT_EQ(GlyphAtlas::BudgetedGlyphResult::Saturated,
              atlas.ensureGlyphBudgeted('B'))
        << "同一 Renderer 帧的后续图层不能重新获得一份预算";
    EXPECT_EQ(1u, atlas.frameGlyphRasterAttempts());

    atlas.beginFrameGlyphBudget(8, 0.0);
    EXPECT_EQ(GlyphAtlas::BudgetedGlyphResult::Ready,
              atlas.ensureGlyphBudgeted('B'));
    EXPECT_EQ(1u, atlas.frameGlyphRasterAttempts());
}

TEST(GlyphAtlasTest, FailedGlyphUploadLeavesGlyphRetryable) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";

    earth_engine::testing::MockRenderDevice device;
    GlyphAtlas atlas(&device);
    if (!atlas.setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }

    device.textureRegionUploadSucceeds = false;
    EXPECT_EQ(nullptr, atlas.ensureGlyph('A'));
    EXPECT_EQ(0u, atlas.residentGlyphCount());
    EXPECT_EQ(0, atlas.shelfUsedHeightPx());

    device.textureRegionUploadSucceeds = true;
    const GlyphAtlas::Glyph* glyph = atlas.ensureGlyph('A');
    ASSERT_NE(nullptr, glyph);
    EXPECT_EQ(1u, atlas.residentGlyphCount());
    EXPECT_GT(atlas.shelfUsedHeightPx(), 0);
}

// 符号刀B:瓦片实例带 name → 准入烘出标签 quads(32B 布局),与点命令
// 并行发出;placement 登记(labelEntries)一并生效。
TEST_F(FeatureRenderLayerTest, TileSymbolLabelsRenderWhenFontReady) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();  // 让 layer 缓存图集指针(commit 依赖 glyphAtlas_)

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    TileSymbolCpu s;
    s.lonRad = 6.0 * kDeg;
    s.latRad = 29.0 * kDeg;
    genericVisual(s).colorPacked = 1.0f;
    s.name = "AB";
    mesh.symbols.push_back(s);
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    RenderCommandList commands = build();
    const RenderCommand* label = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorLabel) label = &cmd;
    }
    ASSERT_NE(nullptr, label) << "瓦片符号标签未出命令";
    EXPECT_EQ(44, label->vertexStride);
    EXPECT_EQ(12, label->indexCount);  // "AB" 2 字形 × 6
}

TEST_F(FeatureRenderLayerTest,
       LabelHaloUsesStyleGroupAndGlobalDefault) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }

    FeatureRenderStyle style = layer_->style();
    style.labelHaloColor = {0.1f, 0.2f, 0.3f, 0.4f};
    style.labelHaloColorByStyleGroup[78] = {1.0f, 1.0f, 1.0f, 0.8471f};
    layer_->setStyleForContractTest(style);
    build();

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    TileSymbolCpu overridden;
    overridden.paintOrder = 78;
    overridden.labelPaintOrder = 78;
    overridden.labelStyleGroup = 78;
    overridden.lonRad = 6.0 * kDeg;
    overridden.latRad = 29.0 * kDeg;
    genericVisual(overridden).colorPacked = 1.0f;
    overridden.name = "A";
    TileSymbolCpu fallback = overridden;
    fallback.paintOrder = 79;
    fallback.labelPaintOrder = 79;
    fallback.labelStyleGroup = 0;
    fallback.lonRad = 6.1 * kDeg;
    fallback.name = "B";
    mesh.symbols = {overridden, fallback};
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    const RenderCommandList commands = build();
    const RenderCommand* overriddenCommand = nullptr;
    const RenderCommand* fallbackCommand = nullptr;
    for (const auto& command : commands) {
        if (command.kind != RenderCommandKind::VectorLabel) continue;
        if (command.vectorPaintOrder == 78) overriddenCommand = &command;
        if (command.vectorPaintOrder == 79) fallbackCommand = &command;
    }
    ASSERT_NE(nullptr, overriddenCommand);
    ASSERT_NE(nullptr, fallbackCommand);
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 0.8471f}),
              overriddenCommand->vectorUniforms.haloColor);
    EXPECT_EQ(style.labelHaloColor,
              fallbackCommand->vectorUniforms.haloColor);
}

TEST_F(FeatureRenderLayerTest, TileLabelsRebakeAfterReadyFontReplacement) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    std::vector<uint8_t> replacementFont = font;
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();  // 缓存首次字体代次

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    TileSymbolCpu symbol;
    symbol.lonRad = 6.0 * kDeg;
    symbol.latRad = 29.0 * kDeg;
    genericVisual(symbol).colorPacked = 1.0f;
    symbol.name = "AB";
    mesh.symbols.push_back(symbol);
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));
    RenderCommandList commands = build();
    ASSERT_TRUE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorLabel;
    }));

    const int buffersBeforeReplacement = device_.createdBufferCount;
    const uint64_t oldRevision = renderer_->glyphAtlas()->revision();
    ASSERT_TRUE(renderer_->glyphAtlas()->setFontData(
        std::move(replacementFont)));
    ASSERT_GT(renderer_->glyphAtlas()->revision(), oldRevision);

    commands = build();
    EXPECT_EQ(buffersBeforeReplacement + 2, device_.createdBufferCount)
        << "换字体后必须重建标签 VBO/IBO，不能继续使用旧图集 UV";
    ASSERT_TRUE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.kind == RenderCommandKind::VectorLabel;
    }));
}

TEST_F(FeatureRenderLayerTest, TileSymbolPaintOrderSplitsLabelCommands) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    TileSymbolCpu high;
    high.paintOrder = 100;
    high.labelPaintOrder = 100;
    high.lonRad = 6.01 * kDeg;
    high.latRad = 29.0 * kDeg;
    genericVisual(high).colorPacked = 1.0f;
    high.name = "B";
    TileSymbolCpu low;
    low.paintOrder = 20;
    low.labelPaintOrder = 20;
    low.lonRad = 6.0 * kDeg;
    low.latRad = 29.0 * kDeg;
    genericVisual(low).colorPacked = 1.0f;
    low.name = "A";
    mesh.symbols = {high, low};
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    RenderCommandList commands = build();
    sortMvpRenderCommands(commands);
    std::vector<const RenderCommand*> labels;
    for (const auto& command : commands) {
        if (command.kind == RenderCommandKind::VectorLabel) {
            labels.push_back(&command);
        }
    }
    ASSERT_EQ(2u, labels.size());
    EXPECT_EQ(20, labels[0]->vectorPaintOrder);
    EXPECT_EQ(100, labels[1]->vectorPaintOrder);
    EXPECT_EQ(0, labels[0]->indexOffset);
    EXPECT_EQ(6, labels[1]->indexOffset);
    EXPECT_EQ(labels[0]->vertexBuffer, labels[1]->vertexBuffer);
    EXPECT_EQ(labels[0]->indexBuffer, labels[1]->indexBuffer);
}

// 字体注入晚于瓦片 commit:标签先缺,字体就绪翻转后由烘焙源补烘 ——
// store 桶的等价物是 rebuildBucket,瓦片桶没有重镶路径,这条钉住补烘钩子。
TEST_F(FeatureRenderLayerTest, TileSymbolLabelsBakeAfterLateFont) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    TileSymbolCpu s;
    s.lonRad = 6.0 * kDeg;
    s.latRad = 29.0 * kDeg;
    genericVisual(s).colorPacked = 1.0f;
    s.name = "AB";
    mesh.symbols.push_back(s);
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));

    RenderCommandList before = build();
    for (const auto& cmd : before) {
        EXPECT_NE(RenderCommandKind::VectorLabel, cmd.kind)
            << "无字体不该有标签命令";
    }

    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    RenderCommandList after = build();  // 翻转帧:补烘
    const RenderCommand* label = nullptr;
    for (const auto& cmd : after) {
        if (cmd.kind == RenderCommandKind::VectorLabel) label = &cmd;
    }
    ASSERT_NE(nullptr, label) << "字体就绪翻转未补烘瓦片标签";
    EXPECT_EQ(12, label->indexCount);
}

// 符号刀D:placement 碰撞判定 ~300ms 节流 —— 节流窗内新候选不触发重算
// (stats 不变),窗到期才重跑;渐变靠 advanceFades 逐帧平滑(fade 语义
// 由既有 FadeIsGradual 测试钉住)。
// V29 刀3:双代胜负 —— 换代双桶并存期,同一标注(刀1/2 后共享同一 id)的
// 两份候选必须去重、细代胜;无去重则 targets[id] 被 placed/collided 后写
// 覆盖 + sort 非稳定 → target 0/1 抖 = 换代闪(V27 残余机制)。判据 =
// placed 恒 1、opacity 不重启、drop 旧桶后仍稳(placed ⊆ 现桶 entry)。
TEST_F(FeatureRenderLayerTest, CrossGenerationDedupNewerWins) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();  // 缓存图集指针

    auto commitNamed = [&](int z, int x, const char* name, double lonDeg,
                           double latDeg) {
        FeatureTileMesh mesh;
        mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic(lonDeg * kDeg, latDeg * kDeg));
        mesh.hasOrigin = true;
        TileSymbolCpu s;
        s.lonRad = lonDeg * kDeg;
        s.latRad = latDeg * kDeg;
        genericVisual(s).colorPacked = 1.0f;
        s.name = name;
        mesh.symbols.push_back(s);
        layer_->commitTileMesh(
            TileKey{SchemeId("XYZ-WebMercator"), z, x, 200}, std::move(mesh));
    };

    // 旧代 z13 commit,一帧收敛(dt > kFadeSeconds)。
    commitNamed(13, 100, "GEN", 6.0, 29.0);
    frame_.deltaSeconds = 0.35;
    build();
    ASSERT_EQ(1, layer_->labelPlacementStats().placed);

    // 新代 z14 commit(锚点微漂,刀1 窗内 → 继承同 id),旧桶未 drop:
    // 双代并存。placed 必须仍 =1(去重),且 opacity 不因"另一份 collided"
    // 被打回 —— 那正是换代闪。
    commitNamed(14, 200, "GEN", 6.0 + 1e-8, 29.0);
    frame_.deltaSeconds = 0.016;
    build();
    EXPECT_EQ(1, layer_->labelPlacementStats().placed)
        << "双代并存:同 id 去重后恒一份 placed(不是 0 也不是 2)";
    EXPECT_EQ(1, layer_->labelPlacementStats().candidates)
        << "同 id 两份 entry 去重成一个候选";

    // drop 旧桶(换代完成)→ 立即重 placement(V27 drop 置位)→ 仍 1 且
    // fade 已收敛的 opacity 不重启(id 继承 + fades_ 直通)。
    layer_->dropTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 13, 100, 200});
    frame_.deltaSeconds = 0.016;
    build();
    EXPECT_EQ(1, layer_->labelPlacementStats().placed)
        << "drop 旧桶后 placed ⊆ 现桶 entry 且不闪(V27 残余收口判据)";
    EXPECT_FALSE(layer_->hasPendingLabelWork())
        << "无重 fade:opacity 已收敛,谓词应为假";
}

// 七态只读 dump(诊断基建):烘焙前后两个时点各 dump 一次 —— 前者必须
// 暴露 SRC-ONLY(源在无产物,V27 家族"标注隐形"的高频形态),后者
// entry 行 fade/applied 齐且与谓词口径一致;name 过滤命中/不命中。
TEST_F(FeatureRenderLayerTest, DumpLabelLifecycleSevenStates) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";

    FeatureTileMesh mesh;
    mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    mesh.hasOrigin = true;
    TileSymbolCpu s;
    s.lonRad = 6.0 * kDeg;
    s.latRad = 29.0 * kDeg;
    genericVisual(s).colorPacked = 1.0f;
    s.name = "AB";
    mesh.symbols.push_back(s);
    layer_->commitTileMesh(TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                           std::move(mesh));
    build();

    // 无字体:源已存、entry 未产 —— dump 能看见这个"隐形态"。
    const std::string pre = layer_->dumpLabelLifecycle();
    EXPECT_NE(std::string::npos, pre.find("srcs=1 entries=0"))
        << "无字体阶段应是源在无 entry:\n" << pre;
    EXPECT_NE(std::string::npos, pre.find("SRC-ONLY")) << pre;

    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    frame_.deltaSeconds = 0.35;
    build();  // 翻转帧:补烘 + 即时 placement
    frame_.deltaSeconds = 0.35;
    build();  // fade 收敛(0.35 > kFadeSeconds)

    const std::string post = layer_->dumpLabelLifecycle();
    EXPECT_NE(std::string::npos, post.find("name=AB")) << post;
    EXPECT_NE(std::string::npos, post.find("fade=1.00->1.00"))
        << "fade 应已收敛:\n" << post;
    EXPECT_NE(std::string::npos, post.find("applied=1.00"))
        << "回写值应与 fade 一致:\n" << post;
    EXPECT_NE(std::string::npos, post.find("settled=1")) << post;
    EXPECT_NE(std::string::npos, post.find("pending=0"))
        << "dump 首行与 hasPendingLabelWork 谓词同口径:\n" << post;

    // name 过滤:命中留行,不命中滤掉 entry 行(层级首行恒在)。
    EXPECT_NE(std::string::npos,
              layer_->dumpLabelLifecycle("AB").find("name=AB"));
    EXPECT_EQ(std::string::npos,
              layer_->dumpLabelLifecycle("ZZZ").find("name="));
}

TEST_F(FeatureRenderLayerTest, PlacementThrottledBetweenIntervals) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty()) GTEST_SKIP() << "no host font available";
    if (!renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "host font not stbtt-parsable";
    }
    build();  // 缓存图集指针

    auto commitNamed = [&](int x, const char* name, double lonDeg) {
        FeatureTileMesh mesh;
        mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic(lonDeg * kDeg, 29.0 * kDeg));
        mesh.hasOrigin = true;
        TileSymbolCpu s;
        s.lonRad = lonDeg * kDeg;
        s.latRad = 29.0 * kDeg;
        genericVisual(s).colorPacked = 1.0f;
        s.name = name;
        mesh.symbols.push_back(s);
        layer_->commitTileMesh(
            TileKey{SchemeId("XYZ-WebMercator"), 10, x, 200},
            std::move(mesh));
    };

    commitNamed(100, "AB", 6.0);
    frame_.deltaSeconds = 0.016;  // 小步:AB 在屏内,fade 0→1 不可能一帧完
    build();
    ASSERT_EQ(1, layer_->labelPlacementStats().candidates);
    // V27 谓词③:屏内新标注 fade 在途 → 谓词真(帧门控续帧依据)。
    EXPECT_TRUE(layer_->hasPendingLabelWork())
        << "新标注 fade 未收敛期间谓词必须为真(续帧依据)";
    frame_.deltaSeconds = 0.35;  // 一帧越过 kFadeSeconds → 收敛
    build();
    frame_.deltaSeconds = 0.016;
    build();  // 稳态帧
    EXPECT_FALSE(layer_->hasPendingLabelWork())
        << "全部收敛后谓词必须转假(终止态,不许白烧帧)";

    commitNamed(101, "CD", 6.3);
    frame_.deltaSeconds = 0.016;
    build();
    // V27 判据翻转(2026-08-18):此前钉的是"节流窗内新候选不触发重算"——
    // 那正是 V27 根因的一半:桶换代若等 300ms 节流窗,而停帧 settle 只 ~3 帧,
    // 新标注的 target 永远没人置 → 冷启动 POI 隐形到用户缩放为止。修复后
    // 换代(bake 出新标注)置 labelsAwaitingPlacement_ → 下一帧即时全量
    // (与 priorityChanged 同款)。无换代时节流照旧(cooldown 逻辑未动)。
    EXPECT_EQ(2, layer_->labelPlacementStats().candidates)
        << "桶换代应即时纳入新候选(绕过节流),不等 300ms 窗";
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
    EXPECT_EQ(44, label->vertexStride);
    EXPECT_EQ(12, label->indexCount);  // 2 字形 × 6
    // [0]=字形图集,[1]=T2 地形深度槽(host 无深度通路 → nullptr 占位)。
    // 下标必须稳定:后端按下标 1:1 绑纹理单元,浮动会把深度绑错采样器。
    ASSERT_EQ(2u, label->textures.size());
    EXPECT_NE(nullptr, label->textures[0]);
    EXPECT_EQ(nullptr, label->textures[1]);
    ASSERT_TRUE(label->hasVectorUniforms);
    EXPECT_TRUE(label->uniforms.empty());
    EXPECT_FLOAT_EQ(0.0f, label->vectorUniforms.terrainOcclusion[0]);
    EXPECT_GT(label->vectorUniforms.sdfEdge, 0.0f);
    EXPECT_GE(label->vectorUniforms.sdfHaloDelta, 0.0f);
    EXPECT_FLOAT_EQ(0.0f, label->vectorUniforms.sdfGamma)
        << "generic labels must retain derivative-based antialiasing";
    EXPECT_EQ("color", label->pass);
    EXPECT_TRUE(label->blend);

    // 顶点打包:2 字形 × 4 顶点 × 44B;offsetPx 水平居中。
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        label->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    ASSERT_EQ(2u * 4u * 44u, vb->bytes().size());
    const auto* floats = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_LT(floats[6], 0.0f);  // 首顶点 offsetPx.x 在锚点左侧
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
    for (size_t i = 8; i < count; i += 11) {
        EXPECT_FLOAT_EQ(1.0f, floats[i]);
    }
}

TEST_F(FeatureLabelPlacementTest, FadeUploadsOnlyChangedLabelRanges) {
    // 同桶、同位置两条标签只会放行一条；碰撞落选者 opacity 始终为 0，
    // 不应因为另一条在 fade 就跟着重传。旧实现会上传整个 label VBO。
    layer_->store().addFeature(makeNamedPoint(0.0, 0.0, "AAAA"));
    layer_->store().addFeature(makeNamedPoint(0.0001, 0.0, "BBBB"));

    const size_t bytesBefore = device_.totalBufferUpdateBytes;
    RenderCommandList commands = build();
    const RenderCommand* label = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorLabel) label = &cmd;
    }
    ASSERT_NE(nullptr, label);
    const auto* vb = dynamic_cast<const DummyBuffer*>(label->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const size_t uploaded = device_.totalBufferUpdateBytes - bytesBefore;
    EXPECT_GT(uploaded, 0u);
    EXPECT_LT(uploaded, vb->bytes().size())
        << "只有 placed 标签变化时不得重传同桶 collided 标签顶点";
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
    layer_->setStyleForContractTest(layer_->style());
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
    for (size_t i = 8; i < count; i += 11) {
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

FeatureTerrainSampling makeGenerationSampling(
    const std::shared_ptr<float>& height) {
    FeatureTerrainSampling s;
    s.makeAreaSampler = [height](const Rectangle&) {
        return [height](double, double) -> std::optional<float> {
            return *height;
        };
    };
    s.revision = []() -> uint64_t {
        return TerrainHeightService::heightmapGeneration();
    };
    return s;
}

} // namespace

TEST_F(FeatureRenderLayerTest,
       OfficialVectorDerivativesReclampAfterHeightmapGenerationChanges) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty() ||
        !renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "no host TrueType font for label VBO verification";
    }
    const std::vector<uint8_t> iconPixels(64 * 64 * 4, 255);
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "official-terrain-icon", 64, 64, iconPixels));

    auto height = std::make_shared<float>(100.0f);
    FeatureRenderLayer regions("official-terrain-regions", &device_,
                               Ellipsoid::WGS84());
    FeatureRenderLayer main("official-terrain-main", &device_,
                            Ellipsoid::WGS84());
    FeatureRenderLayer poi("official-terrain-poi", &device_,
                           Ellipsoid::WGS84());
    regions.installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Regions);
    main.installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);

    FeatureRenderStyle poiStyle =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    installTestOfficialLabelStyle(poiStyle);
    poiStyle.pointStylePropertyA = "amap_class";
    poiStyle.pointStylePropertyB = "amap_subkey";
    poiStyle.pointStyleResolver = [](const std::string& cls,
                                     const std::string& sub,
                                     const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle out;
        if (cls != "12024" || sub != "854") return out;
        out.enabled = true;
        out.image = "official-terrain-icon";
        out.sizePx = 20.0f;
        out.labelLayout.emplace();
        out.labelLayout->iconWidthPx = 20.0f;
        out.labelLayout->iconHeightPx = 20.0f;
        return out;
    };
    poi.setStyleForContractTest(poiStyle);

    regions.setTerrainSampling(makeGenerationSampling(height));
    main.setTerrainSampling(makeGenerationSampling(height));
    poi.setTerrainSampling(makeGenerationSampling(height));

    Feature surface = makePolygon(6.0, 29.0, 0.002);
    addOfficialMetadata(surface, "30001", "1", "73");
    Feature road = makeLine(6.0, 29.0, 0.002);
    addOfficialMetadata(road, "20001", "1", "90");
    Feature building = makePolygon(6.003, 29.0, 0.002);
    addOfficialMetadata(building, "55001", "1", "47");
    building.properties["amap_height"] = "6";
    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(6.001 * kDeg, 29.001 * kDeg)}};
    point.properties = {{"name", "terrain label"},
                        {"amap_class", "12024"},
                        {"amap_subkey", "854"}};
    addOfficialMetadata(point, "12024", "854", "90");

    const TileKey regionKey{SchemeId("XYZ-WebMercator"), 14, 100, 200};
    const TileKey mainKey{SchemeId("XYZ-WebMercator"), 14, 101, 200};
    const TileKey poiKey{SchemeId("XYZ-WebMercator"), 14, 102, 200};
    auto regionMesh = FeatureRenderLayer::tessellateTileMesh(
        regions.workerTessellationContext(), {surface});
    auto mainMesh = FeatureRenderLayer::tessellateTileMesh(
        main.workerTessellationContext(), {road, building});
    auto poiMesh = FeatureRenderLayer::tessellateTileMesh(
        poi.workerTessellationContext(), {point});
    ASSERT_EQ(TileMeshCommitResult::Committed,
              regions.commitTileMesh(regionKey, std::move(regionMesh)));
    ASSERT_EQ(TileMeshCommitResult::Committed,
              main.commitTileMesh(mainKey, std::move(mainMesh)));
    ASSERT_EQ(TileMeshCommitResult::Committed,
              poi.commitTileMesh(poiKey, std::move(poiMesh)));

    auto buildAll = [&](double dt) {
        frame_.deltaSeconds = dt;
        RenderCommandList commands;
        regions.buildRenderCommands(frame_, *renderer_, commands);
        main.buildRenderCommands(frame_, *renderer_, commands);
        poi.buildRenderCommands(frame_, *renderer_, commands);
    };
    const auto firstVertexHeight = [](const Buffer* buffer,
                                      const Vec3& origin,
                                      size_t strideFloats) {
        const auto* dummy = dynamic_cast<const DummyBuffer*>(buffer);
        EXPECT_NE(nullptr, dummy);
        if (!dummy || dummy->bytes().size() < strideFloats * sizeof(float)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const auto* values =
            reinterpret_cast<const float*>(dummy->bytes().data());
        return Ellipsoid::WGS84().cartesianToCartographic(
            origin + Vec3(values[0], values[1], values[2])).height();
    };
    const auto extrusionHeightRange = [](const Buffer* buffer,
                                         const Vec3& origin) {
        const auto* dummy = dynamic_cast<const DummyBuffer*>(buffer);
        EXPECT_NE(nullptr, dummy);
        std::pair<double, double> range{
            std::numeric_limits<double>::max(),
            std::numeric_limits<double>::lowest()};
        if (!dummy) return range;
        const auto* values =
            reinterpret_cast<const float*>(dummy->bytes().data());
        const size_t count = dummy->bytes().size() / sizeof(float);
        for (size_t i = 0; i + 2 < count; i += 7) {
            const double h = Ellipsoid::WGS84().cartesianToCartographic(
                origin + Vec3(values[i], values[i + 1], values[i + 2]))
                                 .height();
            range.first = std::min(range.first, h);
            range.second = std::max(range.second, h);
        }
        return range;
    };
    // First pass observes the current generation and queues every tile;
    // second pass drains those queues and materializes stable derivatives.
    buildAll(2.1);
    for (int i = 0; i < 8; ++i) buildAll(1.0 / 60.0);
    const auto regionsAt100 = regions.terrainReclampSnapshotForTest();
    const auto mainAt100 = main.terrainReclampSnapshotForTest();
    const auto poiAt100 = poi.terrainReclampSnapshotForTest();
    ASSERT_NE(nullptr, regionsAt100.fillVertexBuffer);
    ASSERT_NE(nullptr, mainAt100.lineVertexBuffer);
    ASSERT_NE(nullptr, mainAt100.extrusionVertexBuffer);
    ASSERT_NE(nullptr, poiAt100.pointVertexBuffer);
    ASSERT_NE(nullptr, poiAt100.labelVertexBuffer);
    ASSERT_TRUE(regionsAt100.origin.has_value());
    ASSERT_TRUE(mainAt100.origin.has_value());
    ASSERT_TRUE(poiAt100.origin.has_value());
    ASSERT_TRUE(poiAt100.firstLabelAnchorHeightMeters.has_value());
    EXPECT_NEAR(firstVertexHeight(regionsAt100.fillVertexBuffer,
                                  *regionsAt100.origin, 4),
                100.0, 1.0);
    EXPECT_NEAR(firstVertexHeight(mainAt100.lineVertexBuffer,
                                  *mainAt100.origin, 12),
                100.0, 1.0);
    EXPECT_NEAR(firstVertexHeight(poiAt100.pointVertexBuffer,
                                  *poiAt100.origin, 9),
                100.0, 1.0);
    EXPECT_NEAR(firstVertexHeight(poiAt100.labelVertexBuffer,
                                  *poiAt100.origin, 11),
                100.0, 1.0);
    const auto extrusionAt100 = extrusionHeightRange(
        mainAt100.extrusionVertexBuffer, *mainAt100.origin);
    EXPECT_NEAR(extrusionAt100.first, 100.0, 1.0);
    EXPECT_NEAR(extrusionAt100.second, 106.0, 1.0);
    EXPECT_NEAR(*poiAt100.firstLabelAnchorHeightMeters, 100.0, 1.0);

    *height = 700.0f;
    TileRenderContentState generationSource;
    auto changedHeightmap = std::make_unique<DecodedHeightmap>();
    changedHeightmap->tileSize = 1;
    changedHeightmap->assignHeights(std::vector<float>{700.0f});
    generationSource.setRetainedHeightmap(std::move(changedHeightmap));
    const uint64_t changedGeneration =
        TerrainHeightService::heightmapGeneration();

    buildAll(2.1);               // observe generation and enqueue
    for (int i = 0; i < 8; ++i) {
        buildAll(1.0 / 60.0);    // bounded queue/glyph drain
    }
    const auto regionsAt700 = regions.terrainReclampSnapshotForTest();
    const auto mainAt700 = main.terrainReclampSnapshotForTest();
    const auto poiAt700 = poi.terrainReclampSnapshotForTest();

    EXPECT_EQ(changedGeneration, regionsAt700.appliedRevision);
    EXPECT_EQ(changedGeneration, mainAt700.appliedRevision);
    EXPECT_EQ(changedGeneration, poiAt700.appliedRevision);
    EXPECT_EQ(0u, regionsAt700.pendingBuckets);
    EXPECT_EQ(0u, mainAt700.pendingBuckets);
    EXPECT_EQ(0u, poiAt700.pendingBuckets);
    ASSERT_TRUE(regionsAt700.origin.has_value());
    ASSERT_TRUE(mainAt700.origin.has_value());
    ASSERT_TRUE(poiAt700.origin.has_value());
    EXPECT_NEAR(firstVertexHeight(regionsAt700.fillVertexBuffer,
                                  *regionsAt700.origin, 4),
                700.0, 1.0);
    EXPECT_NEAR(firstVertexHeight(mainAt700.lineVertexBuffer,
                                  *mainAt700.origin, 12),
                700.0, 1.0);
    EXPECT_NEAR(firstVertexHeight(poiAt700.pointVertexBuffer,
                                  *poiAt700.origin, 9),
                700.0, 1.0);
    EXPECT_NEAR(firstVertexHeight(poiAt700.labelVertexBuffer,
                                  *poiAt700.origin, 11),
                700.0, 1.0);
    const auto extrusionAt700 = extrusionHeightRange(
        mainAt700.extrusionVertexBuffer, *mainAt700.origin);
    EXPECT_NEAR(extrusionAt700.first, 700.0, 1.0);
    EXPECT_NEAR(extrusionAt700.second, 706.0, 1.0);
    ASSERT_TRUE(poiAt700.firstLabelAnchorHeightMeters.has_value());
    EXPECT_NEAR(*poiAt700.firstLabelAnchorHeightMeters, 700.0, 1.0);
}

TEST_F(FeatureRenderLayerTest,
       ReclampTriggersImmediatelyWithoutWallClockCooldown) {
    // ② 回归:删 2s 时间冷却后,revision 一变(即便 dt 极小)应立即触发重钳。
    // 旧实现:首轮触发后 cooldown=2s,随后的 revision 变化在 dt<2s 内不会触发
    // → 锚点停在旧高度(最长 2s 陈旧窗口)。新实现:事件驱动,revision 落后且
    // 队列空即起新一轮,无任何 wall-clock 依赖。
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty() ||
        !renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "no host TrueType font";
    }
    const std::vector<uint8_t> iconPixels(64 * 64 * 4, 255);
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "official-reclamp-icon", 64, 64, iconPixels));

    auto height = std::make_shared<float>(100.0f);
    FeatureRenderLayer layer("official-reclamp-poi", &device_,
                             Ellipsoid::WGS84());
    layer.installAmapClassicProfile(FeatureRenderLayer::AmapClassicProfile::Poi);
    FeatureRenderStyle poiStyle =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    installTestOfficialLabelStyle(poiStyle);
    poiStyle.pointStylePropertyA = "amap_class";
    poiStyle.pointStylePropertyB = "amap_subkey";
    poiStyle.pointStyleResolver = [](const std::string& cls,
                                     const std::string& sub,
                                     const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle out;
        if (cls != "12024" || sub != "854") return out;
        out.enabled = true;
        out.image = "official-reclamp-icon";
        out.sizePx = 20.0f;
        out.labelLayout.emplace();
        out.labelLayout->iconWidthPx = 20.0f;
        out.labelLayout->iconHeightPx = 20.0f;
        return out;
    };
    layer.setStyleForContractTest(poiStyle);
    layer.setTerrainSampling(makeGenerationSampling(height));

    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(6.001 * kDeg, 29.001 * kDeg)}};
    point.properties = {{"name", "reclamp label"},
                        {"amap_class", "12024"},
                        {"amap_subkey", "854"}};
    addOfficialMetadata(point, "12024", "854", "90");
    const TileKey key{SchemeId("XYZ-WebMercator"), 14, 102, 200};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer.workerTessellationContext(), {point});
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer.commitTileMesh(key, std::move(mesh)));

    auto build = [&](double dt) {
        frame_.deltaSeconds = dt;
        RenderCommandList commands;
        layer.buildRenderCommands(frame_, *renderer_, commands);
    };
    const auto anchorHeight = [&]() -> double {
        const auto snap = layer.terrainReclampSnapshotForTest();
        if (!snap.firstLabelAnchorHeightMeters.has_value()) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return *snap.firstLabelAnchorHeightMeters;
    };

    // 首轮:任意 dt 触发第一次(rev != 0)。
    for (int i = 0; i < 8; ++i) build(1.0 / 60.0);
    EXPECT_NEAR(anchorHeight(), 100.0, 1.0);

    // 地形变高 + revision 变,仅 1/60s(dt 极小,旧 2s 冷却下不会触发第二轮)。
    *height = 700.0f;
    TileRenderContentState generationSource;
    auto changedHeightmap = std::make_unique<DecodedHeightmap>();
    changedHeightmap->tileSize = 1;
    changedHeightmap->assignHeights(std::vector<float>{700.0f});
    generationSource.setRetainedHeightmap(std::move(changedHeightmap));
    const uint64_t changedGeneration =
        TerrainHeightService::heightmapGeneration();

    // 全程 dt=1/60,无 2s 流逝 → 证明重钳由 revision 事件驱动,与时间解耦。
    for (int i = 0; i < 8; ++i) build(1.0 / 60.0);
    EXPECT_EQ(changedGeneration,
              layer.terrainReclampSnapshotForTest().appliedRevision);
    EXPECT_EQ(0u, layer.terrainReclampSnapshotForTest().pendingBuckets);
    EXPECT_NEAR(anchorHeight(), 700.0, 1.0);
}

TEST_F(FeatureRenderLayerTest,
       HeightOnlyReclampPreservesSelectionAndUpdatesHeights) {
    // ① height-only:地形 revision 变 → 只重采高度 + 重物化,不重选中/重
    // resolve。判定:锚点高度跟随新地形,而 symbolSelectionSignature(选中集
    // 签名)不变 —— 证明重钳走的是缓存 activeResolvedSymbols_,非全量 rebuild。
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty() ||
        !renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "no host TrueType font";
    }
    const std::vector<uint8_t> iconPixels(64 * 64 * 4, 255);
    ASSERT_TRUE(renderer_->iconAtlas()->addImage(
        "official-heightonly-icon", 64, 64, iconPixels));

    auto height = std::make_shared<float>(100.0f);
    FeatureRenderLayer layer("official-heightonly-poi", &device_,
                             Ellipsoid::WGS84());
    layer.installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    FeatureRenderStyle poiStyle =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    installTestOfficialLabelStyle(poiStyle);
    poiStyle.pointStylePropertyA = "amap_class";
    poiStyle.pointStylePropertyB = "amap_subkey";
    poiStyle.pointStyleResolver = [](const std::string& cls,
                                     const std::string& sub,
                                     const std::string&, double, float) {
        FeatureRenderStyle::ResolvedPointStyle out;
        if (cls != "12024" || sub != "854") return out;
        out.enabled = true;
        out.image = "official-heightonly-icon";
        out.sizePx = 20.0f;
        out.labelLayout.emplace();
        out.labelLayout->iconWidthPx = 20.0f;
        out.labelLayout->iconHeightPx = 20.0f;
        return out;
    };
    layer.setStyleForContractTest(poiStyle);
    layer.setTerrainSampling(makeGenerationSampling(height));

    Feature point;
    point.type = GeometryType::Point;
    point.rings = {{Cartographic(6.001 * kDeg, 29.001 * kDeg)}};
    point.properties = {{"name", "heightonly label"},
                        {"amap_class", "12024"},
                        {"amap_subkey", "854"}};
    addOfficialMetadata(point, "12024", "854", "90");
    const TileKey key{SchemeId("XYZ-WebMercator"), 14, 102, 200};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer.workerTessellationContext(), {point});
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer.commitTileMesh(key, std::move(mesh)));

    auto build = [&](double dt) {
        frame_.deltaSeconds = dt;
        RenderCommandList commands;
        layer.buildRenderCommands(frame_, *renderer_, commands);
    };
    const auto anchorHeight = [&]() -> double {
        const auto s = layer.terrainReclampSnapshotForTest();
        return s.firstLabelAnchorHeightMeters.value_or(
            std::numeric_limits<double>::quiet_NaN());
    };

    // 首轮全量 build(选中集签名定稿)。
    for (int i = 0; i < 8; ++i) build(1.0 / 60.0);
    const uint64_t sigBefore =
        layer.terrainReclampSnapshotForTest().symbolSelectionSignature;
    ASSERT_NE(0u, sigBefore);
    EXPECT_NEAR(anchorHeight(), 100.0, 1.0);

    // 地形变高 → height-only 重钳:高度更新、选中集签名不变。
    *height = 700.0f;
    TileRenderContentState generationSource;
    auto changedHeightmap = std::make_unique<DecodedHeightmap>();
    changedHeightmap->tileSize = 1;
    changedHeightmap->assignHeights(std::vector<float>{700.0f});
    generationSource.setRetainedHeightmap(std::move(changedHeightmap));
    for (int i = 0; i < 8; ++i) build(1.0 / 60.0);

    const auto after = layer.terrainReclampSnapshotForTest();
    EXPECT_NEAR(anchorHeight(), 700.0, 1.0);
    EXPECT_EQ(sigBefore, after.symbolSelectionSignature);
    EXPECT_EQ(0u, after.pendingBuckets);
}

namespace {

/// 闭合性断言:体积网格的每条**有向**边,其反向边引用次数必须相等
/// (即曲面闭合、法向一致)。这比"每条无向边恰 2 次"更准确——自交面
/// 预分裂后会在交点处形成非流形的竖直接触边(4 个面共享),那是两个
/// 闭合体块在一条边上相接,z-fail 计数依然正确,不该被判失败;而悬边
/// (只被引用一次)会被抓出。顶点按 stride 取键:pos-only 体(fill,
/// stride 12)只比位置,墙带(line,stride 24)连 extrude 一起比——
/// CPU 侧零宽,±两侧顶点仅 extrude 符号可分。
void expectWatertight(const RenderCommand& vol) {
    const auto* ib =
        dynamic_cast<const DummyBuffer*>(vol.indexBuffer);
    const auto* vb =
        dynamic_cast<const DummyBuffer*>(vol.vertexBuffer);
    ASSERT_NE(nullptr, ib);
    ASSERT_NE(nullptr, vb);
    const auto* idx = reinterpret_cast<const uint32_t*>(ib->bytes().data());
    const size_t idxCount = ib->bytes().size() / sizeof(uint32_t);
    ASSERT_EQ(static_cast<size_t>(vol.indexCount), idxCount);
    const auto* verts = reinterpret_cast<const float*>(vb->bytes().data());
    const size_t strideFloats =
        static_cast<size_t>(vol.vertexStride) / sizeof(float);
    const size_t keyFloats = std::min<size_t>(strideFloats, 6);
    using VertKey = std::array<int64_t, 6>;
    auto posKey = [&](uint32_t v) -> VertKey {
        const float* p = verts + v * strideFloats;
        VertKey k{};
        for (size_t i = 0; i < keyFloats; ++i) {
            k[i] = static_cast<int64_t>(std::llround(p[i] * 1000.0));
        }
        return k;
    };
    std::map<std::pair<VertKey, VertKey>, int> directed;
    for (size_t t = 0; t + 2 < idxCount; t += 3) {
        for (int e = 0; e < 3; ++e) {
            directed[{posKey(idx[t + e]), posKey(idx[t + (e + 1) % 3])}]++;
        }
    }
    for (const auto& [edge, count] : directed) {
        const auto reverse = directed.find({edge.second, edge.first});
        const int back = reverse == directed.end() ? 0 : reverse->second;
        EXPECT_EQ(count, back)
            << "directed edge used " << count << "x but reverse " << back
            << "x (open surface)";
    }
}

} // namespace

TEST_F(FeatureRenderLayerTest, StencilVolumePairForClampedPolygon) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
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
    // 墙复用 cap 顶点(边界边成墙):底/顶 cap 各 4 顶点 = 8;
    // 索引 = 2 cap×2 三角×3 + 4 条边界边×6 = 36。
    EXPECT_EQ(8u * 12u, vb->bytes().size());
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

    // 状态校验通过(两 phase 各自规则 + 与普通矢量共享 order)。
    const auto validation = validateMvpRenderCommands(commands, frame_.frameId);
    EXPECT_FALSE(validation.has_value())
        << (validation ? validation->message : "");
}

TEST_F(FeatureRenderLayerTest, SelfIntersectingFillVolumeIsWatertight) {
    // 编辑可以把面拖成自交(bowtie)。cap 走 PolygonTessellator(含自交
    // 预分裂),墙若仍按原始 ring 走,两者轮廓不一致 → 体不水密 →
    // z-fail 计数错乱 → fill 破碎/泄漏(真机复现)。
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));

    Feature f;
    f.type = GeometryType::Polygon;
    const double w = 0.0, s = 0.0, e = 0.01 * kDeg, n = 0.01 * kDeg;
    // 对角顺序 → 边 (w,s)->(e,n) 与 (e,s)->(w,n) 在中心相交。
    f.rings = {{Cartographic(w, s), Cartographic(e, n), Cartographic(e, s),
                Cartographic(w, n), Cartographic(w, s)}};
    layer_->store().addFeature(std::move(f));

    const RenderCommand* vol = nullptr;
    for (const auto& cmd : build()) {
        if (cmd.kind == RenderCommandKind::VectorStencil &&
            cmd.stencilPhase == StencilPhase::ClassifyVolume &&
            cmd.vertexStride == 12) {
            vol = &cmd;
        }
    }
    ASSERT_NE(nullptr, vol);
    expectWatertight(*vol);
}

TEST_F(FeatureRenderLayerTest, FillVolumeIsWatertight) {
    // z-fail 双面计数要求体封闭。fill 体的墙顶点与 cap 顶点索引不共享、
    // 但**位置同源**,故按位置量化判边(与线墙带同一断言口径)。
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    layer_->store().addFeature(makePolygon(0.0, 0.0, 0.01));

    const RenderCommand* vol = nullptr;
    for (const auto& cmd : build()) {
        if (cmd.kind == RenderCommandKind::VectorStencil &&
            cmd.stencilPhase == StencilPhase::ClassifyVolume &&
            cmd.vertexStride == 12) {
            vol = &cmd;
        }
    }
    ASSERT_NE(nullptr, vol);
    expectWatertight(*vol);
}

namespace {

/// 合成地形:除一个小尖峰区域外恒 0。尖峰放在 8×8 粗网格采样点之间,
/// 用于验证体高范围是否漏掉面内峰值。
FeatureTerrainSampling makeSpikeSampling(double spikeLngDeg,
                                         double spikeLatDeg,
                                         double radiusDeg,
                                         float peak) {
    FeatureTerrainSampling s;
    s.makeAreaSampler = [spikeLngDeg, spikeLatDeg, radiusDeg,
                         peak](const Rectangle&) {
        return [spikeLngDeg, spikeLatDeg, radiusDeg,
                peak](double lng, double lat) -> std::optional<float> {
            const double dl = lng / kDeg - spikeLngDeg;
            const double dt = lat / kDeg - spikeLatDeg;
            return (dl * dl + dt * dt < radiusDeg * radiusDeg) ? peak : 0.0f;
        };
    };
    s.revision = []() -> uint64_t { return 1; };
    return s;
}

/// 体积顶点相对桶原点,原点 = 首个底 cap 顶点。用「顶点到地心距离 −
/// 原点到地心距离」还原相对高差不可行(缺原点绝对值),改用体自身的
/// 底/顶跨度:同一 (lng,lat) 的底顶顶点对间距 = top − bottom。
double volumeShellSpanMeters(const RenderCommand& vol) {
    const auto* vb = dynamic_cast<const DummyBuffer*>(vol.vertexBuffer);
    if (!vb) return 0.0;
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    const size_t count = vb->bytes().size() / 12;
    // 底 cap 与顶 cap 同拓扑,顶 cap 顶点紧随底 cap;取首顶点对。
    const size_t capVerts = count / 2 >= 4 ? 4 : 1;
    double best = 0.0;
    for (size_t i = 0; i < capVerts; ++i) {
        const size_t j = i + capVerts;
        if (j >= count) break;
        double d = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double delta = static_cast<double>(f[j * 3 + k]) -
                                 static_cast<double>(f[i * 3 + k]);
            d += delta * delta;
        }
        best = std::max(best, std::sqrt(d));
    }
    return best;
}

} // namespace

TEST_F(FeatureRenderLayerTest, FillVolumeCoversInteriorPeakBetweenGridSamples) {
    // fill 体高 = 环顶点 + 8×8 粗内部网格采样的 min/max ± 120m margin。
    // 面内尖峰若落在网格采样点之间会被漏掉 → 体顶不够高 → 峰顶处分类
    // 断面。这里把尖峰放在网格缝隙里,验证 margin 是否兜得住。
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
    // 面 0..0.08°,8×8 网格采样点在 0.01/0.02/.../0.07;尖峰放 0.045
    // (恰在 0.04 与 0.05 之间),半径 0.002° ≈ 220m。
    layer_->setTerrainSampling(
        makeSpikeSampling(0.045, 0.045, 0.002, 400.0f));
    layer_->store().addFeature(makePolygon(0.0, 0.0, 0.08));

    const RenderCommand* vol = nullptr;
    for (const auto& cmd : build()) {
        if (cmd.kind == RenderCommandKind::VectorStencil &&
            cmd.stencilPhase == StencilPhase::ClassifyVolume &&
            cmd.vertexStride == 12) {
            vol = &cmd;
        }
    }
    ASSERT_NE(nullptr, vol);
    // 体壳跨度必须罩住 400m 尖峰(顶 ≥ 峰高才不会在峰顶断面)。
    // 采到峰 → 跨度 ≈ 400+240;漏峰 → 仅 240。
    const double span = volumeShellSpanMeters(*vol);
    EXPECT_GT(span, 400.0) << "体高未罩住面内尖峰,峰顶会断面;span=" << span;
}

TEST_F(FeatureRenderLayerTest, StencilVolumePairsKeepFeaturePaintOrder) {
    // 贴地分类体也必须携带逐要素 ordinal；否则同桶内重叠水面/绿地
    // 会再次退化为输入顺序。每个 ordinal 仍是一对相邻的 volume/color
    // 命令，并在全局排序后按低→高稳定发出。
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.paintOrderExpr = StyleExpression::match(
        "surface",
        {{"green", StyleExpression::literal(20.0)},
         {"water", StyleExpression::literal(50.0)}},
        StyleExpression::literal(0.0));
    style.lineStyleGroupExpr = style.paintOrderExpr;
    style.lineStyleGroupExpr = style.paintOrderExpr;
    layer_->setStyleForContractTest(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));

    Feature water = makePolygon(0.0, 0.0, 0.01);
    water.properties["surface"] = "water";
    Feature green = makePolygon(0.002, 0.002, 0.01);
    green.properties["surface"] = "green";
    // 反转输入顺序，模拟 PBF/瓦片提交顺序变化。
    layer_->store().addFeature(std::move(water));
    layer_->store().addFeature(std::move(green));

    RenderCommandList commands = build();
    ASSERT_EQ(4u, commands.size());
    // CPU map 已按 ordinal 产出有序命令；即便未来桶遍历改变，调用全局
    // sorter 后的契约仍相同。
    sortMvpRenderCommands(commands);

    ASSERT_EQ(RenderCommandKind::VectorStencil, commands[0].kind);
    ASSERT_EQ(RenderCommandKind::VectorStencil, commands[1].kind);
    ASSERT_EQ(RenderCommandKind::VectorStencil, commands[2].kind);
    ASSERT_EQ(RenderCommandKind::VectorStencil, commands[3].kind);
    EXPECT_EQ(StencilPhase::ClassifyVolume, commands[0].stencilPhase);
    EXPECT_EQ(StencilPhase::ClassifyColor, commands[1].stencilPhase);
    EXPECT_EQ(StencilPhase::ClassifyVolume, commands[2].stencilPhase);
    EXPECT_EQ(StencilPhase::ClassifyColor, commands[3].stencilPhase);
    EXPECT_EQ(20, commands[0].vectorPaintOrder);
    EXPECT_EQ(20, commands[1].vectorPaintOrder);
    EXPECT_EQ(50, commands[2].vectorPaintOrder);
    EXPECT_EQ(50, commands[3].vectorPaintOrder);
    EXPECT_EQ(commands[0].vertexBuffer, commands[1].vertexBuffer);
    EXPECT_EQ(commands[0].indexBuffer, commands[1].indexBuffer);
    EXPECT_EQ(commands[2].vertexBuffer, commands[3].vertexBuffer);
    EXPECT_EQ(commands[2].indexBuffer, commands[3].indexBuffer);
    EXPECT_FALSE(validateMvpRenderCommands(commands, frame_.frameId).has_value());
}

TEST_F(FeatureRenderLayerTest, StencilFallsBackToSamplingWithoutSupport) {
    device_.stencilClassificationSupported = false;
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
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

TEST_F(FeatureRenderLayerTest,
       OfficialSurfaceClampDoesNotEnterGenericStencilColorContract) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Regions);
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    Feature surface = makePolygon(0.0, 0.0, 0.01);
    surface.properties = {{"amap_class", "30001"},
                          {"amap_subkey", "1"},
                          {"amap_draworder", "73"},
                          {"amap_minzoom", "2"},
                          {"amap_maxzoom", "30"}};
    layer_->store().addFeature(std::move(surface));
    const RenderCommandList commands = build();
    ASSERT_FALSE(commands.empty());
    for (const auto& command : commands)
        EXPECT_NE(RenderCommandKind::VectorStencil, command.kind);
}

TEST_F(FeatureRenderLayerTest,
       OfficialSurfaceTileCommitSamplesTerrainAndKeepsReclampSource) {
    FeatureRenderLayer layer("official-surface-terrain", &device_,
                             Ellipsoid::WGS84());
    layer.installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Regions);
    layer.setTerrainSampling(makeFlatSampling(725.0f));
    auto ctx = layer.workerTessellationContext();

    Feature surface = makePolygon(6.0, 29.0, 0.002);
    surface.properties = {{"amap_class", "30001"},
                          {"amap_subkey", "1"},
                          {"amap_draworder", "73"},
                          {"amap_minzoom", "2"},
                          {"amap_maxzoom", "30"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(ctx, {surface});
    ASSERT_FALSE(mesh.fillVerts.empty());
    ASSERT_FALSE(mesh.fillClampSource.empty());
    EXPECT_EQ(mesh.fillClampSource.size(), mesh.fillVerts.size() / 4 * 3);
    const Vec3 origin = mesh.origin;

    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer.commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200},
                  std::move(mesh)));
    RenderCommandList commands;
    layer.buildRenderCommands(frame_, *renderer_, commands);
    const RenderCommand* fill = nullptr;
    for (const auto& command : commands) {
        if (command.kind == RenderCommandKind::VectorFill) fill = &command;
    }
    ASSERT_NE(nullptr, fill);
    const auto* buffer =
        dynamic_cast<const earth_engine::testing::DummyBuffer*>(
            fill->vertexBuffer);
    ASSERT_NE(nullptr, buffer);
    const float* vertex =
        reinterpret_cast<const float*>(buffer->bytes().data());
    const Cartographic position = Ellipsoid::WGS84().cartesianToCartographic(
        origin + Vec3(vertex[0], vertex[1], vertex[2]));
    EXPECT_NEAR(position.height(), 725.0, 1.0);
}

TEST_F(FeatureRenderLayerTest,
       BakedOfficialSurfaceSkipsFillCdtButKeepsBuildingExtrusionAndRoad) {
    FeatureRenderLayer layer("official-baked-surface", &device_,
                             Ellipsoid::WGS84());
    layer.installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);
    auto ctx = layer.workerTessellationContext();
    ctx.bakeOfficialSurfaceFill = true;

    Feature surface = makePolygon(6.0, 29.0, 0.002);
    surface.properties = {{"amap_class", "30001"},
                          {"amap_subkey", "1"},
                          {"amap_draworder", "73"},
                          {"amap_minzoom", "2"},
                          {"amap_maxzoom", "30"}};
    Feature building = surface;
    building.properties["amap_class"] = "55001";
    building.properties["amap_draworder"] = "47";
    building.properties["amap_minzoom"] = "3";
    building.properties["amap_maxzoom"] = "20";
    building.properties["amap_height"] = "6";
    Feature road = makeLine(6.0, 29.0, 0.002);
    addOfficialMetadata(road, "20001", "1", "90");

    const FeatureTileMesh mesh = FeatureRenderLayer::tessellateTileMesh(
        ctx, {surface, building, road});
    EXPECT_TRUE(mesh.fillIndices.empty());
    EXPECT_EQ(0u, mesh.diagnostics.polygonCdtPointTriangleTests);
    EXPECT_FALSE(mesh.extrudeIndices.empty());
    EXPECT_FALSE(mesh.lineIndices.empty());
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


TEST_F(FeatureRenderLayerTest, StencilLineVolumePairForClampedLineString) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
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
    ASSERT_TRUE(vol->hasVectorUniforms);
    EXPECT_TRUE(vol->uniforms.empty());
    EXPECT_GT(vol->vectorUniforms.halfWidthPerEyeZ, 0.0f);
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

TEST_F(FeatureRenderLayerTest,
       OfficialTransportClampDoesNotEnterGenericStencilLineContract) {
    FeatureRenderStyle style = layer_->style();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    Feature road = makeLine(0.0, 0.0, 0.05);
    road.properties = {{"amap_class", "20001"},
                       {"amap_subkey", "1"},
                       {"amap_draworder", "82"},
                       {"amap_minzoom", "3"},
                       {"amap_maxzoom", "20"}};
    layer_->store().addFeature(std::move(road));
    const Vec3 target = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.01 * kDeg, 0.0));
    camera_.lookAt(target + target.normalized() *
                       (4.0e7 / std::pow(2.0, 13.0)),
                   target, Vec3(0, 0, 1));
    const RenderCommandList commands = build();
    ASSERT_FALSE(commands.empty());
    for (const auto& command : commands)
        EXPECT_NE(RenderCommandKind::VectorStencil, command.kind);
}

TEST_F(FeatureRenderLayerTest, ClampedPolygonOutlineBecomesClosedLineVolume) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.fillOutlineEnabled = true;
    layer_->setStyleForContractTest(style);
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
    // 闭合墙带(实线):n 横截面 wrap → 8n 三角,无端 cap。
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
    layer_->setStyleForContractTest(style);
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

TEST_F(FeatureRenderLayerTest, StencilLineDashSplitsIntoClosedBodies) {
    // P6d dash = 镶嵌期几何切分:每一「划」一个独立封闭墙带体,空隙不
    // 出几何(FS 不判里程 → 侧视零视差)。
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.lineDashPeriodMeters = 300.0f;
    style.lineDashOnFraction = 0.5f;
    style.fillOutlineEnabled = true;
    layer_->setStyleForContractTest(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    layer_->store().addFeature(makePolygon(0.0, 0.0, 0.01));

    RenderCommandList commands = build();
    const RenderCommand* vol = nullptr;
    const RenderCommand* col = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind != RenderCommandKind::VectorStencil ||
            cmd.vertexStride != 24) {
            continue;
        }
        if (cmd.stencilPhase == StencilPhase::ClassifyVolume) vol = &cmd;
        if (cmd.stencilPhase == StencilPhase::ClassifyColor) col = &cmd;
    }
    ASSERT_NE(nullptr, vol);
    ASSERT_NE(nullptr, col);
    // FS 不再判里程:dash uniform 与顶点里程分量一并退役。
    EXPECT_TRUE(vol->uniforms.empty());
    // 体/色 pass 共用同一份几何(前置 ribbon 的双索引已退役)。
    EXPECT_EQ(vol->indexBuffer, col->indexBuffer);
    EXPECT_EQ(vol->indexCount, col->indexCount);

    // 环长 ≈ 4.45km,周期吸附成整数节 → ~15 划,每划独立封闭体
    // (2 端 cap + 段墙);全部划体合起来仍逐边成对(每体自封闭)。
    const auto* vb = dynamic_cast<const DummyBuffer*>(vol->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const size_t sections = vb->bytes().size() / (4 * 24);
    EXPECT_GE(sections, 20u);
    expectWatertight(*vol);

    // 划体总数 = 索引里 cap 对数:每体 2 个 cap(各 2 三角)。用三角总数
    // 反推:体 b 有 k_b 段 → 8·k_b + 4 三角。∑k_b = 覆盖段数。
    const int triangles = vol->indexCount / 3;
    const int totalSections = static_cast<int>(sections);
    // triangles = 8·(totalSections - bodies) + 4·bodies → 解出 bodies。
    ASSERT_EQ(0, (8 * totalSections - triangles) % 4);
    const int bodies = (8 * totalSections - triangles) / 4;
    EXPECT_GE(bodies, 10);
    EXPECT_LE(bodies, 20);
}

TEST_F(FeatureRenderLayerTest, StencilLineSolidWhenDashDisabled) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.lineDashPeriodMeters = 0.0f;  // 实线
    layer_->setStyleForContractTest(style);
    layer_->setTerrainSampling(makeFlatSampling(50.0f));
    layer_->store().addFeature(makeLine(0.0, 0.0, 0.05));

    RenderCommandList commands = build();
    const RenderCommand* vol = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorStencil &&
            cmd.stencilPhase == StencilPhase::ClassifyVolume &&
            cmd.vertexStride == 24) {
            vol = &cmd;
        }
    }
    ASSERT_NE(nullptr, vol);
    // 单条连续墙带:8·(sections-1) 墙 + 4 端 cap 三角。
    const auto* vb = dynamic_cast<const DummyBuffer*>(vol->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const size_t sections = vb->bytes().size() / (4 * 24);
    EXPECT_EQ(static_cast<int>(8 * (sections - 1) + 4) * 3, vol->indexCount);
    expectWatertight(*vol);
}

TEST_F(FeatureRenderLayerTest, StencilLineFallsBackToRibbonWithoutSupport) {
    device_.stencilClassificationSupported = false;
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    layer_->setStyleForContractTest(style);
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
    layer_->setStyleForContractTest(style);
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
    EXPECT_TRUE(commands[0].hasVectorUniforms);
    EXPECT_TRUE(commands[0].uniforms.empty());
}

TEST_F(FeatureRenderLayerTest, PropertyColorTableBakesTransitRouteColor) {
    FeatureRenderStyle style = layer_->style();
    style.lineColor = {0.1f, 0.2f, 0.3f, 1.0f};
    style.lineColorProperty = "route_style_key";
    style.lineColorByProperty["20015:7"] =
        {245.0f / 255.0f, 171.0f / 255.0f, 78.0f / 255.0f, 1.0f};
    layer_->setStyleForContractTest(style);

    Feature transit = makeLine(106.4, 29.5, 0.02);
    transit.properties["route_style_key"] = "20015:7";
    const auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {transit});
    ASSERT_GE(mesh.lineVerts.size(), 12u);
    uint32_t packed = 0;
    std::memcpy(&packed, &mesh.lineVerts[11], sizeof(packed));
    EXPECT_EQ(245u, packed & 0xffu);
    EXPECT_EQ(171u, (packed >> 8) & 0xffu);
    EXPECT_EQ(78u, (packed >> 16) & 0xffu);
    EXPECT_EQ(255u, packed >> 24);
}

TEST_F(FeatureRenderLayerTest, ZoomDrivenLineWidthUniform) {
    FeatureRenderStyle style = layer_->style();
    style.lineWidthExpr = StyleExpression::interpolateLinear(
        StyleExpression::zoom(),
        {{0.0, StyleExpression::literal(2.0)},
         {24.0, StyleExpression::literal(26.0)}});
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    // 相机高 ~8.6e6m → zoom = log2(4e7/高) ≈ 2.2 → 宽度 ≈ 2 + 2.2 ≈ 4.2
    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    ASSERT_TRUE(commands[0].hasVectorUniforms);
    const float width = commands[0].vectorUniforms.lineWidthPx;
    EXPECT_GT(width, 2.0f);
    EXPECT_LT(width, 8.0f);
}

TEST_F(FeatureRenderLayerTest,
       StyleGroupSelectsIndependentZoomWidthAndCasingCurves) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 10.0f;
    style.paintOrderExpr = StyleExpression::match(
        "roadClass", {{"minor", StyleExpression::literal(79.0)},
                       {"major", StyleExpression::literal(82.0)}},
        StyleExpression::literal(0.0));
    style.lineStyleGroupExpr = StyleExpression::match(
        "roadClass", {{"minor", StyleExpression::literal(79.0)},
                       {"major", StyleExpression::literal(82.0)}},
        StyleExpression::literal(0.0));
    style.lineWidthExprByStyleGroup[79] = StyleExpression::literal(2.0);
    style.lineWidthExprByStyleGroup[82] = StyleExpression::literal(6.0);
    style.lineCasingWidthExprByStyleGroup[79] =
        StyleExpression::literal(1.0);
    style.lineCasingWidthExprByStyleGroup[82] =
        StyleExpression::literal(3.0);
    style.lineCasingColorByStyleGroup[79] = {0.1f, 0.2f, 0.3f, 0.4f};
    style.lineCasingColorByStyleGroup[82] = {0.5f, 0.6f, 0.7f, 0.8f};
    layer_->setStyleForContractTest(style);

    Feature minor = makeLine(6.0, 29.0, 0.01);
    minor.properties["roadClass"] = "minor";
    Feature major = makeLine(6.02, 29.0, 0.01);
    major.properties["roadClass"] = "major";
    layer_->store().addFeature(std::move(minor));
    layer_->store().addFeature(std::move(major));

    RenderCommandList commands = build();
    ASSERT_EQ(4u, commands.size());
    for (const auto& cmd : commands) {
        if (cmd.vectorPaintOrder == 79) {
            EXPECT_FLOAT_EQ(cmd.vectorPaintSubOrder == 0 ? 3.0f : 2.0f,
                            cmd.vectorUniforms.lineWidthPx);
            if (cmd.vectorPaintSubOrder == 0) {
                EXPECT_EQ(style.lineCasingColorByStyleGroup.at(79),
                          cmd.vectorUniforms.color);
            }
        } else if (cmd.vectorPaintOrder == 82) {
            EXPECT_FLOAT_EQ(cmd.vectorPaintSubOrder == 0 ? 9.0f : 6.0f,
                            cmd.vectorUniforms.lineWidthPx);
            if (cmd.vectorPaintSubOrder == 0) {
                EXPECT_EQ(style.lineCasingColorByStyleGroup.at(82),
                          cmd.vectorUniforms.color);
            }
        } else {
            FAIL() << "unexpected paintOrder " << cmd.vectorPaintOrder;
        }
    }
}

TEST_F(FeatureRenderLayerTest,
       StyleGroupCasingCurveDoesNotRequireGlobalFallbackWidth) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 0.0f;
    style.paintOrderExpr = StyleExpression::literal(82.0);
    style.lineStyleGroupExpr = StyleExpression::literal(82.0);
    style.lineWidthExprByStyleGroup[82] = StyleExpression::literal(4.0);
    style.lineCasingWidthExprByStyleGroup[82] =
        StyleExpression::literal(2.0);
    layer_->setStyleForContractTest(style);
    Feature road = makeLine(6.0, 29.0, 0.01);
    road.properties = {{"amap_draworder", "1"},
                       {"amap_minzoom", "3"},
                       {"amap_maxzoom", "20"}};
    layer_->store().addFeature(std::move(road));

    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    EXPECT_EQ(0, commands[0].vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(6.0f, commands[0].vectorUniforms.lineWidthPx);
    EXPECT_EQ(1, commands[1].vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(4.0f, commands[1].vectorUniforms.lineWidthPx);
}

TEST_F(FeatureRenderLayerTest,
       StyleGroupWidthsOverrideSharedPaintOrderWithoutChangingSortOrder) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingStyleGroups = {2000101, 2000105};
    style.paintOrderExpr = StyleExpression::literal(82.0);
    style.lineStyleGroupExpr = StyleExpression::match(
        "amap_subkey", {{"1", StyleExpression::literal(2000101.0)},
                         {"5", StyleExpression::literal(2000105.0)}},
        StyleExpression::literal(82.0));
    style.lineWidthExprByStyleGroup[82] = StyleExpression::literal(99.0);
    style.lineWidthExprByStyleGroup[2000101] =
        StyleExpression::literal(8.0);
    style.lineWidthExprByStyleGroup[2000105] =
        StyleExpression::literal(2.0);
    style.lineCasingWidthExprByStyleGroup[2000101] =
        StyleExpression::literal(2.0);
    style.lineCasingWidthExprByStyleGroup[2000105] =
        StyleExpression::literal(1.0);
    style.lineSolidCapExprByStyleGroup[2000101] =
        StyleExpression::literal(2.0);
    style.lineCasingSolidCapExprByStyleGroup[2000101] =
        StyleExpression::literal(2.0);
    layer_->setStyleForContractTest(style);

    Feature major = makeLine(6.0, 29.0, 0.01);
    major.properties["amap_subkey"] = "1";
    Feature narrow = makeLine(6.02, 29.0, 0.01);
    narrow.properties["amap_subkey"] = "5";
    layer_->store().addFeature(std::move(major));
    layer_->store().addFeature(std::move(narrow));

    const RenderCommandList commands = build();
    ASSERT_EQ(4u, commands.size());
    std::multiset<float> widths;
    for (const auto& command : commands) {
        EXPECT_EQ(82, command.vectorPaintOrder);
        widths.insert(command.vectorUniforms.lineWidthPx);
        if (command.vectorUniforms.lineWidthPx == 8.0f ||
            command.vectorUniforms.lineWidthPx == 10.0f) {
            EXPECT_FLOAT_EQ(2.0f,
                            command.vectorUniforms.solidCapStyle);
        }
    }
    EXPECT_EQ((std::multiset<float>{2.0f, 3.0f, 8.0f, 10.0f}), widths);
}

TEST_F(FeatureRenderLayerTest, StyleGroupRoadWidthsScaleWithOptInDpr) {
    FeatureRenderStyle style = layer_->style();
    style.scaleStylePixelsByDevicePixelRatio = true;
    style.paintOrderExpr = StyleExpression::literal(82.0);
    style.lineStyleGroupExpr = StyleExpression::literal(2000101.0);
    style.lineWidthExprByStyleGroup[2000101] =
        StyleExpression::literal(8.0);
    style.lineTypeExprByStyleGroup[2000101] =
        StyleExpression::literal(3.0);
    style.lineTypeResolver = [](int lineType)
        -> std::optional<FeatureRenderStyle::LineDashPattern> {
        if (lineType != 3) return std::nullopt;
        return FeatureRenderStyle::LineDashPattern{
            {12.0f, 12.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Butt};
    };
    layer_->setStyleForContractTest(style);
    Feature road = makeLine(6.0, 29.0, 0.01);
    road.properties = {{"amap_draworder", "1"},
                       {"amap_minzoom", "3"},
                       {"amap_maxzoom", "20"}};
    layer_->store().addFeature(std::move(road));
    frame_.devicePixelRatio = 2.625f;

    const RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_FLOAT_EQ(21.0f, commands.front().vectorUniforms.lineWidthPx);
    EXPECT_FLOAT_EQ(2.0f,
                    commands.front().vectorUniforms.dashPatternCount);
    EXPECT_EQ((std::array<float, 4>{31.5f, 31.5f, 0.0f, 0.0f}),
              commands.front().vectorUniforms.dashPattern);
}

TEST_F(FeatureRenderLayerTest,
       OfficialRoadUsesOfficialDefaultSolidForUnknownLineType) {
    FeatureRenderStyle style = layer_->style();
    constexpr int kOfficialRoad = 20001001;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    style.lineTypeExprByStyleGroup[kOfficialRoad] =
        StyleExpression::literal(99.0);
    style.lineCasingTypeExprByStyleGroup[kOfficialRoad] =
        StyleExpression::literal(99.0);
    layer_->setStyleForContractTest(style);
    Feature road = makeLine(6.0, 29.0, 0.01);
    road.properties = {{"amap_draworder", "1"},
                       {"amap_minzoom", "3"},
                       {"amap_maxzoom", "20"},
                       {"amap_class", "20001"},
                       {"amap_subkey", "1"}};
    layer_->store().addFeature(std::move(road));
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.0)),
                   surface, Vec3(0.0, 0.0, 1.0));

    const auto commands = build();
    ASSERT_EQ(2u, commands.size());
    for (const auto& command : commands) {
        EXPECT_FLOAT_EQ(0.0f, command.vectorUniforms.dashPatternCount);
        EXPECT_FLOAT_EQ(0.0f, command.vectorUniforms.dashPeriodMeters);
    }
}

TEST_F(FeatureRenderLayerTest,
       AmapOfficialScaleIsBinaryAndIgnoresGenericDashContracts) {
    FeatureRenderStyle style = layer_->style();
    constexpr int kOfficialRoad = 20001001;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    style.lineTypeExprByStyleGroup[kOfficialRoad] = StyleExpression::literal(0.0);
    style.lineCasingTypeExprByStyleGroup[kOfficialRoad] =
        StyleExpression::literal(0.0);
    style.lineDashByStyleGroup[kOfficialRoad] =
        FeatureRenderStyle::LineDashPattern{
            {3.0f, 7.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Butt};
    style.lineCasingDashByStyleGroup[kOfficialRoad] =
        FeatureRenderStyle::LineDashPattern{
            {5.0f, 9.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Butt};
    style.lineDashPeriodMeters = 120.0f;
    style.lineCasingMinZoom = 20.0;
    style.lineCasingMaxZoom = 20.0;
    layer_->setStyleForContractTest(style);
    Feature road = makeLine(6.0, 29.0, 0.01);
    road.properties = {{"amap_draworder", "1"},
                       {"amap_minzoom", "3"},
                       {"amap_maxzoom", "20"},
                       {"amap_class", "20001"},
                       {"amap_subkey", "1"}};
    layer_->store().addFeature(std::move(road));
    frame_.devicePixelRatio = 2.625f;
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.0)),
                   surface, Vec3(0.0, 0.0, 1.0));

    const auto commands = build();
    ASSERT_EQ(2u, commands.size());
    std::multiset<float> widths;
    for (const auto& command : commands) {
        widths.insert(command.vectorUniforms.lineWidthPx);
        EXPECT_FLOAT_EQ(0.0f, command.vectorUniforms.dashPatternCount);
        EXPECT_FLOAT_EQ(0.0f, command.vectorUniforms.dashPeriodMeters);
    }
    EXPECT_EQ((std::multiset<float>{16.0f, 20.0f}), widths);
}

TEST_F(FeatureRenderLayerTest,
       AmapOfficialDashUsesBinaryRetinaScaleExactlyOnce) {
    layer_->setStyleForContractTest(
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main));
    Feature road = makeLine(6.0, 29.0, 0.01);
    addOfficialMetadata(road, "20010", "1", "90");
    layer_->store().addFeature(std::move(road));

    frame_.devicePixelRatio = 2.625f;
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.2)),
                   surface, Vec3(0.0, 0.0, 1.0));

    const auto commands = build();
    ASSERT_EQ(2u, commands.size());
    const auto center = std::find_if(
        commands.begin(), commands.end(), [](const auto& command) {
            return command.vectorPaintSubOrder == 1;
        });
    ASSERT_NE(commands.end(), center);
    EXPECT_FLOAT_EQ(2.0f, center->vectorUniforms.dashPatternCount);
    EXPECT_EQ((std::array<float, 4>{24.0f, 24.0f, 0.0f, 0.0f}),
              center->vectorUniforms.dashPattern);
    EXPECT_FLOAT_EQ(4.0f, center->vectorUniforms.lineWidthPx);
}

TEST_F(FeatureRenderLayerTest,
       OfficialCenterAndCasingLineTypesReachIndependentFinalCommands) {
    layer_->setStyleForContractTest(
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main));
    Feature road = makeLine(6.0, 29.0, 0.01);
    addOfficialMetadata(road, "20002", "3", "90");
    layer_->store().addFeature(std::move(road));

    frame_.devicePixelRatio = 2.625f;
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.2)),
                   surface, Vec3(0.0, 0.0, 1.0));

    const RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    const auto casing = std::find_if(
        commands.begin(), commands.end(), [](const auto& command) {
            return command.vectorPaintSubOrder == 0;
        });
    const auto center = std::find_if(
        commands.begin(), commands.end(), [](const auto& command) {
            return command.vectorPaintSubOrder == 1;
        });
    ASSERT_NE(commands.end(), casing);
    ASSERT_NE(commands.end(), center);

    // Official provider zoom 14: center lineType 14 is solid/round, while
    // casingLineType 4 is an independent 2/2 CSS-pixel butt dash. The
    // official retina branch is binary even when device DPR is 2.625.
    EXPECT_FLOAT_EQ(0.0f, center->vectorUniforms.dashPatternCount);
    EXPECT_FLOAT_EQ(2.0f, center->vectorUniforms.solidCapStyle);
    EXPECT_FLOAT_EQ(12.0f, center->vectorUniforms.lineWidthPx);
    EXPECT_FLOAT_EQ(2.0f, casing->vectorUniforms.dashPatternCount);
    EXPECT_EQ((std::array<float, 4>{4.0f, 4.0f, 0.0f, 0.0f}),
              casing->vectorUniforms.dashPattern);
    EXPECT_FLOAT_EQ(0.0f, casing->vectorUniforms.dashCapStyle);
    EXPECT_FLOAT_EQ(0.0f, casing->vectorUniforms.solidCapStyle)
        << "center round-cap state must not leak into the dashed casing";
    EXPECT_FLOAT_EQ(16.0f, casing->vectorUniforms.lineWidthPx);
    EXPECT_EQ(center->vertexBuffer, casing->vertexBuffer);
    EXPECT_EQ(center->indexBuffer, casing->indexBuffer);
    EXPECT_GE(center->indexCount, 12)
        << "official lineType 14 must retain endpoint candidate geometry";
}

TEST_F(FeatureRenderLayerTest,
       AmapOfficialClampedRoundCapReachesFinalCommand) {
    layer_->setStyleForContractTest(
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main));
    Feature road = makeLine(6.0, 29.0, 0.01);
    addOfficialMetadata(road, "20001", "1", "90");
    layer_->store().addFeature(std::move(road));

    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.2)),
                   surface, Vec3(0.0, 0.0, 1.0));

    const auto commands = build();
    ASSERT_FALSE(commands.empty());
    for (const auto& command : commands) {
        EXPECT_FLOAT_EQ(2.0f, command.vectorUniforms.solidCapStyle);
        EXPECT_GE(command.indexCount, 12);
    }
}

TEST_F(FeatureRenderLayerTest,
       AmapOfficialCasingIgnoresGenericCasingZoomWindow) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    style.lineCasingMinZoom = 24.0;
    style.lineCasingMaxZoom = 24.0;
    layer_->setStyleForContractTest(style);

    Feature road = makeLine(6.0, 29.0, 0.01);
    road.properties = {{"amap_draworder", "1"},
                       {"amap_minzoom", "3"},
                       {"amap_maxzoom", "20"},
                       {"amap_class", "20001"},
                       {"amap_subkey", "1"}};
    layer_->store().addFeature(std::move(road));
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    camera_.lookAt(surface + surface.normalized() *
                       (4.0e7 / std::pow(2.0, 13.0)),
                   surface, Vec3(0.0, 0.0, 1.0));

    const auto commands = build();
    ASSERT_EQ(2u, commands.size());
    EXPECT_TRUE(std::any_of(commands.begin(), commands.end(), [](const auto& c) {
        return c.vectorPaintSubOrder == 0;
    }));
}

TEST_F(FeatureRenderLayerTest,
       AmapPhysicalViewportDprSignatureScalesExactlyOnce) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style.lineStyleGroupExpr = amapClassicLineStyleGroupExpression();
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    layer_->setStyleForContractTest(style);

    Feature road = makeLine(6.0, 29.0, 0.01);
    road.properties["amap_class"] = "20001";
    road.properties["amap_subkey"] = "1";
    road.properties["amap_draworder"] = "9137";
    road.properties["amap_minzoom"] = "3";
    road.properties["amap_maxzoom"] = "20";
    layer_->store().addFeature(std::move(road));

    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg, 0.0));
    const double height = 4.0e7 / std::pow(2.0, 13.0);
    camera_.lookAt(surface + surface.normalized() * height, surface,
                   Vec3(0.0, 0.0, 1.0));
    // Official browser fixture: CSS viewport 1280x720, retina backing scale
    // 2, hence a 2560x1440 drawing buffer. FrameState viewport is always the
    // physical drawing-buffer size; DPR remains the CSS->physical conversion.
    frame_.viewportWidthPixels = 2560;
    frame_.viewportHeightPixels = 1440;
    frame_.devicePixelRatio = 2.0f;

    const RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    EXPECT_FLOAT_EQ(2560.0f, commands[0].vectorUniforms.viewport[0]);
    EXPECT_FLOAT_EQ(1440.0f, commands[0].vectorUniforms.viewport[1]);
    std::multiset<float> widths;
    for (const auto& command : commands) {
        widths.insert(command.vectorUniforms.lineWidthPx);
        // Shader raster width = uniform width * actual viewport height /
        // viewport-uniform height. Production keeps both physical heights
        // equal, so the shader cannot apply a second DPR factor.
        EXPECT_FLOAT_EQ(command.vectorUniforms.lineWidthPx,
                        command.vectorUniforms.lineWidthPx * 1440.0f /
                            command.vectorUniforms.viewport[1]);
    }
    EXPECT_EQ((std::multiset<float>{16.0f, 20.0f}), widths);
}

TEST_F(FeatureRenderLayerTest,
       StyleGroupEvaluatesCenterAndCasingColorStepsIndependently) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingStyleGroups.insert(82);
    style.paintOrderExpr = StyleExpression::literal(82.0);
    style.lineStyleGroupExpr = StyleExpression::literal(82.0);
    style.lineWidthExprByStyleGroup[82] = StyleExpression::literal(4.0);
    style.lineCasingWidthExprByStyleGroup[82] =
        StyleExpression::literal(2.0);
    style.lineColorExprByStyleGroup[82] = StyleExpression::step(
        StyleExpression::zoom(),
        {{0.0, StyleExpression::literal({1.0f, 0.0f, 0.0f, 1.0f})},
         {2.0, StyleExpression::literal({0.0f, 1.0f, 0.0f, 1.0f})},
         {3.0, StyleExpression::literal({0.0f, 0.0f, 1.0f, 1.0f})}});
    style.lineCasingColorExprByStyleGroup[82] = StyleExpression::step(
        StyleExpression::zoom(),
        {{0.0, StyleExpression::literal({0.1f, 0.1f, 0.1f, 1.0f})},
         {2.0, StyleExpression::literal({0.2f, 0.3f, 0.4f, 1.0f})},
         {3.0, StyleExpression::literal({0.8f, 0.8f, 0.8f, 1.0f})}});
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    // Fixture zoom is approximately 2.2: both expressions must select the
    // z2 tier without interpolation, while paint order remains unchanged.
    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    EXPECT_EQ(82, commands[0].vectorPaintOrder);
    EXPECT_EQ(0, commands[0].vectorPaintSubOrder);
    EXPECT_EQ((std::array<float, 4>{0.2f, 0.3f, 0.4f, 1.0f}),
              commands[0].vectorUniforms.color);
    EXPECT_EQ(82, commands[1].vectorPaintOrder);
    EXPECT_EQ(1, commands[1].vectorPaintSubOrder);
    EXPECT_EQ((std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f}),
              commands[1].vectorUniforms.color);
}

TEST_F(FeatureRenderLayerTest,
       SolidRoundCapAddsOnlyCandidateEndpointGeometry) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(82.0);
    style.lineStyleGroupExpr = StyleExpression::match(
        "cap", {{"round", StyleExpression::literal(2000101.0)}},
        StyleExpression::literal(82.0));
    style.lineSolidCapExprByStyleGroup[2000101] =
        StyleExpression::literal(2.0);
    layer_->setStyleForContractTest(style);

    Feature round = makeLine(6.0, 29.0, 0.01);
    round.properties["cap"] = "round";
    Feature butt = makeLine(6.02, 29.0, 0.01);
    butt.properties["cap"] = "butt";
    layer_->store().addFeature(std::move(round));
    layer_->store().addFeature(std::move(butt));

    const RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    int roundIndices = 0;
    int buttIndices = 0;
    for (const auto& command : commands) {
        EXPECT_EQ(82, command.vectorPaintOrder);
        if (command.vectorUniforms.solidCapStyle > 1.5f) {
            roundIndices = command.indexCount;
        } else {
            buttIndices = command.indexCount;
        }
    }
    EXPECT_EQ(24, roundIndices);  // 2 segments + 2 endpoint quads.
    EXPECT_EQ(12, buttIndices);   // no unconditional endpoint geometry.
}

TEST_F(FeatureRenderLayerTest,
       OfficialClampToGroundRoadKeepsLineTypeEndpointCandidates) {
    layer_->installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);
    Feature road = makeLine(6.0, 29.0, 0.01);
    addOfficialMetadata(road, "20001", "1", "90");

    auto officialContext = layer_->workerTessellationContext();
    const FeatureTileMesh mesh = FeatureRenderLayer::tessellateTileMesh(
        officialContext, {road});
    ASSERT_EQ(1u, mesh.lineRanges.size());
    EXPECT_EQ(amapClassicStyleIdentity(20001, 1),
              mesh.lineRanges.front().styleGroup);

    officialContext.style.lineSolidCapExprByStyleGroup.clear();
    officialContext.style.lineCasingSolidCapExprByStyleGroup.clear();
    const FeatureTileMesh withoutCandidates =
        FeatureRenderLayer::tessellateTileMesh(officialContext, {road});
    ASSERT_EQ(1u, withoutCandidates.lineRanges.size());
    EXPECT_EQ(withoutCandidates.lineRanges.front().indexCount + 12,
              mesh.lineRanges.front().indexCount)
        << "two endpoint candidate quads are independent of clamp densify";
}

TEST_F(FeatureRenderLayerTest,
       StyleGroupPixelDashIsIndependentFromWorldMeterDash) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(75.0);
    style.lineStyleGroupExpr = StyleExpression::literal(75.0);
    style.lineWidthExprByStyleGroup[75] = StyleExpression::literal(3.0);
    style.lineDashPeriodMeters = 120.0f;
    style.lineDashOnFraction = 0.25f;
    style.lineDashByStyleGroup[75] =
        FeatureRenderStyle::LineDashPattern{
            {2.0f, 2.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Butt};
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const auto& u = commands[0].vectorUniforms;
    EXPECT_FLOAT_EQ(3.0f, u.lineWidthPx);
    EXPECT_FLOAT_EQ(0.0f, u.dashPeriodMeters);
    EXPECT_FLOAT_EQ(2.0f, u.dashPatternCount);
    EXPECT_EQ((std::array<float, 4>{2.0f, 2.0f, 0.0f, 0.0f}),
              u.dashPattern);
    EXPECT_FLOAT_EQ(0.0f, u.dashCapStyle);
    // Reference contract: cumulative ground meters are converted with one
    // command-level Web-Mercator pixels-per-meter scale.  It must be finite
    // and positive, and does not depend on individual endpoint eye depth.
    EXPECT_TRUE(std::isfinite(u.dashPixelsPerMeter));
    EXPECT_GT(u.dashPixelsPerMeter, 0.0f);
}

TEST_F(FeatureRenderLayerTest,
       CenterAndCasingSelectIndependentPixelDashPatterns) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(82.0);
    style.lineStyleGroupExpr = StyleExpression::literal(82.0);
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 2.0f;
    style.lineDashByStyleGroup[82] =
        FeatureRenderStyle::LineDashPattern{
            {6.0f, 3.0f, 2.0f, 3.0f}, 4,
            FeatureRenderStyle::LineCap::Round};
    style.lineCasingDashByStyleGroup[82] =
        FeatureRenderStyle::LineDashPattern{
            {12.0f, 12.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Square};
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    const auto& casing = commands[0].vectorUniforms;
    const auto& center = commands[1].vectorUniforms;
    EXPECT_FLOAT_EQ(2.0f, casing.dashPatternCount);
    EXPECT_FLOAT_EQ(12.0f, casing.dashPattern[0]);
    EXPECT_FLOAT_EQ(1.0f, casing.dashCapStyle);
    EXPECT_FLOAT_EQ(4.0f, center.dashPatternCount);
    EXPECT_FLOAT_EQ(2.0f, center.dashPattern[2]);
    EXPECT_FLOAT_EQ(2.0f, center.dashCapStyle);
    EXPECT_EQ(commands[0].vertexBuffer, commands[1].vertexBuffer);
    EXPECT_EQ(commands[0].indexBuffer, commands[1].indexBuffer);
}

TEST_F(FeatureRenderLayerTest,
       StyleGroupLineTypeCurvesOverrideStaticDashIndependently) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(82.0);
    style.lineStyleGroupExpr = StyleExpression::literal(2000203.0);
    style.lineCasingEnabled = true;
    style.lineCasingStyleGroups.insert(2000203);
    style.lineCasingExtraWidthPx = 2.0f;
    style.lineTypeResolver = [](int lineType)
        -> std::optional<FeatureRenderStyle::LineDashPattern> {
        using P = FeatureRenderStyle::LineDashPattern;
        if (lineType == 14) return P{};
        if (lineType == 4) {
            return P{{2.0f, 2.0f, 0.0f, 0.0f}, 2,
                     FeatureRenderStyle::LineCap::Butt};
        }
        return std::nullopt;
    };
    style.lineTypeExprByStyleGroup[2000203] =
        StyleExpression::literal(14.0);
    style.lineCasingTypeExprByStyleGroup[2000203] =
        StyleExpression::literal(4.0);
    // Contradictory fallback patterns prove the dynamic full-identity curves
    // own both commands when they resolve successfully.
    style.lineDashByStyleGroup[82] =
        FeatureRenderStyle::LineDashPattern{
            {6.0f, 6.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Round};
    style.lineCasingDashByStyleGroup[82] =
        FeatureRenderStyle::LineDashPattern{
            {12.0f, 12.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Square};
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    const RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    const auto& casing = commands[0].vectorUniforms;
    const auto& center = commands[1].vectorUniforms;
    EXPECT_FLOAT_EQ(2.0f, casing.dashPatternCount);
    EXPECT_EQ((std::array<float, 4>{2.0f, 2.0f, 0.0f, 0.0f}),
              casing.dashPattern);
    EXPECT_FLOAT_EQ(0.0f, casing.dashCapStyle);
    EXPECT_FLOAT_EQ(0.0f, center.dashPatternCount);
    EXPECT_EQ((std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
              center.dashPattern);
}

TEST_F(FeatureRenderLayerTest, LineCasingReusesGeometryAndPrecedesCenter) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 3.0f;
    style.lineCasingWidthRatio = 0.5f;
    style.lineCasingColor = {0.9f, 0.8f, 0.7f, 0.95f};
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    const RenderCommand& casing = commands[0];
    const RenderCommand& center = commands[1];
    ASSERT_EQ(RenderCommandKind::VectorLine, casing.kind);
    ASSERT_EQ(RenderCommandKind::VectorLine, center.kind);
    EXPECT_EQ(casing.vertexBuffer, center.vertexBuffer);
    EXPECT_EQ(casing.indexBuffer, center.indexBuffer);
    EXPECT_EQ(casing.indexOffset, center.indexOffset);
    EXPECT_EQ(casing.indexCount, center.indexCount);
    EXPECT_EQ(0, casing.vectorPaintSubOrder);
    EXPECT_EQ(1, center.vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(center.vectorUniforms.lineWidthPx * 1.5f,
                    casing.vectorUniforms.lineWidthPx);
    EXPECT_EQ(style.lineCasingColor, casing.vectorUniforms.color);
    EXPECT_FLOAT_EQ(0.0f, center.vectorUniforms.color[3]);
}

TEST_F(FeatureRenderLayerTest, LineCasingUsesOnlySemanticStyleGroupAllowList) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 3.0f;
    style.lineCasingStyleGroups.insert(7501);
    style.paintOrderExpr = StyleExpression::match(
        "kind", {{"road", StyleExpression::literal(75.0)},
                  {"boundary", StyleExpression::literal(65.0)}},
        StyleExpression::literal(0.0));
    style.lineStyleGroupExpr = StyleExpression::match(
        "kind", {{"road", StyleExpression::literal(7501.0)},
                  {"boundary", StyleExpression::literal(6501.0)}},
        StyleExpression::literal(0.0));
    layer_->setStyleForContractTest(style);

    Feature road = makeLine(6.0, 29.0, 0.01);
    road.properties["kind"] = "road";
    Feature boundary = makeLine(6.02, 29.0, 0.01);
    boundary.properties["kind"] = "boundary";
    layer_->store().addFeature(std::move(road));
    layer_->store().addFeature(std::move(boundary));

    RenderCommandList commands = build();
    int roadCommands = 0;
    int boundaryCommands = 0;
    for (const auto& cmd : commands) {
        if (cmd.kind != RenderCommandKind::VectorLine) continue;
        if (cmd.vectorPaintOrder == 75) ++roadCommands;
        if (cmd.vectorPaintOrder == 65) ++boundaryCommands;
    }
    EXPECT_EQ(2, roadCommands);
    EXPECT_EQ(1, boundaryCommands);
}

TEST_F(FeatureRenderLayerTest, ExactCasingStyleGroupAllowListIsolated) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 1.0f;
    style.lineCasingStyleGroups.insert(6201);
    style.paintOrderExpr = StyleExpression::match(
        "kind", {{"rail", StyleExpression::literal(62.0)},
                  {"boundary", StyleExpression::literal(65.0)}},
        StyleExpression::literal(0.0));
    style.lineStyleGroupExpr = StyleExpression::match(
        "kind", {{"rail", StyleExpression::literal(6201.0)},
                  {"boundary", StyleExpression::literal(6501.0)}},
        StyleExpression::literal(0.0));
    layer_->setStyleForContractTest(style);
    Feature rail = makeLine(6.0, 29.0, 0.01);
    rail.properties["kind"] = "rail";
    Feature boundary = makeLine(6.02, 29.0, 0.01);
    boundary.properties["kind"] = "boundary";
    layer_->store().addFeature(std::move(rail));
    layer_->store().addFeature(std::move(boundary));
    RenderCommandList commands = build();
    int railCommands = 0, boundaryCommands = 0;
    for (const auto& cmd : commands) {
        if (cmd.kind != RenderCommandKind::VectorLine) continue;
        if (cmd.vectorPaintOrder == 62) ++railCommands;
        if (cmd.vectorPaintOrder == 65) ++boundaryCommands;
    }
    EXPECT_EQ(2, railCommands);
    EXPECT_EQ(1, boundaryCommands);
}

TEST_F(FeatureRenderLayerTest, RailwayStyleGroupCarriesOfficialDashAndCasing) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingStyleGroups.insert(62);
    style.paintOrderExpr = StyleExpression::literal(62.0);
    style.lineStyleGroupExpr = StyleExpression::literal(62.0);
    style.lineWidthExprByStyleGroup[62] = StyleExpression::literal(2.0);
    style.lineCasingWidthExprByStyleGroup[62] =
        StyleExpression::literal(1.0);
    style.lineDashByStyleGroup[62] =
        FeatureRenderStyle::LineDashPattern{
            {12.0f, 12.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Butt};
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));
    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    EXPECT_EQ(0, commands[0].vectorPaintSubOrder);
    EXPECT_EQ(1, commands[1].vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(0.0f, commands[0].vectorUniforms.dashPatternCount);
    EXPECT_FLOAT_EQ(2.0f, commands[1].vectorUniforms.dashPatternCount);
    EXPECT_FLOAT_EQ(12.0f, commands[1].vectorUniforms.dashPattern[0]);
    EXPECT_FLOAT_EQ(3.0f, commands[0].vectorUniforms.lineWidthPx);
    EXPECT_FLOAT_EQ(2.0f, commands[1].vectorUniforms.lineWidthPx);
}

TEST_F(FeatureRenderLayerTest,
       LineStyleGroupSplitsSubkeysWithoutChangingPaintOrder) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(62.0);
    style.lineStyleGroupExpr = StyleExpression::match(
        "sub", {{"1", StyleExpression::literal(6201.0)},
                 {"3", StyleExpression::literal(6203.0)}},
        StyleExpression::literal(6203.0));
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 0.0f;
    style.lineCasingStyleGroups.insert(6201);
    style.lineWidthExprByStyleGroup[6201] = StyleExpression::literal(2.0);
    style.lineWidthExprByStyleGroup[6203] = StyleExpression::literal(1.0);
    style.lineCasingWidthExprByStyleGroup[6201] =
        StyleExpression::literal(1.0);
    style.lineDashByStyleGroup[6201] =
        FeatureRenderStyle::LineDashPattern{
            {12.0f, 12.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Butt};
    layer_->setStyleForContractTest(style);
    Feature rail = makeLine(6.0, 29.0, 0.01);
    rail.properties["sub"] = "1";
    Feature plain = makeLine(6.02, 29.0, 0.01);
    plain.properties["sub"] = "3";
    layer_->store().addFeature(std::move(rail));
    layer_->store().addFeature(std::move(plain));

    RenderCommandList commands = build();
    ASSERT_EQ(3u, commands.size());
    int dashed = 0, solid = 0, casing = 0;
    for (const auto& cmd : commands) {
        ASSERT_EQ(62, cmd.vectorPaintOrder);
        if (cmd.vectorUniforms.lineWidthPx == 3.0f) {
            ++casing;
            EXPECT_EQ(0, cmd.vectorPaintSubOrder);
            EXPECT_FLOAT_EQ(3.0f, cmd.vectorUniforms.lineWidthPx);
        } else if (cmd.vectorUniforms.dashPatternCount > 0.0f) {
            ++dashed;
            EXPECT_EQ(1, cmd.vectorPaintSubOrder);
            EXPECT_FLOAT_EQ(2.0f, cmd.vectorUniforms.lineWidthPx);
        } else {
            ++solid;
            EXPECT_EQ(0, cmd.vectorPaintSubOrder);
            EXPECT_FLOAT_EQ(1.0f, cmd.vectorUniforms.lineWidthPx);
        }
    }
    EXPECT_EQ(1, casing);
    EXPECT_EQ(1, dashed);
    EXPECT_EQ(1, solid);
}

TEST_F(FeatureRenderLayerTest,
       OfficialRoadColorsAndSignedWidthsReachFinalCommandsAcrossZoom) {
    layer_->setStyleForContractTest(
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main));
    Feature road = makeLine(6.0, 29.0, 0.01);
    addOfficialMetadata(road, "20025", "1", "90");
    road.properties["amap_maxzoom"] = "30";
    layer_->store().addFeature(std::move(road));
    frame_.devicePixelRatio = 2.0f;

    const Vec3 target = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(6.0 * kDeg, 29.0 * kDeg));
    const Vec3 radial = target.normalized();
    struct Snapshot {
        RenderCommand casing;
        RenderCommand center;
    };
    const auto atDisplayZoom = [&](double displayZoom) {
        camera_.lookAt(target + radial *
                           (4.0e7 / std::pow(2.0, displayZoom)),
                       target, Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
        const RenderCommandList commands = build();
        EXPECT_EQ(2u, commands.size());
        Snapshot out;
        for (const auto& command : commands) {
            if (command.vectorPaintSubOrder == 0) out.casing = command;
            if (command.vectorPaintSubOrder == 1) out.center = command;
        }
        return out;
    };
    const auto verifyColors = [](const Snapshot& snapshot) {
        // Official road field 3 final center-color consumer.
        EXPECT_EQ((std::array<float, 4>{0xce / 255.0f, 0xc2 / 255.0f,
                                        0xc2 / 255.0f, 1.0f}),
                  snapshot.center.vectorUniforms.color);
        // Official road field 6 final casing-color consumer.
        EXPECT_EQ((std::array<float, 4>{0xe6 / 255.0f, 0x37 / 255.0f,
                                        0x19 / 255.0f, 1.0f}),
                  snapshot.casing.vectorUniforms.color);
    };

    // Provider zoom 18: roadWidth 7 and signed borderWidth -5 produce a
    // 2-CSS-pixel secondary stroke, scaled once by the retina branch.
    const Snapshot z18 = atDisplayZoom(17.0);
    verifyColors(z18);
    // Official road field 5 final center-width consumer.
    EXPECT_FLOAT_EQ(14.0f, z18.center.vectorUniforms.lineWidthPx);
    // Official road field 4 final signed casing-width consumer.
    EXPECT_FLOAT_EQ(4.0f, z18.casing.vectorUniforms.lineWidthPx);

    // Provider zoom 19 narrows only the secondary pass: 7 + (-6) = 1 CSS px.
    const Snapshot z19 = atDisplayZoom(18.0);
    verifyColors(z19);
    EXPECT_FLOAT_EQ(14.0f, z19.center.vectorUniforms.lineWidthPx);
    EXPECT_FLOAT_EQ(2.0f, z19.casing.vectorUniforms.lineWidthPx);

    // Provider zoom 20 changes both official widths: 9 + (-7) = 2 CSS px.
    const Snapshot z20 = atDisplayZoom(19.0);
    verifyColors(z20);
    EXPECT_FLOAT_EQ(18.0f, z20.center.vectorUniforms.lineWidthPx);
    EXPECT_FLOAT_EQ(4.0f, z20.casing.vectorUniforms.lineWidthPx);
}

TEST_F(FeatureRenderLayerTest,
       EveryOfficialRoadIdentityReachesMatchingFinalCenterAndCasingCommands) {
    const auto identities = amapClassicRoadIdentitiesForTest();
    ASSERT_EQ(429u, identities.size());
    std::vector<Feature> roads;
    std::map<int, AmapClassicRoadIdentityForTest> byPaintOrder;
    for (size_t i = 0; i < identities.size(); ++i) {
        const int paintOrder = 3000 + static_cast<int>(i);
        byPaintOrder.emplace(paintOrder, identities[i]);
        const double x = (i % 24) * 0.00015;
        const double y = (i / 24) * 0.00015;
        Feature road = makeLine(x, y, 0.00008);
        road.properties = {
            {"amap_class", std::to_string(identities[i].classCode)},
            {"amap_subkey", std::to_string(identities[i].subKey)},
            {"amap_draworder", std::to_string(paintOrder)},
            {"amap_minzoom", "1"}, {"amap_maxzoom", "30"}};
        roads.push_back(std::move(road));
    }
    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    layer_->setStyleForContractTest(style);
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), roads);
    ASSERT_EQ(identities.size(), mesh.lineRanges.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                  std::move(mesh)));

    const auto numberAt = [](const auto& table, int group, double zoom) {
        const auto it = table.find(group);
        if (it == table.end() || !it->second) return std::optional<double>{};
        const auto value = it->second->evaluate(nullptr, zoom);
        if (!value || value->kind() != StyleValue::Kind::Number)
            return std::optional<double>{};
        return std::optional<double>{value->number()};
    };
    const auto colorAt = [](const auto& table, int group, double zoom) {
        const auto it = table.find(group);
        if (it == table.end() || !it->second)
            return std::optional<std::array<float, 4>>{};
        const auto value = it->second->evaluate(nullptr, zoom);
        if (!value || value->kind() != StyleValue::Kind::Color)
            return std::optional<std::array<float, 4>>{};
        return std::optional<std::array<float, 4>>{value->color()};
    };
    struct Pair {
        const RenderCommand* subOrderZero = nullptr;
        const RenderCommand* center = nullptr;
    };
    const double radius = Ellipsoid::WGS84().radii().x();
    frame_.devicePixelRatio = 1.0f;
    for (int displayZoom = 1; displayZoom <= 24; ++displayZoom) {
        const double height = 4.0e7 / std::pow(2.0, displayZoom);
        camera_.lookAt(Vec3(radius + height, 0.0, 0.0),
                       Vec3(radius, 0.0, 0.0), Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
        const auto commands = build();
        std::map<int, Pair> actual;
        for (const auto& command : commands) {
            if (command.kind != RenderCommandKind::VectorLine) continue;
            auto& pair = actual[command.vectorPaintOrder];
            if (command.vectorPaintSubOrder == 0)
                pair.subOrderZero = &command;
            if (command.vectorPaintSubOrder == 1) pair.center = &command;
        }
        for (const auto& [paintOrder, identity] : byPaintOrder) {
            const int group = amapClassicStyleIdentity(
                identity.classCode, identity.subKey);
            const bool inWindow = identity.minZoom <= displayZoom + 1 &&
                                  displayZoom + 1 <= identity.maxZoom;
            const auto centerWidth = numberAt(
                style.lineWidthExprByStyleGroup, group, displayZoom);
            const auto centerColor = colorAt(
                style.lineColorExprByStyleGroup, group, displayZoom);
            const auto centerType = numberAt(
                style.lineTypeExprByStyleGroup, group, displayZoom);
            const bool centerVisible = inWindow && centerWidth && centerColor &&
                centerType && *centerWidth > 0.0 && (*centerColor)[3] > 0.0f;
            const auto casingWidth = numberAt(
                style.lineCasingWidthExprByStyleGroup, group, displayZoom);
            const auto casingColor = colorAt(
                style.lineCasingColorExprByStyleGroup, group, displayZoom);
            const auto casingType = numberAt(
                style.lineCasingTypeExprByStyleGroup, group, displayZoom);
            const bool casingVisible = inWindow && casingWidth && casingColor &&
                casingType && centerWidth &&
                *centerWidth + *casingWidth > 0.0 &&
                (*casingColor)[3] > 0.0f &&
                style.lineCasingStyleGroups.count(group) != 0;
            const auto found = actual.find(paintOrder);
            const Pair empty{};
            const Pair& pair = found == actual.end() ? empty : found->second;
            // Suborder zero is intentionally overloaded by the renderer: it
            // is the casing when both strokes exist, but remains the center
            // when an identity has no visible casing.  Resolve that ABI from
            // the independently evaluated official visibility contract.
            const RenderCommand* centerCommand =
                pair.center ? pair.center
                            : (!casingVisible ? pair.subOrderZero : nullptr);
            const RenderCommand* casingCommand =
                casingVisible ? pair.subOrderZero : nullptr;
            EXPECT_EQ(centerVisible, centerCommand != nullptr)
                << identity.classCode << ':' << identity.subKey
                << " center visibility mismatch at display zoom " << displayZoom;
            EXPECT_EQ(casingVisible, casingCommand != nullptr)
                << identity.classCode << ':' << identity.subKey
                << " casing visibility mismatch at display zoom " << displayZoom;
            const auto verify = [&](const RenderCommand* command,
                                    double width,
                                    const std::array<float, 4>& color,
                                    int lineType) {
                ASSERT_NE(nullptr, command);
                EXPECT_FLOAT_EQ(static_cast<float>(width),
                                command->vectorUniforms.lineWidthPx);
                EXPECT_EQ(color, command->vectorUniforms.color);
                const auto dash = amapClassicRoadDashForLineType(lineType);
                ASSERT_TRUE(dash.has_value());
                EXPECT_FLOAT_EQ(static_cast<float>(dash->count),
                                command->vectorUniforms.dashPatternCount);
                EXPECT_EQ(dash->lengths,
                          command->vectorUniforms.dashPattern);
                EXPECT_FLOAT_EQ(static_cast<float>(dash->cap),
                                dash->count == 0
                                    ? command->vectorUniforms.solidCapStyle
                                    : command->vectorUniforms.dashCapStyle);
            };
            if (centerVisible) verify(centerCommand, *centerWidth, *centerColor,
                                      static_cast<int>(std::lround(*centerType)));
            if (casingVisible) verify(casingCommand,
                                      *centerWidth + *casingWidth,
                                      *casingColor,
                                      static_cast<int>(std::lround(*casingType)));
        }
    }
}

TEST_F(FeatureRenderLayerTest, LineStyleGroupHasIndependentZoomWindow) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(65.0);
    style.lineStyleGroupExpr = StyleExpression::match(
        "sub", {{"short", StyleExpression::literal(6501.0)},
                 {"long", StyleExpression::literal(6502.0)}},
        StyleExpression::literal(6502.0));
    // Fixture is roughly display zoom 2.2: short is already sunset, long is on.
    style.lineMaxZoomByStyleGroup[6501] = 2.0;
    style.lineMinZoomByStyleGroup[6502] = 2.0;
    layer_->setStyleForContractTest(style);
    Feature shortLine = makeLine(6.0, 29.0, 0.01);
    shortLine.properties["sub"] = "short";
    Feature longLine = makeLine(6.02, 29.0, 0.01);
    longLine.properties["sub"] = "long";
    layer_->store().addFeature(std::move(shortLine));
    layer_->store().addFeature(std::move(longLine));
    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(65, commands[0].vectorPaintOrder);
}

TEST_F(FeatureRenderLayerTest,
       LineStyleGroupSelectsCommandColorAndFourPartDash) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(65.0);
    style.lineStyleGroupExpr = StyleExpression::literal(6506.0);
    style.lineWidthExprByStyleGroup[6506] = StyleExpression::literal(1.0);
    style.lineColorExprByStyleGroup[6506] = StyleExpression::literal(
        {0.1f, 0.2f, 0.3f, 0.4f});
    style.lineDashByStyleGroup[6506] =
        FeatureRenderStyle::LineDashPattern{
            {6.0f, 3.0f, 2.0f, 3.0f}, 4,
            FeatureRenderStyle::LineCap::Butt};
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));
    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(65, commands[0].vectorPaintOrder);
    EXPECT_EQ((std::array<float, 4>{0.1f, 0.2f, 0.3f, 0.4f}),
              commands[0].vectorUniforms.color);
    EXPECT_FLOAT_EQ(4.0f,
                    commands[0].vectorUniforms.dashPatternCount);
    EXPECT_EQ((std::array<float, 4>{6.0f, 3.0f, 2.0f, 3.0f}),
              commands[0].vectorUniforms.dashPattern);
}

TEST_F(FeatureRenderLayerTest,
       CasingOnlyStyleGroupKeepsPaintOrderAndOwnsDashAndColor) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(66.0);
    style.lineStyleGroupExpr = StyleExpression::literal(6604.0);
    style.lineCasingEnabled = true;
    style.lineCasingStyleGroups.insert(6604);
    style.lineCasingExtraWidthPx = 0.0f;
    style.lineWidthExprByStyleGroup[6604] = StyleExpression::literal(0.0);
    style.lineColorExprByStyleGroup[6604] =
        StyleExpression::literal({0.0f, 0.0f, 0.0f, 0.0f});
    style.lineCasingWidthExprByStyleGroup[6604] =
        StyleExpression::literal(1.0);
    style.lineCasingColorByStyleGroup[6604] = {1.0f, 1.0f, 1.0f, 1.0f};
    style.lineCasingDashByStyleGroup[6604] =
        FeatureRenderStyle::LineDashPattern{
            {12.0f, 12.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Butt};
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    RenderCommandList commands = build();
    ASSERT_EQ(2u, commands.size());
    EXPECT_EQ(66, commands[0].vectorPaintOrder);
    EXPECT_EQ(0, commands[0].vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(1.0f, commands[0].vectorUniforms.lineWidthPx);
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              commands[0].vectorUniforms.color);
    EXPECT_FLOAT_EQ(2.0f, commands[0].vectorUniforms.dashPatternCount);
    EXPECT_FLOAT_EQ(12.0f, commands[0].vectorUniforms.dashPattern[0]);
    EXPECT_EQ(1, commands[1].vectorPaintSubOrder);
    EXPECT_FLOAT_EQ(0.0f, commands[1].vectorUniforms.lineWidthPx);
    EXPECT_EQ((std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
              commands[1].vectorUniforms.color);
}

TEST_F(FeatureRenderLayerTest,
       TransitStyleGroupsKeepPaintOrderAndIndependentGeometryContracts) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(90.0);
    style.lineStyleGroupExpr = StyleExpression::match(
        "amap_subkey",
        {{"1", StyleExpression::literal(
                   amapClassicStyleIdentity(20015, 1))}},
        StyleExpression::literal(amapClassicStyleIdentity(20015, 3)));
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 0.0f;
    const int transit1 = amapClassicStyleIdentity(20015, 1);
    const int transit3 = amapClassicStyleIdentity(20015, 3);
    style.lineCasingStyleGroups.insert(transit1);
    style.lineCasingStyleGroups.insert(transit3);
    style.lineWidthExprByStyleGroup[transit1] = StyleExpression::literal(0.0);
    style.lineCasingWidthExprByStyleGroup[transit1] =
        StyleExpression::literal(2.0);
    style.lineCasingDashByStyleGroup[transit1] =
        FeatureRenderStyle::LineDashPattern{
            {2.0f, 2.0f, 0.0f, 0.0f}, 2,
            FeatureRenderStyle::LineCap::Butt};
    style.lineWidthExprByStyleGroup[transit3] = StyleExpression::literal(2.0);
    style.lineCasingWidthExprByStyleGroup[transit3] =
        StyleExpression::literal(1.0);
    layer_->setStyleForContractTest(style);

    Feature railway = makeLine(6.0, 29.0, 0.01);
    railway.properties["amap_subkey"] = "1";
    Feature metro = makeLine(6.02, 29.0, 0.01);
    metro.properties["amap_subkey"] = "7";
    layer_->store().addFeature(std::move(railway));
    layer_->store().addFeature(std::move(metro));

    RenderCommandList commands = build();
    ASSERT_EQ(4u, commands.size());
    int railwayCasing = 0;
    int railwayCenter = 0;
    int metroCasing = 0;
    int metroCenter = 0;
    for (const auto& cmd : commands) {
        EXPECT_EQ(90, cmd.vectorPaintOrder);
        if (cmd.vectorPaintSubOrder == 0 &&
            cmd.vectorUniforms.dashPatternCount == 2.0f) {
            ++railwayCasing;
            EXPECT_FLOAT_EQ(2.0f, cmd.vectorUniforms.lineWidthPx);
            EXPECT_EQ((std::array<float, 4>{2.0f, 2.0f, 0.0f, 0.0f}),
                      cmd.vectorUniforms.dashPattern);
        } else if (cmd.vectorPaintSubOrder == 1 &&
                   cmd.vectorUniforms.lineWidthPx == 0.0f) {
            ++railwayCenter;
        } else if (cmd.vectorPaintSubOrder == 0) {
            ++metroCasing;
            EXPECT_FLOAT_EQ(3.0f, cmd.vectorUniforms.lineWidthPx);
            EXPECT_FLOAT_EQ(0.0f, cmd.vectorUniforms.dashPatternCount);
        } else {
            ++metroCenter;
            EXPECT_FLOAT_EQ(2.0f, cmd.vectorUniforms.lineWidthPx);
        }
    }
    EXPECT_EQ(1, railwayCasing);
    EXPECT_EQ(1, railwayCenter);
    EXPECT_EQ(1, metroCasing);
    EXPECT_EQ(1, metroCenter);
}

TEST_F(FeatureRenderLayerTest, LineCasingRespectsZoomWindow) {
    FeatureRenderStyle style = layer_->style();
    style.lineCasingEnabled = true;
    style.lineCasingExtraWidthPx = 3.0f;
    // The fixture camera is a broad view at roughly zoom 2.2.
    style.lineCasingMinZoom = 10.0;
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makeLine(6.0, 29.0, 0.05));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorLine, commands[0].kind);
    EXPECT_EQ(0, commands[0].vectorPaintSubOrder);
}

TEST_F(FeatureRenderLayerTest, FillStyleGroupRespectsDisplayZoomWindow) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(37.0);
    style.fillStyleGroupExpr = StyleExpression::literal(37.0);
    // Fixture zoom is approximately 2.2.
    style.fillMinZoomByStyleGroup[37] = 3.0;
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.02));

    RenderCommandList hidden = build();
    EXPECT_TRUE(std::none_of(hidden.begin(), hidden.end(), [](const auto& cmd) {
        return cmd.kind == RenderCommandKind::VectorFill;
    }));

    style.fillMinZoomByStyleGroup[37] = 2.0;
    layer_->setStyleForContractTest(style);
    RenderCommandList visible = build();
    EXPECT_TRUE(std::any_of(visible.begin(), visible.end(), [](const auto& cmd) {
        return cmd.kind == RenderCommandKind::VectorFill &&
               cmd.vectorPaintOrder == 37;
    }));

    // maxZoom is exclusive, matching point/label and line style windows.
    style.fillMaxZoomByStyleGroup[37] = 2.0;
    layer_->setStyleForContractTest(style);
    RenderCommandList expired = build();
    EXPECT_TRUE(std::none_of(expired.begin(), expired.end(), [](const auto& cmd) {
        return cmd.kind == RenderCommandKind::VectorFill;
    }));
}

TEST_F(FeatureRenderLayerTest,
       ProviderVisibilityWindowUsesOptInFractionalZoomSelector) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::literal(37.0);
    style.fillStyleGroupExpr = StyleExpression::literal(37.0);
    style.fillMinZoomByStyleGroup[37] = 3.0;
    style.visibilityZoomCeilFraction = 0.8;
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makePolygon(0.0, 0.0, 0.02));

    const double radius = Ellipsoid::WGS84().radii().x();
    auto setZoom = [&](double zoom) {
        const double height = 4.0e7 / std::pow(2.0, zoom);
        camera_.lookAt(Vec3(radius + height, 0.0, 0.0), Vec3(radius, 0.0, 0.0),
                       Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
    };
    const auto hasFill = [](const RenderCommandList& commands) {
        return std::any_of(commands.begin(), commands.end(), [](const auto& cmd) {
            return cmd.kind == RenderCommandKind::VectorFill &&
                   cmd.vectorPaintOrder == 37;
        });
    };

    setZoom(2.79);
    EXPECT_FALSE(hasFill(build()));
    setZoom(2.80);
    EXPECT_TRUE(hasFill(build()));
}

TEST_F(FeatureRenderLayerTest, StencilVolumesGroupedByResolvedColor) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.fillColorExpr = StyleExpression::match(
        "kind",
        {{"tower", StyleExpression::literal({1.0f, 0.0f, 0.0f, 0.5f})}},
        StyleExpression::literal({0.0f, 0.0f, 1.0f, 0.5f}));
    layer_->setStyleForContractTest(style);
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
            ASSERT_TRUE(col.hasVectorUniforms);
            reds.push_back(col.vectorUniforms.color[0]);
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
    layer_->setStyleForContractTest(style);
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

TEST(IconAtlasTest, RemovingOfficialNamespacePreservesGenericImages) {
    earth_engine::testing::MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    IconAtlas atlas(&device);
    ASSERT_TRUE(atlas.addImage("generic", 4, 4, solidRgba(4, 4, 1)));
    ASSERT_TRUE(atlas.addImage("amap-icons-4-7", 4, 4,
                               solidRgba(4, 4, 2)));
    const uint64_t before = atlas.revision();
    atlas.removeNamePrefix("amap-icons-");
    EXPECT_NE(nullptr, atlas.frame("generic"));
    EXPECT_EQ(nullptr, atlas.frame("amap-icons-4-7"));
    EXPECT_GT(atlas.revision(), before);
}

TEST_F(FeatureRenderLayerTest, BuiltinShapeBakedIntoVertexShape) {
    FeatureRenderStyle style = layer_->style();
    style.pointImage = "star";
    layer_->setStyleForContractTest(style);
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
    layer_->setStyleForContractTest(style);
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
    layer_->setStyleForContractTest(style);
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
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makePointAt(6.0));

    RenderCommandList commands = build();
    ASSERT_EQ(1u, commands.size());
    const RenderCommand& cmd = commands[0];
    // [0]=图标图集,[1]=T2 地形深度槽(见 LabelCommandForNamedFeature 注释)。
    ASSERT_EQ(2u, cmd.textures.size());
    EXPECT_EQ(renderer_->iconAtlas()->texture(), cmd.textures[0]);
    EXPECT_EQ(nullptr, cmd.textures[1]);

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
    layer_->setStyleForContractTest(style);
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
    layer_->setStyleForContractTest(style);
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
    layer_->setStyleForContractTest(style);
    EXPECT_EQ(nullptr, layer_->style().pointImageExpr);

    layer_->store().addFeature(makePointAt(6.0));
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        build()[0].vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const auto* f = reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_FLOAT_EQ(static_cast<float>(static_cast<int>(SymbolShape::Square)),
                    f[8]);
}

// ---- 逐瓦片贴地高度范围(stencil 体高 → fill 的主导因子) ----
//
// 体高直接换算成屏幕覆盖:全屏一个 union 会让平原上的路背着山地的相对高差。
// 真机实测(GpuPass 逐区间计时)体高 10km→1km 时矢量 GPU 135→19.6ms,故这条
// 收窄逻辑不是微优化,是主干。
TEST_F(FeatureRenderLayerTest, PerAreaHeightRangeNarrowsToIntersectingCells) {
    FeatureRenderLayer layer("t", &device_, Ellipsoid::WGS84());
    // 全局范围 = 两块瓦片的并集(平地 0..100 + 山地 0..3000)
    layer.setWorkerTerrainHeightRange(0.0, 3000.0);
    auto cells =
        std::make_shared<FeatureRenderLayer::TerrainHeightRangeCells>();
    cells->push_back({Rectangle(0.0, 0.0, 1.0 * kDeg, 1.0 * kDeg), 0.0, 100.0});
    cells->push_back({Rectangle(10.0 * kDeg, 0.0, 11.0 * kDeg, 1.0 * kDeg),
                      0.0, 3000.0});
    layer.setWorkerTerrainHeightRangeCells(cells);

    // 平地那块:只与第一格相交 → 拿到 0..100,而不是全局的 0..3000
    const auto flat = layer.workerTessellationContextForArea(
        Rectangle(0.1 * kDeg, 0.1 * kDeg, 0.2 * kDeg, 0.2 * kDeg));
    EXPECT_TRUE(flat.hasTerrainHeightRange);
    EXPECT_DOUBLE_EQ(flat.terrainMinHeight, 0.0);
    EXPECT_DOUBLE_EQ(flat.terrainMaxHeight, 100.0);

    // 山地那块:仍拿到 3000 —— 收窄只允许发生在真的没有高地形的地方
    const auto hilly = layer.workerTessellationContextForArea(
        Rectangle(10.1 * kDeg, 0.1 * kDeg, 10.2 * kDeg, 0.2 * kDeg));
    EXPECT_DOUBLE_EQ(hilly.terrainMaxHeight, 3000.0);

    // 跨两格:并集,不能只取命中的第一格
    const auto both = layer.workerTessellationContextForArea(
        Rectangle(0.5 * kDeg, 0.1 * kDeg, 10.5 * kDeg, 0.2 * kDeg));
    EXPECT_DOUBLE_EQ(both.terrainMaxHeight, 3000.0);
}

TEST_F(FeatureRenderLayerTest, PerAreaHeightRangeFallsBackWhenNoCellIntersects) {
    FeatureRenderLayer layer("t", &device_, Ellipsoid::WGS84());
    layer.setWorkerTerrainHeightRange(-50.0, 900.0);
    auto cells =
        std::make_shared<FeatureRenderLayer::TerrainHeightRangeCells>();
    cells->push_back({Rectangle(0.0, 0.0, 1.0 * kDeg, 1.0 * kDeg), 0.0, 100.0});
    layer.setWorkerTerrainHeightRangeCells(cells);

    // 相交不到 → 必须退回全局范围。**绝不能返回"没有范围"或更窄的范围**:
    // 体穿不透地形是整片消失,不是变淡,比多烧 fill 严重得多。
    const auto ctx = layer.workerTessellationContextForArea(
        Rectangle(50.0 * kDeg, 50.0 * kDeg, 51.0 * kDeg, 51.0 * kDeg));
    EXPECT_TRUE(ctx.hasTerrainHeightRange);
    EXPECT_DOUBLE_EQ(ctx.terrainMinHeight, -50.0);
    EXPECT_DOUBLE_EQ(ctx.terrainMaxHeight, 900.0);

    // 快照根本没发布过 → 同样退回全局
    FeatureRenderLayer bare("b", &device_, Ellipsoid::WGS84());
    bare.setWorkerTerrainHeightRange(-50.0, 900.0);
    const auto bareCtx = bare.workerTessellationContextForArea(
        Rectangle(0.0, 0.0, 1.0 * kDeg, 1.0 * kDeg));
    EXPECT_TRUE(bareCtx.hasTerrainHeightRange);
    EXPECT_DOUBLE_EQ(bareCtx.terrainMaxHeight, 900.0);
}

// ============================================================
// E 方案 P1:ribbon-clamp 瓦片线(几何通道,不做 stencil 墙带)
// ============================================================

TEST_F(FeatureRenderLayerTest, TileRibbonClampDensifiesAtEllipsoidAndSkipsStencil) {
    FeatureRenderLayer layer("t", &device_, Ellipsoid::WGS84());
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.terrainClampRibbon = true;  // E 方案 P1
    style.clampDensifyMeters = 100.0;
    // 故意给一个"山地范围":E-ribbon 必须忽略它(高度由 P2 VS 采高负责),
    // 而不是把全部顶点放在范围中点 —— 那会让整条路飘在半空。
    FeatureRenderLayer::TessellationContext ctx{
        style, Ellipsoid::WGS84(), nullptr, nullptr,
        /*supportsStencilClassification=*/true};
    ctx.hasTerrainHeightRange = true;
    ctx.terrainMinHeight = 500.0;
    ctx.terrainMaxHeight = 2000.0;

    Feature line = makeLine(6.0, 29.0, 0.1);
    auto mesh = FeatureRenderLayer::tessellateTileMesh(ctx, {line});

    // 不走 stencil 墙带,走 ribbon 单 pass。
    EXPECT_TRUE(mesh.lineVolumeGroups.empty());
    EXPECT_FALSE(mesh.lineVerts.empty());
    ASSERT_FALSE(mesh.lineIndices.empty());
    // 0.1°≈11km、100m 细分 → 顶点数远大于未细分的 3 顶点 ribbon(6)。
    EXPECT_GT(mesh.lineVerts.size(), 12u * 6u);
    // worker 无采样 → 产出完整钳高源(每最终 line vertex 9 float)，
    // 供渲染线程 commit/重钳同源采样且保留 join/cap。
    ASSERT_FALSE(mesh.lineClampSource.empty());
    EXPECT_EQ(mesh.lineClampSource.size(), mesh.lineVerts.size() / 12 * 9);
    EXPECT_NEAR(mesh.lineClampSource[0], 6.0 * kDeg, 1e-6);
    EXPECT_NEAR(mesh.lineClampSource[1], 29.0 * kDeg, 1e-6);
    // 顶点落在椭球面(高≈0),而非山地范围中点(1250m)。
    const Ellipsoid& ell = Ellipsoid::WGS84();
    const float* v = mesh.lineVerts.data();
    const Vec3 rel(v[0], v[1], v[2]);
    const Cartographic c = ell.cartesianToCartographic(mesh.origin + rel);
    EXPECT_NEAR(c.height(), 0.0, 0.05);
}

TEST_F(FeatureRenderLayerTest, TileRibbonClampCommitSamplesTerrain) {
    FeatureRenderLayer layer("t", &device_, Ellipsoid::WGS84());
    layer.setTerrainSampling(makeFlatSampling(1500.0f));
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.terrainClampRibbon = true;  // E 方案 P1+P2
    style.clampDensifyMeters = 100.0;
    layer.setStyle(style);  // commit 钳高的采样器按图层样式门控
    FeatureRenderLayer::TessellationContext ctx{
        style, Ellipsoid::WGS84(), nullptr, nullptr,
        /*supportsStencilClassification=*/true};

    Feature line = makeLine(6.0, 29.0, 0.02);
    auto mesh = FeatureRenderLayer::tessellateTileMesh(ctx, {line});
    const Vec3 origin = mesh.origin;
    ASSERT_FALSE(mesh.lineClampSource.empty());
    layer.commitTileMesh(
        TileKey{SchemeId("XYZ-WebMercator"), 14, 100, 200}, std::move(mesh));

    RenderCommandList commands;
    layer.buildRenderCommands(frame_, *renderer_, commands);
    const RenderCommand* lineCmd = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorLine) lineCmd = &cmd;
    }
    ASSERT_NE(nullptr, lineCmd);
    const auto* vb = dynamic_cast<const earth_engine::testing::DummyBuffer*>(
        lineCmd->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    const float* f = reinterpret_cast<const float*>(vb->bytes().data());
    const Vec3 rel(f[0], f[1], f[2]);
    const Cartographic c =
        Ellipsoid::WGS84().cartesianToCartographic(origin + rel);
    // commit 时按 1500m 平地同源采样钳高(worker 给的是椭球面)。
    EXPECT_NEAR(c.height(), 1500.0, 1.0);
}

TEST_F(FeatureRenderLayerTest, TileClampWithoutRibbonKeepsStencilVolumes) {
    FeatureRenderLayer layer("t", &device_, Ellipsoid::WGS84());
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.terrainClampRibbon = false;  // 默认:仍走 stencil 墙带
    FeatureRenderLayer::TessellationContext ctx{
        style, Ellipsoid::WGS84(), nullptr, nullptr,
        /*supportsStencilClassification=*/true};

    Feature line = makeLine(6.0, 29.0, 0.1);
    auto mesh = FeatureRenderLayer::tessellateTileMesh(ctx, {line});

    EXPECT_FALSE(mesh.lineVolumeGroups.empty());
    EXPECT_TRUE(mesh.lineVerts.empty());
}

TEST_F(FeatureRenderLayerTest, BuildingExtrusionEmitsVectorExtrusionCommand) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::Absolute;
    style.buildingExtrusion = true;
    style.fillStyleGroupExpr = StyleExpression::literal(1.0);
    style.extrusionRoofColorByStyleGroup[1] = {1, 1, 1, 1};
    style.extrusionWallColorByStyleGroup[1] = {1, 1, 1, 1};
    layer_->setStyleForContractTest(style);
    Feature b = makePolygon(6.0, 29.0, 0.01);
    b.properties["amap_height"] = "25";
    layer_->store().addFeature(std::move(b));

    RenderCommandList commands = build();
    const RenderCommand* ext = nullptr;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorExtrusion) ext = &cmd;
    }
    ASSERT_NE(nullptr, ext);
    EXPECT_TRUE(ext->depthTest);
    EXPECT_TRUE(ext->depthWrite);
    EXPECT_FALSE(ext->blend);
    EXPECT_EQ(28, ext->vertexStride);
    const auto* vb =
        dynamic_cast<const earth_engine::testing::DummyBuffer*>(
            ext->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    // 墙 4 边 × 4 顶点 + 顶面 ≥4 顶点;7 float/顶点。
    EXPECT_GE(vb->bytes().size() / (sizeof(float) * 7), 20u);
    // 挤出与平 fill 互斥。
    for (const auto& cmd : commands) {
        EXPECT_NE(RenderCommandKind::VectorFill, cmd.kind);
    }
}

TEST_F(FeatureRenderLayerTest,
       OfficialBuildingColorAndWindowReachFinalExtrusionCommand) {
    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    style.globeFillMaxEdgeMeters = 0.0;
    layer_->setStyleForContractTest(style);

    Feature building = makePolygon(6.0, 29.0, 0.01);
    building.properties = {{"amap_class", "55001"},
                           {"amap_subkey", "21"},
                           {"amap_draworder", "47"},
                           {"amap_minzoom", "3"},
                           {"amap_maxzoom", "30"},
                           {"amap_height", "25"}};
    layer_->store().addFeature(std::move(building));

    const Cartographic center((6.005) * kDeg, (29.005) * kDeg);
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(center);
    const Vec3 radial = surface.normalized();
    auto setZoom = [&](double zoom) {
        const double height = 4.0e7 / std::pow(2.0, zoom);
        camera_.lookAt(surface + radial * height, surface,
                       Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
    };
    const auto extrusion = [](const RenderCommandList& commands)
        -> const RenderCommand* {
        const auto it = std::find_if(
            commands.begin(), commands.end(), [](const RenderCommand& cmd) {
                return cmd.kind == RenderCommandKind::VectorExtrusion;
            });
        return it == commands.end() ? nullptr : &*it;
    };
    const auto unpackColor = [](float packed) {
        uint32_t bits = 0;
        std::memcpy(&bits, &packed, sizeof(bits));
        return std::array<int, 4>{
            static_cast<int>(bits & 0xFF),
            static_cast<int>((bits >> 8) & 0xFF),
            static_cast<int>((bits >> 16) & 0xFF),
            static_cast<int>((bits >> 24) & 0xFF)};
    };

    // Official provider minZoom 17 maps to display zoom 16 and uses the
    // shared fractional selector: 15.79 stays hidden, 15.80 becomes visible.
    setZoom(15.79);
    EXPECT_EQ(nullptr, extrusion(build()));

    setZoom(15.80);
    const RenderCommandList visible = build();
    const RenderCommand* command = extrusion(visible);
    ASSERT_NE(nullptr, command);
    EXPECT_EQ(47, command->vectorPaintOrder);
    EXPECT_TRUE(command->depthTest);
    EXPECT_FALSE(command->depthWrite);
    EXPECT_TRUE(command->blend)
        << "official subKey 21 alpha=0x80 must reach command state";
    ASSERT_EQ(28, command->vertexStride);
    const auto* vb = dynamic_cast<const DummyBuffer*>(command->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    ASSERT_EQ(0u, vb->bytes().size() % (7u * sizeof(float)));
    const auto* vertices =
        reinterpret_cast<const float*>(vb->bytes().data());
    const size_t vertexCount = vb->bytes().size() / (7u * sizeof(float));
    ASSERT_GT(vertexCount, 0u);
    for (size_t i = 0; i < vertexCount; ++i) {
        EXPECT_EQ((std::array<int, 4>{77, 166, 255, 128}),
                  unpackColor(vertices[i * 7 + 6]));
    }

    // All official building records currently end at provider zoom 30 while
    // the production camera contract caps view zoom at 24. The max gate is
    // covered by the all-record style oracle; it is intentionally not faked
    // here with an unreachable camera state.
}

TEST_F(FeatureRenderLayerTest,
       EveryOfficialBuildingIdentityReachesMatchingFinalExtrusionCommand) {
    const auto records = amapClassicBuildingRecordsForTest();
    ASSERT_EQ(25u, records.size());
    std::vector<Feature> buildings;
    std::map<int, AmapClassicBuildingRecordForTest> byPaintOrder;
    for (size_t i = 0; i < records.size(); ++i) {
        const int paintOrder = 2000 + static_cast<int>(i);
        byPaintOrder.emplace(paintOrder, records[i]);
        Feature building = makePolygon(
            (i % 8) * 0.0002, (i / 8) * 0.0002, 0.0001);
        building.properties = {
            {"amap_class", "55001"},
            {"amap_subkey", std::to_string(records[i].subKey)},
            {"amap_draworder", std::to_string(paintOrder)},
            {"amap_minzoom", "1"}, {"amap_maxzoom", "30"},
            {"amap_height", "12"}};
        buildings.push_back(std::move(building));
    }

    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    style.globeFillMaxEdgeMeters = 0.0;
    layer_->setStyleForContractTest(style);
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), buildings);
    ASSERT_EQ(records.size(), mesh.extrudeRanges.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 16, 100, 200},
                  std::move(mesh)));

    const auto rgba8 = [](const std::array<float, 4>& color) {
        return std::array<int, 4>{
            static_cast<int>(std::lround(color[0] * 255.0f)),
            static_cast<int>(std::lround(color[1] * 255.0f)),
            static_cast<int>(std::lround(color[2] * 255.0f)),
            static_cast<int>(std::lround(color[3] * 255.0f))};
    };
    const auto unpackColor = [](float packed) {
        uint32_t bits = 0;
        std::memcpy(&bits, &packed, sizeof(bits));
        return std::array<int, 4>{
            static_cast<int>(bits & 0xff),
            static_cast<int>((bits >> 8) & 0xff),
            static_cast<int>((bits >> 16) & 0xff),
            static_cast<int>((bits >> 24) & 0xff)};
    };
    const double radius = Ellipsoid::WGS84().radii().x();
    for (int displayZoom = 15; displayZoom <= 24; ++displayZoom) {
        const double height = 4.0e7 / std::pow(2.0, displayZoom);
        camera_.lookAt(Vec3(radius + height, 0.0, 0.0),
                       Vec3(radius, 0.0, 0.0), Vec3(0.0, 0.0, 1.0));
        ++frame_.frameId;
        const auto commands = build();
        std::map<int, const RenderCommand*> actual;
        for (const auto& command : commands) {
            if (command.kind == RenderCommandKind::VectorExtrusion)
                actual.emplace(command.vectorPaintOrder, &command);
        }
        for (const auto& [paintOrder, record] : byPaintOrder) {
            const bool visible = record.minZoom <= displayZoom + 1 &&
                                 displayZoom + 1 <= record.maxZoom;
            const auto found = actual.find(paintOrder);
            if (!visible) {
                EXPECT_EQ(actual.end(), found)
                    << "55001:" << record.subKey
                    << " must be absent at display zoom " << displayZoom;
                continue;
            }
            ASSERT_NE(actual.end(), found)
                << "55001:" << record.subKey
                << " missing final extrusion at display zoom " << displayZoom;
            const RenderCommand& command = *found->second;
            const bool translucent = record.roofColor[3] < 0.999f ||
                                     record.wallColor[3] < 0.999f;
            EXPECT_EQ(translucent, command.blend);
            EXPECT_EQ(!translucent, command.depthWrite);
            const auto* vb = dynamic_cast<const DummyBuffer*>(command.vertexBuffer);
            const auto* ib = dynamic_cast<const DummyBuffer*>(command.indexBuffer);
            ASSERT_NE(nullptr, vb);
            ASSERT_NE(nullptr, ib);
            const auto* vertices =
                reinterpret_cast<const float*>(vb->bytes().data());
            const auto* indices =
                reinterpret_cast<const uint32_t*>(ib->bytes().data());
            const auto expectedRoof = rgba8(record.roofColor);
            const auto expectedWall = rgba8(record.wallColor);
            for (int i = 0; i < command.indexCount; ++i) {
                const uint32_t vertex = indices[command.indexOffset + i];
                const auto actualColor = unpackColor(vertices[vertex * 7 + 6]);
                EXPECT_TRUE(actualColor == expectedRoof ||
                            actualColor == expectedWall)
                    << "55001:" << record.subKey
                    << " vertex color escaped official roof/wall contract";
            }
        }
    }
}

TEST_F(FeatureRenderLayerTest,
       OfficialGuideFrameReachesExactAtlasDemandWithoutFallbackCommand) {
    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    layer_->setStyleForContractTest(style);
    build();  // Cache renderer-owned atlases before committing tile symbols.
    std::vector<int> demandedAtlases;
    layer_->setOfficialIconAtlasDemandForTest(
        [&](int atlas) { demandedAtlases.push_back(atlas); });

    Feature guide;
    guide.type = GeometryType::Point;
    guide.rings = {{Cartographic(0.0, 0.0)}};
    guide.properties = {{"amap_class", "40001"},
                        {"amap_subkey", "110100"},
                        {"amap_draworder", "90"},
                        {"amap_minzoom", "3"},
                        {"amap_maxzoom", "20"},
                        {"amap_rank", "1"},
                        {"name", "ABCD"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer_->workerTessellationContext(), {guide});
    ASSERT_EQ(1u, mesh.symbols.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              layer_->commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                  std::move(mesh)));

    const double radius = Ellipsoid::WGS84().radii().x();
    const double height = 4.0e7 / std::pow(2.0, 10.0);
    camera_.lookAt(Vec3(radius + height, 0.0, 0.0),
                   Vec3(radius, 0.0, 0.0), Vec3(0.0, 0.0, 1.0));
    ++frame_.frameId;
    const RenderCommandList commands = build();

    EXPECT_EQ((std::vector<int>{1}), demandedAtlases)
        << "official guide field-8 frame must demand its exact atlas";
    EXPECT_TRUE(std::none_of(
        commands.begin(), commands.end(), [](const RenderCommand& command) {
            return command.kind == RenderCommandKind::VectorPoint ||
                   command.kind == RenderCommandKind::VectorLabel;
        })) << "missing official atlas must not fall back to a synthetic icon or bare text";
}

TEST_F(FeatureRenderLayerTest,
       OfficialGuideFrameReachesFinalUvAndShieldGeometry) {
    const AmapClassicPoiIconStyle guideIcon =
        resolveAmapClassicPoiIconStyle(40001, 110100, 10.0);
    ASSERT_TRUE(guideIcon.enabled);
    EXPECT_EQ(1, guideIcon.atlas);         // Official guide field 3.
    EXPECT_EQ(114, guideIcon.iconIndex);   // Official guide field 4.
    EXPECT_EQ(64, guideIcon.cellWidth);    // Official guide field 5.
    EXPECT_EQ(64, guideIcon.cellHeight);   // Official guide field 6.
    EXPECT_EQ(512, guideIcon.atlasWidth);  // Official guide field 7.
    EXPECT_EQ(1024, guideIcon.atlasHeight);// Official guide field 8.

    MockPlatformBridge bridge;
    DecodedImage atlasImage;
    atlasImage.width = 512;
    atlasImage.height = 1024;
    atlasImage.channels = 4;
    atlasImage.pixels.resize(512u * 1024u * 4u);
    for (int y = 0; y < atlasImage.height; ++y) {
        for (int x = 0; x < atlasImage.width; ++x) {
            const size_t offset =
                (static_cast<size_t>(y) * atlasImage.width + x) * 4u;
            atlasImage.pixels[offset] = static_cast<uint8_t>(x & 0xff);
            atlasImage.pixels[offset + 1] = static_cast<uint8_t>(y & 0xff);
            atlasImage.pixels[offset + 2] =
                static_cast<uint8_t>((x / 64) | ((y / 64) << 4));
            atlasImage.pixels[offset + 3] = 255;
        }
    }
    bridge.setDecodedImage(std::move(atlasImage));
    Engine engine(&device_);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 1.0f);
    auto pool = std::make_shared<ThreadPool>(1);
    const AmapClassicRuntime* runtime = engine.installAmapClassicRuntime(
        bridge, pool, pool, pool, {});
    ASSERT_NE(nullptr, runtime);
    Renderer* officialRenderer = const_cast<Renderer*>(engine.renderer());
    ASSERT_NE(nullptr, officialRenderer);

    FeatureRenderLayer officialLayer(
        "guide-atlas1-official", &device_, Ellipsoid::WGS84());
    FrameState officialFrame = frame_;
    Camera officialCamera;
    officialFrame.camera = &officialCamera;
    auto buildOfficial = [&]() {
        RenderCommandList commands;
        officialLayer.buildRenderCommands(
            officialFrame, *officialRenderer, commands);
        return commands;
    };

    GlyphAtlas* glyphAtlas = officialRenderer->glyphAtlas();
    ASSERT_NE(nullptr, glyphAtlas);
    glyphAtlas->activateAmapOfficialProviderForTest([](uint32_t) {});
    std::vector<uint8_t> glyphPixels(128u * 32u, 127);
    const std::vector<GlyphAtlas::ProviderGlyph> guideGlyphs = {
        {19968, 22, 22, 1, -2, 24, 0, 0},
        {20108, 22, 22, 1, -2, 24, 32, 0},
        {19977, 22, 22, 1, -2, 24, 64, 0},
        {22235, 22, 22, 1, -2, 24, 96, 0},
    };
    ASSERT_TRUE(glyphAtlas->installAmapOfficialGlyphBatchForTest(
        128, 32, glyphPixels, guideGlyphs));
    buildOfficial();
    ASSERT_TRUE(const_cast<AmapClassicRuntime*>(runtime)
                    ->installAtlasForContractTest(
                        1, {0x89, 0x50, 0x4e, 0x47}));
    const IconAtlas::Frame* frame =
        officialRenderer->iconAtlas()->frame("amap-icons-1-114");
    ASSERT_NE(nullptr, frame);
    const auto exactIndex114Payload = std::find_if(
        device_.textureRegionPayloads.begin(),
        device_.textureRegionPayloads.end(), [](const auto& pixels) {
            constexpr size_t kCellBytes = 64u * 64u * 4u;
            if (pixels.size() != kCellBytes) return false;
            // atlas1 has eight columns. One-based index114 maps to zero-based
            // cell113: x=64, y=14*64=896.
            return pixels[0] == 64 && pixels[1] == 128 &&
                   pixels[2] == 0xe1 && pixels[3] == 255 &&
                   pixels[kCellBytes - 4] == 127 &&
                   pixels[kCellBytes - 3] == 191 &&
                   pixels[kCellBytes - 2] == 0xe1 &&
                   pixels[kCellBytes - 1] == 255;
        });
    ASSERT_NE(device_.textureRegionPayloads.end(), exactIndex114Payload)
        << "guide index/cell/atlas dimensions must crop source cell "
           "(64,896)-(127,959)";

    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    officialLayer.setStyleForContractTest(style);
    Feature guide;
    guide.type = GeometryType::Point;
    guide.rings = {{Cartographic(0.0, 0.0)}};
    guide.properties = {{"amap_class", "40001"},
                        {"amap_subkey", "110100"},
                        {"amap_draworder", "90"},
                        {"amap_minzoom", "3"},
                        {"amap_maxzoom", "20"},
                        {"amap_rank", "1"},
                        {"name", "一二三四"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        officialLayer.workerTessellationContext(), {guide});
    ASSERT_EQ(1u, mesh.symbols.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              officialLayer.commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 10, 100, 200},
                  std::move(mesh)));
    const double radius = Ellipsoid::WGS84().radii().x();
    const double height = 4.0e7 / std::pow(2.0, 10.0);
    officialCamera.lookAt(Vec3(radius + height, 0.0, 0.0),
                          Vec3(radius, 0.0, 0.0),
                          Vec3(0.0, 0.0, 1.0));
    officialFrame.devicePixelRatio = 1.0f;
    ++officialFrame.frameId;
    const RenderCommandList commands = buildOfficial();
    const auto point = std::find_if(
        commands.begin(), commands.end(), [](const RenderCommand& command) {
            return command.kind == RenderCommandKind::VectorPoint;
    });
    ASSERT_NE(commands.end(), point);
    EXPECT_FLOAT_EQ(1.0f, point->vectorUniforms.pointSizePx);
    const auto* pointVb = dynamic_cast<const DummyBuffer*>(point->vertexBuffer);
    ASSERT_NE(nullptr, pointVb);
    ASSERT_EQ(4u * 9u * sizeof(float), pointVb->bytes().size());
    const auto* pointVertices =
        reinterpret_cast<const float*>(pointVb->bytes().data());
    EXPECT_FLOAT_EQ(frame->u0, pointVertices[5]);
    EXPECT_FLOAT_EQ(frame->v1, pointVertices[6]);
    EXPECT_FLOAT_EQ(frame->u1, pointVertices[2 * 9 + 5]);
    EXPECT_FLOAT_EQ(frame->v0, pointVertices[2 * 9 + 6]);
    EXPECT_FLOAT_EQ(24.0f * 9.0f / 7.0f,
                    pointVertices[2 * 9 + 3] - pointVertices[3]);
    EXPECT_FLOAT_EQ(24.0f,
                    pointVertices[2 * 9 + 4] - pointVertices[4]);
    const auto label = std::find_if(
        commands.begin(), commands.end(), [](const RenderCommand& command) {
            return command.kind == RenderCommandKind::VectorLabel;
        });
    ASSERT_NE(commands.end(), label);
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              label->vectorUniforms.color);
    EXPECT_EQ((std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
              label->vectorUniforms.haloColor);
    EXPECT_FLOAT_EQ(6.0f, FeatureRenderLayer::resolvedLabelSizePx(
                              style, -110100, 10.0, 0.0f));
    EXPECT_FLOAT_EQ(0.0f, FeatureRenderLayer::resolvedLabelHaloWidthPx(
                              style, -110100, 10.0));
    EXPECT_NEAR(0.78125f, label->vectorUniforms.sdfEdge, 1e-6f);
    EXPECT_NEAR(0.0f, label->vectorUniforms.sdfHaloDelta, 1e-6f);
    EXPECT_NEAR(1.4142f * 1.5f / 6.0f,
                label->vectorUniforms.sdfGamma, 1e-6f);
}

TEST_F(FeatureRenderLayerTest,
       OfficialAtlas50CellReachesFinalUvAndNineteenCssPixelQuad) {
    MockPlatformBridge bridge;
    DecodedImage atlasImage;
    atlasImage.width = 512;
    atlasImage.height = 1024;
    atlasImage.channels = 4;
    atlasImage.pixels.resize(512u * 1024u * 4u, 255);
    bridge.setDecodedImage(std::move(atlasImage));
    Engine engine(&device_);
    engine.onSurfaceCreated();
    engine.onSurfaceChanged(800, 600, 2.0f);
    auto pool = std::make_shared<ThreadPool>(1);
    const AmapClassicRuntime* runtime = engine.installAmapClassicRuntime(
        bridge, pool, pool, pool, {});
    ASSERT_NE(nullptr, runtime);
    ASSERT_TRUE(const_cast<AmapClassicRuntime*>(runtime)
                    ->installAtlasForContractTest(
                        50, {0x89, 0x50, 0x4e, 0x47}));
    Renderer* officialRenderer = const_cast<Renderer*>(engine.renderer());
    ASSERT_NE(nullptr, officialRenderer);
    const IconAtlas::Frame* frame =
        officialRenderer->iconAtlas()->frame("amap-icons-50-31");
    ASSERT_NE(nullptr, frame);
    // Official label field 4 final atlas consumer.
    // Official label field 5 final one-based icon-index consumer.
    // Official label field 8 final source-atlas-width consumer.
    EXPECT_FLOAT_EQ(48.0f, frame->widthPx);
    // Official label field 6 final cell-width consumer.
    EXPECT_FLOAT_EQ(48.0f, frame->heightPx);
    // Official label field 7 final cell-height consumer.

    FeatureRenderLayer officialLayer(
        "atlas50-official", &device_, Ellipsoid::WGS84());
    FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    officialLayer.setStyleForContractTest(style);
    FrameState officialFrame = frame_;
    officialFrame.devicePixelRatio = 2.0f;

    Feature poi;
    poi.type = GeometryType::Point;
    poi.rings = {{Cartographic(0.0, 0.0)}};
    poi.properties = {{"amap_class", "10037"},
                      {"amap_subkey", "43"},
                      {"amap_draworder", "90"},
                      {"amap_minzoom", "3"},
                      {"amap_maxzoom", "20"},
                      {"amap_rank", "1"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        officialLayer.workerTessellationContext(), {poi});
    ASSERT_EQ(1u, mesh.symbols.size());
    ASSERT_EQ(TileMeshCommitResult::Committed,
              officialLayer.commitTileMesh(
                  TileKey{SchemeId("XYZ-WebMercator"), 17, 100, 200},
                  std::move(mesh)));

    const double radius = Ellipsoid::WGS84().radii().x();
    const double height = 4.0e7 / std::pow(2.0, 17.0);
    Camera officialCamera;
    officialCamera.lookAt(Vec3(radius + height, 0.0, 0.0),
                          Vec3(radius, 0.0, 0.0),
                          Vec3(0.0, 0.0, 1.0));
    officialFrame.camera = &officialCamera;
    ++officialFrame.frameId;
    RenderCommandList commands;
    officialLayer.buildRenderCommands(
        officialFrame, *officialRenderer, commands);
    const auto point = std::find_if(
        commands.begin(), commands.end(), [](const RenderCommand& command) {
            return command.kind == RenderCommandKind::VectorPoint;
        });
    ASSERT_NE(commands.end(), point);
    EXPECT_FLOAT_EQ(2.0f, point->vectorUniforms.pointSizePx);
    const auto* vb = dynamic_cast<const DummyBuffer*>(point->vertexBuffer);
    ASSERT_NE(nullptr, vb);
    ASSERT_EQ(4u * 9u * sizeof(float), vb->bytes().size());
    const auto* vertices =
        reinterpret_cast<const float*>(vb->bytes().data());
    EXPECT_LT(vertices[8], 0.0f);
    EXPECT_FLOAT_EQ(frame->u0, vertices[5]);
    EXPECT_FLOAT_EQ(frame->v1, vertices[6]);
    EXPECT_FLOAT_EQ(frame->u1, vertices[2 * 9 + 5]);
    EXPECT_FLOAT_EQ(frame->v0, vertices[2 * 9 + 6]);
    EXPECT_FLOAT_EQ(38.0f,
                    (vertices[2 * 9 + 3] - vertices[3]) *
                        point->vectorUniforms.pointSizePx);
    // Official label field 10 final display-width consumer.
    EXPECT_FLOAT_EQ(38.0f,
                    (vertices[2 * 9 + 4] - vertices[4]) *
                        point->vectorUniforms.pointSizePx);
    // Official label field 9 final display-height consumer.
}

TEST_F(FeatureRenderLayerTest,
       OfficialBuildingRejectsTransparentAndUnknownBeforeTessellation) {
    FeatureRenderStyle style = layer_->style();
    style.paintOrderExpr = StyleExpression::get("amap_draworder");
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Regions);
    FeatureRenderLayer::TessellationContext ctx{style, Ellipsoid::WGS84()};

    auto building = [](const char* subKey) {
        Feature feature = makePolygon(6.0, 29.0, 0.01);
        feature.properties["amap_class"] = "55001";
        feature.properties["amap_subkey"] = subKey;
        feature.properties["amap_draworder"] = "47";
        feature.properties["amap_height"] = "6";
        feature.properties["amap_minzoom"] = "3";
        feature.properties["amap_maxzoom"] = "20";
        return feature;
    };

    auto visible = FeatureRenderLayer::tessellateTileMesh(ctx, {building("1")});
    ASSERT_FALSE(visible.extrudeIndices.empty());
    ASSERT_EQ(1u, visible.extrudeRanges.size());
    EXPECT_EQ(47, visible.extrudeRanges[0].paintOrder);
    EXPECT_EQ(55001001, visible.extrudeRanges[0].styleGroup);

    auto inherited =
        FeatureRenderLayer::tessellateTileMesh(ctx, {building("2")});
    EXPECT_FALSE(inherited.extrudeIndices.empty());

    auto unknown =
        FeatureRenderLayer::tessellateTileMesh(ctx, {building("5")});
    EXPECT_TRUE(unknown.extrudeIndices.empty());
    EXPECT_TRUE(unknown.fillIndices.empty());

    Feature zeroHeight = building("1");
    zeroHeight.properties["amap_height"] = "0";
    auto zero = FeatureRenderLayer::tessellateTileMesh(ctx, {zeroHeight});
    EXPECT_TRUE(zero.extrudeIndices.empty());
    EXPECT_TRUE(zero.fillIndices.empty())
        << "official building height=0 must not revive planar fill";

    Feature negativeHeight = building("1");
    negativeHeight.properties["amap_height"] = "-1";
    auto negative =
        FeatureRenderLayer::tessellateTileMesh(ctx, {negativeHeight});
    EXPECT_TRUE(negative.extrudeIndices.empty());
    EXPECT_TRUE(negative.fillIndices.empty())
        << "invalid official building height must fail closed";
}

TEST_F(FeatureRenderLayerTest,
       OfficialBuildingCommitAddsRelativeHeightToSampledTerrain) {
    FeatureRenderLayer layer("official-building-terrain", &device_,
                             Ellipsoid::WGS84());
    layer.installAmapClassicProfile(
        FeatureRenderLayer::AmapClassicProfile::Main);
    layer.setTerrainSampling(makeFlatSampling(600.0f));

    Feature building = makePolygon(6.0, 29.0, 0.002);
    building.properties = {{"amap_class", "55001"},
                           {"amap_subkey", "1"},
                           {"amap_draworder", "47"},
                           {"amap_minzoom", "3"},
                           {"amap_maxzoom", "20"},
                           {"amap_height", "6"}};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(
        layer.workerTessellationContext(), {building});
    ASSERT_FALSE(mesh.extrudeVerts.empty());
    ASSERT_FALSE(mesh.extrudeClampSource.empty());
    EXPECT_EQ(mesh.extrudeClampSource.size(), mesh.extrudeVerts.size());
    const Vec3 origin = mesh.origin;
    layer.clampTileMeshForTest(mesh);
    const float* vertices = mesh.extrudeVerts.data();
    const Cartographic wallBase =
        Ellipsoid::WGS84().cartesianToCartographic(
            origin + Vec3(vertices[0], vertices[1], vertices[2]));
    const Cartographic wallTop =
        Ellipsoid::WGS84().cartesianToCartographic(
            origin + Vec3(vertices[21], vertices[22], vertices[23]));
    EXPECT_NEAR(wallBase.height(), 600.0, 1.0);
    EXPECT_NEAR(wallTop.height(), 606.0, 1.0);
}

TEST_F(FeatureRenderLayerTest, DisabledStencilFillFallsBackToSinglePassFill) {
    FeatureRenderStyle style = layer_->style();
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.terrainClampRibbon = true;  // 镜像 demo:描边走 ribbon,不进 stencil
    style.stencilFillEnabled = false;
    style.heightOffset = 2.5;
    layer_->setStyleForContractTest(style);
    layer_->store().addFeature(makePolygon(6.0, 29.0, 0.02));

    RenderCommandList commands = build();
    bool sawFill = false;
    bool sawStencil = false;
    for (const auto& cmd : commands) {
        if (cmd.kind == RenderCommandKind::VectorFill) sawFill = true;
        if (cmd.kind == RenderCommandKind::VectorStencil) sawStencil = true;
    }
    EXPECT_TRUE(sawFill);
    EXPECT_FALSE(sawStencil);  // 2-pass stencil 被单 pass fill 取代
}

// ============================================================
// 顶点预算分片:单桶线重钳逐顶点采样很贵(2026-09 真机 ~100ms/3.8万顶点),
// 改为按 kReclampVertsPerFrame 分多帧推进。本测验证分片不变量:
//   1) 分片未完成时 lineVertexBuffer 保持旧值(无半成品上屏);
//   2) 多次 build 推进后 pendingBuckets 归 0,lineVertexBuffer 被替换为
//      新高度版本(与整桶路径同一套采样逻辑 → 输出逐位一致)。
// ============================================================
TEST_F(FeatureRenderLayerTest, VertexBudgetSlicesLargeLineBucketAcrossFrames) {
    FeatureRenderLayer layer("slice", &device_, Ellipsoid::WGS84());
    FeatureRenderStyle style;
    style.altitudeMode = FeatureAltitudeMode::ClampToGround;
    style.terrainClampRibbon = true;  // E 方案 P1:线走 ribbon 单 pass
    style.clampDensifyMeters = 8.0;   // 密致 → 单桶 > kReclampVertsPerFrame
    layer.setStyleForContractTest(style);

    // 大折线(0.5°≈55km / 8m → 数千顶点),超过 2000 顶点预算。
    Feature line = makeLine(6.0, 29.0, 0.5);
    FeatureRenderLayer::TessellationContext ctx{
        style, Ellipsoid::WGS84(), nullptr, nullptr, /*stencil=*/true};
    auto mesh = FeatureRenderLayer::tessellateTileMesh(ctx, {line});
    ASSERT_FALSE(mesh.lineClampSource.empty());
    const size_t lineVerts = mesh.lineClampSource.size() / 9;
    ASSERT_GT(lineVerts, FeatureRenderLayer::kReclampVertsPerFrame)
        << "fixture must exceed the per-frame vertex budget to exercise "
           "slicing (lineVerts=" << lineVerts << ")";

    const TileKey key{"slice-test", 3, 4, 1};
    layer.commitTileMesh(key, std::move(mesh));

    // 旧高度 100,触发 revision 变化后应重钳到 700。
    auto height = std::make_shared<float>(100.0f);
    layer.setTerrainSampling(makeGenerationSampling(height));

    // 递增 heightmap generation → revision 变化 → 重钳入队。
    TileRenderContentState generationSource;
    auto changedHeightmap = std::make_unique<DecodedHeightmap>();
    changedHeightmap->tileSize = 1;
    changedHeightmap->assignHeights(std::vector<float>{700.0f});
    generationSource.setRetainedHeightmap(std::move(changedHeightmap));
    const uint64_t changedGeneration =
        TerrainHeightService::heightmapGeneration();

    // 第一次 build:观察到 generation,入队(分片只推进一段)。
    auto buildFrame = [&](double dt) {
        frame_.frameId++;
        frame_.deltaSeconds = dt;
        RenderCommandList cmds;
        layer.buildRenderCommands(frame_, *renderer_, cmds);
        return layer.terrainReclampSnapshotForTest();
    };
    auto snap0 = buildFrame(1.0 / 60.0);
    // 已入队且未完成分片时,line buffer 仍为旧版本(100)。立即拷贝字节,
    // 因为后续 build 会替换 buffer 使指针悬垂。
    const Buffer* oldLineBuffer = snap0.lineVertexBuffer;
    ASSERT_NE(nullptr, oldLineBuffer);
    const auto* oldVb = dynamic_cast<const DummyBuffer*>(oldLineBuffer);
    ASSERT_NE(nullptr, oldVb);
    const std::vector<uint8_t> oldBytes = oldVb->bytes();

    // 多次 build 推进分片直到 pending 清空。
    int guard = 0;
    auto snap = snap0;
    while (snap.pendingBuckets > 0 && guard++ < 5000) {
        snap = buildFrame(1.0 / 60.0);
    }
    ASSERT_LT(guard, 5000) << "sliced reclamp must terminate";
    ASSERT_GT(guard, 0)
        << "fixture must actually slice across frames (single-frame "
           "completion means the vertex budget was not exercised)";
    ASSERT_EQ(0u, snap.pendingBuckets);
    // 完成后 buffer 应替换为新版本(指针变化)。
    const Buffer* newLineBuffer = snap.lineVertexBuffer;
    ASSERT_NE(nullptr, newLineBuffer);

    // 新版本高度应为 700(与 HeightOnlyReclamp 同口径);旧版本应为 100。
    const auto* newVb = dynamic_cast<const DummyBuffer*>(newLineBuffer);
    ASSERT_NE(nullptr, newVb);
    EXPECT_NE(newVb->bytes(), oldBytes)
        << "sliced reclamp must replace the vertex buffer with a new-height "
           "version (no in-place partial overwrite)";
    // 完整顶点数:lineVerts × 12 float × 4B。分片不得丢顶点/重复顶点。
    EXPECT_EQ(lineVerts * 12 * 4u, newVb->bytes().size());
    // 分片最终应用到的 revision 应为递增后的 generation。
    EXPECT_EQ(changedGeneration, snap.appliedRevision);
}

// ============================================================
// 标注烘焙桶预算(2026-09):瓦片换代一帧对全部未烘桶全量 bake 实测
// 25-39ms 掉帧。kLabelBakeBucketsPerFrame=16 每帧限烘 N 桶,烘焙幂等
// (labelBakeSettled 短路 + 每帧重试),未烘桶由 hasPendingLabelWork 持续
// 供帧。本测验证:1) 单次 build 不烘完全部(预算生效,摊多帧);
// 2) 多次 build 后全部烘完(不饿死不丢标)。
// ============================================================
TEST_F(FeatureRenderLayerTest, LabelBakeBudgetSlicesLargeRebakeAcrossFrames) {
    std::vector<uint8_t> font = loadHostFont();
    if (font.empty() ||
        !renderer_->glyphAtlas()->setFontData(std::move(font))) {
        GTEST_SKIP() << "no host TrueType font for label VBO verification";
    }
    build();  // 缓存 glyph atlas(避免首个 build 混入 atlas 一次性准备)

    // 构造 > kLabelBakeBucketsPerFrame 个待烘焙桶,每个带一个标签符号。
    const size_t bucketCount = FeatureRenderLayer::kLabelBakeBucketsPerFrame + 6;
    for (size_t i = 0; i < bucketCount; ++i) {
        FeatureTileMesh mesh;
        mesh.origin = Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic(0.0, static_cast<double>(i) * 1e-5));
        mesh.hasOrigin = true;
        TileSymbolCpu symbol;
        symbol.lonRad = 0.0;
        symbol.latRad = static_cast<double>(i) * 1e-5;
        genericVisual(symbol).colorPacked = 1.0f;
        symbol.name = "N";  // 单字,减少字形依赖
        symbol.rank = 1;
        symbol.minZoom = 0;
        symbol.maxZoom = 30;
        mesh.symbols = {symbol};
        layer_->commitTileMesh(
            TileKey{SchemeId("XYZ-WebMercator"), 14,
                    static_cast<int>(100 + i), 200},
            std::move(mesh));
    }

    // 首次 build:预算生效,不应一帧烘完全部桶。
    // (host 环境 glyphAtlas 恒 needsFrame,不能靠 hasPendingLabelWork 判收敛,
    // 用已烘焙标签数 candidates 作收敛信号。)
    ++frame_.frameId;
    {
        RenderCommandList cmds;
        layer_->buildRenderCommands(frame_, *renderer_, cmds);
    }
    const int afterFirst = layer_->labelPlacementStats().candidates;
    EXPECT_LT(static_cast<size_t>(afterFirst), bucketCount)
        << ">16 pending label buckets must not all bake in one frame "
           "(kLabelBakeBucketsPerFrame budget); got candidates="
        << afterFirst;

    // 多次 build 推进:每次重算 placement,候选应单调逼近 bucketCount。
    int guard = 0;
    int candidates = afterFirst;
    while (candidates < static_cast<int>(bucketCount) && guard++ < 2000) {
        ++frame_.frameId;
        RenderCommandList cmds;
        layer_->buildRenderCommands(frame_, *renderer_, cmds);
        candidates = layer_->labelPlacementStats().candidates;
    }
    ASSERT_LT(guard, 2000)
        << "label bake must terminate; candidates=" << candidates
        << " target=" << bucketCount;
    EXPECT_EQ(static_cast<int>(bucketCount), candidates)
        << "after sufficient frames every label bucket must be baked";
}
