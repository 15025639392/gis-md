// Selector 对拍差分测试 —— gis-md 回放侧（见 tools/selector_diff/DESIGN.md）。
//
// 按 tools/selector_diff/scenarios.h 的 S1 规格展开 4 层满四叉树（85 瓦片）
// 与 12 帧 nadir 阶梯下降相机，逐帧跑 gis-md 选择管线并用共享 traceLine()
// 生成轨迹行：
//   - golden/s1.trace 存在 → 逐帧 EXPECT_EQ（行数也断言）；
//   - 不存在 → GTEST_SKIP（golden 由 cesium 侧生成器离线产出后提交入库）；
//   - 环境变量 SELECTOR_DIFF_DUMP=<path> → 无论 golden 是否存在都把 gis-md
//     轨迹完整写到该文件，便于人工 diff。
//
// 选择复用（TileSelectionReusePolicy，DESIGN 白名单 5）在本驱动下天然禁用：
// TilesetSelectionFrameFacade::selectTiles 直接走 TileSelectionFrameRunner
// 全遍历，不经过 Tileset::update / TileFrameWorkCoordinator 的 reuse 分类。
// 每帧 counters.visited > 0（含重复相机帧）作为复用未生效的回归证据。
//
// kicked 计数已对齐 cesium 语义（TilesetSelection.cpp:756，bfc2c574c）：
// 仅在 notYetRenderableCount > loadingDescendantLimit 的 restore 分支按
// 恢复移除的 load-queue 条数累加（TileSelectionPostTraversalCommitter），
// 普通 kick（父替子渲染）不计数。全行 byte 级严格对拍，无豁免字段。

#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileLoadQueue.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileSelectionCounters.h"
#include "earth_engine/tiling/TileSelectionRootPolicy.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetSelectionFrameFacade.h"

#include "scenarios.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static TilesetTile* findTile(Tileset& tileset, const TileKey& key) {
        return tileset.tileRegistry_.findTile(key);
    }

    static void ensureTileChildren(Tileset& tileset, TilesetTile& tile) {
        tileset.contentAccess_.ensureTileChildren(tile);
    }

    static const TileLoadQueue& loadQueue(const Tileset& tileset) {
        return tileset.loadQueue_;
    }

    static const TileSelectionCounters& selectionCounters(
        const Tileset& tileset) {
        return tileset.selectionCounters_;
    }

    static void selectTiles(Tileset& tileset, const FrameState& frameState) {
        TilesetSelectionFrameFacade::selectTiles(tileset, frameState);
    }

    static void setLastCamera(
        Tileset& tileset,
        const Vec3& position,
        const Vec3& direction) {
        tileset.lastCameraPosition_ = position;
        tileset.lastCameraDirection_ = direction;
    }
};
} // namespace earth_engine

namespace {

#ifndef SELECTOR_DIFF_GOLDEN_DIR
#define SELECTOR_DIFF_GOLDEN_DIR ""
#endif

constexpr const char* kSchemeId = "Geographic-TMS";

/// 由 QuadtreeSpec 程序化枚举整棵满四叉树的内容 provider
/// （仿 test_tileset_selection_refinement.cpp 的 SelectionTreeContentProvider，
/// 但树由规格递推而非逐瓦片列表）。叶子（z == maxDepth）childTiles 返回空。
/// rootTiles 只返回 {z=0,x=0,y=0} 单根；supportsTile 拒绝树外 key（含
/// Geographic-TMS scheme 的第二个 z0 根 {0,1,0}），保证 gis-md 不会物化它。
class QuadtreeSpecContentProvider final : public TilesetContentProvider {
public:
    explicit QuadtreeSpecContentProvider(const selector_diff::QuadtreeSpec& tree)
        : tree_(tree) {}

    std::string id() const override { return "selector-diff-quadtree"; }

    bool supportsTile(const TileKey& key) const override {
        return key.schemeId == kSchemeId && key.z >= 0 &&
               key.z <= tree_.maxDepth && key.x >= 0 &&
               key.x < (1 << key.z) && key.y >= 0 && key.y < (1 << key.z);
    }

    std::vector<TileKey> rootTiles() const override {
        return {TileKey{kSchemeId, 0, 0, 0}};
    }

    std::vector<TileKey> childTiles(const TileKey& key) const override {
        if (!supportsTile(key) || key.z >= tree_.maxDepth) {
            return {};
        }
        std::vector<TileKey> children;
        children.reserve(4);
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                children.push_back(TileKey{
                    kSchemeId,
                    key.z + 1,
                    2 * key.x + dx,
                    2 * key.y + dy});
            }
        }
        return children;
    }

    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        // 加载完成由测试驱动在帧末显式推进（DESIGN 加载模型），
        // 真实请求路径不参与。
        callback(key, TileContentLoadResult::retryLater());
    }

    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

private:
    selector_diff::QuadtreeSpec tree_;
};

SelectorView makeSelectorView(
    const Camera& camera,
    int viewportWidth,
    int viewportHeight) {
    SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    const double width = static_cast<double>(viewportWidth);
    const double height = static_cast<double>(viewportHeight);
    view.projectionMatrix = camera.projectionMatrix(width, height);
    view.frustum = Frustum::fromViewProjection(
        view.projectionMatrix * camera.viewMatrix());
    view.viewportHeightPixels = viewportHeight;
    return view;
}

TilesetOptions makeS1TilesetOptions(const selector_diff::OptionsSpec& spec) {
    TilesetOptions options;
    options.maximumScreenSpaceError = spec.maximumScreenSpaceError;
    options.enableFrustumCulling = spec.enableFrustumCulling;
    options.enableFogCulling = spec.enableFogCulling;
    options.enableOcclusionCulling = spec.enableOcclusionCulling;
    options.enableLodTransitionPeriod = spec.enableLodTransitionPeriod;
    options.forbidHoles = spec.forbidHoles;
    options.loadingDescendantLimit = spec.loadingDescendantLimit;
    options.maximumSimultaneousTileLoads = spec.maximumSimultaneousTileLoads;
    options.preloadAncestors = spec.preloadAncestors;
    options.preloadSiblings = spec.preloadSiblings;
    options.enforceCulledScreenSpaceError = spec.enforceCulledScreenSpaceError;
    options.culledScreenSpaceError = spec.culledScreenSpaceError;
    options.fogDensityTable.clear();
    for (const selector_diff::FogEntry& entry : selector_diff::kFogTable) {
        options.fogDensityTable.push_back({entry.height, entry.density});
    }
    return options;
}

/// 物化整棵 S1 树并按场景规格逐瓦片覆写属性。
/// 区域配对：子 region 由 childRegion(parent, dx, dy) 递推，dx/dy 从
/// provider 返回的子 key 反推（dx = child.x - 2*parent.x），不依赖 scheme
/// 的 y 方向约定——key (2y+dy) 与显式计算的 bounds 恒定配对。
int materializeS1Tree(Tileset& tileset, const selector_diff::QuadtreeSpec& tree) {
    struct PendingTile {
        TilesetTile* tile;
        selector_diff::RegionSpec region;
    };

    const TileKey rootKey{kSchemeId, 0, 0, 0};
    EXPECT_FALSE(TileSelectionRootPolicy::isVirtualTerrainRoot(rootKey));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    EXPECT_NE(root, nullptr);
    if (!root) {
        return 0;
    }

    int materialized = 0;
    std::vector<PendingTile> stack{
        {root,
         selector_diff::RegionSpec{
             tree.west,
             tree.south,
             tree.east,
             tree.north}}};
    while (!stack.empty()) {
        PendingTile entry = stack.back();
        stack.pop_back();
        TilesetTile* tile = entry.tile;

        // Rectangle(west, south, east, north) —— 与 RegionSpec 字段同序。
        tile->bounds = Rectangle(
            entry.region.west,
            entry.region.south,
            entry.region.east,
            entry.region.north);
        tile->geometricError =
            selector_diff::geometricErrorForLevel(tree, tile->key.z);
        tile->refine = TileRefine::Replace;
        tile->boundingVolume =
            TileBoundingVolume::fromRegion(tile->bounds, 0.0, 0.0);
        tile->content.loadState = TileLoadState::Unloaded;
        tile->content.contentKind = TileContentKind::Unknown;
        ++materialized;

        if (tile->key.z >= tree.maxDepth) {
            continue;
        }
        TilesetTestAccess::ensureTileChildren(tileset, *tile);
        for (TilesetTile* child : tile->children) {
            EXPECT_NE(child, nullptr);
            if (!child) {
                continue;
            }
            const int dx = child->key.x - 2 * tile->key.x;
            const int dy = child->key.y - 2 * tile->key.y;
            EXPECT_TRUE(dx == 0 || dx == 1);
            EXPECT_TRUE(dy == 0 || dy == 1);
            stack.push_back(PendingTile{
                child,
                selector_diff::childRegion(entry.region, dx, dy)});
        }
    }
    return materialized;
}

struct S1FrameResult {
    std::string traceLine;
    std::vector<std::string> renderKeys;
    int visited = 0;
};

/// 逐帧跑 S1 场景，返回逐帧轨迹（traceLine 由 scenarios.h 共享函数拼行）。
std::vector<S1FrameResult> runS1Scenario() {
    const selector_diff::QuadtreeSpec& tree = selector_diff::kS1Tree;
    const selector_diff::OptionsSpec& spec = selector_diff::kS1Options;

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        makeS1TilesetOptions(spec),
        std::make_unique<QuadtreeSpecContentProvider>(tree));

    const int materialized = materializeS1Tree(tileset, tree);
    EXPECT_EQ(materialized, 85);

    const int viewportWidth = static_cast<int>(spec.viewportWidth);
    const int viewportHeight = static_cast<int>(spec.viewportHeight);

    std::vector<S1FrameResult> results;
    for (size_t frame = 0; frame < selector_diff::kS1Frames.size(); ++frame) {
        const selector_diff::CameraFrameSpec& frameSpec =
            selector_diff::kS1Frames[frame];
        const selector_diff::Vec3d posD = selector_diff::cartographicToEcef(
            frameSpec.lonRad,
            frameSpec.latRad,
            frameSpec.heightMeters);
        const selector_diff::Vec3d dirD = selector_diff::nadirDirection(posD);
        const selector_diff::Vec3d upD =
            selector_diff::nadirUpLocalNorth(posD);

        Camera camera;
        // near/far 取 0.1 / 1e8：gis-md Frustum 含 near/far 平面，取极小
        // near/极大 far 使其在场景内永不参与剔除（DESIGN 白名单 6）。
        camera.setPerspective(spec.verticalFovRadians, 0.1, 1e8);
        camera.setView(
            Vec3(posD.x, posD.y, posD.z),
            Vec3(dirD.x, dirD.y, dirD.z),
            Vec3(upD.x, upD.y, upD.z));

        FrameState frameState;
        frameState.frameId = static_cast<uint64_t>(frame) + 1;
        frameState.camera = &camera;
        frameState.viewportWidthPixels = viewportWidth;
        frameState.viewportHeightPixels = viewportHeight;
        frameState.selectorViews.push_back(
            makeSelectorView(camera, viewportWidth, viewportHeight));
        TilesetTestAccess::setLastCamera(
            tileset,
            camera.position(),
            camera.direction());
        TilesetTestAccess::selectTiles(tileset, frameState);

        std::vector<std::string> renderKeys;
        for (const TileKey& key : tileset.tilePlan().visibleTiles) {
            renderKeys.push_back(
                selector_diff::tileKeyString(key.z, key.x, key.y));
        }

        // loads = 本帧 loadQueue 中仍 Unloaded 的瓦片 key 去重集合；
        // 随后帧末完成加载（Done + Empty），下一帧才可渲染（DESIGN 加载模型）。
        std::set<std::string> loadKeySet;
        std::vector<TilesetTile*> loadedThisFrame;
        for (const TileLoadRequest& request :
             TilesetTestAccess::loadQueue(tileset)) {
            TilesetTile* tile =
                TilesetTestAccess::findTile(tileset, request.key);
            if (!tile || tile->content.loadState != TileLoadState::Unloaded) {
                continue;
            }
            const bool inserted =
                loadKeySet
                    .insert(selector_diff::tileKeyString(
                        request.key.z,
                        request.key.x,
                        request.key.y))
                    .second;
            if (inserted) {
                loadedThisFrame.push_back(tile);
            }
        }
        for (TilesetTile* tile : loadedThisFrame) {
            tile->content.loadState = TileLoadState::Done;
            tile->content.contentKind = TileContentKind::Empty;
        }

        const TileSelectionCounters& counters =
            TilesetTestAccess::selectionCounters(tileset);
        S1FrameResult result;
        result.renderKeys = renderKeys;
        result.visited = counters.visited;
        result.traceLine = selector_diff::traceLine(
            static_cast<int>(frame),
            std::move(renderKeys),
            std::vector<std::string>(loadKeySet.begin(), loadKeySet.end()),
            counters.visited,
            counters.culled,
            counters.culledVisited,
            counters.kicked);
        results.push_back(std::move(result));
    }
    return results;
}

std::vector<std::string> traceLines(const std::vector<S1FrameResult>& results) {
    std::vector<std::string> lines;
    lines.reserve(results.size());
    for (const S1FrameResult& result : results) {
        lines.push_back(result.traceLine);
    }
    return lines;
}

} // namespace

TEST(SelectorCesiumGoldenDiffTest, S1TraceMatchesCesiumGolden) {
    const std::vector<S1FrameResult> results = runS1Scenario();
    const std::vector<std::string> lines = traceLines(results);
    ASSERT_EQ(lines.size(), selector_diff::kS1Frames.size());

    // 确定性：同一场景独立重跑 byte 级一致。
    {
        const std::vector<std::string> rerunLines =
            traceLines(runS1Scenario());
        ASSERT_EQ(rerunLines.size(), lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            EXPECT_EQ(lines[i], rerunLines[i]) << "frame " << i
                                               << " 两次运行不一致";
        }
    }

    // 轨迹合理性（golden 缺失时也生效）：
    // 1) 每帧 visited > 0 —— 选择复用未生效（facade 全遍历）的回归证据；
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_GT(results[i].visited, 0)
            << "frame " << i << " visited=0，疑似选择复用被引入 facade 路径";
    }
    // 2) 相机降至 200km 后终态应细化到叶层（z=3）。
    const std::vector<std::string>& finalRender = results.back().renderKeys;
    bool finalHasLeaf = false;
    for (const std::string& key : finalRender) {
        if (key.rfind("3-", 0) == 0) {
            finalHasLeaf = true;
            break;
        }
    }
    EXPECT_TRUE(finalHasLeaf)
        << "尾帧未渲染任何 z=3 叶瓦片：" << results.back().traceLine;

    // SELECTOR_DIFF_DUMP=<path>：无论 golden 是否存在都写出完整轨迹。
    if (const char* dumpPath = std::getenv("SELECTOR_DIFF_DUMP")) {
        std::ofstream dump(dumpPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(dump.is_open())
            << "无法写 SELECTOR_DIFF_DUMP 文件: " << dumpPath;
        for (const std::string& line : lines) {
            dump << line << "\n";
        }
    }

    const std::filesystem::path goldenPath =
        std::filesystem::path(SELECTOR_DIFF_GOLDEN_DIR) / "s1.trace";
    if (std::string(SELECTOR_DIFF_GOLDEN_DIR).empty() ||
        !std::filesystem::exists(goldenPath)) {
        GTEST_SKIP() << "golden 未生成（" << goldenPath.string()
                     << " 不存在），跳过对拍；cesium 侧生成器提交 golden "
                        "后本测试自动生效";
    }

    std::ifstream golden(goldenPath, std::ios::binary);
    ASSERT_TRUE(golden.is_open()) << "无法读 golden: " << goldenPath.string();
    std::vector<std::string> goldenLines;
    std::string line;
    while (std::getline(golden, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        goldenLines.push_back(line);
    }
    while (!goldenLines.empty() && goldenLines.back().empty()) {
        goldenLines.pop_back();
    }

    ASSERT_EQ(goldenLines.size(), lines.size())
        << "golden 帧数与 gis-md 轨迹帧数不一致";
    for (size_t i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i], goldenLines[i])
            << "frame " << i << " 轨迹不一致\n  gis-md: " << lines[i]
            << "\n  golden: " << goldenLines[i];
    }
}
