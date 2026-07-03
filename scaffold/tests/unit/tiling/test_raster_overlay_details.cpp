#include <gtest/gtest.h>

#include <memory>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/SurfaceTile.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileLoadResultMetadataApplicator.h"
#include "earth_engine/tiling/TileRasterOverlayDetailsDeriver.h"
#include "earth_engine/tiling/TileRasterOverlayDetailsGenerator.h"
#include "earth_engine/tiling/TileRasterOverlayMappingPolicy.h"
#include "earth_engine/tiling/TileRenderContentState.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"

using namespace earth_engine;

namespace {

class TestTexture final : public Texture {
public:
    TestTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
};

class TestBuffer final : public Buffer {
public:
    explicit TestBuffer(size_t byteSize) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }

private:
    size_t byteSize_ = 0;
};

void prepareGltfRenderContent(TileRenderContentState& renderContent,
                              RasterOverlayDetails details = {}) {
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails = std::move(details);
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
}

std::unique_ptr<GltfModel> makeTerrainQuadModel(const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const std::array<Cartographic, 4> corners = {
        Cartographic::fromRadians(rectangle.west(), rectangle.south(), 0.0),
        Cartographic::fromRadians(rectangle.east(), rectangle.south(), 0.0),
        Cartographic::fromRadians(rectangle.east(), rectangle.north(), 0.0),
        Cartographic::fromRadians(rectangle.west(), rectangle.north(), 0.0)};
    for (const Cartographic& corner : corners) {
        SurfaceVertex vertex;
        vertex.positionEcef = ellipsoid.cartographicToCartesian(corner);
        primitive.vertices.push_back(vertex);
    }
    primitive.indices = {0, 1, 2, 0, 2, 3};
    model->primitives.push_back(std::move(primitive));
    return model;
}

std::unique_ptr<GltfModel> makeRtcTerrainQuadModel(const Rectangle& rectangle) {
    auto model = makeTerrainQuadModel(rectangle);
    const Cartographic center = Cartographic::fromRadians(
        rectangle.west() + rectangle.width() * 0.5,
        rectangle.south() + rectangle.height() * 0.5,
        0.0);
    const Vec3 origin =
        Ellipsoid::WGS84().cartographicToCartesian(center);

    GltfNodeRuntime node;
    node.baseLocalTransform = Mat4::translation(origin);
    node.localTransform = node.baseLocalTransform;
    node.globalTransform = node.baseLocalTransform;
    node.baseTranslation = {origin.x(), origin.y(), origin.z()};
    node.translation = node.baseTranslation;
    node.mesh = 0;
    node.hasMatrix = false;
    model->nodes.push_back(node);
    model->sceneRootNodes.push_back(0);

    GltfPrimitive& primitive = model->primitives.front();
    primitive.runtime.nodeIndex = 0;
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.positionEcef = vertex.positionEcef - origin;
    }
    primitive.runtime.baseVertices = primitive.vertices;
    return model;
}

std::unique_ptr<GltfModel> makeContentTransformTerrainQuadModel(
    const Rectangle& rectangle,
    Mat4& contentTransform) {
    auto model = makeTerrainQuadModel(rectangle);
    const Cartographic center = Cartographic::fromRadians(
        rectangle.west() + rectangle.width() * 0.5,
        rectangle.south() + rectangle.height() * 0.5,
        0.0);
    const Vec3 origin =
        Ellipsoid::WGS84().cartographicToCartesian(center);
    contentTransform = Mat4::translation(origin);

    GltfPrimitive& primitive = model->primitives.front();
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.positionEcef = vertex.positionEcef - origin;
    }
    primitive.runtime.baseVertices = primitive.vertices;
    return model;
}

std::unique_ptr<GltfModel> makeTerrainQuadModelWithSkirt(
    const Rectangle& rectangle,
    double skirtHeight) {
    auto model = makeTerrainQuadModel(rectangle);
    GltfPrimitive& primitive = model->primitives.front();

    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Cartographic skirtCartographic = Cartographic::fromRadians(
        rectangle.west(),
        rectangle.south(),
        -skirtHeight);
    SurfaceVertex skirtVertex;
    skirtVertex.positionEcef =
        ellipsoid.cartographicToCartesian(skirtCartographic);
    skirtVertex.normalEcef =
        ellipsoid.geodeticSurfaceNormal(skirtVertex.positionEcef);
    primitive.vertices.push_back(skirtVertex);
    primitive.indices = {0, 1, 2, 0, 2, 3, 0, 4, 1};

    SkirtMetadata skirt;
    skirt.noSkirtIndicesBegin = 0;
    skirt.noSkirtIndicesCount = 6;
    skirt.noSkirtVerticesBegin = 0;
    skirt.noSkirtVerticesCount = 4;
    skirt.meshCenter = Vec3::zero();
    skirt.skirtWestHeight = skirtHeight;
    skirt.skirtSouthHeight = skirtHeight;
    skirt.skirtEastHeight = skirtHeight;
    skirt.skirtNorthHeight = skirtHeight;
    primitive.skirtMetadata = skirt;
    return model;
}

void prepareTerrainQuadRenderContent(TileRenderContentState& renderContent,
                                     const Rectangle& rectangle,
                                     RasterOverlayDetails details = {}) {
    auto model = makeTerrainQuadModel(rectangle);
    model->rasterOverlayDetails = std::move(details);
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
}

void expectRectangleNear(const Rectangle& expected,
                         const Rectangle& actual,
                         double epsilon = 1e-12) {
    EXPECT_NEAR(expected.west(), actual.west(), epsilon);
    EXPECT_NEAR(expected.south(), actual.south(), epsilon);
    EXPECT_NEAR(expected.east(), actual.east(), epsilon);
    EXPECT_NEAR(expected.north(), actual.north(), epsilon);
}

} // namespace

TEST(RasterOverlayDetailsTest,
     PreparingGltfTerrainReplacesPreviousGltfContent) {
    TileRenderContentState renderContent;
    auto model = std::make_unique<GltfModel>();
    RasterOverlayDetails gltfDetails;
    const Rectangle gltfRectangle(0.0, 0.0, 1.0, 1.0);
    gltfDetails.setGeographicRectangle(gltfRectangle, -1.0, 1.0);
    model->rasterOverlayDetails = std::move(gltfDetails);
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const Rectangle staleRectangle(2.0, 2.0, 3.0, 3.0);
    auto staleModel = makeTerrainQuadModel(staleRectangle);
    RasterOverlayDetails staleDetails;
    staleDetails.setGeographicRectangle(
        staleRectangle,
        10.0,
        20.0);
    staleModel->rasterOverlayDetails = std::move(staleDetails);
    renderContent.prepareGltfContent(std::move(staleModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
    renderContent.addGltfPrimitiveResource(GltfPrimitiveRenderResources{});
    renderContent.markRenderContentReady();
    renderContent.setRetainedHeightmap(std::make_unique<DecodedHeightmap>());
    renderContent.setSurfaceSource(SurfaceDrawableSource::HeightmapTerrain);

    EXPECT_TRUE(renderContent.hasGltfContent());
    EXPECT_TRUE(renderContent.isTerrainRenderContent());
    EXPECT_FALSE(renderContent.hasRetainedHeightmap());
    EXPECT_EQ(SurfaceDrawableSource::GltfContent,
              renderContent.currentSurfaceSource());
    ASSERT_EQ(1u,
              renderContent.rasterOverlayDetails()
                  .rasterOverlayRectangles.size());
    expectRectangleNear(
        staleRectangle,
        renderContent.rasterOverlayDetails().rasterOverlayRectangles.front());
}

TEST(RasterOverlayDetailsTest,
     ContentLoadingTerrainGltfResidueDoesNotMapRasterOverlays) {
    const Rectangle tileRectangle(0.0, 0.0, 1.0, 1.0);
    RasterOverlayDetails details;
    details.setGeographicRectangle(tileRectangle, -1.0, 1.0);

    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        tileRectangle);
    prepareTerrainQuadRenderContent(
        tile.content.renderContent,
        tileRectangle,
        details);
    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::ContentLoading;

    const TileRasterOverlayMappingContext context =
        TileRasterOverlayMappingPolicy::contextFor(tile);

    EXPECT_FALSE(context.hasRenderContentDetails);
    EXPECT_FALSE(context.mapsLoadedRenderContent);
    EXPECT_TRUE(context.waitForContentTerrainDetails);
    EXPECT_TRUE(context.details().empty());
    EXPECT_EQ(
        nullptr,
        TileRasterOverlayMappingPolicy::geometryRectangle(
            context,
            RasterOverlayProjection::Geographic));
    EXPECT_FALSE(
        TileRasterOverlayMappingPolicy::boundingVolumeRectangle(
            tile,
            context,
            RasterOverlayProjection::Geographic)
            .has_value());
}

TEST(RasterOverlayDetailsTest,
     ContentLoadedTerrainGltfMapsRasterOverlaysBeforeRenderResources) {
    const Rectangle tileRectangle(0.0, 0.0, 1.0, 1.0);
    RasterOverlayDetails details;
    details.setGeographicRectangle(tileRectangle, -1.0, 1.0);

    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        tileRectangle);
    prepareTerrainQuadRenderContent(
        tile.content.renderContent,
        tileRectangle,
        details);
    tile.markRenderContentLoaded();

    const TileRasterOverlayMappingContext context =
        TileRasterOverlayMappingPolicy::contextFor(tile);

    EXPECT_TRUE(context.hasRenderContentDetails);
    EXPECT_TRUE(context.mapsLoadedRenderContent);
    EXPECT_FALSE(context.waitForContentTerrainDetails);
    ASSERT_NE(
        nullptr,
        TileRasterOverlayMappingPolicy::geometryRectangle(
            context,
            RasterOverlayProjection::Geographic));
    expectRectangleNear(
        tileRectangle,
        *TileRasterOverlayMappingPolicy::geometryRectangle(
            context,
            RasterOverlayProjection::Geographic));
}

TEST(RasterOverlayDetailsTest,
     PreparingGltfTerrainClearsPreviousWaterMaskTexture) {
    TileRenderContentState renderContent;
    auto legacyModel = makeTerrainQuadModel(Rectangle(0.0, 0.0, 0.5, 0.5));
    renderContent.prepareGltfContent(std::move(legacyModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
    renderContent.addGltfPrimitiveResource(GltfPrimitiveRenderResources{});
    renderContent.markRenderContentReady();

    const int64_t retainedWithLegacy =
        renderContent.estimateRetainedBytes();
    ASSERT_TRUE(renderContent.hasGltfContent());
    EXPECT_EQ(nullptr, renderContent.surfaceWaterMaskTexture());

    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.setGeographicRectangle(
        Rectangle(0.0, 0.0, 1.0, 1.0),
        -1.0,
        1.0);
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    EXPECT_TRUE(renderContent.hasGltfContent());
    EXPECT_TRUE(renderContent.isTerrainRenderContent());
    EXPECT_EQ(nullptr, renderContent.surfaceWaterMaskTexture());
    EXPECT_LE(renderContent.estimateRetainedBytes(),
              retainedWithLegacy);
}

TEST(RasterOverlayDetailsTest,
     GltfTerrainRejectsLateLegacySurfaceGpuPayload) {
    TileRenderContentState renderContent;
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails.setGeographicRectangle(
        Rectangle(0.0, 0.0, 1.0, 1.0),
        -1.0,
        1.0);
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
    ASSERT_TRUE(renderContent.hasGltfContent());
    ASSERT_TRUE(renderContent.isTerrainRenderContent());

    renderContent.setSurfaceGpuBuffers(
        std::make_unique<TestBuffer>(64),
        std::make_unique<TestBuffer>(16));
    renderContent.setSurfaceWaterMaskTexture(
        std::make_unique<TestTexture>(8, 4));
    renderContent.setMeshReady(true);

    EXPECT_TRUE(renderContent.hasGltfContent());
    EXPECT_TRUE(renderContent.isTerrainRenderContent());
    EXPECT_FALSE(renderContent.isSurfaceMeshReady());
    EXPECT_EQ(nullptr, renderContent.surfaceVertexBuffer());
    EXPECT_EQ(nullptr, renderContent.surfaceIndexBuffer());
    EXPECT_EQ(nullptr, renderContent.surfaceWaterMaskTexture());
    EXPECT_EQ(SurfaceDrawableSource::GltfContent,
              renderContent.currentSurfaceSource());
}

TEST(RasterOverlayDetailsTest,
     ClearingGltfTerrainContentDoesNotExposeStaleGltfState) {
    TileRenderContentState renderContent;
    auto model = std::make_unique<GltfModel>();
    RasterOverlayDetails gltfDetails;
    const Rectangle rectangle(0.0, 0.0, 1.0, 1.0);
    gltfDetails.setGeographicRectangle(rectangle, -1.0, 1.0);
    model->rasterOverlayDetails = std::move(gltfDetails);
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    auto staleModel = makeTerrainQuadModel(Rectangle(2.0, 2.0, 3.0, 3.0));
    RasterOverlayDetails staleSurfaceDetails;
    staleSurfaceDetails.setGeographicRectangle(
        Rectangle(2.0, 2.0, 3.0, 3.0),
        10.0,
        20.0);
    staleModel->rasterOverlayDetails = std::move(staleSurfaceDetails);
    renderContent.prepareGltfContent(std::move(staleModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
    renderContent.addGltfPrimitiveResource(GltfPrimitiveRenderResources{});
    renderContent.markRenderContentReady();
    renderContent.setRetainedHeightmap(std::make_unique<DecodedHeightmap>());
    renderContent.setSurfaceDrawable(true);
    renderContent.setSurfaceSource(SurfaceDrawableSource::GltfContent);
    renderContent.setMeshReady(true);
    renderContent.setSurfaceLocalOrigin(Vec3(1.0, 2.0, 3.0));
    renderContent.setTerrainHeightRange(10.0, 20.0);
    ASSERT_TRUE(renderContent.hasTerrainHeightRange());

    renderContent.clearGltfContent();

    EXPECT_FALSE(renderContent.hasGltfContent());
    EXPECT_FALSE(renderContent.hasRenderableTerrainContent());
    EXPECT_FALSE(renderContent.hasRasterOverlayDetailsContent());
    EXPECT_FALSE(renderContent.hasRetainedHeightmap());
    EXPECT_FALSE(renderContent.isSurfaceDrawable());
    EXPECT_FALSE(renderContent.isSurfaceMeshReady());
    EXPECT_FALSE(renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(0.0, renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(0.0, renderContent.terrainMaximumHeight());
    EXPECT_EQ(SurfaceDrawableSource::None,
              renderContent.currentSurfaceSource());
    EXPECT_EQ(Vec3::zero(), renderContent.renderLocalOrigin());
    EXPECT_TRUE(renderContent.rasterOverlayDetails()
                    .rasterOverlayRectangles.empty());
}

TEST(RasterOverlayDetailsTest,
     ClearingOnlyRetainedHeightmapDropsOrphanTerrainHeightRange) {
    TileRenderContentState renderContent;
    renderContent.setRetainedHeightmap(std::make_unique<DecodedHeightmap>());
    renderContent.setTerrainHeightRange(11.0, 22.0);
    ASSERT_TRUE(renderContent.hasRetainedHeightmap());
    ASSERT_TRUE(renderContent.hasTerrainHeightRange());

    renderContent.clearRetainedHeightmap();

    EXPECT_FALSE(renderContent.hasRetainedHeightmap());
    EXPECT_FALSE(renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(0.0, renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(0.0, renderContent.terrainMaximumHeight());
}

TEST(RasterOverlayDetailsTest,
     ClearingRetainedHeightmapKeepsGltfTerrainHeightRange) {
    TileRenderContentState renderContent;
    auto terrainModel = makeTerrainQuadModel(Rectangle(0.0, 0.0, 1.0, 1.0));
    renderContent.prepareGltfContent(std::move(terrainModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
    renderContent.addGltfPrimitiveResource(GltfPrimitiveRenderResources{});
    renderContent.markRenderContentReady();
    renderContent.setTerrainHeightRange(11.0, 22.0);
    ASSERT_TRUE(renderContent.hasGltfContent());
    ASSERT_TRUE(renderContent.hasTerrainHeightRange());

    renderContent.clearRetainedHeightmap();

    EXPECT_TRUE(renderContent.hasGltfContent());
    EXPECT_FALSE(renderContent.hasRetainedHeightmap());
    EXPECT_TRUE(renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(11.0, renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(22.0, renderContent.terrainMaximumHeight());
}

TEST(RasterOverlayDetailsTest,
     MergeAppendsProjectionSlotsLikeCesiumNative) {
    RasterOverlayDetails first;
    const Rectangle firstRectangle(0.0, 1.0, 2.0, 3.0);
    first.setGeographicRectangle(firstRectangle, -10.0, 20.0);

    RasterOverlayDetails second;
    const Rectangle secondRectangle(4.0, 5.0, 6.0, 7.0);
    second.setGeographicRectangle(secondRectangle, -30.0, 40.0);
    second.rasterOverlayInvertedVCoordinates = {true};

    first.merge(second);

    ASSERT_EQ(2u, first.rasterOverlayProjections.size());
    ASSERT_EQ(2u, first.rasterOverlayRectangles.size());
    ASSERT_EQ(2u, first.rasterOverlayInvertedVCoordinates.size());
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              first.rasterOverlayProjections[0]);
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              first.rasterOverlayProjections[1]);
    EXPECT_FALSE(first.rasterOverlayInvertedVCoordinates[0]);
    EXPECT_TRUE(first.rasterOverlayInvertedVCoordinates[1]);
    EXPECT_EQ(firstRectangle, first.rasterOverlayRectangles[0]);
    EXPECT_EQ(secondRectangle, first.rasterOverlayRectangles[1]);

    const Rectangle* found = first.findRectangleForOverlayProjection(
        RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(found, &first.rasterOverlayRectangles[0]);
    EXPECT_EQ(0, first.textureCoordinateIDForProjection(
                     RasterOverlayProjection::Geographic));
    EXPECT_EQ(firstRectangle.computeUnion(secondRectangle),
              first.boundingRegion.rectangle);
    EXPECT_DOUBLE_EQ(-30.0, first.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(40.0, first.boundingRegion.maximumHeight);

    RasterOverlayDetails webMercator;
    const Rectangle webMercatorRectangle(8.0, 9.0, 10.0, 11.0);
    webMercator.rasterOverlayProjections = {
        RasterOverlayProjection::WebMercator};
    webMercator.rasterOverlayRectangles = {webMercatorRectangle};
    webMercator.rasterOverlayInvertedVCoordinates = {false};
    first.merge(webMercator);

    ASSERT_EQ(3u, first.rasterOverlayProjections.size());
    EXPECT_EQ(2, first.textureCoordinateIDForProjection(
                     RasterOverlayProjection::WebMercator));
    EXPECT_EQ(webMercatorRectangle, first.rasterOverlayRectangles[2]);

    RasterOverlayDetails emptyGeographic;
    emptyGeographic.rasterOverlayProjections = {
        RasterOverlayProjection::Geographic};
    emptyGeographic.rasterOverlayRectangles = {Rectangle::EMPTY};
    emptyGeographic.rasterOverlayInvertedVCoordinates = {false};
    first.merge(emptyGeographic);
    ASSERT_EQ(4u, first.rasterOverlayProjections.size());
    EXPECT_EQ(Rectangle::EMPTY, first.rasterOverlayRectangles[3]);
    EXPECT_EQ(firstRectangle, first.rasterOverlayRectangles[0]);
    EXPECT_FALSE(first.rasterOverlayInvertedVCoordinates[0]);

    RasterOverlayDetails emptyOnly;
    emptyOnly.rasterOverlayProjections = {RasterOverlayProjection::WebMercator};
    emptyOnly.rasterOverlayRectangles = {Rectangle::EMPTY};
    const Rectangle* emptyFound =
        emptyOnly.findRectangleForOverlayProjection(
            RasterOverlayProjection::WebMercator);
    ASSERT_NE(nullptr, emptyFound);
    EXPECT_TRUE(emptyFound->isEmpty());
    EXPECT_EQ(0, emptyOnly.textureCoordinateIDForProjection(
                     RasterOverlayProjection::WebMercator));
}

TEST(RasterOverlayDetailsTest,
     SetGeographicRectangleStoresBoundingRegionLikeCesiumNative) {
    RasterOverlayDetails details;
    const Rectangle rectangle(0.25, 0.5, 0.75, 1.0);

    details.setGeographicRectangle(rectangle, -123.0, 456.0);

    EXPECT_EQ(rectangle, details.boundingRegion.rectangle);
    EXPECT_DOUBLE_EQ(-123.0, details.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(456.0, details.boundingRegion.maximumHeight);
    EXPECT_FALSE(details.hasInvertedVCoordinateForProjection(
        RasterOverlayProjection::Geographic));
}

TEST(RasterOverlayDetailsTest,
     ExactEqualityCoversProjectionSlotsAndBoundingRegion) {
    RasterOverlayDetails first;
    const Rectangle rectangle = Rectangle::fromDegrees(-10.0, -5.0, 10.0, 5.0);
    first.setGeographicRectangle(rectangle, -20.0, 30.0);
    first.rasterOverlayInvertedVCoordinates = {true};

    RasterOverlayDetails same = first;
    EXPECT_TRUE(first.equalsExact(same));

    RasterOverlayDetails differentProjection = first;
    differentProjection.rasterOverlayProjections = {
        RasterOverlayProjection::WebMercator};
    EXPECT_FALSE(first.equalsExact(differentProjection));

    RasterOverlayDetails differentRectangle = first;
    differentRectangle.rasterOverlayRectangles = {
        Rectangle::fromDegrees(-9.0, -5.0, 10.0, 5.0)};
    EXPECT_FALSE(first.equalsExact(differentRectangle));

    RasterOverlayDetails differentInvertedV = first;
    differentInvertedV.rasterOverlayInvertedVCoordinates = {false};
    EXPECT_FALSE(first.equalsExact(differentInvertedV));

    RasterOverlayDetails differentRegion = first;
    differentRegion.boundingRegion.maximumHeight = 31.0;
    EXPECT_FALSE(first.equalsExact(differentRegion));
}

TEST(RasterOverlayDetailsTest,
     RenderTerrainDoesNotDuplicateIdenticalModelAndMetadataDetails) {
    auto model = std::make_unique<GltfModel>();
    const Rectangle rectangle = Rectangle::fromDegrees(-10.0, -5.0, 10.0, 5.0);
    model->rasterOverlayDetails.setGeographicRectangle(rectangle, -20.0, 30.0);

    TileLoadResultMetadata metadata;
    metadata.rasterOverlayDetails = model->rasterOverlayDetails;

    TileContentLoadResult result =
        TileContentLoadResult::renderTerrain(std::move(model), metadata);

    ASSERT_EQ(TileLoadStatus::Renderable, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_TRUE(result.metadata.rasterOverlayDetails.has_value());
    ASSERT_EQ(1u, result.gltfModel->rasterOverlayDetails
                      .rasterOverlayProjections.size());
    ASSERT_EQ(1u, result.metadata.rasterOverlayDetails
                      ->rasterOverlayProjections.size());
    EXPECT_EQ(rectangle,
              result.gltfModel->rasterOverlayDetails.rasterOverlayRectangles[0]);
}

TEST(RasterOverlayDetailsTest,
     RenderTerrainUsesExplicitMetadataDetailsAsFinalLoadResult) {
    auto model = std::make_unique<GltfModel>();
    const Rectangle modelRectangle =
        Rectangle::fromDegrees(-10.0, -5.0, 0.0, 5.0);
    model->rasterOverlayDetails.setGeographicRectangle(
        modelRectangle,
        -20.0,
        30.0);

    TileLoadResultMetadata metadata;
    RasterOverlayDetails metadataDetails;
    const Rectangle metadataRectangle =
        Rectangle::fromDegrees(0.0, -5.0, 10.0, 5.0);
    metadataDetails.setGeographicRectangle(metadataRectangle, -10.0, 40.0);
    metadata.rasterOverlayDetails = metadataDetails;

    TileContentLoadResult result =
        TileContentLoadResult::renderTerrain(std::move(model), metadata);

    ASSERT_EQ(TileLoadStatus::Renderable, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    const RasterOverlayDetails& details = result.gltfModel->rasterOverlayDetails;
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(metadataRectangle, details.rasterOverlayRectangles[0]);
    EXPECT_EQ(metadataRectangle, details.boundingRegion.rectangle);
}

TEST(RasterOverlayDetailsTest,
     TerrainMetadataApplicatorReplacesStaleRenderContentDetails) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    auto model = std::make_unique<GltfModel>();
    const Rectangle staleRectangle =
        Rectangle::fromDegrees(-10.0, -5.0, 0.0, 5.0);
    model->rasterOverlayDetails.setGeographicRectangle(
        staleRectangle,
        -20.0,
        30.0);
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);

    const Rectangle loadResultRectangle =
        Rectangle::fromDegrees(0.0, -5.0, 10.0, 5.0);
    TileLoadResultMetadata metadata;
    metadata.rasterOverlayDetails.emplace();
    metadata.rasterOverlayDetails->setGeographicRectangle(
        loadResultRectangle,
        -10.0,
        40.0);

    TileLoadResultMetadataApplicator::apply(tile, std::move(metadata));

    const RasterOverlayDetails& details =
        tile.content.renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(loadResultRectangle, details.rasterOverlayRectangles[0]);
    EXPECT_EQ(loadResultRectangle, details.boundingRegion.rectangle);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationSkipsExistingProjectionSlotWithoutRectangleLikeCesiumNative) {
    TileRenderContentState renderContent;
    RasterOverlayDetails existingDetails;
    existingDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);
    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    prepareTerrainQuadRenderContent(
        renderContent,
        region,
        std::move(existingDetails));
    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::Geographic);

    EXPECT_FALSE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    EXPECT_TRUE(details.rasterOverlayRectangles.empty());
    EXPECT_EQ(-1, details.textureCoordinateIDForProjection(
                    RasterOverlayProjection::Geographic));
    EXPECT_TRUE(details.boundingRegion.rectangle.isEmpty());

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_TRUE(model->primitives.front().vertexTexCoords[0].empty());
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationAppendsMissingProjectionAfterExistingSlotLikeCesiumNative) {
    TileRenderContentState renderContent;
    RasterOverlayDetails existingDetails;
    existingDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);

    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    prepareTerrainQuadRenderContent(
        renderContent,
        region,
        std::move(existingDetails));
    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);
    const Rectangle projected = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        region);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::WebMercator);

    EXPECT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(2u, details.rasterOverlayProjections.size());
    ASSERT_EQ(2u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              details.rasterOverlayProjections[0]);
    EXPECT_EQ(RasterOverlayProjection::WebMercator,
              details.rasterOverlayProjections[1]);
    EXPECT_TRUE(details.rasterOverlayRectangles[0].isEmpty());
    EXPECT_EQ(projected, details.rasterOverlayRectangles[1]);
    EXPECT_EQ(1, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::WebMercator));
    expectRectangleNear(region, details.boundingRegion.rectangle);
    EXPECT_NEAR(0.0, details.boundingRegion.minimumHeight, 1e-6);
    EXPECT_NEAR(0.0, details.boundingRegion.maximumHeight, 1e-6);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationSkipsExistingRectangleLikeCesiumNative) {
    TileRenderContentState renderContent;
    RasterOverlayDetails existingDetails;
    const Rectangle existing = Rectangle::fromDegrees(1.0, 2.0, 3.0, 4.0);
    existingDetails.setGeographicRectangle(existing, 10.0, 20.0);
    prepareGltfRenderContent(renderContent, std::move(existingDetails));

    const TileBoundingVolume boundingRegion = TileBoundingVolume::fromRegion(
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0),
        -25.0,
        125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::Geographic);

    EXPECT_FALSE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(existing, details.rasterOverlayRectangles[0]);
    EXPECT_EQ(0, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::Geographic));
    EXPECT_DOUBLE_EQ(10.0, details.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, details.boundingRegion.maximumHeight);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationProjectsWebMercatorRectangleLikeCesiumNative) {
    TileRenderContentState renderContent;

    const Rectangle region = Rectangle::fromDegrees(-90.0, -45.0, 45.0, 60.0);
    prepareTerrainQuadRenderContent(renderContent, region);
    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -15.0, 250.0);
    const Rectangle projected = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        region);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::WebMercator);

    EXPECT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator,
              details.rasterOverlayProjections[0]);
    EXPECT_EQ(projected, details.rasterOverlayRectangles[0]);
    EXPECT_EQ(0, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::WebMercator));
    EXPECT_EQ(nullptr, details.findRectangleForOverlayProjection(
                           RasterOverlayProjection::Geographic));
    expectRectangleNear(region, details.boundingRegion.rectangle);
    EXPECT_NEAR(0.0, details.boundingRegion.minimumHeight, 1e-6);
    EXPECT_NEAR(0.0, details.boundingRegion.maximumHeight, 1e-6);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationWritesGltfOverlayTexCoordsLikeCesiumNative) {
    TileRenderContentState renderContent;
    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    auto quadModel = makeTerrainQuadModel(region);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::WebMercator);

    ASSERT_TRUE(generated);
    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive =
        model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][1][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][3][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][3][1], 1e-6f);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationWritesGltfOverlayTexCoordsFromRtcWorldPositions) {
    TileRenderContentState renderContent;
    const Rectangle region = Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    auto quadModel = makeRtcTerrainQuadModel(region);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(region, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::WebMercator);

    ASSERT_TRUE(generated);
    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][1][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][3][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][3][1], 1e-6f);

    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Rectangle projectedRegion =
        projectRectangleSimple(projection, region);
    const Cartographic localOnlyCartographic =
        Ellipsoid::WGS84().cartesianToCartographic(
            primitive.vertices[2].positionEcef);
    const Vec3 localOnlyProjected =
        projectPosition(projection, localOnlyCartographic);
    const double wrongV =
        (localOnlyProjected.y() - projectedRegion.south()) /
        projectedRegion.height();
    EXPECT_GT(std::abs(wrongV - primitive.vertexTexCoords[0][2][1]), 1e-3);
}

TEST(RasterOverlayDetailsGeneratorTest,
     ModelBoundsGenerationWritesGltfOverlayTexCoordsLikeCesiumNativeNullRegion) {
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    auto quadModel = makeTerrainQuadModel(modelRegion);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const bool generated = TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromModelBounds(
            renderContent,
            RasterOverlayProjection::Geographic);

    ASSERT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(RasterOverlayProjection::Geographic,
              details.rasterOverlayProjections[0]);
    expectRectangleNear(modelRegion, details.rasterOverlayRectangles[0]);
    expectRectangleNear(modelRegion, details.boundingRegion.rectangle);
    EXPECT_NEAR(0.0, details.boundingRegion.minimumHeight, 1e-6);
    EXPECT_NEAR(0.0, details.boundingRegion.maximumHeight, 1e-6);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][1][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][3][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][3][1], 1e-6f);
}

TEST(RasterOverlayDetailsGeneratorTest,
     ModelBoundsGenerationExcludesNearPoleLongitudesLikeCesiumNative) {
    // cesium RasterOverlayUtilities.cpp:106-110 sets the pole tolerance to
    // 1/1000th of the tile height and ignores vertices within that distance of
    // a pole, because their longitudes are untrustworthy. Without the tolerance
    // the default (Epsilon10) never triggers and a near-pole noise longitude
    // pollutes the computed bounding region's east/west.
    TileRenderContentState renderContent;
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const std::array<Cartographic, 4> corners = {
        Cartographic::fromDegrees(0.0, 80.0, 0.0),
        Cartographic::fromDegrees(10.0, 80.0, 0.0),
        Cartographic::fromDegrees(10.0, 89.0, 0.0),
        Cartographic::fromDegrees(0.0, 89.0, 0.0)};
    for (const Cartographic& corner : corners) {
        SurfaceVertex vertex;
        vertex.positionEcef = ellipsoid.cartographicToCartesian(corner);
        primitive.vertices.push_back(vertex);
    }
    // Near-pole vertex (< 1/1000th of the ~10deg tile height from the pole)
    // whose longitude (170deg) is far outside the tile and must be ignored.
    SurfaceVertex nearPole;
    nearPole.positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(170.0, 89.995, 0.0));
    primitive.vertices.push_back(nearPole);
    primitive.indices = {0, 1, 2, 0, 2, 3};
    model->primitives.push_back(std::move(primitive));
    renderContent.prepareGltfContent(std::move(model), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const bool generated = TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromModelBounds(
            renderContent,
            RasterOverlayProjection::Geographic);

    ASSERT_TRUE(generated);
    const Rectangle& region =
        renderContent.rasterOverlayDetails().boundingRegion.rectangle;
    const double west0 = Cartographic::fromDegrees(0.0, 0.0).longitude();
    const double east10 = Cartographic::fromDegrees(10.0, 0.0).longitude();
    // The near-pole 170deg longitude is excluded, so east stays at the tile
    // edge (10deg). Before the poleTolerance fix, east would jump to ~170deg.
    EXPECT_NEAR(west0, region.west(), 1e-6);
    EXPECT_NEAR(east10, region.east(), 1e-3);
}

TEST(RasterOverlayDetailsGeneratorTest,
     ModelBoundsGenerationSkipsExistingProjectionSlotWithoutRectangleLikeCesiumNative) {
    TileRenderContentState renderContent;
    RasterOverlayDetails existingDetails;
    existingDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    prepareTerrainQuadRenderContent(
        renderContent,
        modelRegion,
        std::move(existingDetails));

    const bool generated = TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromModelBounds(
            renderContent,
            RasterOverlayProjection::Geographic);

    EXPECT_FALSE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    EXPECT_TRUE(details.rasterOverlayRectangles.empty());
    EXPECT_EQ(-1, details.textureCoordinateIDForProjection(
                    RasterOverlayProjection::Geographic));
    EXPECT_TRUE(details.boundingRegion.rectangle.isEmpty());

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    EXPECT_TRUE(primitive.vertexTexCoords[0].empty());
}

TEST(RasterOverlayDetailsGeneratorTest,
     ModelBoundsGenerationProjectsWebMercatorFromRtcWorldPositions) {
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    auto quadModel = makeRtcTerrainQuadModel(modelRegion);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);
    const Rectangle projected = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        modelRegion);

    const bool generated = TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromModelBounds(
            renderContent,
            RasterOverlayProjection::WebMercator);

    ASSERT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator,
              details.rasterOverlayProjections[0]);
    EXPECT_EQ(projected, details.rasterOverlayRectangles[0]);
    expectRectangleNear(modelRegion, details.boundingRegion.rectangle);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][1][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][3][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][3][1], 1e-6f);

    const Cartographic localOnlyCartographic =
        Ellipsoid::WGS84().cartesianToCartographic(
            primitive.vertices[2].positionEcef);
    const Vec3 localOnlyProjected = projectPosition(
        WebMercatorProjection(Ellipsoid::WGS84()),
        localOnlyCartographic);
    const double wrongV =
        (localOnlyProjected.y() - projected.south()) / projected.height();
    EXPECT_GT(std::abs(wrongV - primitive.vertexTexCoords[0][2][1]), 1e-3);
}

TEST(RasterOverlayDetailsGeneratorTest,
     ModelBoundsGenerationAppliesContentTransformLikeCesiumNative) {
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    Mat4 contentTransform = Mat4::identity();
    auto quadModel =
        makeContentTransformTerrainQuadModel(modelRegion, contentTransform);
    renderContent.prepareGltfContent(std::move(quadModel), contentTransform);
    renderContent.setTerrainRenderContent(true);
    const Rectangle projected = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        modelRegion);

    const bool generated = TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromModelBounds(
            renderContent,
            RasterOverlayProjection::WebMercator);

    ASSERT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(projected, details.rasterOverlayRectangles[0]);
    expectRectangleNear(modelRegion, details.boundingRegion.rectangle);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][1][1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][2][1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][3][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][3][1], 1e-6f);
}

TEST(RasterOverlayDetailsGeneratorTest,
     ActiveOverlayGenerationUsesModelBoundsWhenContentVolumeIsNotRegion) {
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    prepareTerrainQuadRenderContent(renderContent, modelRegion);

    RasterOverlay::Options options{};
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    ActivatedRasterOverlay activated(*overlay);
    ASSERT_NE(nullptr, activated.ensureTileProvider(nullptr));
    std::vector<ActivatedRasterOverlay*> overlays{&activated};

    const TileBoundingVolume sphere = TileBoundingVolume::fromSphere(
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(
                modelRegion.west() + modelRegion.width() * 0.5,
                modelRegion.south() + modelRegion.height() * 0.5,
                0.0)),
        1000.0);

    const int generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromActiveOverlays(
            renderContent,
            &sphere,
            overlays,
            nullptr);

    EXPECT_EQ(1, generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayProjections.size());
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(RasterOverlayProjection::WebMercator,
              details.rasterOverlayProjections[0]);
    EXPECT_EQ(0, details.textureCoordinateIDForProjection(
                     RasterOverlayProjection::WebMercator));
    expectRectangleNear(modelRegion, details.boundingRegion.rectangle);

    const Rectangle projected = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        modelRegion);
    EXPECT_EQ(projected, details.rasterOverlayRectangles[0]);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
}

TEST(RasterOverlayDetailsGeneratorTest,
     ActiveOverlayGenerationUsesModelBoundsWhenRegionVolumeExceedsMesh) {
    // Regression: a glTF-terrain-upsampled tile carries a declared bounding
    // volume region that is a full LOD coarser than its mesh (twice the size in
    // each axis, centered here). Normalizing overlay texcoords against that loose
    // region compresses U/V into the middle band and — when the mesh sits at a
    // region edge — collapses V to a constant, smearing imagery into vertical
    // streaks on-device. The overlay rectangle must come from the mesh's own
    // projected bounds (cesium's behavior) so texcoords span the full [0,1].
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(106.0, 29.0, 106.5, 29.5);
    prepareTerrainQuadRenderContent(renderContent, modelRegion);

    RasterOverlay::Options options{};
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    ActivatedRasterOverlay activated(*overlay);
    ASSERT_NE(nullptr, activated.ensureTileProvider(nullptr));
    std::vector<ActivatedRasterOverlay*> overlays{&activated};

    // 2x-larger region centered on the mesh (the upsampled-tile failure mode).
    const Rectangle looseRegion =
        Rectangle::fromDegrees(105.75, 28.75, 106.75, 29.75);
    const TileBoundingVolume bounds =
        TileBoundingVolume::fromRegion(looseRegion, -25.0, 125.0);

    const int generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromActiveOverlays(
            renderContent,
            &bounds,
            overlays,
            nullptr);

    EXPECT_EQ(1, generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());

    // Rectangle comes from the mesh bounds (projected), not the loose region.
    const Rectangle projectedModel = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        modelRegion);
    expectRectangleNear(projectedModel, details.rasterOverlayRectangles[0], 1e-3);

    // Texcoords span the full [0,1] — no compression / V collapse.
    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
    for (const std::array<float, 2>& tc : primitive.vertexTexCoords[0]) {
        minU = std::min(minU, tc[0]);
        maxU = std::max(maxU, tc[0]);
        minV = std::min(minV, tc[1]);
        maxV = std::max(maxV, tc[1]);
    }
    EXPECT_NEAR(0.0f, minU, 1e-3f);
    EXPECT_NEAR(1.0f, maxU, 1e-3f);
    EXPECT_NEAR(0.0f, minV, 1e-3f);
    EXPECT_NEAR(1.0f, maxV, 1e-3f);
}

TEST(RasterOverlayDetailsGeneratorTest,
     ActiveOverlayGenerationSkipsInvisibleOverlayLikeCesiumNative) {
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    RasterOverlayDetails existingDetails;
    existingDetails.setGeographicRectangle(modelRegion, -25.0, 125.0);
    prepareTerrainQuadRenderContent(
        renderContent,
        modelRegion,
        existingDetails);

    RasterOverlay::Options options{};
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        options);
    overlay->setVisible(false);
    ActivatedRasterOverlay activated(*overlay);
    std::vector<ActivatedRasterOverlay*> overlays{&activated};

    const TileBoundingVolume bounds =
        TileBoundingVolume::fromRegion(modelRegion, -25.0, 125.0);
    const int generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromActiveOverlays(
            renderContent,
            &bounds,
            overlays,
            nullptr);

    EXPECT_EQ(0, generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    EXPECT_TRUE(details.equalsExact(existingDetails));
    EXPECT_EQ(nullptr,
              details.findRectangleForOverlayProjection(
                  RasterOverlayProjection::WebMercator));

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    EXPECT_TRUE(primitive.vertexTexCoords[1].empty());
}

TEST(RasterOverlayDetailsGeneratorTest,
     ModelBoundsGenerationExcludesSkirtVerticesFromComputedRegionLikeCesiumNative) {
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    auto quadModel = makeTerrainQuadModelWithSkirt(modelRegion, 750.0);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const bool generated = TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromModelBounds(
            renderContent,
            RasterOverlayProjection::Geographic);

    ASSERT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    expectRectangleNear(modelRegion, details.boundingRegion.rectangle);
    EXPECT_NEAR(0.0, details.boundingRegion.minimumHeight, 1e-6);
    EXPECT_NEAR(0.0, details.boundingRegion.maximumHeight, 1e-6);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_TRUE(primitive.skirtMetadata.has_value());
    ASSERT_EQ(5u, primitive.vertices.size());
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][4][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][4][1], 1e-6f);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationUsesLooseRectangleForUvButTightModelBoundingRegion) {
    TileRenderContentState renderContent;
    const Rectangle modelRegion =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    const Rectangle looseRegion =
        Rectangle::fromDegrees(-20.0, -10.0, 10.0, 20.0);
    prepareTerrainQuadRenderContent(renderContent, modelRegion);

    const TileBoundingVolume looseBoundingRegion =
        TileBoundingVolume::fromRegion(looseRegion, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            looseBoundingRegion,
            RasterOverlayProjection::Geographic);

    ASSERT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_EQ(looseRegion, details.rasterOverlayRectangles[0]);
    expectRectangleNear(modelRegion, details.boundingRegion.rectangle);
    EXPECT_NEAR(0.0, details.boundingRegion.minimumHeight, 1e-6);
    EXPECT_NEAR(0.0, details.boundingRegion.maximumHeight, 1e-6);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(
        static_cast<float>(
            (modelRegion.west() - looseRegion.west()) /
            looseRegion.width()),
        primitive.vertexTexCoords[0][0][0],
        1e-6f);
    EXPECT_NEAR(
        static_cast<float>(
            (modelRegion.south() - looseRegion.south()) /
            looseRegion.height()),
        primitive.vertexTexCoords[0][0][1],
        1e-6f);
    EXPECT_NEAR(
        static_cast<float>(
            (modelRegion.east() - looseRegion.west()) /
            looseRegion.width()),
        primitive.vertexTexCoords[0][1][0],
        1e-6f);
    EXPECT_NEAR(
        static_cast<float>(
            (modelRegion.north() - looseRegion.south()) /
            looseRegion.height()),
        primitive.vertexTexCoords[0][2][1],
        1e-6f);
}

TEST(RasterOverlayDetailsGeneratorTest,
     RegionGenerationSplitsAntimeridianNoiseAndWrapsUvLikeCesiumNative) {
    TileRenderContentState renderContent;
    const Rectangle noisyAntimeridianRegion(
        MathUtils::OnePi - 1e-8,
        -0.1,
        -MathUtils::OnePi + 1e-8,
        0.1);
    auto quadModel = makeTerrainQuadModel(noisyAntimeridianRegion);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const TileBoundingVolume boundingRegion =
        TileBoundingVolume::fromRegion(noisyAntimeridianRegion, -25.0, 125.0);

    const bool generated =
        TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
            renderContent,
            boundingRegion,
            RasterOverlayProjection::Geographic);

    ASSERT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    const Rectangle expectedSplit(
        -MathUtils::OnePi,
        -0.1,
        -MathUtils::OnePi + 1e-8,
        0.1);
    expectRectangleNear(expectedSplit, details.rasterOverlayRectangles[0]);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    ASSERT_GE(primitive.vertexTexCoords[0].size(), 2u);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
}

TEST(RasterOverlayDetailsGeneratorTest,
     ModelBoundsGenerationUsesAntimeridianBuilderLikeCesiumNative) {
    TileRenderContentState renderContent;
    const Rectangle noisyAntimeridianRegion(
        MathUtils::OnePi - 1e-8,
        -0.1,
        -MathUtils::OnePi + 1e-8,
        0.1);
    auto quadModel = makeTerrainQuadModel(noisyAntimeridianRegion);
    renderContent.prepareGltfContent(std::move(quadModel), Mat4::identity());
    renderContent.setTerrainRenderContent(true);

    const bool generated = TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromModelBounds(
            renderContent,
            RasterOverlayProjection::Geographic);

    ASSERT_TRUE(generated);
    const RasterOverlayDetails& details = renderContent.rasterOverlayDetails();
    ASSERT_EQ(1u, details.rasterOverlayRectangles.size());
    EXPECT_TRUE(details.boundingRegion.rectangle.crossesAntimeridian());
    EXPECT_FALSE(details.boundingRegion.rectangle.contains(0.0, 0.0));

    const Rectangle expectedSplit(
        -MathUtils::OnePi,
        -0.1,
        -MathUtils::OnePi + 1e-8,
        0.1);
    expectRectangleNear(expectedSplit, details.rasterOverlayRectangles[0]);

    const GltfModel* model = renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives.front();
    ASSERT_EQ(primitive.vertices.size(), primitive.vertexTexCoords[0].size());
    ASSERT_GE(primitive.vertexTexCoords[0].size(), 2u);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[0][1][0], 1e-6f);
}

TEST(RasterOverlayDetailsDeriverTest,
     ChildOverlayRectangleInterpolatesAcrossAntimeridianLikeCesiumNative) {
    RasterOverlayDetails parentDetails;
    const Rectangle parentBounds =
        Rectangle::fromDegrees(170.0, -10.0, -170.0, 10.0);
    parentDetails.setGeographicRectangle(parentBounds, -50.0, 150.0);

    const Rectangle westernChild =
        Rectangle::fromDegrees(170.0, -10.0, 180.0, 10.0);
    const RasterOverlayDetails westernDetails =
        TileRasterOverlayDetailsDeriver::deriveChildFromParent(
            parentDetails,
            parentBounds,
            westernChild,
            -50.0,
            150.0);

    ASSERT_EQ(1u, westernDetails.rasterOverlayRectangles.size());
    const Rectangle expectedWestern =
        Rectangle::fromDegrees(170.0, -10.0, -180.0, 10.0);
    expectRectangleNear(
        expectedWestern,
        westernDetails.rasterOverlayRectangles.front());
    EXPECT_TRUE(
        westernDetails.rasterOverlayRectangles.front().crossesAntimeridian());
    EXPECT_EQ(westernChild, westernDetails.boundingRegion.rectangle);

    const Rectangle easternChild =
        Rectangle::fromDegrees(-180.0, -10.0, -170.0, 10.0);
    const RasterOverlayDetails easternDetails =
        TileRasterOverlayDetailsDeriver::deriveChildFromParent(
            parentDetails,
            parentBounds,
            easternChild,
            -50.0,
            150.0);

    ASSERT_EQ(1u, easternDetails.rasterOverlayRectangles.size());
    expectRectangleNear(
        easternChild,
        easternDetails.rasterOverlayRectangles.front());
    EXPECT_FALSE(
        easternDetails.rasterOverlayRectangles.front().crossesAntimeridian());
    EXPECT_EQ(easternChild, easternDetails.boundingRegion.rectangle);
}
