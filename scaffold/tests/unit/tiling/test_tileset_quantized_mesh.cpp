#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/content/QuantizedMeshContentLoader.h"
#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileBoundsMetrics.h"
#include "earth_engine/tiling/TileContentUploadCommitter.h"
#include "earth_engine/tiling/TileGltfTerrainUpsampledChildMaterializer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static void ensureTileMesh(Tileset& tileset, TilesetTile& tile) {
        tileset.meshPreparation_.ensureTileMesh(tile);
    }

    static void ensureTileChildren(Tileset& tileset, TilesetTile& tile) {
        tileset.contentAccess_.ensureTileChildren(tile);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.contentLifecycle_.terrainCache()[TileCacheKey::forTile(key)] =
            std::move(heightmap);
    }

    static bool hasTerrainCache(Tileset& tileset, const TileKey& key) {
        return tileset.contentLifecycle_.terrainCache().find(
                   TileCacheKey::forTile(key)) !=
            tileset.contentLifecycle_.terrainCache().end();
    }

    static Vec3 tileBoundsCenter(const Rectangle& bounds) {
        return TileBoundsMetrics::tileBoundsCenter(bounds);
    }

    static double tileBoundsRadius(const TilesetTile& tile,
                                   const Vec3& center) {
        return TileBoundsMetrics::tileBoundsRadius(tile, center);
    }

    static std::optional<OrientedBoundingBox> tileBoundingRegionObb(
        const TilesetTile& tile) {
        return TileBoundsMetrics::tileBoundingRegionObb(tile);
    }

    static bool tileIntersectsFrustum(const TilesetTile& tile,
                                      const Frustum& frustum) {
        return TileBoundsMetrics::tileIntersectsFrustum(tile, frustum);
    }

    static double approximateDistanceToTileBounds(
        const TilesetTile& tile,
        const Vec3& cameraPosition) {
        return TileBoundsMetrics::approximateDistanceToTileBounds(
            tile,
            cameraPosition);
    }
};
} // namespace earth_engine

namespace {

template <typename T>
void appendPod(std::vector<uint8_t>& bytes, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
}

uint16_t zigZagEncode16(int32_t value) {
    return static_cast<uint16_t>(
        value >= 0 ? value * 2 : (-value * 2) - 1);
}

std::vector<uint8_t> makeQuantizedMeshBytes(
    const Vec3& boundingSphereCenterEcef,
    const Vec3& tileCenterEcef,
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f) {
    std::vector<uint8_t> bytes;

    appendPod<double>(bytes, tileCenterEcef.x());
    appendPod<double>(bytes, tileCenterEcef.y());
    appendPod<double>(bytes, tileCenterEcef.z());
    appendPod<float>(bytes, minimumHeight);
    appendPod<float>(bytes, maximumHeight);
    appendPod<double>(bytes, boundingSphereCenterEcef.x());
    appendPod<double>(bytes, boundingSphereCenterEcef.y());
    appendPod<double>(bytes, boundingSphereCenterEcef.z());
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
    appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, 3);

    const uint16_t u[] = {
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(-32767)
    };
    const uint16_t v[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(32767)
    };
    const uint16_t h[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(0)
    };
    for (uint16_t value : u) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : v) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : h) appendPod<uint16_t>(bytes, value);

    appendPod<uint32_t>(bytes, 1);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);

    return bytes;
}

void installQuantizedMeshTerrainContent(TilesetTile& tile,
                                        const std::vector<uint8_t>& bytes,
                                        RasterOverlayProjection projection =
                                            RasterOverlayProjection::Geographic) {
    QuantizedMeshContentLoadResult loadResult =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            tile.bounds,
            false,
            {},
            projection);
    ASSERT_TRUE(loadResult.success());

    TileLoadedContent content;
    content.gltfModel = std::move(loadResult.gltfModel);
    content.terrainRenderContent = true;
    content.metadata = std::move(loadResult.metadata);
    content.quantizedMeshAvailabilityUpdates =
        std::move(loadResult.availabilityUpdates);
    TileContentUploadCommitter::prepareRenderContent(
        tile,
        std::move(content),
        {},
        nullptr);
    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::Done;
}

class SparseTerrainProvider final : public TerrainProvider {
public:
    explicit SparseTerrainProvider(
        std::string scheme = "Geographic-TMS")
        : schemeId_(std::move(scheme)) {}

    std::string id() const override { return "sparse-terrain"; }
    std::string schemeId() const override { return schemeId_; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 4; }
    int tileSize() const override { return 2; }

    TileAvailabilityState availabilityState(const TileKey& key) const override {
        if (key.schemeId != schemeId()) {
            return TileAvailabilityState::NotAvailable;
        }
        if (key.z == 0) {
            return TileAvailabilityState::Available;
        }
        if (key.z == 1 && key.x == 0 && key.y == 0) {
            return TileAvailabilityState::Available;
        }
        return TileAvailabilityState::NotAvailable;
    }

    std::string buildUrl(const TileKey&) const override {
        return "memory://sparse";
    }

    void requestTile(const TileKey&,
                     CancellationToken,
                     TerrainCallback callback,
                     HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(TileKey{}, TerrainTileLoadResult::retryLater());
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

private:
    std::string schemeId_;
};

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {
        heightMeters,
        heightMeters,
        heightMeters,
        heightMeters};
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

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

TEST(TilesetQuantizedMeshTest,
     ManualAvailabilityUsesConfiguredMaximumZoomBeyondDefaultLayerLimit) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    provider.setZoomRange(0, 20);
    provider.addAvailabilityRects(16, {{0, 0, 0, 0}});

    const TileKey availableDeepTile{"Geographic-TMS", 16, 0, 0};
    const TileKey unavailableSibling{"Geographic-TMS", 16, 1, 0};

    EXPECT_EQ(TileAvailabilityState::Available,
              provider.availabilityState(availableDeepTile));
    EXPECT_TRUE(provider.supportsTile(availableDeepTile));
    EXPECT_EQ(TileAvailabilityState::NotAvailable,
              provider.availabilityState(unavailableSibling));
}

TEST(TilesetQuantizedMeshTest,
     ContentTerrainProviderRejectsLegacyTerrainProviderCompanion) {
    auto legacyProvider = std::make_unique<SparseTerrainProvider>();
    auto contentProvider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");

    EXPECT_THROW(
        Tileset(
            std::move(legacyProvider),
            TileScheme::createGeographicTMS(),
            {},
            nullptr,
            TilesetOptions{},
            std::move(contentProvider)),
        std::invalid_argument);
}

TEST(TilesetQuantizedMeshTest,
     RtcOriginComesFromBoundingSphereCenterLikeCesiumNative) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);

    const Vec3 boundingSphereCenter(3456.0, -7890.0, 12345.0);
    const Vec3 tileCenter(-3456.0, 7890.0, -12345.0);
    installQuantizedMeshTerrainContent(
        *root,
        makeQuantizedMeshBytes(boundingSphereCenter, tileCenter));

    TilesetTestAccess::ensureTileMesh(tileset, *root);

    EXPECT_NEAR(
        boundingSphereCenter.x(),
        root->content.renderContent.renderLocalOrigin().x(),
        1e-12);
    EXPECT_NEAR(
        boundingSphereCenter.y(),
        root->content.renderContent.renderLocalOrigin().y(),
        1e-12);
    EXPECT_NEAR(
        boundingSphereCenter.z(),
        root->content.renderContent.renderLocalOrigin().z(),
        1e-12);
}

TEST(TilesetQuantizedMeshTest,
     ContentTerrainMeshPreparationWaitsForContentInsteadOfLegacyFallback) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(1234.0f));

    TilesetTestAccess::ensureTileMesh(tileset, *root);

    EXPECT_FALSE(root->content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(root->content.renderContent.isTerrainRenderContent());
    EXPECT_FALSE(root->content.renderContent.hasRetainedHeightmap());
    EXPECT_NE(root->content.loadState, TileLoadState::Done);
}

TEST(TilesetQuantizedMeshTest,
     ContentTerrainProviderDiscardsLegacyTerrainCacheDuringFrameRuntime) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(4321.0f));
    ASSERT_TRUE(TilesetTestAccess::hasTerrainCache(tileset, rootKey));

    Camera camera;
    camera.lookAt(
        Vec3(Ellipsoid::WGS84().semiMajorAxis() * 2.0, 0.0, 0.0),
        Vec3(Ellipsoid::WGS84().semiMajorAxis(), 0.0, 0.0),
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 17;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));

    tileset.update(frameState);

    EXPECT_FALSE(TilesetTestAccess::hasTerrainCache(tileset, rootKey));
    EXPECT_FALSE(root->content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(root->content.renderContent.isTerrainRenderContent());
}

TEST(TilesetQuantizedMeshTest,
     HeaderHeightRangeOverridesHeightmapFallbackLikeCesiumNative) {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/{z}/{x}/{y}.terrain");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);

    constexpr float minimumHeight = -250.0f;
    constexpr float maximumHeight = 1789.0f;
    installQuantizedMeshTerrainContent(
        *root,
        makeQuantizedMeshBytes(
            Vec3::zero(),
            Vec3::zero(),
            minimumHeight,
            maximumHeight));

    TilesetTestAccess::ensureTileMesh(tileset, *root);

    EXPECT_TRUE(root->content.renderContent.hasTerrainHeightRange());
    EXPECT_NEAR(
        minimumHeight,
        root->content.renderContent.terrainMinimumHeight(),
        1e-6);
    EXPECT_NEAR(
        maximumHeight,
        root->content.renderContent.terrainMaximumHeight(),
        1e-6);
}

TEST(TilesetQuantizedMeshTest,
     BoundsUseHeaderHeightRangeLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey key{"Geographic-TMS", 5, 20, 12};
    const Rectangle bounds = scheme->tileToRectangle(key);
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(bounds);

    TilesetTile looseTile(key, bounds);
    looseTile.content.renderContent.setTerrainHeightRange(-1000.0, 9000.0);

    TilesetTile exactTile(key, bounds);
    exactTile.content.renderContent.setTerrainHeightRange(-50.0, 1234.0);

    const double looseRadius =
        TilesetTestAccess::tileBoundsRadius(looseTile, center);
    const double exactRadius =
        TilesetTestAccess::tileBoundsRadius(exactTile, center);
    EXPECT_NEAR(9000.0 - 1234.0, looseRadius - exactRadius, 1e-6);

    const double centerLng = (bounds.west() + bounds.east()) * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    const auto& ellipsoid = Ellipsoid::WGS84();

    const Vec3 insideCamera = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 100.0));
    EXPECT_LT(
        TilesetTestAccess::approximateDistanceToTileBounds(
            exactTile,
            insideCamera),
        1e-6);

    const Vec3 camera = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 13000.0));
    const double looseDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            looseTile,
            camera);
    const double exactDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            exactTile,
            camera);
    EXPECT_NEAR(9000.0 - 1234.0, exactDistance - looseDistance, 1e-6);

    const Vec3 outsideCamera = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.east() + bounds.width() * 0.25,
                                  centerLat,
                                  100.0));
    const double outsideDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            exactTile,
            outsideCamera);
    EXPECT_GT(outsideDistance, 1000.0);

    const auto exactObb = TilesetTestAccess::tileBoundingRegionObb(exactTile);
    ASSERT_TRUE(exactObb.has_value());
    EXPECT_GE(
        outsideDistance * outsideDistance + 1e-3,
        exactObb->computeDistanceSquaredToPosition(outsideCamera));

    const double centerFallbackDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            exactTile,
            Vec3::zero());
    const double centerObbDistance =
        std::sqrt(exactObb->computeDistanceSquaredToPosition(Vec3::zero()));
    EXPECT_NEAR(centerObbDistance, centerFallbackDistance, 1e-6);
}

TEST(TilesetQuantizedMeshTest,
     BoundingRegionObbUsesHeaderHeightRangeLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey key{"Geographic-TMS", 6, 40, 24};
    const Rectangle bounds = scheme->tileToRectangle(key);

    TilesetTile looseTile(key, bounds);
    looseTile.content.renderContent.setTerrainHeightRange(-1000.0, 9000.0);

    TilesetTile exactTile(key, bounds);
    exactTile.content.renderContent.setTerrainHeightRange(-50.0, 1234.0);

    const auto looseObb =
        TilesetTestAccess::tileBoundingRegionObb(looseTile);
    const auto exactObb =
        TilesetTestAccess::tileBoundingRegionObb(exactTile);
    ASSERT_TRUE(looseObb.has_value());
    ASSERT_TRUE(exactObb.has_value());

    EXPECT_LT(exactObb->getHalfAxis(2).length(),
              looseObb->getHalfAxis(2).length());

    const auto& ellipsoid = Ellipsoid::WGS84();
    const double centerLng = bounds.west() + bounds.width() * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    const Vec3 tangentPoint = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 0.0));
    const Vec3 origin = ellipsoid.scaleToGeodeticSurface(tangentPoint);
    const Mat4 tangentFrame =
        Transforms::eastNorthUpToFixedFrame(origin, ellipsoid);
    const Vec3 expectedEast(tangentFrame(0, 0),
                            tangentFrame(1, 0),
                            tangentFrame(2, 0));
    const Vec3 expectedNorth(tangentFrame(0, 1),
                             tangentFrame(1, 1),
                             tangentFrame(2, 1));
    const Vec3 expectedUp(tangentFrame(0, 2),
                          tangentFrame(1, 2),
                          tangentFrame(2, 2));
    EXPECT_GT(exactObb->getHalfAxis(0).normalized().dot(expectedEast),
              1.0 - 1e-12);
    EXPECT_GT(exactObb->getHalfAxis(1).normalized().dot(expectedNorth),
              1.0 - 1e-12);
    EXPECT_GT(exactObb->getHalfAxis(2).normalized().dot(expectedUp),
              1.0 - 1e-12);

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 200000.0,
                  center,
                  Vec3::unitZ());
    const Frustum frustum = camera.frustum(800.0, 800.0);
    EXPECT_TRUE(TilesetTestAccess::tileIntersectsFrustum(exactTile, frustum));
}

TEST(TilesetQuantizedMeshTest,
     ChildrenInheritParentHeaderHeightRangeLikeCesiumNative) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);

    constexpr float minimumHeight = -320.0f;
    constexpr float maximumHeight = 2048.0f;
    installQuantizedMeshTerrainContent(
        *root,
        makeQuantizedMeshBytes(
            Vec3::zero(),
            Vec3::zero(),
            minimumHeight,
            maximumHeight));

    TilesetTestAccess::ensureTileMesh(tileset, *root);
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(4u, root->children.size());

    for (const TilesetTile* child : root->children) {
        ASSERT_NE(nullptr, child);
        EXPECT_TRUE(child->content.renderContent.hasTerrainHeightRange());
        EXPECT_NEAR(
            minimumHeight,
            child->content.renderContent.terrainMinimumHeight(),
            1e-6);
        EXPECT_NEAR(
            maximumHeight,
            child->content.renderContent.terrainMaximumHeight(),
            1e-6);

        ASSERT_TRUE(child->boundingVolume.has_value());
        EXPECT_EQ(TileBoundingVolumeKind::Region, child->boundingVolume->kind);
        EXPECT_NEAR(minimumHeight,
                    child->boundingVolume->minimumHeight,
                    1e-6);
        EXPECT_NEAR(maximumHeight,
                    child->boundingVolume->maximumHeight,
                    1e-6);

        EXPECT_FALSE(child->contentBoundingVolume.has_value());
    }
}

TEST(TilesetQuantizedMeshTest,
     ContentTerrainAvailabilityUpsamplePreservesRasterOverlayProjectionUvLikeCesiumNative) {
    auto provider = std::make_unique<SparseTerrainProvider>(
        "XYZ-WebMercator");
    auto scheme = TileScheme::createXYZWebMercator();
    Tileset tileset(std::move(provider),
                    std::move(scheme),
                    {},
                    nullptr,
                    TilesetOptions{});

    const TileKey rootKey{"XYZ-WebMercator", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);

    installQuantizedMeshTerrainContent(
        *root,
        makeQuantizedMeshBytes(Vec3::zero(), Vec3::zero()),
        RasterOverlayProjection::WebMercator);
    TilesetTestAccess::ensureTileChildren(tileset, *root);

    ASSERT_EQ(4u, root->children.size());
    TilesetTile* upsampledChild = root->children[1];
    ASSERT_NE(nullptr, upsampledChild);
    ASSERT_EQ((TileKey{"XYZ-WebMercator", 1, 1, 0}), upsampledChild->key);
    ASSERT_TRUE(upsampledChild->content.isTerrainAvailabilityUpsample());

    std::optional<TileLoadResult> childLoad =
        TileGltfTerrainUpsampledChildMaterializer::createLoadResult(
            *upsampledChild);
    ASSERT_TRUE(childLoad.has_value());
    ASSERT_EQ(TileLoadStatus::Renderable, childLoad->status);
    ASSERT_NE(nullptr, childLoad->content.gltfModel);

    const GltfModel& childModel = *childLoad->content.gltfModel;
    const int webMercatorTexCoord =
        childModel.rasterOverlayDetails.textureCoordinateIDForProjection(
            RasterOverlayProjection::WebMercator);
    ASSERT_GE(webMercatorTexCoord, 0);
    ASSERT_LT(webMercatorTexCoord,
              static_cast<int>(kGltfMaxTexCoordSets));
    ASSERT_FALSE(childModel.primitives.empty());

    bool sawNonDegenerateTexCoords = false;
    for (const GltfPrimitive& primitive : childModel.primitives) {
        ASSERT_LT(static_cast<size_t>(webMercatorTexCoord),
                  primitive.vertexTexCoords.size());
        const auto& texCoords =
            primitive.vertexTexCoords[static_cast<size_t>(
                webMercatorTexCoord)];
        ASSERT_EQ(primitive.vertices.size(), texCoords.size());
        for (const auto& uv : texCoords) {
            EXPECT_GE(uv[0], 0.5f - 1e-6f);
            EXPECT_LE(uv[0], 1.0f + 1e-6f);
            EXPECT_GE(uv[1], 0.0f - 1e-6f);
            EXPECT_LE(uv[1], 0.5f + 1e-6f);
            sawNonDegenerateTexCoords |=
                std::abs(uv[0]) > 1e-6f || std::abs(uv[1]) > 1e-6f;
        }
    }
    EXPECT_TRUE(sawNonDegenerateTexCoords);

    ASSERT_TRUE(childLoad->content.metadata.rasterOverlayDetails.has_value());
    EXPECT_EQ(
        webMercatorTexCoord,
        childLoad->content.metadata.rasterOverlayDetails
            ->textureCoordinateIDForProjection(
                RasterOverlayProjection::WebMercator));
}

} // namespace
