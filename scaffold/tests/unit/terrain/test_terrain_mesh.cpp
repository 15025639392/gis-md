#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/terrain/TerrainMesh.h"
#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/globe/Globe.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/providers/TerrainProvider.h"

using namespace earth_engine;

// ============================================================
// 基础网格生成
// ============================================================

TEST(TerrainMeshTest, BuildFlatMeshNoTerrain) {
    // 无地形数据 → 网格应与基础网格相同
    GlobeMesh base = Globe::createMesh(12, 6);

    GlobeMesh terrain = TerrainMeshBuilder::buildDisplaced(base, nullptr);

    EXPECT_EQ(base.vertices.size(), terrain.vertices.size());
    EXPECT_EQ(base.indices.size(), terrain.indices.size());

    // 验证顶点位置未改变
    for (size_t i = 0; i < base.vertices.size(); ++i) {
        EXPECT_FLOAT_EQ(base.vertices[i].position[0], terrain.vertices[i].position[0]);
        EXPECT_FLOAT_EQ(base.vertices[i].position[1], terrain.vertices[i].position[1]);
        EXPECT_FLOAT_EQ(base.vertices[i].position[2], terrain.vertices[i].position[2]);
    }
}

TEST(TerrainMeshTest, BuildDisplacedWithHeight) {
    GlobeMesh base = Globe::createMesh(12, 6);

    // 创建模拟高度图（全 tile 高度 500m）
    auto hm = std::make_unique<DecodedHeightmap>();
    hm->tileSize = 256;
    hm->heights.resize(256 * 256, 500.0f);
    hm->minHeight = 500.0f;
    hm->maxHeight = 500.0f;

    auto scheme = TileScheme::createXYZWebMercator();
    TerrainTile tile(TileKey{"XYZ-WebMercator", 0, 0, 0},
                     *scheme, std::move(hm));

    GlobeMesh terrain = TerrainMeshBuilder::buildDisplaced(base, &tile);

    EXPECT_EQ(base.vertices.size(), terrain.vertices.size());

    // 验证有顶点被位移（位置与原始不同）
    bool anyDisplaced = false;
    for (size_t i = 0; i < base.vertices.size(); ++i) {
        if (std::abs(base.vertices[i].position[0] - terrain.vertices[i].position[0]) > 1e-7f ||
            std::abs(base.vertices[i].position[1] - terrain.vertices[i].position[1]) > 1e-7f ||
            std::abs(base.vertices[i].position[2] - terrain.vertices[i].position[2]) > 1e-7f) {
            anyDisplaced = true;
            break;
        }
    }
    EXPECT_TRUE(anyDisplaced);
}

TEST(TerrainMeshTest, DisplacementDirectionIsAlongNormal) {
    GlobeMesh base = Globe::createMesh(12, 6);

    auto hm = std::make_unique<DecodedHeightmap>();
    hm->tileSize = 256;
    hm->heights.resize(256 * 256, 1000.0f);
    hm->minHeight = 1000.0f;
    hm->maxHeight = 1000.0f;

    auto scheme = TileScheme::createXYZWebMercator();
    TerrainTile tile(TileKey{"XYZ-WebMercator", 0, 0, 0},
                     *scheme, std::move(hm));

    GlobeMesh terrain = TerrainMeshBuilder::buildDisplaced(base, &tile);

    // 抽取一个顶点验证位移方向与法线一致
    constexpr double kEarthRadius = 6378137.0;
    for (size_t i = 0; i < std::min(base.vertices.size(), size_t(10)); ++i) {
        float dx = terrain.vertices[i].position[0] - base.vertices[i].position[0];
        float dy = terrain.vertices[i].position[1] - base.vertices[i].position[1];
        float dz = terrain.vertices[i].position[2] - base.vertices[i].position[2];

        float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (len < 1e-7f) continue;

        // 归一化位移方向
        float nx = dx / len;
        float ny = dy / len;
        float nz = dz / len;

        // 应与法线方向一致（点积 ≈ 1）
        float dot = nx * base.vertices[i].normal[0] +
                    ny * base.vertices[i].normal[1] +
                    nz * base.vertices[i].normal[2];
        EXPECT_NEAR(1.0f, dot, 0.01f);
    }
}

// ============================================================
// Skirt（裙边）
// ============================================================

TEST(TerrainMeshTest, BuildWithSkirtAddsGeometry) {
    GlobeMesh base = Globe::createMesh(8, 4);

    auto hm = std::make_unique<DecodedHeightmap>();
    hm->tileSize = 256;
    hm->heights.resize(256 * 256, 200.0f);
    hm->minHeight = 200.0f;
    hm->maxHeight = 200.0f;

    auto scheme = TileScheme::createXYZWebMercator();
    TerrainTile tile(TileKey{"XYZ-WebMercator", 0, 0, 0},
                     *scheme, std::move(hm));

    // build（含裙边）
    GlobeMesh withSkirt = TerrainMeshBuilder::build(base, &tile, -50.0f);

    // buildDisplaced（无裙边）
    GlobeMesh noSkirt = TerrainMeshBuilder::buildDisplaced(base, &tile);

    // 带裙边的网格应有更多顶点和索引
    EXPECT_GT(withSkirt.vertices.size(), noSkirt.vertices.size());
    EXPECT_GT(withSkirt.indices.size(), noSkirt.indices.size());

    // 裙边额外顶点：4 边 × (kEdgeSegments+1) × 2（顶+底）= 4 × 25 × 2 = 200
    size_t expectedExtraVerts = 4 * 25 * 2;  // 4 edges × 25 pairs × 2 vertices
    EXPECT_EQ(withSkirt.vertices.size() - noSkirt.vertices.size(),
              expectedExtraVerts);

    // 裙边额外索引：4 边 × kEdgeSegments × 6（每段 2 三角形）= 4 × 24 × 6 = 576
    size_t expectedExtraIndices = 4 * 24 * 6;
    EXPECT_EQ(withSkirt.indices.size() - noSkirt.indices.size(),
              expectedExtraIndices);
}

TEST(TerrainMeshTest, SkirtVerticesAreBelowSurface) {
    GlobeMesh base = Globe::createMesh(8, 4);

    auto hm = std::make_unique<DecodedHeightmap>();
    hm->tileSize = 256;
    hm->heights.resize(256 * 256, 300.0f);
    hm->minHeight = 300.0f;
    hm->maxHeight = 300.0f;

    auto scheme = TileScheme::createXYZWebMercator();
    TerrainTile tile(TileKey{"XYZ-WebMercator", 0, 0, 0},
                     *scheme, std::move(hm));

    GlobeMesh mesh = TerrainMeshBuilder::build(base, &tile, -50.0f);

    // 裙边顶点是成对添加的：top（表面）→ bottom（下沉）
    // 验证每对中 bottom 顶点更接近地球中心（长度更短）
    size_t baseVertCount = base.vertices.size();
    for (size_t i = baseVertCount; i + 1 < mesh.vertices.size(); i += 2) {
        float topLen = std::sqrt(
            mesh.vertices[i].position[0] * mesh.vertices[i].position[0] +
            mesh.vertices[i].position[1] * mesh.vertices[i].position[1] +
            mesh.vertices[i].position[2] * mesh.vertices[i].position[2]);
        float botLen = std::sqrt(
            mesh.vertices[i + 1].position[0] * mesh.vertices[i + 1].position[0] +
            mesh.vertices[i + 1].position[1] * mesh.vertices[i + 1].position[1] +
            mesh.vertices[i + 1].position[2] * mesh.vertices[i + 1].position[2]);
        EXPECT_LT(botLen, topLen)
            << "Skirt bottom vertex should be closer to Earth center than top";
    }
}

TEST(TerrainMeshTest, NoSkirtWithoutTile) {
    GlobeMesh base = Globe::createMesh(8, 4);

    // nullptr tile → 无裙边
    GlobeMesh mesh = TerrainMeshBuilder::build(base, nullptr, -50.0f);
    EXPECT_EQ(base.vertices.size(), mesh.vertices.size());
    EXPECT_EQ(base.indices.size(), mesh.indices.size());
}

TEST(TerrainMeshTest, NoSkirtWithPositiveHeight) {
    GlobeMesh base = Globe::createMesh(8, 4);

    auto hm = std::make_unique<DecodedHeightmap>();
    hm->tileSize = 2;
    hm->heights = {0, 0, 0, 0};
    hm->minHeight = 0;
    hm->maxHeight = 0;

    auto scheme = TileScheme::createXYZWebMercator();
    TerrainTile tile(TileKey{"XYZ-WebMercator", 0, 0, 0},
                     *scheme, std::move(hm));

    // 正 skirt 高度 → 不生成裙边
    GlobeMesh mesh = TerrainMeshBuilder::build(base, &tile, 50.0f);
    EXPECT_EQ(mesh.vertices.size(), base.vertices.size());
}
