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
    SurfaceTileMesh* mesh = tile.content.renderContent.mutableSurfaceMesh();
    if (!mesh) {
        return;
    }

    if (mesh->hasLocalOriginEcef) {
        tile.content.renderContent.setSurfaceLocalOrigin(mesh->localOriginEcef);
    } else {
        tile.content.renderContent.setSurfaceLocalOrigin(Vec3::zero());
    }
    if (!mesh->hasLocalOriginEcef && !mesh->vertices.empty()) {
        size_t vertexBegin = 0;
        size_t vertexCount = mesh->vertices.size();
        const SkirtMetadata& skirt = mesh->skirtMeta;
        if (skirt.noSkirtVerticesCount > 0 &&
            skirt.noSkirtVerticesBegin < mesh->vertices.size() &&
            skirt.noSkirtVerticesCount <=
                mesh->vertices.size() - skirt.noSkirtVerticesBegin) {
            vertexBegin = skirt.noSkirtVerticesBegin;
            vertexCount = skirt.noSkirtVerticesCount;
        }
        Vec3 localOrigin = Vec3::zero();
        for (size_t i = 0; i < vertexCount; ++i) {
            localOrigin += mesh->vertices[vertexBegin + i].positionEcef;
        }
        tile.content.renderContent.setSurfaceLocalOrigin(
            localOrigin / static_cast<double>(vertexCount));
    }

    if (!device || mesh->vertices.empty()) {
        return;
    }

    std::vector<SurfaceGpuVertex> generatedGpuVertices;
    const std::vector<SurfaceGpuVertex>* gpuVertices =
        mesh->gpuVertices.size() == mesh->vertices.size()
            ? &mesh->gpuVertices
            : nullptr;
    if (!gpuVertices) {
        generatedGpuVertices.resize(mesh->vertices.size());
        for (size_t i = 0; i < mesh->vertices.size(); ++i) {
            const auto& src = mesh->vertices[i];
            SurfaceGpuVertex& dst = generatedGpuVertices[i];
            Vec3 rel =
                src.positionEcef - tile.content.renderContent.renderLocalOrigin();
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
    std::unique_ptr<Buffer> vertexBuffer = device->createBuffer(vbDesc);

    std::unique_ptr<Buffer> indexBuffer;
    if (!mesh->indices.empty()) {
        BufferDesc ibDesc;
        ibDesc.size = mesh->indices.size() * sizeof(uint32_t);
        ibDesc.data = mesh->indices.data();
        ibDesc.usage = BufferDesc::Usage::Static;
        ibDesc.type = BufferDesc::Type::Index;
        indexBuffer = device->createBuffer(ibDesc);
    }
    tile.content.renderContent.setSurfaceGpuBuffers(
        std::move(vertexBuffer),
        std::move(indexBuffer));
}

} // namespace earth_engine
