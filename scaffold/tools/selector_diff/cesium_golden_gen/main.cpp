// cesium-native golden 轨迹生成器（见 ../DESIGN.md）。
//
// 用 scenarios.h 的单一场景定义驱动 cesium-native（bfc2c574c）的
// Cesium3DTilesSelection::Tileset，逐帧输出选择轨迹到 golden/<scenario>.trace。
// 行格式完全由 scenarios.h 的 traceLine()/tileKeyString()/joinSorted() 生成，
// 保证与 gis-md 侧 driver byte 级同构。
//
// 用法：cesium_golden_gen <输出目录>
// 每个场景一个全新 Tileset（冷启动隔离），依次跑 S1 → s1.trace、
// S2 → s2.trace（树复用 kS1Tree，选项 kS2Options）。
//
// 加载模型（DESIGN.md「加载模型」）：
//   - InlineTaskProcessor 内联执行 worker 任务；
//   - GoldenQuadtreeLoader::loadTileContent 立即 resolve TileEmptyContent；
//   - 帧 N 内 loadTiles() 发起的加载在帧 N+1 帧首 dispatchMainThreadTasks()
//     时完成 → 「帧 N 请求 → 帧 N+1 可渲染」。

#include "scenarios.h"

#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileID.h>
#include <Cesium3DTilesSelection/TileLoadResult.h>
#include <Cesium3DTilesSelection/TileRefine.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetContentLoader.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <Cesium3DTilesSelection/TilesetViewGroup.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <Cesium3DTilesSelection/ViewUpdateResult.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/ITaskProcessor.h>
#include <CesiumGeometry/QuadtreeTileID.h>
#include <CesiumGeospatial/BoundingRegion.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGeospatial/GlobeRectangle.h>
#include <CesiumUtility/CreditSystem.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <any>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace Cesium3DTilesSelection;
using namespace CesiumAsync;
using namespace CesiumGeometry;
using namespace CesiumGeospatial;
using namespace CesiumUtility;

namespace {

// 内联任务处理器：worker 任务立即在当前线程执行，配合帧首
// dispatchMainThreadTasks() 形成确定性的「帧 N 请求 → 帧 N+1 完成」时序。
class InlineTaskProcessor : public ITaskProcessor {
public:
  void startTask(std::function<void()> f) override { f(); }
};

// 网络访问器：本场景 loader 不走网络，任何调用都是异常路径——
// 记录调用次数并返回 rejected future，运行结束时校验为 0。
class RejectingAssetAccessor : public IAssetAccessor {
public:
  int callCount = 0;

  Future<std::shared_ptr<IAssetRequest>>
  get(const AsyncSystem& asyncSystem,
      const std::string& /*url*/,
      const std::vector<THeader>& /*headers*/) override {
    ++this->callCount;
    return asyncSystem.createFuture<std::shared_ptr<IAssetRequest>>(
        [](const auto& promise) {
          promise.reject(std::runtime_error(
              "selector_diff golden gen: unexpected network request"));
        });
  }

  Future<std::shared_ptr<IAssetRequest>> request(
      const AsyncSystem& asyncSystem,
      const std::string& /*verb*/,
      const std::string& /*url*/,
      const std::vector<THeader>& /*headers*/,
      const std::span<const std::byte>& /*contentPayload*/) override {
    ++this->callCount;
    return asyncSystem.createFuture<std::shared_ptr<IAssetRequest>>(
        [](const auto& promise) {
          promise.reject(std::runtime_error(
              "selector_diff golden gen: unexpected network request"));
        });
  }

  void tick() noexcept override {}
};

// 渲染资源准备：全 no-op（签名参照
// Cesium3DTilesSelection/test/SimplePrepareRendererResource.h）。
class NoopPrepareRendererResources : public IPrepareRendererResources {
public:
  Future<TileLoadResultAndRenderResources> prepareInLoadThread(
      const AsyncSystem& asyncSystem,
      TileLoadResult&& tileLoadResult,
      const glm::dmat4& /*transform*/,
      const std::any& /*rendererOptions*/) override {
    return asyncSystem.createResolvedFuture(
        TileLoadResultAndRenderResources{std::move(tileLoadResult), nullptr});
  }

  void* prepareInMainThread(Tile& /*tile*/, void* /*pLoadThreadResult*/)
      override {
    return nullptr;
  }

  void free(
      Tile& /*tile*/,
      void* /*pLoadThreadResult*/,
      void* /*pMainThreadResult*/) noexcept override {}

  void* prepareRasterInLoadThread(
      CesiumGltf::ImageAsset& /*image*/,
      const std::any& /*rendererOptions*/) override {
    return nullptr;
  }

  void* prepareRasterInMainThread(
      CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
      void* /*pLoadThreadResult*/) override {
    return nullptr;
  }

  void freeRaster(
      const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
      void* /*pLoadThreadResult*/,
      void* /*pMainThreadResult*/) noexcept override {}

  void attachRasterInMainThread(
      const Tile& /*tile*/,
      int32_t /*overlayTextureCoordinateID*/,
      const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
      void* /*pMainThreadRendererResources*/,
      const glm::dvec2& /*translation*/,
      const glm::dvec2& /*scale*/) override {}

  void detachRasterInMainThread(
      const Tile& /*tile*/,
      int32_t /*overlayTextureCoordinateID*/,
      const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
      void* /*pMainThreadRendererResources*/) noexcept override {}
};

// 场景四叉树 loader：
//   - loadTileContent：记录被请求瓦片 key（loads 对拍面），立即 resolve
//     TileEmptyContent（照 test/TestTilesetSelection.cpp EmptyLoader）；
//   - createTileChildren：按 scenarios.h 的 childRegion/geometricErrorForLevel
//     生成 4 个子瓦；父 region 取自父瓦片自己的 BoundingRegion（构造时写入的
//     同一批 double，与 gis-md 侧递归展开逐位一致）；到 maxDepth 返回空。
class GoldenQuadtreeLoader : public TilesetContentLoader {
public:
  explicit GoldenQuadtreeLoader(const selector_diff::QuadtreeSpec& tree)
      : _tree(tree) {}

  Future<TileLoadResult> loadTileContent(const TileLoadInput& input) override {
    const QuadtreeTileID* pID =
        std::get_if<QuadtreeTileID>(&input.tile.getTileID());
    if (pID) {
      this->_requestedThisFrame.push_back(selector_diff::tileKeyString(
          static_cast<int>(pID->level),
          static_cast<int>(pID->x),
          static_cast<int>(pID->y)));
    } else {
      ++this->nonQuadtreeLoadCount;
    }

    return input.asyncSystem.createResolvedFuture(TileLoadResult{
        .contentKind = TileEmptyContent(),
        .glTFUpAxis = CesiumGeometry::Axis::Z,
        .updatedBoundingVolume = std::nullopt,
        .updatedContentBoundingVolume = std::nullopt,
        .rasterOverlayDetails = std::nullopt,
        .pAssetAccessor = input.pAssetAccessor,
        .pCompletedRequest = nullptr,
        .tileInitializer = {},
        .state = TileLoadResultState::Success,
        .ellipsoid = Ellipsoid::WGS84});
  }

  TileChildrenResult createTileChildren(
      const Tile& tile,
      const Ellipsoid& ellipsoid) override {
    const QuadtreeTileID* pID = std::get_if<QuadtreeTileID>(&tile.getTileID());
    if (!pID) {
      ++this->nonQuadtreeChildrenCount;
      return TileChildrenResult{{}, TileLoadResultState::Failed};
    }
    if (static_cast<int>(pID->level) >= this->_tree.maxDepth) {
      return TileChildrenResult{{}, TileLoadResultState::Success};
    }

    const BoundingRegion* pRegion =
        std::get_if<BoundingRegion>(&tile.getBoundingVolume());
    if (!pRegion) {
      ++this->nonRegionBoundsCount;
      return TileChildrenResult{{}, TileLoadResultState::Failed};
    }
    const GlobeRectangle& rect = pRegion->getRectangle();
    const selector_diff::RegionSpec parent{
        rect.getWest(),
        rect.getSouth(),
        rect.getEast(),
        rect.getNorth()};

    std::vector<Tile> children;
    children.reserve(4);
    for (int dy = 0; dy < 2; ++dy) {
      for (int dx = 0; dx < 2; ++dx) {
        const selector_diff::RegionSpec r =
            selector_diff::childRegion(parent, dx, dy);
        Tile& child = children.emplace_back(tile.getLoader());
        child.setTileID(QuadtreeTileID(
            pID->level + 1,
            pID->x * 2 + static_cast<uint32_t>(dx),
            pID->y * 2 + static_cast<uint32_t>(dy)));
        child.setRefine(TileRefine::Replace);
        child.setBoundingVolume(BoundingRegion(
            GlobeRectangle(r.west, r.south, r.east, r.north),
            0.0,
            0.0,
            ellipsoid));
        child.setGeometricError(selector_diff::geometricErrorForLevel(
            this->_tree,
            static_cast<int>(pID->level) + 1));
      }
    }
    return TileChildrenResult{std::move(children), TileLoadResultState::Success};
  }

  // 取走并清空本帧被请求加载的瓦片 key（loadTiles() 后调用）。
  std::vector<std::string> takeRequestedThisFrame() {
    return std::exchange(this->_requestedThisFrame, {});
  }

  int nonQuadtreeLoadCount = 0;
  int nonQuadtreeChildrenCount = 0;
  int nonRegionBoundsCount = 0;

private:
  selector_diff::QuadtreeSpec _tree;
  std::vector<std::string> _requestedThisFrame;
};

TilesetOptions makeOptions(const selector_diff::OptionsSpec& spec) {
  // 全部显式赋值，不依赖 cesium 默认值（DESIGN.md「选项」）。
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
    options.fogDensityTable.push_back(
        FogDensityAtHeight{entry.height, entry.density});
  }
  return options;
}

ViewState makeViewState(
    const selector_diff::CameraFrameSpec& frame,
    const selector_diff::OptionsSpec& spec) {
  // position/direction/up 一律用 scenarios.h 的实现计算后逐分量转 glm。
  const selector_diff::Vec3d p = selector_diff::cartographicToEcef(
      frame.lonRad,
      frame.latRad,
      frame.heightMeters);
  const selector_diff::Vec3d d = selector_diff::nadirDirection(p);
  const selector_diff::Vec3d u = selector_diff::nadirUpLocalNorth(p);

  const glm::dvec3 position(p.x, p.y, p.z);
  const glm::dvec3 direction(d.x, d.y, d.z);
  const glm::dvec3 up(u.x, u.y, u.z);
  const glm::dvec2 viewportSize(spec.viewportWidth, spec.viewportHeight);
  const double vFov = spec.verticalFovRadians;
  const double hFov = 2.0 * std::atan(
                                std::tan(vFov * 0.5) *
                                (spec.viewportWidth / spec.viewportHeight));
  return ViewState(position, direction, up, viewportSize, hFov, vFov);
}

// 跑一个场景（全新 Tileset 冷启动），写出 trace 并打印每帧概览。
// 返回 0 = 正常；非 0 = 出现与「已探明事实」不符的异常路径。
int runScenario(
    const std::string& name,
    const selector_diff::QuadtreeSpec& tree,
    const selector_diff::CameraFrameSpec* frames,
    size_t frameCount,
    const selector_diff::OptionsSpec& optionsSpec,
    const std::string& outPath) {
  auto pLoader = std::make_unique<GoldenQuadtreeLoader>(tree);
  GoldenQuadtreeLoader* pLoaderRaw = pLoader.get();

  // 根瓦片：Unloaded 状态（走加载管线），BV/GE/refine 按场景（仿
  // EllipsoidTilesetLoader.cpp:56-90 的构造方式，但不 setUnconditionallyRefine）。
  auto pRootTile =
      std::make_unique<Tile>(pLoaderRaw, TileID(QuadtreeTileID(0, 0, 0)));
  pRootTile->setRefine(TileRefine::Replace);
  pRootTile->setBoundingVolume(BoundingRegion(
      GlobeRectangle(tree.west, tree.south, tree.east, tree.north),
      0.0,
      0.0,
      Ellipsoid::WGS84));
  pRootTile->setGeometricError(tree.rootGeometricError);

  auto pAssetAccessor = std::make_shared<RejectingAssetAccessor>();
  TilesetExternals externals{
      pAssetAccessor,
      std::make_shared<NoopPrepareRendererResources>(),
      AsyncSystem(std::make_shared<InlineTaskProcessor>()),
      std::make_shared<CreditSystem>()};

  Tileset tileset(
      externals,
      std::move(pLoader),
      std::move(pRootTile),
      makeOptions(optionsSpec));

  std::vector<std::string> lines;
  lines.reserve(frameCount);
  struct FrameSummary {
    size_t renderCount;
    size_t loadCount;
    int maxRenderLevel;
    long kicked;
  };
  std::vector<FrameSummary> summaries;
  bool sawNonQuadtreeRenderID = false;

  for (size_t i = 0; i < frameCount; ++i) {
    const ViewState viewState = makeViewState(frames[i], optionsSpec);

    // 帧循环（TestTilesetSelection.cpp:155-215 模式；见文件头「加载模型」）。
    externals.asyncSystem.dispatchMainThreadTasks();
    const ViewUpdateResult& result =
        tileset.updateViewGroup(tileset.getDefaultViewGroup(), {viewState});
    externals.asyncSystem.dispatchMainThreadTasks();
    tileset.loadTiles();

    std::vector<std::string> renderKeys;
    renderKeys.reserve(result.tilesToRenderThisFrame.size());
    int maxRenderLevel = -1;
    for (const Tile::ConstPointer& pTile : result.tilesToRenderThisFrame) {
      const QuadtreeTileID* pID =
          std::get_if<QuadtreeTileID>(&pTile->getTileID());
      if (!pID) {
        std::cerr << name << " frame " << i
                  << ": non-QuadtreeTileID tile in tilesToRenderThisFrame"
                  << std::endl;
        sawNonQuadtreeRenderID = true;
        continue;
      }
      renderKeys.push_back(selector_diff::tileKeyString(
          static_cast<int>(pID->level),
          static_cast<int>(pID->x),
          static_cast<int>(pID->y)));
      maxRenderLevel =
          std::max(maxRenderLevel, static_cast<int>(pID->level));
    }

    std::vector<std::string> loadKeys = pLoaderRaw->takeRequestedThisFrame();
    summaries.push_back(FrameSummary{
        renderKeys.size(),
        loadKeys.size(),
        maxRenderLevel,
        static_cast<long>(result.tilesKicked)});

    lines.push_back(selector_diff::traceLine(
        static_cast<int>(i),
        std::move(renderKeys),
        std::move(loadKeys),
        static_cast<long>(result.tilesVisited),
        static_cast<long>(result.tilesCulled),
        static_cast<long>(result.culledTilesVisited),
        static_cast<long>(result.tilesKicked)));
  }

  std::ofstream out(outPath, std::ios::binary);
  if (!out) {
    std::cerr << "cannot open output file: " << outPath << std::endl;
    return 1;
  }
  for (const std::string& line : lines) {
    out << line << '\n';
  }
  out.close();

  std::cout << name << ": wrote " << lines.size() << " frames to " << outPath
            << std::endl;
  for (size_t i = 0; i < summaries.size(); ++i) {
    std::cout << name << " frame " << i
              << ": render=" << summaries[i].renderCount
              << " loads=" << summaries[i].loadCount
              << " maxZ=" << summaries[i].maxRenderLevel
              << " kicked=" << summaries[i].kicked << std::endl;
  }

  // 异常路径校验：任何一项非零都说明与「已探明事实」不符，返回失败。
  if (sawNonQuadtreeRenderID || pAssetAccessor->callCount != 0 ||
      pLoaderRaw->nonQuadtreeLoadCount != 0 ||
      pLoaderRaw->nonQuadtreeChildrenCount != 0 ||
      pLoaderRaw->nonRegionBoundsCount != 0) {
    std::cerr << name << " anomalies detected: nonQuadtreeRenderID="
              << sawNonQuadtreeRenderID
              << " assetAccessorCalls=" << pAssetAccessor->callCount
              << " nonQuadtreeLoads=" << pLoaderRaw->nonQuadtreeLoadCount
              << " nonQuadtreeChildren="
              << pLoaderRaw->nonQuadtreeChildrenCount
              << " nonRegionBounds=" << pLoaderRaw->nonRegionBoundsCount
              << std::endl;
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  const std::string outDir = argc > 1 ? argv[1] : ".";

  const int rcS1 = runScenario(
      "s1",
      selector_diff::kS1Tree,
      selector_diff::kS1Frames.data(),
      selector_diff::kS1Frames.size(),
      selector_diff::kS1Options,
      outDir + "/s1.trace");
  // S2 复用 kS1Tree（场景规格），仅相机序列与选项不同
  const int rcS2 = runScenario(
      "s2",
      selector_diff::kS1Tree,
      selector_diff::kS2Frames.data(),
      selector_diff::kS2Frames.size(),
      selector_diff::kS2Options,
      outDir + "/s2.trace");

  return (rcS1 != 0 || rcS2 != 0) ? 1 : 0;
}
