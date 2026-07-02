// [TDIAG] Temporary diagnostic test: does a real nadir quantized-mesh tile
// project onto the screen with the exact MinimalGlobe demo camera?
//
// Reproduces the macOS demo situation headlessly:
//   camera at (106.508E, 29.617N, 30km) looking straight down at Chongqing,
//   viewport 1280x720, FOV 60deg reverse-Z (Camera defaults).
// Loads the real z10 tile 1629/680 (the nadir tile), decodes it, and projects
// every vertex both in world space (viewProj * ecef) and via the GPU path
// (float mvp = viewProj * translate(localOrigin), applied to float rel pos).
//
// If on-screen vertex count > 0 -> geometry/camera path is correct and the
// "blue screen" is a load/convergence artifact, not a render bug.

#include <gtest/gtest.h>

#include "earth_engine/content/QuantizedMeshContentLoader.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace earth_engine;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

}  // namespace

TEST(TerrainNadirProjection, RealNadirTileProjectsOnScreen) {
    const std::string tilePath =
        "/Users/ldy/Desktop/work/dem_test/data/tiles/"
        "fabdem-quantized-mesh-chongqing/10/1629/680.terrain";
    const std::vector<uint8_t> bytes = readFile(tilePath);
    if (bytes.empty()) {
        GTEST_SKIP() << "tile file not available: " << tilePath;
    }

    // Rectangle for key via the engine's own geographic-TMS scheme.
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey key{"Geographic-TMS", 10, 1629, 680};
    const Rectangle rect = scheme->tileToRectangle(key);
    std::fprintf(stderr,
        "[NADIR] tile rect deg = [W %.4f, S %.4f, E %.4f, N %.4f]\n",
        rect.westDegrees(), rect.southDegrees(),
        rect.eastDegrees(), rect.northDegrees());

    TileContentLoadResult res = QuantizedMeshContentLoader::loadTileContent(
        bytes.data(), bytes.size(), rect, /*enableWaterMask=*/false, {});
    ASSERT_EQ(TileLoadStatus::Renderable, res.status);
    ASSERT_NE(nullptr, res.gltfModel);
    ASSERT_FALSE(res.gltfModel->primitives.empty());
    ASSERT_TRUE(res.gltfModel->preferredLocalOriginEcef.has_value());

    const GltfPrimitive& prim = res.gltfModel->primitives.front();
    const Vec3 lo = *res.gltfModel->preferredLocalOriginEcef;
    std::fprintf(stderr,
        "[NADIR] verts=%zu localOrigin=(%.0f,%.0f,%.0f)  v0=(%.0f,%.0f,%.0f)\n",
        prim.vertices.size(), lo.x(), lo.y(), lo.z(),
        prim.vertices.empty() ? 0.0 : prim.vertices[0].positionEcef.x(),
        prim.vertices.empty() ? 0.0 : prim.vertices[0].positionEcef.y(),
        prim.vertices.empty() ? 0.0 : prim.vertices[0].positionEcef.z());

    // Exact demo camera (EarthEngineSdkFacade::resetCamera).
    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(106.508, 29.617, 0.0));
    const Vec3 camPos = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(106.508, 29.617, 30000.0));
    const Vec3 up = ellipsoid.geodeticSurfaceNormal(target);
    Camera cam;
    cam.lookAt(camPos, target, up);

    const double vpW = 1280.0, vpH = 720.0;
    const glm::dmat4 viewD(cam.viewMatrix().raw());
    const glm::dmat4 projD(cam.projectionMatrix(vpW, vpH).raw());
    const glm::dmat4 viewProj = projD * viewD;

    // GPU path: float mvp = viewProj * translate(localOrigin); vertices are
    // float rel = ecef - localOrigin.
    const glm::dmat4 modelD = glm::translate(glm::dmat4(1.0),
        glm::dvec3(lo.x(), lo.y(), lo.z()));
    const glm::mat4 mvpF(viewProj * modelD);

    int onScreenWorld = 0, onScreenGpu = 0, behind = 0;
    for (const SurfaceVertex& v : prim.vertices) {
        const glm::dvec3 e(v.positionEcef.x(), v.positionEcef.y(),
                           v.positionEcef.z());
        const glm::dvec4 clipW = viewProj * glm::dvec4(e, 1.0);
        if (clipW.w > 0.0) {
            const double nx = clipW.x / clipW.w, ny = clipW.y / clipW.w;
            if (nx >= -1.0 && nx <= 1.0 && ny >= -1.0 && ny <= 1.0)
                ++onScreenWorld;
        } else {
            ++behind;
        }
        const glm::vec3 rel(static_cast<float>(e.x - lo.x()),
                            static_cast<float>(e.y - lo.y()),
                            static_cast<float>(e.z - lo.z()));
        const glm::vec4 clipG = mvpF * glm::vec4(rel, 1.0f);
        if (clipG.w > 0.0f) {
            const float nx = clipG.x / clipG.w, ny = clipG.y / clipG.w;
            if (nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f)
                ++onScreenGpu;
        }
    }

    // Project the tile center (localOrigin) explicitly.
    const glm::dvec4 loClip = viewProj * glm::dvec4(lo.x(), lo.y(), lo.z(), 1.0);
    std::fprintf(stderr,
        "[NADIR] localOrigin ndc=(%.3f,%.3f,%.3f) w=%.1f | onScreen world=%d gpu=%d behind=%d of %zu\n",
        loClip.x / loClip.w, loClip.y / loClip.w, loClip.z / loClip.w, loClip.w,
        onScreenWorld, onScreenGpu, behind, prim.vertices.size());

    // Project the EXACT bytes the demo uploads: terrainGpuVertexBytes (rel pos,
    // built async in makeQuantizedMeshGltfModel with the SAME localOrigin).
    const std::vector<uint8_t>& gb = prim.terrainGpuVertexBytes;
    std::fprintf(stderr, "[NADIR] terrainGpuVertexBytes=%zu (%zu verts)\n",
                 gb.size(), gb.size() / 32);
    int gbOn = 0, gbBehind = 0;
    double nxMin = 1e9, nxMax = -1e9, nyMin = 1e9, nyMax = -1e9;
    for (size_t off = 0; off + 32 <= gb.size(); off += 32) {
        float p[3];
        std::memcpy(p, gb.data() + off, 12);
        const glm::vec4 clip = mvpF * glm::vec4(p[0], p[1], p[2], 1.0f);
        if (clip.w <= 0.0f) { ++gbBehind; continue; }
        const double nx = clip.x / clip.w, ny = clip.y / clip.w;
        if (nx >= -1.0 && nx <= 1.0 && ny >= -1.0 && ny <= 1.0) ++gbOn;
        nxMin = std::min(nxMin, nx); nxMax = std::max(nxMax, nx);
        nyMin = std::min(nyMin, ny); nyMax = std::max(nyMax, ny);
    }
    std::fprintf(stderr,
        "[NADIR] gpuBytes onScreen=%d behind=%d  NDC bbox x=[%.2f,%.2f] y=[%.2f,%.2f]\n",
        gbOn, gbBehind, nxMin, nxMax, nyMin, nyMax);

    // --- Triangle validity: are interior triangles degenerate? ---
    // Skirt metadata tells where interior ends.
    ASSERT_TRUE(prim.skirtMetadata.has_value());
    const uint32_t interiorEnd = prim.skirtMetadata->noSkirtIndicesCount;
    const auto triStats = [&](size_t begin, size_t end, const char* label) {
        size_t total = 0, dupIdx = 0, zeroArea = 0, oob = 0;
        double areaSum = 0.0;
        for (size_t i = begin; i + 2 < end; i += 3) {
            const uint32_t a = prim.indices[i], b = prim.indices[i + 1],
                           c = prim.indices[i + 2];
            ++total;
            if (a >= prim.vertices.size() || b >= prim.vertices.size() ||
                c >= prim.vertices.size()) { ++oob; continue; }
            if (a == b || b == c || a == c) { ++dupIdx; continue; }
            const Vec3& pa = prim.vertices[a].positionEcef;
            const Vec3& pb = prim.vertices[b].positionEcef;
            const Vec3& pc = prim.vertices[c].positionEcef;
            const Vec3 cr = (pb - pa).cross(pc - pa);
            const double area = 0.5 * std::sqrt(cr.lengthSquared());
            if (area < 1e-6) { ++zeroArea; continue; }
            areaSum += area;
        }
        std::fprintf(stderr,
            "[NADIR] %s tris=%zu dupIdx=%zu zeroArea=%zu oob=%zu avgArea=%.1fm2 "
            "idx[0..5]=%u,%u,%u,%u,%u,%u\n",
            label, total, dupIdx, zeroArea, oob,
            total > dupIdx + zeroArea + oob
                ? areaSum / double(total - dupIdx - zeroArea - oob) : 0.0,
            begin + 5 < prim.indices.size() ? prim.indices[begin] : 0,
            begin + 5 < prim.indices.size() ? prim.indices[begin + 1] : 0,
            begin + 5 < prim.indices.size() ? prim.indices[begin + 2] : 0,
            begin + 5 < prim.indices.size() ? prim.indices[begin + 3] : 0,
            begin + 5 < prim.indices.size() ? prim.indices[begin + 4] : 0,
            begin + 5 < prim.indices.size() ? prim.indices[begin + 5] : 0);
        return total - dupIdx - zeroArea - oob;
    };
    std::fprintf(stderr, "[NADIR] indices=%zu interiorEnd=%u (skirt begins there)\n",
                 prim.indices.size(), interiorEnd);
    const size_t interiorValid = triStats(0, interiorEnd, "INTERIOR");
    const size_t skirtValid =
        triStats(interiorEnd, prim.indices.size(), "SKIRT   ");
    EXPECT_GT(interiorValid, 0u) << "ALL interior triangles degenerate!";
    (void)skirtValid;

    EXPECT_GT(onScreenWorld, 0) << "nadir tile world-space geometry is off-screen";
    EXPECT_GT(onScreenGpu, 0) << "nadir tile GPU-path (float rel+mvp) geometry is off-screen";
}
