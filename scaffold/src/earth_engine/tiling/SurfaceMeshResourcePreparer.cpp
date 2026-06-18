#include "SurfaceMeshResourcePreparer.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../renderer/RenderDevice.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

void SurfaceMeshResourcePreparer::prepare(TilesetTile& tile,
                                          RenderDevice* device) {
    if (!tile.mesh) {
        return;
    }

    if (tile.mesh->hasLocalOriginEcef) {
        tile.localOrigin = tile.mesh->localOriginEcef;
    } else {
        tile.localOrigin = Vec3::zero();
    }
    if (!tile.mesh->hasLocalOriginEcef && !tile.mesh->vertices.empty()) {
        for (const auto& v : tile.mesh->vertices) {
            tile.localOrigin += v.positionEcef;
        }
        tile.localOrigin =
            tile.localOrigin /
            static_cast<double>(tile.mesh->vertices.size());
    }

    if (!device || tile.mesh->vertices.empty()) {
        return;
    }

    std::vector<SurfaceGpuVertex> generatedGpuVertices;
    const std::vector<SurfaceGpuVertex>* gpuVertices =
        tile.mesh->gpuVertices.size() == tile.mesh->vertices.size()
            ? &tile.mesh->gpuVertices
            : nullptr;
    if (!gpuVertices) {
        generatedGpuVertices.resize(tile.mesh->vertices.size());
        for (size_t i = 0; i < tile.mesh->vertices.size(); ++i) {
            const auto& src = tile.mesh->vertices[i];
            SurfaceGpuVertex& dst = generatedGpuVertices[i];
            Vec3 rel = src.positionEcef - tile.localOrigin;
            dst.pos[0] = static_cast<float>(rel.x());
            dst.pos[1] = static_cast<float>(rel.y());
            dst.pos[2] = static_cast<float>(rel.z());
            Vec3 nrm = src.normalEcef;
            if (nrm.lengthSquared() > 0.0) {
                nrm = nrm.normalized();
            } else {
                nrm = Ellipsoid::WGS84().geodeticSurfaceNormal(
                    src.positionEcef);
            }
            dst.nrm[0] = static_cast<float>(nrm.x());
            dst.nrm[1] = static_cast<float>(nrm.y());
            dst.nrm[2] = static_cast<float>(nrm.z());
            dst.uv[0] = src.uv[0];
            dst.uv[1] = src.uv[1];
        }
        gpuVertices = &generatedGpuVertices;
    }

    BufferDesc vbDesc;
    vbDesc.size = gpuVertices->size() * sizeof(SurfaceGpuVertex);
    vbDesc.data = gpuVertices->data();
    vbDesc.usage = BufferDesc::Usage::Static;
    vbDesc.type = BufferDesc::Type::Vertex;
    tile.gpuVertexBuffer = device->createBuffer(vbDesc);

    if (!tile.mesh->indices.empty()) {
        BufferDesc ibDesc;
        ibDesc.size = tile.mesh->indices.size() * sizeof(uint32_t);
        ibDesc.data = tile.mesh->indices.data();
        ibDesc.usage = BufferDesc::Usage::Static;
        ibDesc.type = BufferDesc::Type::Index;
        tile.gpuIndexBuffer = device->createBuffer(ibDesc);
    }
}

} // namespace earth_engine
