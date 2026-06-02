#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/globe/Globe.h"

using namespace earth_engine;

TEST(GlobeTest, CreateDefaultMesh) {
    GlobeMesh mesh = Globe::createMesh(96, 48);

    // 顶点数：(96+1) * (48+1) = 97 * 49 = 4753
    EXPECT_EQ(4753u, mesh.vertices.size());

    // 索引数：96 * 48 * 6 = 27648
    EXPECT_EQ(27648u, mesh.indices.size());
}

TEST(GlobeTest, MeshBounds) {
    GlobeMesh mesh = Globe::createMesh(96, 48);

    // 检查顶点位置在合理范围内
    float maxAbsX = 0.0f, maxAbsY = 0.0f, maxAbsZ = 0.0f;
    for (const auto& v : mesh.vertices) {
        maxAbsX = std::max(maxAbsX, std::abs(v.position[0]));
        maxAbsY = std::max(maxAbsY, std::abs(v.position[1]));
        maxAbsZ = std::max(maxAbsZ, std::abs(v.position[2]));
    }

    // X: 半长轴方向，最大应接近 1.0
    EXPECT_NEAR(1.0f, maxAbsX, 0.01f);
    // Y: 半长轴方向，同 X
    EXPECT_NEAR(1.0f, maxAbsY, 0.01f);
    // Z: 半短轴方向，最大应接近 polarRadiusRatio
    EXPECT_NEAR(Globe::polarRadiusRatio, maxAbsZ, 0.01f);
}

TEST(GlobeTest, VertexNormalsAreUnitLength) {
    GlobeMesh mesh = Globe::createMesh(48, 24);

    for (const auto& v : mesh.vertices) {
        float len = std::sqrt(v.normal[0] * v.normal[0] +
                              v.normal[1] * v.normal[1] +
                              v.normal[2] * v.normal[2]);
        EXPECT_NEAR(1.0f, len, 1e-4f);
    }
}

TEST(GlobeTest, TexcoordsInRange) {
    GlobeMesh mesh = Globe::createMesh(48, 24);

    for (const auto& v : mesh.vertices) {
        EXPECT_GE(v.texcoord[0], 0.0f);
        EXPECT_LE(v.texcoord[0], 1.0f);
        EXPECT_GE(v.texcoord[1], 0.0f);
        EXPECT_LE(v.texcoord[1], 1.0f);
    }
}

TEST(GlobeTest, IndicesInRange) {
    GlobeMesh mesh = Globe::createMesh(32, 16);
    uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());

    for (uint32_t idx : mesh.indices) {
        EXPECT_LT(idx, vertexCount);
    }
}

TEST(GlobeTest, CreateCoarseMesh) {
    GlobeMesh mesh = Globe::createMesh(4, 2);

    // (4+1) * (2+1) = 5 * 3 = 15 vertices
    EXPECT_EQ(15u, mesh.vertices.size());
    // 4 * 2 * 6 = 48 indices
    EXPECT_EQ(48u, mesh.indices.size());
}

TEST(GlobeTest, RejectsInvalidSegmentCounts) {
    EXPECT_THROW(Globe::createMesh(0, 8), std::invalid_argument);
    EXPECT_THROW(Globe::createMesh(16, 0), std::invalid_argument);
    EXPECT_THROW(Globe::createMesh(2, 2), std::invalid_argument);
    EXPECT_THROW(Globe::createMesh(3, 1), std::invalid_argument);
}

TEST(GlobeTest, PolarVertices) {
    // 南极：v=0, phi=-π/2 → position[2] = -polarRadiusRatio
    // 北极：v=1, phi=+π/2 → position[2] = +polarRadiusRatio
    GlobeMesh mesh = Globe::createMesh(16, 8);

    float minZ = 0.0f, maxZ = 0.0f;
    for (const auto& v : mesh.vertices) {
        minZ = std::min(minZ, v.position[2]);
        maxZ = std::max(maxZ, v.position[2]);
    }

    EXPECT_NEAR(-Globe::polarRadiusRatio, minZ, 0.01f);
    EXPECT_NEAR(Globe::polarRadiusRatio, maxZ, 0.01f);
}
