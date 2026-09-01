#include <gtest/gtest.h>

#include "earth_engine/providers/AmapSurfaceMaskImageryProvider.h"
#include "earth_engine/style/AmapClassicStyleInternal.h"

#include <algorithm>
#include <memory>

using namespace earth_engine;

namespace {

Feature makeSurface(double west, double south, double east, double north,
                    int classCode = 30001, int subKey = 1) {
    Feature feature;
    feature.type = GeometryType::Polygon;
    feature.rings = {{Cartographic::fromDegrees(west, south),
                      Cartographic::fromDegrees(east, south),
                      Cartographic::fromDegrees(east, north),
                      Cartographic::fromDegrees(west, north)}};
    feature.properties["amap_class"] = std::to_string(classCode);
    feature.properties["amap_subkey"] = std::to_string(subKey);
    return feature;
}

}  // namespace

TEST(AmapSurfaceMaskImageryProvider, ProducesGeographicTmsRgbaPage) {
    auto features = std::make_shared<std::vector<Feature>>();
    features->push_back(makeSurface(-170.0, -30.0, -100.0, 30.0));

    AmapSurfaceMaskImageryProvider provider(
        [features](const TileKey&, CancellationToken,
                   AmapSurfaceMaskImageryProvider::FeatureFetchCallback done,
                   HttpRequestPriority) { done(features); },
        2.0);
    EXPECT_EQ("Geographic-TMS", provider.schemeId());
    EXPECT_EQ("amap-surface-mask-256", provider.id());

    const TileKey key{"Geographic-TMS", 0, 0, 0};
    std::unique_ptr<DecodedImage> image;
    provider.requestTile(
        key, CancellationToken{},
        [&image](const TileKey&, std::unique_ptr<DecodedImage> result) {
            image = std::move(result);
        });

    ASSERT_NE(nullptr, image);
    EXPECT_EQ(256, image->width);
    EXPECT_EQ(256, image->height);
    EXPECT_EQ(4, image->channels);
    ASSERT_EQ(image->pixels.size(), static_cast<size_t>(256 * 256 * 4));

    size_t covered = 0;
    for (size_t i = 3; i < image->pixels.size(); i += 4) {
        covered += image->pixels[i] != 0;
    }
    EXPECT_GT(covered, 0u);
    EXPECT_LT(covered, static_cast<size_t>(256 * 256));
}

TEST(AmapSurfaceMaskImageryProvider, SkipsBuildingsAndPropagatesUnavailable) {
    auto features = std::make_shared<std::vector<Feature>>();
    features->push_back(makeSurface(-170.0, -30.0, -100.0, 30.0));
    Feature building = makeSurface(-170.0, -30.0, -100.0, 30.0, 55001, 1);
    building.properties["amap_height"] = "20";
    features->push_back(std::move(building));

    AmapSurfaceMaskImageryProvider provider(
        [features](const TileKey&, CancellationToken,
                   AmapSurfaceMaskImageryProvider::FeatureFetchCallback done,
                   HttpRequestPriority) { done(features); },
        2.0);
    const TileKey key{"Geographic-TMS", 0, 0, 0};
    std::unique_ptr<DecodedImage> image;
    provider.requestTile(
        key, CancellationToken{},
        [&image](const TileKey&, std::unique_ptr<DecodedImage> result) {
            image = std::move(result);
        });
    ASSERT_NE(nullptr, image);
    size_t covered = 0;
    for (size_t i = 3; i < image->pixels.size(); i += 4) {
        covered += image->pixels[i] != 0;
    }
    EXPECT_GT(covered, 0u);

    AmapSurfaceMaskImageryProvider unavailable(
        [](const TileKey&, CancellationToken,
           AmapSurfaceMaskImageryProvider::FeatureFetchCallback done,
           HttpRequestPriority) { done(nullptr); },
        2.0);
    bool called = false;
    unavailable.requestTile(
        key, CancellationToken{},
        [&called](const TileKey&, std::unique_ptr<DecodedImage> result) {
            called = true;
            EXPECT_EQ(nullptr, result);
        });
    EXPECT_TRUE(called);
}

TEST(AmapSurfaceMaskImageryProvider,
     DiscreteDisplayZoomChangesInvalidatePageContent) {
    auto features = std::make_shared<std::vector<Feature>>();
    features->push_back(makeSurface(-170.0, -30.0, -100.0, 30.0));
    auto styleState = std::make_shared<AmapSurfaceMaskStyleState>(2.1);
    AmapSurfaceMaskImageryProvider provider(
        [features](const TileKey&, CancellationToken,
                   AmapSurfaceMaskImageryProvider::FeatureFetchCallback done,
                   HttpRequestPriority) { done(features); },
        styleState);

    EXPECT_EQ(0u, provider.contentRevision());
    styleState->setDisplayZoom(2.7);
    EXPECT_EQ(0u, provider.contentRevision())
        << "official .8 threshold must not churn pages early";
    styleState->setDisplayZoom(2.8);
    EXPECT_EQ(1u, provider.contentRevision());
    EXPECT_DOUBLE_EQ(3.0, provider.displayZoom());
}
