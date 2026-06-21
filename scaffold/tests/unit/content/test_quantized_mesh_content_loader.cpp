#include <gtest/gtest.h>

#include "earth_engine/content/QuantizedMeshContentLoader.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace earth_engine;

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
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f) {
    std::vector<uint8_t> bytes;

    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<float>(bytes, minimumHeight);
    appendPod<float>(bytes, maximumHeight);
    for (int i = 0; i < 7; ++i) appendPod<double>(bytes, 0.0);
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
        zigZagEncode16(32767),
        zigZagEncode16(0)
    };
    for (uint16_t value : u) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : v) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : h) appendPod<uint16_t>(bytes, value);

    appendPod<uint32_t>(bytes, 1);
    for (int i = 0; i < 3; ++i) appendPod<uint16_t>(bytes, 0);
    for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    return bytes;
}

std::vector<uint8_t> makeQuantizedMeshBytesWithMetadata(
    const std::string& metadataJson,
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f) {
    std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(minimumHeight, maximumHeight);

    appendPod<uint8_t>(bytes, 4);
    appendPod<uint32_t>(
        bytes,
        static_cast<uint32_t>(sizeof(uint32_t) + metadataJson.size()));
    appendPod<uint32_t>(bytes, static_cast<uint32_t>(metadataJson.size()));
    bytes.insert(bytes.end(), metadataJson.begin(), metadataJson.end());
    return bytes;
}

Rectangle geographicRootWestRectangle() {
    constexpr double kPi = 3.14159265358979323846264338327950288;
    return Rectangle(-kPi, -0.5 * kPi, 0.0, 0.5 * kPi);
}

} // namespace

TEST(QuantizedMeshContentLoaderTest,
     LoadsSurfaceMeshResultWithoutTerrainProvider) {
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(-10.0f, 150.0f);

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            {});

    EXPECT_TRUE(result.success());
    ASSERT_NE(nullptr, result.surfaceMesh);
    EXPECT_FALSE(result.surfaceMesh->vertices.empty());
    EXPECT_FALSE(result.surfaceMesh->indices.empty());
    EXPECT_TRUE(result.surfaceMesh->hasHeightRange);
    EXPECT_NEAR(-10.0, result.surfaceMesh->minimumHeight, 1e-6);
    EXPECT_NEAR(150.0, result.surfaceMesh->maximumHeight, 1e-6);
    EXPECT_EQ(geographicRootWestRectangle(),
              result.surfaceMesh->rasterOverlayDetails.boundingRegion
                  .rectangle);
    EXPECT_NEAR(-10.0,
                result.surfaceMesh->rasterOverlayDetails.boundingRegion
                    .minimumHeight,
                1e-6);
    EXPECT_NEAR(150.0,
                result.surfaceMesh->rasterOverlayDetails.boundingRegion
                    .maximumHeight,
                1e-6);
    ASSERT_TRUE(result.metadata.rasterOverlayDetails.has_value());
    const Rectangle* rasterRectangle =
        result.metadata.rasterOverlayDetails->findRectangleForOverlayProjection(
            RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, rasterRectangle);
    EXPECT_EQ(geographicRootWestRectangle(), *rasterRectangle);
    EXPECT_EQ(geographicRootWestRectangle(),
              result.metadata.rasterOverlayDetails->boundingRegion.rectangle);
    EXPECT_NEAR(-10.0,
                result.metadata.rasterOverlayDetails->boundingRegion
                    .minimumHeight,
                1e-6);
    EXPECT_NEAR(150.0,
                result.metadata.rasterOverlayDetails->boundingRegion
                    .maximumHeight,
                1e-6);
    ASSERT_TRUE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_EQ(TileBoundingVolumeKind::Region,
              result.metadata.updatedBoundingVolume->kind);
    EXPECT_EQ(geographicRootWestRectangle(),
              result.metadata.updatedBoundingVolume->region);
    EXPECT_NEAR(-10.0,
                result.metadata.updatedBoundingVolume->minimumHeight,
                1e-6);
    EXPECT_NEAR(150.0,
                result.metadata.updatedBoundingVolume->maximumHeight,
                1e-6);
    EXPECT_TRUE(result.availabilityUpdates.empty());
}

TEST(QuantizedMeshContentLoaderTest,
     CarriesMetadataAvailabilityUpdatesLikeLayerJsonTerrainLoader) {
    const std::string metadataJson = R"json({
        "available": [
            [
                {"startX": 0, "startY": 0, "endX": 1, "endY": 1}
            ],
            [
                {"startX": 1, "startY": 2, "endX": 3, "endY": 4}
            ]
        ]
    })json";
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytesWithMetadata(metadataJson);
    const TileKey subtreeKey{"Geographic-TMS", 2, 1, 1};
    std::vector<QuantizedMeshMetadataContent> metadata;
    metadata.push_back(QuantizedMeshMetadataContent{
        3,
        subtreeKey,
        bytes.data(),
        bytes.size()});

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            bytes.data(),
            bytes.size(),
            geographicRootWestRectangle(),
            false,
            metadata);

    EXPECT_TRUE(result.success());
    ASSERT_EQ(1u, result.availabilityUpdates.size());
    const QuantizedMeshAvailabilityUpdate& update =
        result.availabilityUpdates.front();
    EXPECT_EQ(3, update.layerIndex);
    EXPECT_EQ(subtreeKey, update.subtreeKey);
    ASSERT_EQ(2u, update.metadataAvailability.size());
    EXPECT_EQ(0u, update.metadataAvailability[0][0]);
    EXPECT_EQ(0u, update.metadataAvailability[0][1]);
    EXPECT_EQ(1u, update.metadataAvailability[0][4]);
    EXPECT_EQ(1u, update.metadataAvailability[1][0]);
    EXPECT_EQ(1u, update.metadataAvailability[1][1]);
    EXPECT_EQ(4u, update.metadataAvailability[1][4]);
}

TEST(QuantizedMeshContentLoaderTest, InvalidBodyFailsWithoutUpdates) {
    const uint8_t invalid[] = {0, 1, 2, 3};

    QuantizedMeshContentLoadResult result =
        QuantizedMeshContentLoader::load(
            invalid,
            sizeof(invalid),
            geographicRootWestRectangle(),
            false,
            {QuantizedMeshMetadataContent{
                0,
                TileKey{"Geographic-TMS", 0, 0, 0},
                invalid,
                sizeof(invalid)}});

    EXPECT_FALSE(result.success());
    EXPECT_EQ(QuantizedMeshContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.surfaceMesh);
    EXPECT_FALSE(result.metadata.updatedBoundingVolume.has_value());
    EXPECT_FALSE(result.metadata.rasterOverlayDetails.has_value());
    EXPECT_TRUE(result.availabilityUpdates.empty());
}
