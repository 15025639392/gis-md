#include "TerrainDisplacementTemplatePool.h"

#include <functional>
#include <vector>

#include "../content/TerrainDisplacementTemplate.h"
#include "../renderer/RenderDevice.h"
#include "GltfRenderGeometryBuilder.h"  // TerrainGpuVertex
#include "SchemeId.h"

namespace earth_engine {

uint64_t TerrainDisplacementTemplatePool::cacheKey(const TileKey& key,
                                                   int gridSize) {
    // 列无关：只用 schemeId/z/y(row)/gridSize，跨 x(列)复用同一模板。
    // schemeId 是 interned handle → 折其 std::hash 进 key（O(1)）。
    uint64_t h = static_cast<uint64_t>(std::hash<SchemeId>()(key.schemeId));
    auto mix = [&h](uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(static_cast<uint64_t>(key.z));
    mix(static_cast<uint64_t>(key.y));
    mix(static_cast<uint64_t>(gridSize));
    return h;
}

const TerrainDisplacementTemplatePool::TemplateBuffers*
TerrainDisplacementTemplatePool::acquire(const TileKey& key,
                                         const Rectangle& bounds,
                                         int gridSize) {
    if (!device_ || gridSize < 1) {
        return nullptr;
    }

    const uint64_t k = cacheKey(key, gridSize);
    auto it = cache_.find(k);
    if (it != cache_.end()) {
        return &it->second.view;
    }

    // 首次见到该 {LOD,row}：生成共享模板（列无关，用本瓦片 bounds 代表）。
    const TerrainDisplacementTemplate tmpl =
        buildTerrainDisplacementTemplate(bounds, gridSize);
    if (tmpl.vertices.empty() || tmpl.indices.empty()) {
        return nullptr;
    }

    // 打包成 32B TerrainGpuVertex（与现有地形顶点布局/shader 逐字节一致）：
    // pos=ENU 局部面点，nrm=ENU 法线(snorm16)，texcoord01=uv 两套(unorm16)，
    // heightDelta=0（Stage A 零起伏；Stage B 由高度纹理在 shader 位移）。
    std::vector<TerrainGpuVertex> packed(tmpl.vertices.size());
    for (size_t i = 0; i < tmpl.vertices.size(); ++i) {
        const TerrainDisplacementTemplateVertex& v = tmpl.vertices[i];
        packed[i].pos[0] = v.localPos[0];
        packed[i].pos[1] = v.localPos[1];
        packed[i].pos[2] = v.localPos[2];
        packed[i].nrm[0] = TerrainGpuVertex::packSnorm16(v.localNormal[0]);
        packed[i].nrm[1] = TerrainGpuVertex::packSnorm16(v.localNormal[1]);
        packed[i].nrm[2] = TerrainGpuVertex::packSnorm16(v.localNormal[2]);
        packed[i].nrmPad = 0;
        packed[i].texcoord01[0] = TerrainGpuVertex::packUnorm16(v.uv[0]);
        packed[i].texcoord01[1] = TerrainGpuVertex::packUnorm16(v.uv[1]);
        packed[i].texcoord01[2] = TerrainGpuVertex::packUnorm16(v.uv[0]);
        packed[i].texcoord01[3] = TerrainGpuVertex::packUnorm16(v.uv[1]);
        packed[i].heightDelta = 0.0f;
    }

    BufferDesc vboDesc;
    vboDesc.size = packed.size() * sizeof(TerrainGpuVertex);
    vboDesc.data = packed.data();
    vboDesc.usage = BufferDesc::Usage::Static;
    vboDesc.type = BufferDesc::Type::Vertex;

    BufferDesc iboDesc;
    iboDesc.size = tmpl.indices.size() * sizeof(uint32_t);
    iboDesc.data = tmpl.indices.data();
    iboDesc.usage = BufferDesc::Usage::Static;
    iboDesc.type = BufferDesc::Type::Index;

    Entry entry;
    entry.vertexBuffer = device_->createBuffer(vboDesc);
    entry.indexBuffer = device_->createBuffer(iboDesc);
    if (!entry.vertexBuffer || !entry.indexBuffer) {
        return nullptr;
    }
    entry.view.vertexBuffer = entry.vertexBuffer.get();
    entry.view.indexBuffer = entry.indexBuffer.get();
    entry.view.indexCount = static_cast<int>(tmpl.indices.size());
    entry.view.vertexCount = static_cast<int>(packed.size());
    totalVertexBytes_ += vboDesc.size;

    auto inserted = cache_.emplace(k, std::move(entry));
    return &inserted.first->second.view;
}

}  // namespace earth_engine
