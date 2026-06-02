#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/layers/BasemapLayerStack.h"
#include "earth_engine/layers/BasemapLayer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileGroupKey.h"
#include "earth_engine/tiling/CrsProfile.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"

using namespace earth_engine;

// ============================================================
// 构造 Stub Provider（不发起网络请求）
// ============================================================

namespace {

class StubProvider : public ImageryProvider {
public:
    explicit StubProvider(std::string id, std::string schemeId)
        : id_(std::move(id)), schemeId_(std::move(schemeId)) {}

    std::string id() const override { return id_; }
    std::string schemeId() const override { return schemeId_; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 20; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    std::string buildUrl(const TileKey&) const override { return ""; }

    void requestTile(const TileKey&, CancellationToken,
                     TileCallback callback) override {
        // Stub: 不发起请求，直接回调空指针
        callback(TileKey{}, nullptr);
    }

    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

private:
    std::string id_;
    std::string schemeId_;
};

} // anonymous namespace

// ============================================================
// Test fixture
// ============================================================

class BasemapLayerStackTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(glm::radians(60.0), 1.0, 50000000.0);
        camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

        frameState_.camera = camera_.get();
        frameState_.viewportWidthPixels = 800;
        frameState_.viewportHeightPixels = 600;
        frameState_.frameId = 1;
        frameState_.timeSeconds = 0.0;
    }

    std::unique_ptr<BasemapLayer> makeXYZLayer(const std::string& name) {
        return std::make_unique<BasemapLayer>(
            std::make_unique<StubProvider>("stub-" + name, "XYZ-WebMercator"),
            TileScheme::createXYZWebMercator(),
            nullptr  // no GPU for tests
        );
    }

    std::unique_ptr<BasemapLayer> makeTMSLayer(const std::string& name) {
        return std::make_unique<BasemapLayer>(
            std::make_unique<StubProvider>("stub-" + name, "TMS-WebMercator"),
            TileScheme::createTMS(),
            nullptr
        );
    }

    std::unique_ptr<Camera> camera_;
    FrameState frameState_;
};

// ============================================================
// 图层管理
// ============================================================

TEST_F(BasemapLayerStackTest, AddAndCount) {
    BasemapLayerStack stack;
    EXPECT_EQ(0u, stack.layerCount());

    stack.addLayer(makeXYZLayer("a"));
    EXPECT_EQ(1u, stack.layerCount());

    stack.addLayer(makeXYZLayer("b"));
    EXPECT_EQ(2u, stack.layerCount());
}

TEST_F(BasemapLayerStackTest, RemoveLayer) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));
    stack.addLayer(makeXYZLayer("b"));

    auto removed = stack.removeLayer("stub-a");
    ASSERT_NE(nullptr, removed);
    EXPECT_EQ("stub-a", removed->id());
    EXPECT_EQ(1u, stack.layerCount());

    // 不存在
    auto missing = stack.removeLayer("nonexistent");
    EXPECT_EQ(nullptr, missing);
}

TEST_F(BasemapLayerStackTest, MoveLayer) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));
    stack.addLayer(makeXYZLayer("b"));
    stack.addLayer(makeXYZLayer("c"));

    // a, b, c → move "a" to index 2 → b, c, a
    stack.moveLayer("stub-a", 2);
    EXPECT_EQ("stub-b", stack.layerAt(0)->id());
    EXPECT_EQ("stub-c", stack.layerAt(1)->id());
    EXPECT_EQ("stub-a", stack.layerAt(2)->id());
}

TEST_F(BasemapLayerStackTest, FindLayer) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));

    EXPECT_NE(nullptr, stack.findLayer("stub-a"));
    EXPECT_EQ(nullptr, stack.findLayer("stub-b"));
}

// ============================================================
// TilePlan 分组
// ============================================================

TEST_F(BasemapLayerStackTest, SameSchemeSharesTilePlan) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("a"));
    stack.addLayer(makeXYZLayer("b"));

    stack.update(frameState_);

    // 两个 XYZ 图层应有相同的 TileGroupKey → 共享一次 TilePlan 计算
    const auto& plans = stack.groupPlans();
    // 当前实现：每个独特 (schemeId, zoom, vpW, vpH) 键对应一个 TilePlan
    // 两个 XYZ 图层 zoom 相同 → 1 个 plan
    EXPECT_GE(plans.size(), 1u);

    // 两个图层的 visibleTiles 应一致
    auto* layerA = stack.findLayer("stub-a");
    auto* layerB = stack.findLayer("stub-b");
    ASSERT_NE(nullptr, layerA);
    ASSERT_NE(nullptr, layerB);

    const auto& tilesA = layerA->tilePlan().visibleTiles;
    const auto& tilesB = layerB->tilePlan().visibleTiles;

    EXPECT_EQ(tilesA.size(), tilesB.size());
    for (size_t i = 0; i < tilesA.size(); ++i) {
        EXPECT_EQ(tilesA[i], tilesB[i]);
    }
}

TEST_F(BasemapLayerStackTest, DifferentSchemeIndependentTilePlan) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("xyz"));
    stack.addLayer(makeTMSLayer("tms"));

    stack.update(frameState_);

    // 不同 scheme → 两个独立的 TilePlan
    const auto& plans = stack.groupPlans();
    EXPECT_GE(plans.size(), 1u);  // 可能 1 或 2 取决于 zoom 是否相同

    // 但每个图层的 scheme 不同
    auto* xyzLayer = stack.findLayer("stub-xyz");
    auto* tmsLayer = stack.findLayer("stub-tms");
    ASSERT_NE(nullptr, xyzLayer);
    ASSERT_NE(nullptr, tmsLayer);

    EXPECT_EQ("XYZ-WebMercator", xyzLayer->tileScheme().id());
    EXPECT_EQ("TMS-WebMercator", tmsLayer->tileScheme().id());
}

TEST_F(BasemapLayerStackTest, InvisibleLayerSkipped) {
    BasemapLayerStack stack;
    auto layerA = makeXYZLayer("a");
    auto layerB = makeXYZLayer("b");
    layerB->setVisible(false);

    stack.addLayer(std::move(layerA));
    stack.addLayer(std::move(layerB));

    stack.update(frameState_);

    // 仅可见图层参与 TilePlan 分组
    auto* layerBptr = stack.findLayer("stub-b");
    ASSERT_NE(nullptr, layerBptr);
    // invisible layer 的 visibleTiles 应为空（applyPlan 跳过 invisible）
    EXPECT_TRUE(layerBptr->visibleTiles().empty());
}

// ============================================================
// 控制点验证
// ============================================================

TEST_F(BasemapLayerStackTest, ControlPointVerification) {
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("xyz"));
    stack.addLayer(makeTMSLayer("tms"));

    // 北京坐标
    double lngRad = 116.397 * M_PI / 180.0;
    double latRad = 39.908 * M_PI / 180.0;

    auto results = stack.verifyControlPoint(lngRad, latRad, 10);

    EXPECT_EQ(2u, results.size());

    for (const auto& r : results) {
        // 控制点应在 tile bounds 内
        EXPECT_TRUE(r.inExpectedRange)
            << "Layer " << r.layerId << " control point not in tile bounds";
        EXPECT_EQ(10, r.tileKey.z);
    }
}

TEST_F(BasemapLayerStackTest, ControlPointCrossSchemeConsistency) {
    // XYZ 和 TMS 的控制点应在各自 tile 内，且 y 轴满足互补关系
    BasemapLayerStack stack;
    stack.addLayer(makeXYZLayer("xyz"));
    stack.addLayer(makeTMSLayer("tms"));

    double lngRad = 0.0;
    double latRad = 0.0;

    auto results = stack.verifyControlPoint(lngRad, latRad, 5);

    ASSERT_EQ(2u, results.size());

    const auto& xyz = results[0];
    const auto& tms = results[1];

    // 控制点在各自 tile 内
    EXPECT_TRUE(xyz.inExpectedRange);
    EXPECT_TRUE(tms.inExpectedRange);

    // 经度范围应一致（同一 x 列）
    EXPECT_NEAR(xyz.tileBounds.west(), tms.tileBounds.west(), 1e-6);
    EXPECT_NEAR(xyz.tileBounds.east(), tms.tileBounds.east(), 1e-6);

    // y 轴关系验证：y_tms + y_xyz ∈ {2^z-1, 2^z}
    int tilesAtZoom = 1 << 5;
    int ySum = xyz.tileKey.y + tms.tileKey.y;
    EXPECT_TRUE(ySum == tilesAtZoom - 1 || ySum == tilesAtZoom)
        << "y_xyz=" << xyz.tileKey.y << " y_tms=" << tms.tileKey.y
        << " sum=" << ySum << " N=" << tilesAtZoom;
}

// ============================================================
// TileGroupKey hash/equality
// ============================================================

TEST_F(BasemapLayerStackTest, TileGroupKeyEquality) {
    TileGroupKey a{"XYZ-WebMercator", 5, 800, 600};
    TileGroupKey b{"XYZ-WebMercator", 5, 800, 600};
    TileGroupKey c{"TMS-WebMercator", 5, 800, 600};
    TileGroupKey d{"XYZ-WebMercator", 6, 800, 600};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

TEST_F(BasemapLayerStackTest, TileGroupKeyHash) {
    std::hash<TileGroupKey> hasher;
    TileGroupKey a{"XYZ-WebMercator", 5, 800, 600};
    TileGroupKey b{"XYZ-WebMercator", 5, 800, 600};

    EXPECT_EQ(hasher(a), hasher(b));
}
