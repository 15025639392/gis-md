#include "Globe.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <stdexcept>

namespace earth_engine {

GlobeMesh Globe::createMesh(int longitudeSegments, int latitudeSegments) {
    if (longitudeSegments < 3 || latitudeSegments < 2) {
        throw std::invalid_argument(
            "Globe mesh requires at least 3 longitude segments and 2 latitude segments.");
    }

    GlobeMesh mesh;

    const int lonVerts = longitudeSegments + 1;
    const int latVerts = latitudeSegments + 1;
    const int totalVertices = lonVerts * latVerts;

    mesh.vertices.reserve(static_cast<size_t>(totalVertices));
    mesh.indices.reserve(static_cast<size_t>(longitudeSegments * latitudeSegments * 6));

    constexpr float kPi = glm::pi<float>();

    // 生成顶点：逐行从南极（v=0）到北极（v=1）。
    // 坐标轴遵守项目 ECEF 约定：x=经度0赤道，y=经度90赤道，z=北极。
    for (int lat = 0; lat <= latitudeSegments; ++lat) {
        const float v = static_cast<float>(lat) / static_cast<float>(latitudeSegments);
        // phi: -π/2（南极）→ +π/2（北极）
        const float phi = -0.5f * kPi + v * kPi;
        const float cosPhi = std::cos(phi);
        const float sinPhi = std::sin(phi);

        for (int lon = 0; lon <= longitudeSegments; ++lon) {
            const float u = static_cast<float>(lon) / static_cast<float>(longitudeSegments);
            // theta: 0 → 2π（从西向东绕一圈）
            const float theta = u * 2.0f * kPi;
            const float cosTheta = std::cos(theta);
            const float sinTheta = std::sin(theta);

            GlobeVertex vertex{};

            // 椭球面位置（单位球 × WGS84 扁率）
            vertex.position[0] = cosPhi * cosTheta;
            vertex.position[1] = cosPhi * sinTheta;
            vertex.position[2] = polarRadiusRatio * sinPhi;

            // 椭球面法线：对单位椭球 x² + y² + (z/prr)² = 1，
            // 梯度为 (2x, 2y, 2z/prr²)，归一化即得法线
            glm::vec3 normal(vertex.position[0],
                             vertex.position[1],
                             vertex.position[2] / (polarRadiusRatio * polarRadiusRatio));
            normal = glm::normalize(normal);
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;

            vertex.texcoord[0] = u;
            vertex.texcoord[1] = v;

            mesh.vertices.push_back(vertex);
        }
    }

    // 生成索引：每格两个三角形
    for (int lat = 0; lat < latitudeSegments; ++lat) {
        for (int lon = 0; lon < longitudeSegments; ++lon) {
            const uint32_t row0 = static_cast<uint32_t>(lat * lonVerts);
            const uint32_t row1 = static_cast<uint32_t>((lat + 1) * lonVerts);
            const uint32_t a = row0 + static_cast<uint32_t>(lon);
            const uint32_t b = row0 + static_cast<uint32_t>(lon + 1);
            const uint32_t c = row1 + static_cast<uint32_t>(lon);
            const uint32_t d = row1 + static_cast<uint32_t>(lon + 1);

            // CCW winding（从外部看）
            mesh.indices.push_back(a);
            mesh.indices.push_back(c);
            mesh.indices.push_back(b);

            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
            mesh.indices.push_back(d);
        }
    }

    return mesh;
}

} // namespace earth_engine
