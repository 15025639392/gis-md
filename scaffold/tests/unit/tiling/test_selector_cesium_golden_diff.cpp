// Selector 对拍差分测试 —— gis-md 回放侧（见 tools/selector_diff/DESIGN.md）。
//
// 按 tools/selector_diff/scenarios.h 的场景规格展开 4 层满四叉树（85 瓦片）
// 与 12 帧相机序列，逐帧跑 gis-md 选择管线并用共享 traceLine() 生成轨迹行。
// 四场景参数化（各自独立 TEST，对 golden/<scenario>.trace）：
//   - S1：nadir 阶梯下降（8000km→200km），D=1；
//   - S2：冷启动深观察（12 帧全 400km，loadingDescendantLimit=8，
//     覆盖 restore/kick 计数面），D=1；
//   - S3：S2 相机/选项 + 加载延迟 D=2（restore/kick 多帧渐进收敛面，
//     覆盖 wasReallyRenderedLastFrame 时序）；
//   - S4：fog 边界（region 南缘低空 80° 俯仰朝北看地平线，800m→320km，
//     穿越 fog 必然剔除/临界/不可达三段），D=1。
// 相机统一由 scenarios.h 的 pitchedDirection/pitchedUp 构造（pitch=0 与
// nadir 逐位一致）；kS1CenterLon 含 +0.0037 rad 偏移消灭精确并列优先级
// （DESIGN 白名单 7——并列项顺序是 stdlib introsort 产物，无对拍语义）。
// 各 TEST：golden 存在 → 逐帧 EXPECT_EQ（行数也断言）；不存在 →
// GTEST_SKIP；SELECTOR_DIFF_DUMP=<path> → 无论 golden 是否存在都把轨迹
// 写到 <path>.<scenario>（四文件），便于人工 diff。
//
// loads 保序（P3）：gis-md 侧 loads 顺序 = 生产分发排序——对 selectTiles
// 后的 loadQueue_ 快照复制，用生产 TileLoadPriorityPolicy::sortByPriority
// （TileLoadScheduler.h requestMissingTiles 同款）排序，再按 Unloaded
// 过滤 + 保序去重（保留首次出现 = 该 key 最高优先级实例）。
//
// culled 映射（P3）：cesium 把 fog 剔除计入 tilesCulled（无独立 fog
// 计数），gis-md 的 counters.culled 只含 frustum 剔除、fogCulled 独立
// ——trace 的 culled 字段 = counters.culled + counters.fogCulled
// （所有场景统一；S1/S2/S3 相机高度下 fogCulled 恒 0，不影响旧轨迹）。
//
// 加载模型（时序契约见 scenarios.h S3 注释，D=loadDelayFrames）：
//   1. 帧 N 选择前：完成"发起帧 + D <= N"的请求（loadState=Done）；
//   2. selectTiles；
//   3. 发起本帧请求并记录（loads 输出）：post-restore 存活队列中仍
//      Unloaded 的瓦片；发起后置 ContentLoading（与 cesium 在途请求的
//      tile 状态一致，天然防止延迟窗口内重复发起）。
//   D=1 等价于 P1 的"帧 N 请求 → 帧 N+1 可渲染"。
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
#include "earth_engine/tiling/TileLoadPriorityPolicy.h"
#include "earth_engine/tiling/TileLoadQueue.h"
#include "earth_engine/tiling/TileLoadTypes.h"
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

/// 场景参数包：name 同时决定 golden 文件名（golden/<name>.trace）与
/// SELECTOR_DIFF_DUMP 导出后缀（<path>.<name>）。loadDelayFrames = 帧 N
/// 发起的请求在帧 N+D 的选择开始前完成（scenarios.h S3 时序契约）。
struct ScenarioSpec {
    std::string name;
    const selector_diff::QuadtreeSpec& tree;
    const selector_diff::OptionsSpec& options;
    std::vector<selector_diff::CameraFrameSpec> frames;
    int loadDelayFrames = 1;
    // 尾帧应渲染 z=maxDepth 叶（golden 缺失时的合理性断言）。S4 尾帧是
    // 320km 倾斜相机看地平线，叶层覆盖非场景意图，不作此断言。
    bool expectFinalLeaf = true;
    // asyncSelection=true 时走影子树选择路径；对同一 golden 逐帧断言，
    // 证明"影子选择 == 同步选择 == cesium golden"。
    bool asyncSelection = false;
    // incrementalSelection=true 时走 ③ 增量切面路径;不与 cesium golden trace
    // 对拍(load issue-order 可能不同),只靠 runScenario 内每帧 §8 oracle
    // 断言「增量==全量」验证。
    bool incrementalSelection = false;
};

// 返回同场景的 asyncSelection 变体（走影子树 + reconcile 回 live）。
ScenarioSpec asyncOf(ScenarioSpec scenario) {
    scenario.asyncSelection = true;
    return scenario;
}

// 返回同场景的 ③ 增量变体（走增量路径;§8 oracle 逐帧验证增量==全量）。
ScenarioSpec incrementalOf(ScenarioSpec scenario) {
    scenario.incrementalSelection = true;
    return scenario;
}

ScenarioSpec makeS1Scenario() {
    return ScenarioSpec{
        "s1",
        selector_diff::kS1Tree,
        selector_diff::kS1Options,
        {selector_diff::kS1Frames.begin(), selector_diff::kS1Frames.end()},
        1};
}

ScenarioSpec makeS2Scenario() {
    return ScenarioSpec{
        "s2",
        selector_diff::kS1Tree,  // S2 复用 S1 树
        selector_diff::kS2Options,
        {selector_diff::kS2Frames.begin(), selector_diff::kS2Frames.end()},
        1};
}

ScenarioSpec makeS3Scenario() {
    return ScenarioSpec{
        "s3",
        selector_diff::kS1Tree,  // S3 复用 S1 树 + S2 相机/选项
        selector_diff::kS3Options,
        {selector_diff::kS3Frames.begin(), selector_diff::kS3Frames.end()},
        selector_diff::kS3LoadDelayFrames};
}

ScenarioSpec makeS4Scenario() {
    return ScenarioSpec{
        "s4",
        selector_diff::kS1Tree,  // S4 复用 S1 树
        selector_diff::kS4Options,
        {selector_diff::kS4Frames.begin(), selector_diff::kS4Frames.end()},
        1,
        /*expectFinalLeaf=*/false};
}

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
        // 加载完成由测试驱动按时序契约显式推进（DESIGN 加载模型），
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

TilesetOptions makeTilesetOptions(const selector_diff::OptionsSpec& spec) {
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

int expectedTileCount(const selector_diff::QuadtreeSpec& tree) {
    int count = 0;
    for (int z = 0; z <= tree.maxDepth; ++z) {
        count += 1 << (2 * z);
    }
    return count;
}

/// 物化整棵场景树并按规格逐瓦片覆写属性。
/// 区域配对：子 region 由 childRegion(parent, dx, dy) 递推，dx/dy 从
/// provider 返回的子 key 反推（dx = child.x - 2*parent.x），不依赖 scheme
/// 的 y 方向约定——key (2y+dy) 与显式计算的 bounds 恒定配对。
int materializeScenarioTree(
    Tileset& tileset,
    const selector_diff::QuadtreeSpec& tree) {
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

struct ScenarioFrameResult {
    std::string traceLine;
    std::vector<std::string> renderKeys;
    int visited = 0;
    int fogCulled = 0;
};

/// 逐帧跑场景，返回逐帧轨迹（traceLine 由 scenarios.h 共享函数拼行）。
std::vector<ScenarioFrameResult> runScenario(const ScenarioSpec& scenario) {
    const selector_diff::QuadtreeSpec& tree = scenario.tree;
    const selector_diff::OptionsSpec& spec = scenario.options;

    TilesetOptions tilesetOptions = makeTilesetOptions(spec);
    tilesetOptions.asyncSelection = scenario.asyncSelection;
    tilesetOptions.incrementalSelection = scenario.incrementalSelection;
    // §8 等价性 oracle:sync 场景每帧额外跑一遍全量参考并断言逐位相同
    // (full==full 自证 oracle 接线正确;async 场景 facade 早退不触发,恒空)。
    tilesetOptions.verifySelectionEquivalence = true;
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        std::move(tilesetOptions),
        std::make_unique<QuadtreeSpecContentProvider>(tree));

    const int materialized = materializeScenarioTree(tileset, tree);
    EXPECT_EQ(materialized, expectedTileCount(tree));

    const int viewportWidth = static_cast<int>(spec.viewportWidth);
    const int viewportHeight = static_cast<int>(spec.viewportHeight);

    // 在途请求：帧 N 发起 → 帧 N+D 选择前完成（scenarios.h S3 时序契约）。
    struct PendingLoadBatch {
        size_t issueFrame;
        std::vector<TilesetTile*> tiles;
    };
    std::vector<PendingLoadBatch> pendingLoads;

    std::vector<ScenarioFrameResult> results;
    for (size_t frame = 0; frame < scenario.frames.size(); ++frame) {
        // 1) 完成到期请求（发起帧 + D <= 当前帧）。
        for (auto it = pendingLoads.begin(); it != pendingLoads.end();) {
            if (it->issueFrame +
                    static_cast<size_t>(scenario.loadDelayFrames) <=
                frame) {
                for (TilesetTile* tile : it->tiles) {
                    tile->content.loadState = TileLoadState::Done;
                    tile->content.contentKind = TileContentKind::Empty;
                }
                it = pendingLoads.erase(it);
            } else {
                ++it;
            }
        }

        // 2) 选择。
        const selector_diff::CameraFrameSpec& frameSpec =
            scenario.frames[frame];
        const selector_diff::Vec3d posD = selector_diff::cartographicToEcef(
            frameSpec.lonRad,
            frameSpec.latRad,
            frameSpec.heightMeters);
        // 所有场景统一用共享 pitchedDirection/pitchedUp（pitch=0 与旧
        // nadir 构造逐位一致；S4 为 80° 朝北看地平线的倾斜相机）。
        const selector_diff::Vec3d dirD =
            selector_diff::pitchedDirection(posD, frameSpec.pitchRad);
        const selector_diff::Vec3d upD =
            selector_diff::pitchedUp(posD, frameSpec.pitchRad);

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
        // §8 oracle:sync 场景下 live 全量选择必须与 pre-selection 影子全量
        // 逐位相同(空=相等)。这把 12 帧×每 sync 场景变成 oracle 自证。
        EXPECT_TRUE(tileset.selectionEquivalenceMismatch().empty())
            << "frame " << (frame + 1) << " §8 mismatch: "
            << tileset.selectionEquivalenceMismatch();
        // ③ Layer 1:增量路径捕获的子树缓存必须覆盖每个 visibleTile
        //(每个渲染瓦片都被访问过 → 有 frontier 条目)。证明捕获真跑了、非空。
        if (scenario.incrementalSelection) {
            for (const TileKey& key : tileset.tilePlan().visibleTiles) {
                EXPECT_NE(tileset.incrementalFrontier().find(key), nullptr)
                    << "frame " << (frame + 1)
                    << " 增量缓存缺 visibleTile 贡献: "
                    << selector_diff::tileKeyString(key.z, key.x, key.y);
            }
        }

        std::vector<std::string> renderKeys;
        for (const TileKey& key : tileset.tilePlan().visibleTiles) {
            renderKeys.push_back(
                selector_diff::tileKeyString(key.z, key.x, key.y));
        }

        // 3) 发起本帧请求并记录（loads 保序输出，P3）：
        //    对 post-restore 存活的 loadQueue 快照复制，用生产
        //    TileLoadPriorityPolicy::sortByPriority（TileLoadScheduler.h
        //    requestMissingTiles 同款）排序 = 分发实际发起顺序；再过滤仍
        //    Unloaded 的瓦片并保序去重（保留首次出现 = 该 key 最高优先级
        //    实例）。发起后置 ContentLoading（cesium 在途请求同状态），
        //    到期前不会被再次发起。
        std::vector<TileLoadRequest> sortedRequests(
            TilesetTestAccess::loadQueue(tileset).begin(),
            TilesetTestAccess::loadQueue(tileset).end());
        TileLoadPriorityPolicy::sortByPriority(sortedRequests);

        std::vector<std::string> loadKeysInIssueOrder;
        std::set<std::string> seenLoadKeys;
        std::vector<TilesetTile*> issuedThisFrame;
        for (const TileLoadRequest& request : sortedRequests) {
            TilesetTile* tile =
                TilesetTestAccess::findTile(tileset, request.key);
            if (!tile || tile->content.loadState != TileLoadState::Unloaded) {
                continue;
            }
            const std::string keyString = selector_diff::tileKeyString(
                request.key.z,
                request.key.x,
                request.key.y);
            if (!seenLoadKeys.insert(keyString).second) {
                continue;
            }
            loadKeysInIssueOrder.push_back(keyString);
            issuedThisFrame.push_back(tile);
        }
        for (TilesetTile* tile : issuedThisFrame) {
            tile->content.loadState = TileLoadState::ContentLoading;
        }
        if (!issuedThisFrame.empty()) {
            pendingLoads.push_back(
                PendingLoadBatch{frame, std::move(issuedThisFrame)});
        }

        const TileSelectionCounters& counters =
            TilesetTestAccess::selectionCounters(tileset);
        ScenarioFrameResult result;
        result.renderKeys = renderKeys;
        result.visited = counters.visited;
        result.fogCulled = counters.fogCulled;
        // culled 映射（P3）：cesium tilesCulled 含 fog 剔除，gis-md 的
        // frustum/fog 计数分离 —— trace culled = culled + fogCulled。
        result.traceLine = selector_diff::traceLine(
            static_cast<int>(frame),
            std::move(renderKeys),
            std::move(loadKeysInIssueOrder),
            counters.visited,
            counters.culled + counters.fogCulled,
            counters.culledVisited,
            counters.kicked);
        results.push_back(std::move(result));
    }
    return results;
}

std::vector<std::string> traceLines(
    const std::vector<ScenarioFrameResult>& results) {
    std::vector<std::string> lines;
    lines.reserve(results.size());
    for (const ScenarioFrameResult& result : results) {
        lines.push_back(result.traceLine);
    }
    return lines;
}

/// 完整的单场景对拍流程：确定性重跑、合理性断言、可选 dump、golden 逐帧
/// 对拍（缺失则 GTEST_SKIP——在子函数里跳过后调用方 TEST 体已无后续逻辑）。
void runGoldenDiffScenario(const ScenarioSpec& scenario) {
    SCOPED_TRACE("scenario " + scenario.name);

    const std::vector<ScenarioFrameResult> results = runScenario(scenario);
    const std::vector<std::string> lines = traceLines(results);
    ASSERT_EQ(lines.size(), scenario.frames.size());

    // 确定性：同一场景独立重跑 byte 级一致。
    {
        const std::vector<std::string> rerunLines =
            traceLines(runScenario(scenario));
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
    // 2) 尾帧相机低于 z3 细化阈值的场景（S1 200km、S2/S3 400km nadir），
    //    终态应渲染到叶层（z=3）。
    if (scenario.expectFinalLeaf) {
        const std::vector<std::string>& finalRender =
            results.back().renderKeys;
        bool finalHasLeaf = false;
        for (const std::string& key : finalRender) {
            if (key.rfind("3-", 0) == 0) {
                finalHasLeaf = true;
                break;
            }
        }
        EXPECT_TRUE(finalHasLeaf)
            << "尾帧未渲染任何 z=3 叶瓦片：" << results.back().traceLine;
    }

    // SELECTOR_DIFF_DUMP=<path>：无论 golden 是否存在都写出完整轨迹到
    // <path>.<scenario>（四场景各自独立文件）。
    if (const char* dumpPath = std::getenv("SELECTOR_DIFF_DUMP")) {
        const std::string scenarioDumpPath =
            std::string(dumpPath) + "." + scenario.name;
        std::ofstream dump(
            scenarioDumpPath,
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(dump.is_open())
            << "无法写 SELECTOR_DIFF_DUMP 文件: " << scenarioDumpPath;
        for (const std::string& line : lines) {
            dump << line << "\n";
        }
    }

    // golden 已全量入库（s1-s4.trace），缺失即基建回归（宏丢失/目录移动/
    // 误删），必须硬失败——SKIP 会被普通 add_test 当作通过
    const std::filesystem::path goldenPath =
        std::filesystem::path(SELECTOR_DIFF_GOLDEN_DIR) /
        (scenario.name + ".trace");
    ASSERT_FALSE(std::string(SELECTOR_DIFF_GOLDEN_DIR).empty())
        << "SELECTOR_DIFF_GOLDEN_DIR 未注入（tests/CMakeLists.txt 编译定义丢失）";
    ASSERT_TRUE(std::filesystem::exists(goldenPath))
        << "golden 缺失: " << goldenPath.string()
        << "；如需重生成见 tools/selector_diff/cesium_golden_gen";

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

} // namespace

TEST(SelectorCesiumGoldenDiffTest, S1TraceMatchesCesiumGolden) {
    runGoldenDiffScenario(makeS1Scenario());
}

TEST(SelectorCesiumGoldenDiffTest, S2TraceMatchesCesiumGolden) {
    runGoldenDiffScenario(makeS2Scenario());
}

TEST(SelectorCesiumGoldenDiffTest, S3TraceMatchesCesiumGolden) {
    runGoldenDiffScenario(makeS3Scenario());
}

TEST(SelectorCesiumGoldenDiffTest, S4TraceMatchesCesiumGolden) {
    runGoldenDiffScenario(makeS4Scenario());
}

// asyncSelection 变体：走影子树选择 + reconcile 回 live，对同一 cesium
// golden 逐帧断言 —— 证明影子选择与同步路径逐字一致（golden-by-construction）。
TEST(SelectorCesiumGoldenDiffTest, S1AsyncShadowMatchesCesiumGolden) {
    runGoldenDiffScenario(asyncOf(makeS1Scenario()));
}

TEST(SelectorCesiumGoldenDiffTest, S2AsyncShadowMatchesCesiumGolden) {
    runGoldenDiffScenario(asyncOf(makeS2Scenario()));
}

TEST(SelectorCesiumGoldenDiffTest, S3AsyncShadowMatchesCesiumGolden) {
    runGoldenDiffScenario(asyncOf(makeS3Scenario()));
}

TEST(SelectorCesiumGoldenDiffTest, S4AsyncShadowMatchesCesiumGolden) {
    runGoldenDiffScenario(asyncOf(makeS4Scenario()));
}

// ③ 增量切面:走增量路径,靠 runScenario 内每帧 §8 oracle 断言「增量==全量」。
// 不与 cesium golden trace 对拍(增量的 load issue-order 可能不同)。S3 含在途
// 加载时序,是 dirty/剪枝正确性的关键场景。Layer 0 identity → 平凡通过;后续
// Layer 引入真剪枝后,这里是增量正确性的回归网。
TEST(SelectorIncrementalTest, S1MatchesFullViaOracle) {
    const auto results = runScenario(incrementalOf(makeS1Scenario()));
    EXPECT_FALSE(results.empty());
}

TEST(SelectorIncrementalTest, S3MatchesFullViaOracle) {
    const auto results = runScenario(incrementalOf(makeS3Scenario()));
    EXPECT_FALSE(results.empty());
}
