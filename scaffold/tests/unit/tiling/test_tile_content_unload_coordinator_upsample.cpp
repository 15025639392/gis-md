#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TileContentUnloadCoordinator.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

std::unique_ptr<GltfModel> makeQuadTerrainGltfModel(
    const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    const Vec3 nodeOrigin(100.0, 200.0, 300.0);
    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(nodeOrigin);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.mesh = 0;
    rootNode.hasMatrix = true;
    rootNode.baseTranslation = {nodeOrigin.x(), nodeOrigin.y(), nodeOrigin.z()};
    rootNode.translation = rootNode.baseTranslation;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);
    GltfPrimitive primitive;
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = nodeOrigin + Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = nodeOrigin + Vec3(2.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = nodeOrigin + Vec3(0.0, 2.0, 0.0);
    primitive.vertices[3].positionEcef = nodeOrigin + Vec3(2.0, 2.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertices[3].uv = {1.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f},
        std::array<float, 2>{1.0f, 1.0f}};
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.runtime.baseVertices = primitive.vertices;
    for (SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        vertex.positionEcef = vertex.positionEcef - nodeOrigin;
    }
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

struct ProtectedSourceFixture {
    TilesetTile parent{TileKey{"test", 0, 0, 0}, Rectangle{}};
    TilesetTile child{TileKey{"test", 1, 0, 0}, Rectangle{}, &parent};
    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    TileEmptyContentRegistry emptyContentRegistry;

    ProtectedSourceFixture() {
        parent.children.push_back(&child);
        parent.content.contentKind = TileContentKind::Render;
        parent.content.loadState = TileLoadState::Done;
        auto gltfModel = makeQuadTerrainGltfModel(parent.bounds);
        parent.content.renderContent.prepareGltfContent(
            std::move(gltfModel), Mat4::identity());
        parent.content.renderContent.setTerrainRenderContent(true);
        parent.content.renderContent.addGltfPrimitiveResource(
            GltfPrimitiveRenderResources{});
        parent.content.renderContent.markRenderContentReady();
        parent.selectionFrameState.renderable = true;
        parent.selectionFrameState.completeRenderable = true;
        child.content.markTerrainAvailabilityUpsample();
        child.content.loadState = TileLoadState::ContentLoading;
        terrainCache[cacheKey] = std::make_unique<DecodedHeightmap>();
    }

    TileCacheUnloadContentResult unloadParent() {
        return TileContentUnloadCoordinator::unloadContent(
            parent,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);
    }
};

} // namespace

TEST(
    TileContentUnloadCoordinatorUpsampleTest,
    ProtectedUpsampleSourceReleasesMainThreadResourcesAndKeepsContent) {
    ProtectedSourceFixture fixture;

    const TileCacheUnloadContentResult firstResult = fixture.unloadParent();

    EXPECT_EQ(firstResult, TileCacheUnloadContentResult::Keep);
    EXPECT_EQ(fixture.parent.content.contentKind, TileContentKind::Render);
    EXPECT_EQ(fixture.parent.content.loadState, TileLoadState::Unloading);
    EXPECT_TRUE(fixture.parent.content.renderContent.hasGltfContent());
    EXPECT_FALSE(fixture.parent.content.renderContent.isMeshReady());
    EXPECT_FALSE(fixture.parent.content.renderContent.isSurfaceDrawable());
    EXPECT_EQ(fixture.parent.content.renderContent.currentSurfaceSource(),
              SurfaceDrawableSource::None);
    EXPECT_FALSE(fixture.parent.selectionFrameState.renderable);
    EXPECT_FALSE(fixture.parent.selectionFrameState.completeRenderable);
    EXPECT_NE(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());

    const TileCacheUnloadContentResult secondResult = fixture.unloadParent();

    EXPECT_EQ(secondResult, TileCacheUnloadContentResult::Keep);
    EXPECT_EQ(fixture.parent.content.contentKind, TileContentKind::Render);
    EXPECT_EQ(fixture.parent.content.loadState, TileLoadState::Unloading);
    EXPECT_TRUE(fixture.parent.content.renderContent.hasGltfContent());
    EXPECT_NE(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());
}

TEST(
    TileContentUnloadCoordinatorUpsampleTest,
    CompletedProtectedUpsampleSourceFinishesUnload) {
    ProtectedSourceFixture fixture;
    fixture.parent.content.loadState = TileLoadState::Unloading;
    fixture.child.content.loadState = TileLoadState::Done;

    const TileCacheUnloadContentResult result = fixture.unloadParent();

    EXPECT_EQ(result, TileCacheUnloadContentResult::Remove);
    EXPECT_EQ(fixture.parent.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(fixture.parent.content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(fixture.parent.content.renderContent.hasGltfContent());
    EXPECT_FALSE(fixture.parent.content.renderContent.isMeshReady());
    EXPECT_EQ(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());
}

TEST(
    TileContentUnloadCoordinatorUpsampleTest,
    NestedUpsampleDoesNotProtectNonSourceAncestorContent) {
    ProtectedSourceFixture fixture;
    TilesetTile grandchild{
        TileKey{"test", 2, 0, 0},
        Rectangle{},
        &fixture.child};
    fixture.child.children.push_back(&grandchild);
    fixture.child.content.clearUpsampleKind();
    fixture.child.content.loadState = TileLoadState::Done;
    grandchild.content.markTerrainAvailabilityUpsample();
    grandchild.content.loadState = TileLoadState::ContentLoading;

    const TileCacheUnloadContentResult result = fixture.unloadParent();

    EXPECT_EQ(result, TileCacheUnloadContentResult::Remove);
    EXPECT_EQ(fixture.parent.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(fixture.parent.content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(fixture.parent.content.renderContent.hasGltfContent());
    EXPECT_EQ(fixture.terrainCache.find(fixture.cacheKey),
              fixture.terrainCache.end());
}
