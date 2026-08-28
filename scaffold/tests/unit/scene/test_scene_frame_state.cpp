#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/renderer/RenderCommand.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/Frustum.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/scene/SceneFrameDiagnostics.h"
#include "earth_engine/scene/SceneFrameStateBuilder.h"
#include "earth_engine/scene/Scene.h"
#include "earth_engine/scene/ScenePresentationTraceBuilder.h"
#include "earth_engine/scene/ScenePrimaryTilesetRenderComposer.h"
#include "earth_engine/scene/ScenePrimaryTilesetTakeoverPolicy.h"
#include "earth_engine/scene/SceneTilesetCoordinator.h"
#include "earth_engine/tiling/GltfRenderResourcePreparer.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRasterOverlayPrefetcher.h"
#include "earth_engine/tiling/TileRenderPlanFrameRefresher.h"
#include "earth_engine/tiling/TileSelectionPlanAppender.h"
#include "earth_engine/tiling/TileSelectionRasterOverlayPreparer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static std::string terrainCacheKey(const TileKey& key) {
        return TileCacheKey::forTile(key);
    }

    static void setLoadedGltfTerrainContent(
        TilesetTile& tile,
        std::unique_ptr<GltfModel> model,
        double heightMeters,
        RenderDevice* device = nullptr) {
        tile.content.renderContent.prepareGltfContent(
            std::move(model),
            Mat4::identity());
        tile.content.renderContent.setTerrainRenderContent(true);
        // heightmap 地形虽以 glTF 交付,原始高度图仍随内容保留:GPU 位移建
        // per-tile 高度纹理、高度查询(LoadedTerrainHeightSampler)都以它为唯一
        // 真值源,而不读渲染网格顶点。所以「已加载的地形瓦片」这个夹具必须像
        // 生产上传路径(TileContentUploadPolicy)那样两者都给。
        auto heightmap = std::make_unique<DecodedHeightmap>();
        heightmap->tileSize = 2;
        heightmap->stagedHeights.assign(4, static_cast<float>(heightMeters));
        heightmap->assignHeights();
        heightmap->minHeight = static_cast<float>(heightMeters);
        heightmap->maxHeight = static_cast<float>(heightMeters);
        tile.content.renderContent.setRetainedHeightmap(std::move(heightmap));
        if (device) {
            tile.markRenderContentLoaded();
            GltfRenderResourcePreparer::prepare(tile, device, 0.0);
        } else {
            tile.markRenderContentDone();
        }
    }

    static TileOcclusionState checkOcclusion(
        const Tileset& tileset,
        const TilesetTile& tile) {
        return tileset.checkOcclusion(tile);
    }

    static void prefetchRasterOverlays(Tileset& tileset, TilesetTile& tile) {
        const std::vector<size_t> overlayOrder =
            TileSelectionRasterOverlayPreparer::processingOrder(
                tileset.directRasterOverlays());
        TileRasterOverlayPrefetcher::prefetch(
            tile,
            tileset.rasterOverlays(),
            overlayOrder,
            tileset.device_,
            tileset.options_.maximumScreenSpaceError,
            tileset.frameResourceBudget_);
    }

    static void setInteractionActiveForFrame(Tileset& tileset, bool active) {
        tileset.interactionActiveForFrame_ = active;
    }

    static void beginTilePlan(Tileset& tileset) {
        tileset.tilePlan_ = TilePlan{};
    }

    static TilePlan& mutableTilePlan(Tileset& tileset) {
        return tileset.tilePlan_;
    }

    static void addTileToCurrentPlan(Tileset& tileset, TilesetTile& tile) {
        TileSelectionPlanAppender::addTileToCurrentPlan(
            tileset.tilePlan_,
            tileset.loadQueue_,
            tile,
            1.0,
            true,
            std::numeric_limits<double>::max());
        TileRenderPlanFrameRefresher::refresh(
            tileset.tilePlan_,
            tileset.contentAccess_,
            tileset.directRasterOverlays(),
            TileRenderPlanFrameRefreshOptions{
                tileset.interactionActiveForFrame_,
                tileset.resourceSmoothingActiveForFrame_,
                0.0,
                false,
                -1,
                tileset.rasterOverlayRuntime().frameContext()});
    }
};
} // namespace earth_engine

namespace {

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }

private:
    size_t byteSize_ = 0;
};

class DummyShaderProgram final : public ShaderProgram {};

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }
    size_t sizeBytes() const override {
        return static_cast<size_t>(width_) *
               static_cast<size_t>(height_) * 4u;
    }

private:
    int width_ = 0;
    int height_ = 0;
};

class DummyRenderDevice final : public RenderDevice {
public:
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override { return 4096; }
    int maxDrawBuffers() const override { return 4; }
    bool supportsFloatTextures() const override { return true; }
    bool supportsInstancing() const override { return true; }
    std::string rendererString() const override { return "DummyRenderDevice"; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        return std::make_unique<DummyTexture>(desc.width, desc.height);
    }

    bool updateTextureRegion(
        Texture*,
        int,
        int,
        int,
        int,
        const uint8_t*,
        size_t,
        int = 0) override {
        return false;
    }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        return std::make_unique<DummyBuffer>(desc.size);
    }

    bool updateBuffer(Buffer*, size_t, const void*, size_t) override {
        return false;
    }

    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        return std::make_unique<DummyShaderProgram>();
    }

    std::unique_ptr<Framebuffer> createFramebuffer(
        const FramebufferDesc&) override {
        return nullptr;
    }

    void beginFrame() override {}
    void submit(const RenderCommandList& commands) override {
        submittedCommands = commands;
    }
    void endFrame() override {}
    void onSurfaceCreated() override {}
    void onSurfaceChanged(int, int) override {}
    void onSurfaceDestroyed() override {}

    RenderCommandList submittedCommands;
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

Camera makeCameraFromCenterPitchHeading(
    double longitudeDegrees,
    double latitudeDegrees,
    double cameraHeightMeters,
    double pitchRadians,
    double headingRadians) {
    const Cartographic targetCartographic =
        Cartographic::fromDegrees(longitudeDegrees, latitudeDegrees, 0.0);
    const Vec3 target =
        Ellipsoid::WGS84().cartographicToCartesian(targetCartographic);
    const Vec3 localUp =
        Ellipsoid::WGS84().geodeticSurfaceNormal(targetCartographic);
    const Vec3 east = Vec3(-target.y(), target.x(), 0.0).normalized();
    const Vec3 north = localUp.cross(east).normalized();

    const double horizontalScale = std::cos(pitchRadians);
    const Vec3 direction =
        (east * (std::sin(headingRadians) * horizontalScale) +
         north * (std::cos(headingRadians) * horizontalScale) +
         localUp * std::sin(pitchRadians)).normalized();
    const Vec3 cameraUp =
        (north * std::cos(headingRadians) -
         east * std::sin(headingRadians)).normalized();

    Camera camera;
    camera.lookAt(target - direction * cameraHeightMeters, target, cameraUp);
    return camera;
}

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->stagedHeights = {
        heightMeters,
        heightMeters,
        heightMeters,
        heightMeters};
    heightmap->assignHeights();
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

std::unique_ptr<GltfModel> makeFlatGeographicTerrainGltfModel(
    const Rectangle& rectangle,
    double heightMeters) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            rectangle.west(),
            rectangle.south(),
            heightMeters));
    primitive.vertices[1].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            rectangle.east(),
            rectangle.south(),
            heightMeters));
    primitive.vertices[2].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            rectangle.west(),
            rectangle.north(),
            heightMeters));
    primitive.vertices[3].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            rectangle.east(),
            rectangle.north(),
            heightMeters));
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

// 极帽(PolarCapRenderer)每帧发两条 glTF primitive 命令补 web-mercator ±85°
// 缺口,不带 terrainRenderContent。凡是拿「全部 glTF 命令数」对账的用例都要把
// 它算进来。
int countPolarCapCommands(const DummyRenderDevice& device) {
    return static_cast<int>(std::count_if(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive &&
                   cmd.owner == "polar_cap";
        }));
}

RasterOverlay::Options makeRasterOverlayOptions() {
    RasterOverlay::Options options{};
    options.maximumSimultaneousTileLoads = 20;
    options.maximumScreenSpaceError = 2.0;
    options.minimumZoom = 0;
    options.maximumZoom = 0;
    options.visible = true;
    options.opacity = 1.0f;
    options.role = RasterOverlayRole::BaseImagery;
    return options;
}

const std::vector<std::unique_ptr<Tileset>>& emptyContentTilesets() {
    static const std::vector<std::unique_ptr<Tileset>> empty;
    return empty;
}

std::unique_ptr<GltfModel> makeTriangleGltfModel() {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    primitive.vertices[0].normalEcef = Vec3::unitZ();
    primitive.vertices[1].normalEcef = Vec3::unitZ();
    primitive.vertices[2].normalEcef = Vec3::unitZ();
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.indices = {0, 1, 2};
    model->primitives.push_back(std::move(primitive));
    return model;
}

GltfPrimitive makeTransparentTrianglePrimitiveAt(const Vec3& center) {
    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef =
        center + Vec3(-1.0, -1.0, 0.0);
    primitive.vertices[1].positionEcef =
        center + Vec3(2.0, -1.0, 0.0);
    primitive.vertices[2].positionEcef =
        center + Vec3(-1.0, 2.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f}};
    primitive.indices = {0, 1, 2};
    primitive.alphaMode = GltfAlphaMode::Blend;
    primitive.baseColorFactor = {1.0f, 1.0f, 1.0f, 0.5f};
    return primitive;
}

std::unique_ptr<Tileset> makeTakeoverTileset(DummyRenderDevice& device) {
    return std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
}

TilesetTile* makeTakeoverRenderTile(
    Tileset& tileset,
    const TileKey& key,
    SurfaceDrawableSource source =
        SurfaceDrawableSource::GltfContent) {
    TilesetTile* tile = TilesetTestAccess::ensureTile(tileset, key);
    if (!tile) {
        return nullptr;
    }
    tile->content.renderContent.setGltfContent(
        std::make_unique<GltfModel>());
    tile->content.renderContent.setTerrainRenderContent(true);
    tile->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile->markRenderContentDone();
    tile->content.renderContent.setSurfaceSource(source);
    return tile;
}

void setSingleTakeoverEntry(
    Tileset& tileset,
    TilesetTile& renderTile,
    const TileKey& selectedKey,
    const TileKey& renderKey) {
    TilePlan& plan = TilesetTestAccess::mutableTilePlan(tileset);
    plan = TilePlan{};
    plan.visibleTiles.push_back(selectedKey);
    TileRenderEntry entry;
    entry.selectedKey = selectedKey;
    entry.renderKey = renderKey;
    entry.selectedTile = &renderTile;
    entry.renderTile = &renderTile;
    entry.selectedThisFrame = true;
    plan.renderEntries.push_back(entry);
}

} // namespace

TEST(
    SceneFrameStateTest,
    StagedPrimaryReplacementKeepsCurrentPrimaryAlive) {
    DummyRenderDevice device;
    SceneTilesetCoordinator coordinator;
    auto current = makeTakeoverTileset(device);
    Tileset* currentRaw = current.get();
    coordinator.setPrimary(std::move(current));

    auto pending = makeTakeoverTileset(device);
    Tileset* pendingRaw = pending.get();
    coordinator.stagePrimaryReplacement(std::move(pending));

    EXPECT_EQ(currentRaw, coordinator.primary());
    EXPECT_EQ(pendingRaw, coordinator.pendingPrimary());
}

TEST(
    SceneFrameStateTest,
    PendingRealTerrainReplacesOnlyItsReadyCoverageRegion) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey currentRoot{"Geographic-TMS", 0, 0, 0};
    const TileKey pendingChild{"Geographic-TMS", 1, 0, 0};
    TilesetTile* currentTile =
        makeTakeoverRenderTile(*current, currentRoot);
    TilesetTile* pendingTile =
        makeTakeoverRenderTile(*pending, pendingChild);
    ASSERT_NE(nullptr, currentTile);
    ASSERT_NE(nullptr, pendingTile);
    setSingleTakeoverEntry(
        *current,
        *currentTile,
        currentRoot,
        currentRoot);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        pendingChild,
        pendingChild);

    const ScenePrimaryTilesetRenderComposition composition =
        ScenePrimaryTilesetRenderComposer::compose(*current, *pending);

    ASSERT_EQ(1u, composition.pendingEntries.size());
    EXPECT_EQ(pendingChild, composition.pendingEntries.front().selectedKey);
    ASSERT_EQ(3u, composition.currentEntries.size());
    for (const TileRenderEntry& entry : composition.currentEntries) {
        EXPECT_TRUE(entry.selectedThisFrame);
        EXPECT_TRUE(entry.surfaceClipEnabled);
        EXPECT_EQ(currentRoot, entry.renderKey);
        EXPECT_NE(pendingChild, entry.selectedKey);
        EXPECT_EQ(1, entry.selectedKey.z);
    }
}

TEST(
    SceneFrameStateTest,
    PendingTransientSurfaceDoesNotReplaceCurrentCoverage) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey currentRoot{"Geographic-TMS", 0, 0, 0};
    const TileKey pendingChild{"Geographic-TMS", 1, 0, 0};
    TilesetTile* currentTile =
        makeTakeoverRenderTile(*current, currentRoot);
    TilesetTile* pendingTile = makeTakeoverRenderTile(
        *pending,
        pendingChild,
        SurfaceDrawableSource::EllipsoidFallback);
    ASSERT_NE(nullptr, currentTile);
    ASSERT_NE(nullptr, pendingTile);
    setSingleTakeoverEntry(
        *current,
        *currentTile,
        currentRoot,
        currentRoot);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        pendingChild,
        pendingChild);

    const ScenePrimaryTilesetRenderComposition composition =
        ScenePrimaryTilesetRenderComposer::compose(*current, *pending);

    EXPECT_TRUE(composition.pendingEntries.empty());
    ASSERT_EQ(1u, composition.currentEntries.size());
    EXPECT_EQ(currentRoot, composition.currentEntries.front().selectedKey);
    EXPECT_FALSE(composition.currentEntries.front().surfaceClipEnabled);
}

TEST(
    SceneFrameStateTest,
    PendingCoverageInOneGeographicRootKeepsTheOtherRootUnchanged) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey westRoot{"Geographic-TMS", 0, 0, 0};
    const TileKey eastRoot{"Geographic-TMS", 0, 1, 0};
    const TileKey westChild{"Geographic-TMS", 1, 0, 0};
    TilesetTile* westTile =
        makeTakeoverRenderTile(*current, westRoot);
    TilesetTile* eastTile =
        makeTakeoverRenderTile(*current, eastRoot);
    TilesetTile* pendingTile =
        makeTakeoverRenderTile(*pending, westChild);
    ASSERT_NE(nullptr, westTile);
    ASSERT_NE(nullptr, eastTile);
    ASSERT_NE(nullptr, pendingTile);

    setSingleTakeoverEntry(*current, *westTile, westRoot, westRoot);
    TilePlan& currentPlan = TilesetTestAccess::mutableTilePlan(*current);
    TileRenderEntry eastEntry;
    eastEntry.selectedKey = eastRoot;
    eastEntry.renderKey = eastRoot;
    eastEntry.selectedTile = eastTile;
    eastEntry.renderTile = eastTile;
    eastEntry.selectedThisFrame = true;
    currentPlan.visibleTiles.push_back(eastRoot);
    currentPlan.renderEntries.push_back(eastEntry);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        westChild,
        westChild);

    const ScenePrimaryTilesetRenderComposition composition =
        ScenePrimaryTilesetRenderComposer::compose(*current, *pending);

    ASSERT_EQ(4u, composition.currentEntries.size());
    EXPECT_EQ(
        1,
        std::count_if(
            composition.currentEntries.begin(),
            composition.currentEntries.end(),
            [&](const TileRenderEntry& entry) {
                return entry.selectedKey == eastRoot &&
                       !entry.surfaceClipEnabled;
            }));
}

TEST(
    SceneFrameStateTest,
    MultiplePendingPatchesProduceCompleteNonOverlappingCoverage) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey root{"Geographic-TMS", 0, 0, 0};
    const TileKey firstPatch{"Geographic-TMS", 2, 0, 0};
    const TileKey secondPatch{"Geographic-TMS", 2, 3, 3};
    TilesetTile* currentTile =
        makeTakeoverRenderTile(*current, root);
    TilesetTile* firstPendingTile =
        makeTakeoverRenderTile(*pending, firstPatch);
    TilesetTile* secondPendingTile =
        makeTakeoverRenderTile(*pending, secondPatch);
    ASSERT_NE(nullptr, currentTile);
    ASSERT_NE(nullptr, firstPendingTile);
    ASSERT_NE(nullptr, secondPendingTile);
    setSingleTakeoverEntry(*current, *currentTile, root, root);
    setSingleTakeoverEntry(
        *pending,
        *firstPendingTile,
        firstPatch,
        firstPatch);
    TilePlan& pendingPlan = TilesetTestAccess::mutableTilePlan(*pending);
    TileRenderEntry secondEntry;
    secondEntry.selectedKey = secondPatch;
    secondEntry.renderKey = secondPatch;
    secondEntry.selectedTile = secondPendingTile;
    secondEntry.renderTile = secondPendingTile;
    secondEntry.selectedThisFrame = true;
    pendingPlan.visibleTiles.push_back(secondPatch);
    pendingPlan.renderEntries.push_back(secondEntry);

    const ScenePrimaryTilesetRenderComposition composition =
        ScenePrimaryTilesetRenderComposer::compose(*current, *pending);

    ASSERT_EQ(2u, composition.pendingEntries.size());
    auto isAncestorOrSame = [](const TileKey& ancestor,
                               const TileKey& descendant) {
        if (ancestor.schemeId != descendant.schemeId ||
            ancestor.z > descendant.z) {
            return false;
        }
        const int delta = descendant.z - ancestor.z;
        return (descendant.x >> delta) == ancestor.x &&
               (descendant.y >> delta) == ancestor.y;
    };
    double coveredArea = 0.0;
    for (const TileRenderEntry& entry : composition.currentEntries) {
        coveredArea += std::pow(0.25, entry.selectedKey.z - root.z);
        for (const TileRenderEntry& pendingEntry :
             composition.pendingEntries) {
            EXPECT_FALSE(isAncestorOrSame(
                entry.selectedKey,
                pendingEntry.selectedKey));
            EXPECT_FALSE(isAncestorOrSame(
                pendingEntry.selectedKey,
                entry.selectedKey));
        }
    }
    for (const TileRenderEntry& entry : composition.pendingEntries) {
        coveredArea += std::pow(0.25, entry.selectedKey.z - root.z);
    }
    EXPECT_NEAR(1.0, coveredArea, 1e-9);
}

TEST(
    SceneFrameStateTest,
    CoarsePendingRealTerrainDoesNotReplaceDetailedCurrentCoverage) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey currentKey{"Geographic-TMS", 6, 32, 20};
    const TileKey pendingRoot{"Geographic-TMS", 0, 0, 0};
    TilesetTile* currentTile =
        makeTakeoverRenderTile(*current, currentKey);
    TilesetTile* pendingTile =
        makeTakeoverRenderTile(*pending, pendingRoot);
    ASSERT_NE(nullptr, currentTile);
    ASSERT_NE(nullptr, pendingTile);
    setSingleTakeoverEntry(
        *current,
        *currentTile,
        currentKey,
        currentKey);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        pendingRoot,
        pendingRoot);

    const ScenePrimaryTilesetRenderComposition composition =
        ScenePrimaryTilesetRenderComposer::compose(*current, *pending);

    EXPECT_TRUE(composition.pendingEntries.empty());
    ASSERT_EQ(1u, composition.currentEntries.size());
    EXPECT_EQ(currentKey, composition.currentEntries.front().selectedKey);
}

TEST(
    SceneFrameStateTest,
    PendingCoverageClipsCurrentFadingEntriesInsteadOfDroppingThem) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey root{"Geographic-TMS", 0, 0, 0};
    const TileKey selectedChild{"Geographic-TMS", 1, 0, 0};
    const TileKey pendingGrandchild{"Geographic-TMS", 2, 0, 0};
    TilesetTile* rootTile =
        makeTakeoverRenderTile(*current, root);
    TilesetTile* selectedTile =
        makeTakeoverRenderTile(*current, selectedChild);
    TilesetTile* pendingTile =
        makeTakeoverRenderTile(*pending, pendingGrandchild);
    ASSERT_NE(nullptr, rootTile);
    ASSERT_NE(nullptr, selectedTile);
    ASSERT_NE(nullptr, pendingTile);
    setSingleTakeoverEntry(
        *current,
        *selectedTile,
        selectedChild,
        selectedChild);
    TilePlan& currentPlan = TilesetTestAccess::mutableTilePlan(*current);
    TileRenderEntry fadingEntry;
    fadingEntry.selectedKey = root;
    fadingEntry.renderKey = root;
    fadingEntry.opacity = 0.5f;
    fadingEntry.selectedThisFrame = false;
    fadingEntry.selectedTile = rootTile;
    fadingEntry.renderTile = rootTile;
    currentPlan.renderEntries.push_back(fadingEntry);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        pendingGrandchild,
        pendingGrandchild);

    const ScenePrimaryTilesetRenderComposition composition =
        ScenePrimaryTilesetRenderComposer::compose(*current, *pending);

    EXPECT_EQ(
        6,
        std::count_if(
            composition.currentEntries.begin(),
            composition.currentEntries.end(),
            [](const TileRenderEntry& entry) {
                return !entry.selectedThisFrame &&
                       entry.surfaceClipEnabled;
            }));
    EXPECT_EQ(
        3,
        std::count_if(
            composition.currentEntries.begin(),
            composition.currentEntries.end(),
            [](const TileRenderEntry& entry) {
                return entry.selectedThisFrame &&
                       entry.surfaceClipEnabled;
            }));
}

TEST(
    SceneFrameStateTest,
    PrimaryTakeoverRejectsCoarseAncestorAndEllipsoidFallback) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey selectedKey{"Geographic-TMS", 6, 32, 20};
    const TileKey coarseKey{"Geographic-TMS", 2, 2, 1};
    TilesetTile* currentTile =
        makeTakeoverRenderTile(*current, selectedKey);
    TilesetTile* pendingTile =
        makeTakeoverRenderTile(*pending, coarseKey);
    ASSERT_NE(nullptr, currentTile);
    ASSERT_NE(nullptr, pendingTile);
    setSingleTakeoverEntry(
        *current,
        *currentTile,
        selectedKey,
        selectedKey);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        selectedKey,
        coarseKey);

    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::isCandidateReady(
        *current,
        *pending));

    pendingTile->content.renderContent.setSurfaceSource(
        SurfaceDrawableSource::EllipsoidFallback);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        selectedKey,
        selectedKey);
    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::isCandidateReady(
        *current,
        *pending));

    pendingTile->content.renderContent.clearGltfContentPreservingFill();
    pendingTile->content.renderContent.setFillContent(
        std::make_unique<GltfModel>());
    pendingTile->content.renderContent.beginFillGpuResourceBuild(0, 1);
    pendingTile->content.renderContent.addFillPrimitiveResource(
        GltfPrimitiveRenderResources{});
    const auto fillSignature = TileFillGeometrySignature::tryCreate(
        pendingTile->bounds,
        RasterOverlayProjection::Geographic,
        4);
    ASSERT_TRUE(fillSignature.has_value());
    pendingTile->content.renderContent.commitFillResourcesReady(
        *fillSignature);
    ASSERT_TRUE(pendingTile->content.renderContent.drawsFill());
    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::isCandidateReady(
        *current,
        *pending));
}

TEST(
    SceneFrameStateTest,
    PrimaryTakeoverAcceptsStableRealTerrainNearSelectedLevel) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey selectedKey{"Geographic-TMS", 6, 32, 20};
    const TileKey parentKey{"Geographic-TMS", 5, 16, 10};
    TilesetTile* currentTile =
        makeTakeoverRenderTile(*current, selectedKey);
    TilesetTile* pendingTile =
        makeTakeoverRenderTile(*pending, parentKey);
    ASSERT_NE(nullptr, currentTile);
    ASSERT_NE(nullptr, pendingTile);
    setSingleTakeoverEntry(
        *current,
        *currentTile,
        selectedKey,
        selectedKey);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        selectedKey,
        parentKey);

    EXPECT_TRUE(ScenePrimaryTilesetTakeoverPolicy::isCandidateReady(
        *current,
        *pending));
}

TEST(
    SceneFrameStateTest,
    PrimaryTakeoverRejectsReadyRootBelowCurrentCoverageLevel) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey currentKey{"Geographic-TMS", 6, 32, 20};
    const TileKey pendingRoot{"Geographic-TMS", 0, 0, 0};
    TilesetTile* currentTile =
        makeTakeoverRenderTile(*current, currentKey);
    TilesetTile* pendingTile =
        makeTakeoverRenderTile(*pending, pendingRoot);
    ASSERT_NE(nullptr, currentTile);
    ASSERT_NE(nullptr, pendingTile);
    setSingleTakeoverEntry(
        *current,
        *currentTile,
        currentKey,
        currentKey);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        pendingRoot,
        pendingRoot);

    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::isCandidateReady(
        *current,
        *pending));
}

TEST(
    SceneFrameStateTest,
    PrimaryTakeoverRequiresConsecutiveStableSelectionFrames) {
    DummyRenderDevice device;
    auto current = makeTakeoverTileset(device);
    auto pending = makeTakeoverTileset(device);
    const TileKey selectedKey{"Geographic-TMS", 6, 32, 20};
    const TileKey parentKey{"Geographic-TMS", 5, 16, 10};
    const TileKey coarseKey{"Geographic-TMS", 2, 2, 1};
    TilesetTile* currentTile =
        makeTakeoverRenderTile(*current, selectedKey);
    TilesetTile* pendingTile =
        makeTakeoverRenderTile(*pending, parentKey);
    ASSERT_NE(nullptr, currentTile);
    ASSERT_NE(nullptr, pendingTile);
    setSingleTakeoverEntry(
        *current,
        *currentTile,
        selectedKey,
        selectedKey);
    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        selectedKey,
        parentKey);

    ScenePrimaryTilesetTakeoverState state;
    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::shouldCommit(
        state,
        *current,
        *pending));
    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::shouldCommit(
        state,
        *current,
        *pending));

    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        selectedKey,
        coarseKey);
    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::shouldCommit(
        state,
        *current,
        *pending));
    EXPECT_EQ(0, state.consecutiveReadyFrames);

    setSingleTakeoverEntry(
        *pending,
        *pendingTile,
        selectedKey,
        parentKey);
    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::shouldCommit(
        state,
        *current,
        *pending));
    EXPECT_FALSE(ScenePrimaryTilesetTakeoverPolicy::shouldCommit(
        state,
        *current,
        *pending));
    EXPECT_TRUE(ScenePrimaryTilesetTakeoverPolicy::shouldCommit(
        state,
        *current,
        *pending));
}

TEST(SceneFrameStateTest, SelectorViewOverrideFeedsMultipleViews) {
    Scene scene;
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());
    scene.update(1.0 / 60.0);
    EXPECT_EQ(scene.frameState().selectorViews.size(), 1u);

    Camera secondCamera;
    secondCamera.lookAt(
        target + Vec3(0.0, 1000000.0, 0.0),
        target,
        Vec3::unitZ());
    scene.setSelectorViewOverride({
        makeSelectorView(scene.camera(), 800, 600),
        makeSelectorView(secondCamera, 800, 600)});
    scene.update(1.0 / 60.0);
    EXPECT_EQ(scene.frameState().selectorViews.size(), 2u);

    scene.setSelectorViewOverride({});
    scene.update(1.0 / 60.0);
    EXPECT_TRUE(scene.frameState().selectorViews.empty());

    scene.clearSelectorViewOverride();
    scene.update(1.0 / 60.0);
    EXPECT_EQ(scene.frameState().selectorViews.size(), 1u);
}

TEST(SceneFrameStateTest, FrameDiagnosticsResetSmoothsAndRecordsTiming) {
    Diagnostics diagnostics;
    diagnostics.fps = 30.0;
    diagnostics.frameTimeMs = 123.0;
    diagnostics.cameraUpdateMs = 12.0;
    diagnostics.environmentUpdateMs = 13.0;
    diagnostics.basemapStackUpdateMs = 14.0;
    diagnostics.terrainUpdateMs = 15.0;
    diagnostics.contentTilesetUpdateMs = 16.0;
    diagnostics.renderCommandBuildMs = 17.0;
    diagnostics.renderSubmitMs = 18.0;
    diagnostics.drawCalls = 9;

    SceneFrameDiagnostics::resetPerFrame(diagnostics);
    EXPECT_EQ(diagnostics.cameraUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.environmentUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.basemapStackUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.terrainUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.contentTilesetUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.renderCommandBuildMs, 0.0);
    EXPECT_EQ(diagnostics.renderSubmitMs, 0.0);
    EXPECT_EQ(diagnostics.drawCalls, 9);

    SceneFrameDiagnostics::updateFrameRate(diagnostics, 0.5);
    EXPECT_NEAR(diagnostics.frameTimeMs, 500.0, 1e-9);
    EXPECT_NEAR(diagnostics.fps, 27.2, 1e-9);

    SceneFrameDiagnostics::updateFrameRate(diagnostics, 0.0);
    EXPECT_NEAR(diagnostics.frameTimeMs, 500.0, 1e-9);
    EXPECT_NEAR(diagnostics.fps, 27.2, 1e-9);

    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics,
        SceneFrameDiagnostics::EngineTimingScope::BeginFrame,
        1.25);
    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics,
        SceneFrameDiagnostics::EngineTimingScope::SceneUpdate,
        2.5);
    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics,
        SceneFrameDiagnostics::EngineTimingScope::SceneRender,
        3.75);
    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics,
        SceneFrameDiagnostics::EngineTimingScope::EndFrame,
        4.0);
    SceneFrameDiagnostics::finishEngineFrame(diagnostics, 11.5);
    EXPECT_NEAR(diagnostics.engineBeginFrameMs, 1.25, 1e-9);
    EXPECT_NEAR(diagnostics.sceneUpdateMs, 2.5, 1e-9);
    EXPECT_NEAR(diagnostics.sceneRenderMs, 3.75, 1e-9);
    EXPECT_NEAR(diagnostics.engineEndFrameMs, 4.0, 1e-9);
    EXPECT_NEAR(diagnostics.engineFrameCpuMs, 11.5, 1e-9);
}

TEST(SceneFrameStateTest, FrameStateBuilderPopulatesPerFrameState) {
    Camera camera;
    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    camera.lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    FrameState frameState;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 600;
    SceneFrameStateBuildResult buildResult =
        SceneFrameStateBuilder::build(SceneFrameStateBuildInput{
            frameState,
            &camera,
            42,
            10.0,
            0.5,
            false,
            nullptr,
            true,
            Vec3::unitX(),
            8.0,
            nullptr,
            nullptr});

    EXPECT_EQ(frameState.frameId, 42u);
    EXPECT_NEAR(frameState.timeSeconds, 10.0, 1e-9);
    EXPECT_NEAR(frameState.deltaSeconds, 0.5, 1e-9);
    EXPECT_EQ(frameState.camera, &camera);
    ASSERT_EQ(frameState.selectorViews.size(), 1u);
    EXPECT_EQ(frameState.selectorViews.front().viewportHeightPixels, 600);
    EXPECT_TRUE(frameState.hasInteractionFocus);
    EXPECT_EQ(frameState.interactionFocusDirection, Vec3::unitX());
    EXPECT_EQ(buildResult.environmentUpdateMs, 0.0);

    std::vector<SelectorView> overrideViews{
        makeSelectorView(camera, 320, 240),
        makeSelectorView(camera, 640, 480)};
    SceneFrameStateBuilder::build(SceneFrameStateBuildInput{
        frameState,
        &camera,
        43,
        12.6,
        0.0,
        true,
        &overrideViews,
        true,
        Vec3::unitY(),
        10.0,
        nullptr,
        nullptr});

    ASSERT_EQ(frameState.selectorViews.size(), 2u);
    EXPECT_EQ(frameState.selectorViews[0].viewportHeightPixels, 240);
    EXPECT_EQ(frameState.selectorViews[1].viewportHeightPixels, 480);
    EXPECT_FALSE(frameState.hasInteractionFocus);
    EXPECT_EQ(frameState.interactionFocusDirection, Vec3::zero());

    SceneFrameStateBuilder::build(SceneFrameStateBuildInput{
        frameState,
        &camera,
        44,
        12.7,
        0.0,
        true,
        nullptr,
        false,
        Vec3::unitZ(),
        -1.0,
        nullptr,
        nullptr});

    EXPECT_TRUE(frameState.selectorViews.empty());
}

TEST(SceneFrameStateTest, PresentationTraceRecordsDeterministicCameraState) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(1024, 768, 2.0f);

    constexpr double kLongitudeDegrees = 116.3913;
    constexpr double kLatitudeDegrees = 39.9075;
    constexpr double kRangeMeters = 750000.0;
    const double pitch = Transforms::toRadians(-65.0);
    const double heading = Transforms::toRadians(35.0);
    scene.camera() = makeCameraFromCenterPitchHeading(
        kLongitudeDegrees,
        kLatitudeDegrees,
        kRangeMeters,
        pitch,
        heading);

    scene.update(1.0 / 60.0);
    scene.render();

    const PresentationTrace& trace = scene.presentationTrace();
    EXPECT_EQ(trace.camera.frameId, scene.frameState().frameId);
    EXPECT_EQ(trace.camera.viewportWidthPixels, 1024);
    EXPECT_EQ(trace.camera.viewportHeightPixels, 768);
    EXPECT_NEAR(trace.camera.devicePixelRatio, 2.0f, 1e-6f);
    EXPECT_NEAR(
        trace.camera.targetLongitudeDegrees,
        kLongitudeDegrees,
        1e-8);
    EXPECT_NEAR(
        trace.camera.targetLatitudeDegrees,
        kLatitudeDegrees,
        1e-8);
    EXPECT_NEAR(trace.camera.pitchRadians, pitch, 1e-10);
    EXPECT_NEAR(trace.camera.headingRadians, heading, 1e-10);
    ASSERT_EQ(trace.selectorViews.size(), 1u);
    EXPECT_EQ(trace.selectorViews.front().viewportHeightPixels, 768);
    EXPECT_FALSE(trace.commands.empty());
}

TEST(SceneFrameStateTest, OcclusionCallbackFeedsPrimaryAndAdditionalTilesets) {
    Scene scene;
    scene.setOcclusionCallback(
        [](const TilesetTile&) { return TileOcclusionState::Occluded; });

    auto makeTileset = []() {
        return std::make_unique<Tileset>(
            TileScheme::createGeographicTMS(),
            std::vector<ActivatedRasterOverlay*>{},
            nullptr,
            TilesetOptions{});
    };

    auto primaryTileset = makeTileset();
    Tileset* primaryRaw = primaryTileset.get();
    scene.setTileset(std::move(primaryTileset));

    auto additionalTileset = makeTileset();
    Tileset* additionalRaw = additionalTileset.get();
    scene.addTileset(std::move(additionalTileset));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* primaryRoot =
        TilesetTestAccess::ensureTile(*primaryRaw, rootKey);
    TilesetTile* additionalRoot =
        TilesetTestAccess::ensureTile(*additionalRaw, rootKey);
    ASSERT_NE(primaryRoot, nullptr);
    ASSERT_NE(additionalRoot, nullptr);

    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(*primaryRaw, *primaryRoot),
        TileOcclusionState::Occluded);
    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(*additionalRaw, *additionalRoot),
        TileOcclusionState::Occluded);

    scene.setOcclusionCallback(
        [](const TilesetTile&) { return TileOcclusionState::NotOccluded; });
    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(*primaryRaw, *primaryRoot),
        TileOcclusionState::NotOccluded);
    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(*additionalRaw, *additionalRoot),
        TileOcclusionState::NotOccluded);
}

TEST(
    SceneFrameStateTest,
    AdditionalTilesetDoesNotReplacePrimaryTerrainSampling) {
    Scene scene;
    scene.setViewport(800, 600, 1.0f);

    const TileKey westRoot{"Geographic-TMS", 0, 0, 0};
    const TileKey eastRoot{"Geographic-TMS", 0, 1, 0};

    auto terrainTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        nullptr,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();
    TilesetTile* westTile = TilesetTestAccess::ensureTile(
        *terrainRaw,
        westRoot);
    TilesetTile* eastTile = TilesetTestAccess::ensureTile(
        *terrainRaw,
        eastRoot);
    ASSERT_NE(westTile, nullptr);
    ASSERT_NE(eastTile, nullptr);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *westTile,
        makeFlatGeographicTerrainGltfModel(westTile->bounds, 123.0),
        123.0);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *eastTile,
        makeFlatGeographicTerrainGltfModel(eastTile->bounds, 123.0),
        123.0);

    scene.setTileset(std::move(terrainTileset));
    ASSERT_EQ(scene.tileset(), terrainRaw);
    EXPECT_NEAR(scene.tileset()->sampleHeight(0.0, 0.0), 123.0f, 1e-6f);

    auto contentTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        nullptr,
        TilesetOptions{});
    TilesetTestAccess::ensureTile(*contentTileset, westRoot);
    TilesetTestAccess::ensureTile(*contentTileset, eastRoot);

    scene.addTileset(std::move(contentTileset));

    EXPECT_EQ(scene.tileset(), terrainRaw);
    EXPECT_EQ(scene.additionalTilesetCount(), 1u);
    EXPECT_NEAR(scene.tileset()->sampleHeight(0.0, 0.0), 123.0f, 1e-6f);
}

TEST(SceneFrameStateTest, AdditionalTilesetRendersGltfContent) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto terrainTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();
    TilesetTile* terrainRoot =
        TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    ASSERT_NE(terrainRoot, nullptr);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *terrainRoot,
        makeFlatGeographicTerrainGltfModel(terrainRoot->bounds, 123.0),
        123.0,
        &device);
    scene.setTileset(std::move(terrainTileset));

    auto contentTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    TilesetTile* contentRoot =
        TilesetTestAccess::ensureTile(*contentTileset, rootKey);
    ASSERT_NE(contentRoot, nullptr);
    contentRoot->content.renderContent.setGltfContent(
        makeTriangleGltfModel());
    contentRoot->markRenderContentLoaded();
    GltfRenderResourcePreparer::prepare(*contentRoot, &device, 0.0);

    scene.addTileset(std::move(contentTileset));
    scene.update(1.0 / 60.0);
    scene.render();

    const bool submittedGltf = std::any_of(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive;
        });
    const bool submittedTerrainGltf = std::any_of(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive &&
                   cmd.terrainRenderContent;
        });
    const bool submittedNonTerrainGltf = std::any_of(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive &&
                   !cmd.terrainRenderContent;
        });
    EXPECT_TRUE(submittedGltf);
    EXPECT_TRUE(submittedTerrainGltf);
    EXPECT_TRUE(submittedNonTerrainGltf);
    EXPECT_GT(scene.diagnostics().renderGltfPrimitives, 0);
    EXPECT_GT(scene.diagnostics().terrainSurfaceTileCommands, 0);
    EXPECT_GT(scene.diagnostics().terrainGltfPrimitiveCommands, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderContentCommands, 0);
    EXPECT_EQ(scene.diagnostics().contentTilesets, 1);
    EXPECT_GT(scene.diagnostics().contentVisibleTiles, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderEntriesPlanned, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderEntriesSelectedPlanned, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesAncestorFallback, 0);
    EXPECT_GE(scene.diagnostics().terrainRenderEntriesSynchronousPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferredPrep, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderEntriesDrawn, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderEntriesSelectedDrawn, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferred, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDeferred, 0);
    EXPECT_GT(scene.diagnostics().terrainSurfaceCommandsSubmitted, 0);
    EXPECT_EQ(scene.tileset(), terrainRaw);
    EXPECT_NEAR(scene.tileset()->sampleHeight(0.0, 0.0), 123.0f, 1e-6f);
}

TEST(SceneFrameStateTest, GltfTerrainCountsAsTerrainRenderContent) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto terrainTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    TilesetTile* root =
        TilesetTestAccess::ensureTile(*terrainTileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.renderContent.setGltfContent(makeTriangleGltfModel());
    root->content.renderContent.setTerrainRenderContent(true);
    root->markRenderContentLoaded();
    GltfRenderResourcePreparer::prepare(*root, &device, 0.0);
    scene.setTileset(std::move(terrainTileset));

    scene.update(1.0 / 60.0);
    scene.render();

    const int terrainGltfCommands = static_cast<int>(std::count_if(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive &&
                   cmd.terrainRenderContent;
        }));
    const int gltfCommands = static_cast<int>(std::count_if(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive;
        }));
    // renderGltfPrimitives 统计的是本帧提交的全部 glTF primitive 命令,不只是
    // 地形:极帽(PolarCapRenderer)复用同一命令种类但不带 terrainRenderContent,
    // 所以总数 = 地形命令 + 两片极帽。本用例断言的是「glTF 地形被计成地形内容」,
    // 由 terrainRenderContentCommands 来钉,总数只需与真实提交量一致。
    EXPECT_GT(terrainGltfCommands, 0);
    EXPECT_EQ(scene.diagnostics().renderGltfPrimitives, gltfCommands);
    EXPECT_EQ(gltfCommands - terrainGltfCommands, countPolarCapCommands(device));
    EXPECT_GE(
        scene.diagnostics().terrainRenderContentCommands,
        terrainGltfCommands);
}

TEST(SceneFrameStateTest, PresentationTraceCopiesRenderEntryPassFailures) {
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});
    TilePlan& plan = TilesetTestAccess::mutableTilePlan(tileset);
    plan.renderEntryCommandMissedDrawCount = 5;
    plan.renderEntrySelectedCommandMissedDrawCount = 2;
    plan.renderEntryCommandDeferredCount = 7;
    plan.renderEntrySelectedCommandDeferredCount = 4;

    FrameState frameState;
    frameState.frameId = 3;
    RenderCommandList commands;
    const PresentationTrace trace = ScenePresentationTraceBuilder::build(
        ScenePresentationTraceInput{
            frameState,
            &tileset,
            emptyContentTilesets(),
            commands});

    ASSERT_EQ(1u, trace.tilesets.size());
    const PresentationTilesetTrace& tilesetTrace = trace.tilesets.front();
    EXPECT_EQ(5, tilesetTrace.renderEntryCommandMissedDrawCount);
    EXPECT_EQ(2, tilesetTrace.renderEntrySelectedCommandMissedDrawCount);
    EXPECT_EQ(7, tilesetTrace.renderEntryCommandDeferredCount);
    EXPECT_EQ(4, tilesetTrace.renderEntrySelectedCommandDeferredCount);
}

TEST(SceneFrameStateTest, PresentationTraceExposesFadingRenderEntry) {
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});
    TilePlan& plan = TilesetTestAccess::mutableTilePlan(tileset);
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TileRenderEntry entry;
    entry.selectedKey = rootKey;
    entry.renderKey = rootKey;
    entry.opacity = 0.75f;
    entry.selectedThisFrame = false;
    plan.renderEntries.push_back(entry);
    plan.renderEntryPlannedCommandCount = 1;
    plan.renderEntrySelectedPlannedCommandCount = 0;
    plan.renderEntryCommandDrawCount = 1;
    plan.renderEntrySelectedCommandDrawCount = 0;

    FrameState frameState;
    frameState.frameId = 7;
    RenderCommandList commands;
    const PresentationTrace trace = ScenePresentationTraceBuilder::build(
        ScenePresentationTraceInput{
            frameState,
            &tileset,
            emptyContentTilesets(),
            commands});

    ASSERT_EQ(1u, trace.tilesets.size());
    const PresentationTilesetTrace& tilesetTrace = trace.tilesets.front();
    ASSERT_EQ(1u, tilesetTrace.renderEntries.size());
    const PresentationRenderEntryTrace& entryTrace =
        tilesetTrace.renderEntries.front();
    EXPECT_FALSE(entryTrace.selectedThisFrame);
    EXPECT_NEAR(0.75f, entryTrace.opacity, 1e-6f);
    EXPECT_EQ(1, tilesetTrace.renderEntryPlannedCommandCount);
    EXPECT_EQ(0, tilesetTrace.renderEntrySelectedPlannedCommandCount);
    EXPECT_EQ(1, tilesetTrace.renderEntryCommandDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntrySelectedCommandDrawCount);
}

TEST(SceneFrameStateTest, PresentationTraceExposesAdditiveSelectedEntries) {
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});
    TilePlan& plan = TilesetTestAccess::mutableTilePlan(tileset);
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TileRenderEntry rootEntry;
    rootEntry.selectedKey = rootKey;
    rootEntry.renderKey = rootKey;
    TileRenderEntry childEntry;
    childEntry.selectedKey = childKey;
    childEntry.renderKey = childKey;
    plan.renderEntries = {rootEntry, childEntry};
    plan.renderEntryPlannedCommandCount = 2;
    plan.renderEntrySelectedPlannedCommandCount = 2;
    plan.renderEntryCommandDrawCount = 2;
    plan.renderEntrySelectedCommandDrawCount = 2;

    FrameState frameState;
    frameState.frameId = 11;
    RenderCommandList commands;
    const PresentationTrace trace = ScenePresentationTraceBuilder::build(
        ScenePresentationTraceInput{
            frameState,
            &tileset,
            emptyContentTilesets(),
            commands});

    ASSERT_EQ(1u, trace.tilesets.size());
    const PresentationTilesetTrace& tilesetTrace = trace.tilesets.front();
    ASSERT_EQ(2u, tilesetTrace.renderEntries.size());
    EXPECT_EQ(rootKey, tilesetTrace.renderEntries[0].selectedKey);
    EXPECT_EQ(childKey, tilesetTrace.renderEntries[1].selectedKey);
    EXPECT_TRUE(tilesetTrace.renderEntries[0].selectedThisFrame);
    EXPECT_TRUE(tilesetTrace.renderEntries[1].selectedThisFrame);
    EXPECT_EQ(2, tilesetTrace.renderEntryPlannedCommandCount);
    EXPECT_EQ(2, tilesetTrace.renderEntrySelectedPlannedCommandCount);
    EXPECT_EQ(2, tilesetTrace.renderEntryCommandDrawCount);
    EXPECT_EQ(2, tilesetTrace.renderEntrySelectedCommandDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryAncestorFallbackCount);
}

TEST(SceneFrameStateTest, GltfTerrainDiagnosticsDoNotUseLegacySurfacePrep) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    auto baseOverlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        makeRasterOverlayOptions());
    ActivatedRasterOverlay baseActivated(*baseOverlay);
    auto terrainTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::setLoadedGltfTerrainContent(
        *root,
        makeFlatGeographicTerrainGltfModel(root->bounds, 0.0),
        0.0,
        &device);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    DirectRasterMapping* rootMapped =
        root->rasterOverlayState.mappings().empty()
            ? nullptr
            : root->rasterOverlayState.mappings()[0].get();
    RasterOverlayTile* rootRaster =
        rootMapped ? rootMapped->getLoadingTile() : nullptr;
    ASSERT_NE(rootRaster, nullptr);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    ASSERT_TRUE(root->hasSurfaceDrawable());

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *root);
    scene.render();

    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesPlanned, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedPlanned, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesAncestorFallback, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSynchronousPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferredPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesMissed, 0);
    EXPECT_GT(scene.diagnostics().terrainSurfaceCommandsSubmitted, 0);
    // 地形一条 + 极帽两条(见 countPolarCapCommands):本用例钉的是「地形走 glTF
    // 命令、不走 legacy surface prep」,由 terrainRenderContentCommands 断言。
    EXPECT_EQ(
        scene.diagnostics().renderGltfPrimitives,
        1 + countPolarCapCommands(device));
    EXPECT_EQ(scene.diagnostics().terrainRenderContentCommands, 1);
}

TEST(SceneFrameStateTest, DiagnosticsRejectImageryOnlyAncestorFallback) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    auto baseOverlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        makeRasterOverlayOptions());
    ActivatedRasterOverlay baseActivated(*baseOverlay);
    auto terrainTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(*terrainRaw, childKey);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);

    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    DirectRasterMapping* rootMapped =
        root->rasterOverlayState.mappings().empty()
            ? nullptr
            : root->rasterOverlayState.mappings()[0].get();
    RasterOverlayTile* rootRaster =
        rootMapped ? rootMapped->getLoadingTile() : nullptr;
    ASSERT_NE(rootRaster, nullptr);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    ASSERT_FALSE(root->hasSurfaceDrawable());

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::setInteractionActiveForFrame(*terrainRaw, true);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *child);
    scene.render();

    EXPECT_GE(scene.diagnostics().terrainRenderEntriesPlanned, 0);
    EXPECT_GE(scene.diagnostics().terrainRenderEntriesSelectedPlanned, 0);
    EXPECT_GE(scene.diagnostics().terrainRenderEntriesAncestorFallback, 0);
    EXPECT_GE(scene.diagnostics().terrainRenderEntriesSynchronousPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferredPrep, 0);
    EXPECT_GE(scene.diagnostics().terrainRenderEntriesDrawn, 0);
    EXPECT_GE(scene.diagnostics().terrainRenderEntriesSelectedDrawn, 0);
    EXPECT_GE(scene.diagnostics().terrainRenderEntriesMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferred, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDeferred, 0);
    EXPECT_GE(scene.diagnostics().terrainSurfaceCommandsSubmitted, 0);
}

TEST(SceneFrameStateTest, SortsTransparentGltfByCameraDepth) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    const Vec3 cameraPosition = target + Vec3(1000000.0, 0.0, 0.0);
    scene.camera().lookAt(cameraPosition, target, Vec3::unitZ());

    auto contentTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root =
        TilesetTestAccess::ensureTile(*contentTileset, rootKey);
    ASSERT_NE(root, nullptr);

    auto model = std::make_unique<GltfModel>();
    model->primitives.push_back(
        makeTransparentTrianglePrimitiveAt(target + Vec3(900000.0, 0.0, 0.0)));
    model->primitives.push_back(makeTransparentTrianglePrimitiveAt(target));
    root->content.renderContent.setGltfContent(std::move(model));
    root->markRenderContentLoaded();
    GltfRenderResourcePreparer::prepare(*root, &device, 0.0);
    scene.setTileset(std::move(contentTileset));

    scene.update(1.0 / 60.0);
    scene.render();

    std::vector<RenderCommand> transparentGltf;
    for (const RenderCommand& cmd : device.submittedCommands) {
        if (cmd.kind == RenderCommandKind::GltfPrimitive && cmd.blend) {
            transparentGltf.push_back(cmd);
        }
    }

    ASSERT_EQ(transparentGltf.size(), 2u);
    EXPECT_TRUE(transparentGltf[0].hasTranslucentSortDepth);
    EXPECT_TRUE(transparentGltf[1].hasTranslucentSortDepth);
    EXPECT_GT(
        transparentGltf[0].translucentSortDepth,
        transparentGltf[1].translucentSortDepth);
    EXPECT_LT(
        std::abs(transparentGltf[0].translucentSortDepth - 1000000.0),
        1.0);
    EXPECT_LT(
        std::abs(transparentGltf[1].translucentSortDepth - 100000.0),
        1.0);
}
