#include "PolarCapRenderer.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../renderer/RenderDevice.h"
#include "../renderer/Renderer.h"
#include "../tiling/GltfRenderGeometryBuilder.h"  // GltfGpuVertex

#include <algorithm>
#include <cstdint>
#include <vector>

namespace earth_engine {
namespace {

constexpr int kLonSegments = 64;
constexpr int kLatRings = 8;
// Web-mercator maximum latitude (WebMercatorProjection edge). The terrain and
// imagery tiling stops here, so the cap starts exactly at this parallel and
// meets the top terrain row with no gap.
constexpr double kMercatorEdgeLatDeg = 85.05112877980659;

void buildCapMesh(double poleSign,  // +1 north, -1 south
                  std::vector<GltfGpuVertex>& vertices,
                  std::vector<uint32_t>& indices,
                  float originEcef[3]) {
    const Ellipsoid& e = Ellipsoid::WGS84();
    const double poleLatDeg = 90.0 * poleSign;
    const double edgeLatDeg = kMercatorEdgeLatDeg * poleSign;

    const Vec3 originV = e.cartographicToCartesian(
        Cartographic::fromDegrees(0.0, poleLatDeg, 0.0));
    originEcef[0] = static_cast<float>(originV.x());
    originEcef[1] = static_cast<float>(originV.y());
    originEcef[2] = static_cast<float>(originV.z());

    // Rows run north→south (decreasing latitude) and columns west→east, matching
    // EllipsoidTerrainMeshBuilder's vertex order, so the shared (a,c,b,b,c,d)
    // winding is outward-front for both poles and renders correctly with
    // back-face culling. Positions follow the ellipsoid surface (height 0) so
    // the cap is curved. ring 0 = the northern edge of the cap band.
    const double topLatDeg = std::max(poleLatDeg, edgeLatDeg);
    const double botLatDeg = std::min(poleLatDeg, edgeLatDeg);
    for (int ri = 0; ri <= kLatRings; ++ri) {
        const double t = static_cast<double>(ri) / kLatRings;
        const double latDeg = topLatDeg - (topLatDeg - botLatDeg) * t;
        for (int si = 0; si <= kLonSegments; ++si) {
            const double lonDeg =
                -180.0 + 360.0 * (static_cast<double>(si) / kLonSegments);
            const Vec3 p = e.cartographicToCartesian(
                Cartographic::fromDegrees(lonDeg, latDeg, 0.0));
            const Vec3 n = e.geodeticSurfaceNormal(p);
            GltfGpuVertex v{};
            v.pos[0] = static_cast<float>(p.x() - originV.x());
            v.pos[1] = static_cast<float>(p.y() - originV.y());
            v.pos[2] = static_cast<float>(p.z() - originV.z());
            v.nrm[0] = static_cast<float>(n.x());
            v.nrm[1] = static_cast<float>(n.y());
            v.nrm[2] = static_cast<float>(n.z());
            v.color[0] = 1.0f;
            v.color[1] = 1.0f;
            v.color[2] = 1.0f;
            v.color[3] = 1.0f;
            vertices.push_back(v);
        }
    }

    const int rowStride = kLonSegments + 1;
    for (int ri = 0; ri < kLatRings; ++ri) {
        for (int si = 0; si < kLonSegments; ++si) {
            const uint32_t a = static_cast<uint32_t>(ri * rowStride + si);
            const uint32_t b = a + 1;
            const uint32_t c = static_cast<uint32_t>((ri + 1) * rowStride + si);
            const uint32_t d = c + 1;
            // Winding is irrelevant: the cap draws double-sided (cullFace off).
            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(d);
        }
    }
}

} // namespace

PolarCapRenderer::PolarCapRenderer() = default;
PolarCapRenderer::~PolarCapRenderer() = default;

bool PolarCapRenderer::ensureBuilt(RenderDevice* device) {
    if (built_) {
        return true;
    }
    if (buildFailed_ || !device) {
        return false;
    }

    auto buildOne = [&](double poleSign, Cap& cap) -> bool {
        std::vector<GltfGpuVertex> vertices;
        std::vector<uint32_t> indices;
        buildCapMesh(poleSign, vertices, indices, cap.originEcef);

        BufferDesc vbDesc;
        vbDesc.size = vertices.size() * sizeof(GltfGpuVertex);
        vbDesc.data = vertices.data();
        vbDesc.usage = BufferDesc::Usage::Static;
        vbDesc.type = BufferDesc::Type::Vertex;
        cap.vertexBuffer = device->createBuffer(vbDesc);

        BufferDesc ibDesc;
        ibDesc.size = indices.size() * sizeof(uint32_t);
        ibDesc.data = indices.data();
        ibDesc.usage = BufferDesc::Usage::Static;
        ibDesc.type = BufferDesc::Type::Index;
        cap.indexBuffer = device->createBuffer(ibDesc);

        cap.indexCount = static_cast<int>(indices.size());
        cap.vertexCount = static_cast<int>(vertices.size());
        return cap.vertexBuffer != nullptr && cap.indexBuffer != nullptr;
    };

    if (!buildOne(1.0, northCap_) || !buildOne(-1.0, southCap_)) {
        buildFailed_ = true;
        return false;
    }
    built_ = true;
    return true;
}

void PolarCapRenderer::appendCommands(RenderCommandList& commands,
                                      Renderer& renderer,
                                      RenderDevice* device,
                                      uint64_t frameId) {
    if (!ensureBuilt(device)) {
        return;
    }

    auto emit = [&](const Cap& cap, const char* stableKey) {
        if (!cap.vertexBuffer || !cap.indexBuffer || cap.indexCount <= 0) {
            return;
        }
        RenderCommand cmd = renderer.makeGltfPrimitiveCommand(
            cap.vertexBuffer.get(),
            cap.indexBuffer.get(),
            cap.indexCount,
            cap.vertexCount);
        cmd.owner = "polar_cap";
        cmd.stableKey = stableKey;
        // MVP-command contract (validateMvpRenderCommands): frameId must match
        // the current frame and generation must be non-zero. The cap is static
        // globe geometry, so a constant non-zero generation is sufficient.
        cmd.frameId = frameId;
        cmd.generation = 1;
        // Lit like the surrounding terrain (default icy baseColor, no texture),
        // so the cap shades with day/night rather than glowing at a constant
        // brightness. Rows are wound outward-front (see buildCapMesh) so
        // back-face culling (the makeGltfPrimitiveCommand default) keeps it.
        // RTC origin at the pole; the per-frame uniform updater fills
        // modelViewProjection / eyePositionRTC / lightDir / ambient from this.
        cmd.gltfUniforms.modelOrigin = {
            cap.originEcef[0],
            cap.originEcef[1],
            cap.originEcef[2]};
        // The default glTF material is fully metallic (materialFactors.x = 1),
        // which has no diffuse term and reflects only image-based lighting we
        // don't have — so it renders black. Make the cap a matte dielectric
        // (metallic 0, roughness 1) so the diffuse day/night term shades it like
        // the surrounding terrain instead of a black hole.
        cmd.gltfUniforms.materialFactors = {0.0f, 1.0f, 1.0f, 1.0f};
        // Snow-white base: the poles are permanent ice, so this blends the cap
        // into the surrounding polar imagery instead of reading as a grey disc,
        // while still shading with day/night through the diffuse term.
        cmd.gltfUniforms.baseColor = {0.92f, 0.94f, 0.97f, 1.0f};
        commands.push_back(cmd);
    };

    emit(northCap_, "polar_cap_north");
    emit(southCap_, "polar_cap_south");
}

} // namespace earth_engine
