#include "TerrainDisplacementTemplatePool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "../content/TerrainDisplacementTemplate.h"
#include "../providers/TerrainProvider.h"  // DecodedHeightmap
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
    // 索引按档共享,已建过就不再生成(dense 档每次 393k 次 push_back 的纯浪费)。
    SharedIndexBuffer& shared = sharedIndexBuffers_[gridSize];
    const bool needIndices = !shared.buffer;
    const TerrainDisplacementTemplate tmpl =
        buildTerrainDisplacementTemplate(bounds, gridSize, needIndices);
    if (tmpl.vertices.empty() || (needIndices && tmpl.indices.empty())) {
        if (needIndices) sharedIndexBuffers_.erase(gridSize);
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
        // heightDelta：栅格顶点 0（无 geomorph）；裙顶点 -1 哨兵 → 位移 shader
        // 认出后对其跳过位移（停在椭球面 h=0），使裙墙自适应撑到位移后边缘之下。
        packed[i].heightDelta =
            (i >= tmpl.skirtVerticesBegin) ? -1.0f : 0.0f;
    }

    BufferDesc vboDesc;
    vboDesc.size = packed.size() * sizeof(TerrainGpuVertex);
    vboDesc.data = packed.data();
    vboDesc.usage = BufferDesc::Usage::Static;
    vboDesc.type = BufferDesc::Type::Vertex;

    // 索引**只取决于 gridSize**:规则网格三角化 + 裙边缠绕都是纯拓扑,与瓦片
    // bounds 无关 → 同档所有 {z,row} 模板逐字节相同。原来每个模板各建各的,
    // dense 档每份 393k×4B = 1.57MB,白白重复分配 + 上传。改为按档共享一份。
    if (needIndices) {
        BufferDesc iboDesc;
        iboDesc.size = tmpl.indices.size() * sizeof(uint32_t);
        iboDesc.data = tmpl.indices.data();
        iboDesc.usage = BufferDesc::Usage::Static;
        iboDesc.type = BufferDesc::Type::Index;
        shared.buffer = device_->createBuffer(iboDesc);
        shared.indexCount = static_cast<int>(tmpl.indices.size());
        if (!shared.buffer) {
            sharedIndexBuffers_.erase(gridSize);
            return nullptr;
        }
    }

    Entry entry;
    entry.vertexBuffer = device_->createBuffer(vboDesc);
    if (!entry.vertexBuffer) {
        return nullptr;
    }
    entry.view.vertexBuffer = entry.vertexBuffer.get();
    entry.view.indexBuffer = shared.buffer.get();
    entry.view.indexCount = shared.indexCount;
    entry.view.vertexCount = static_cast<int>(packed.size());
    totalVertexBytes_ += vboDesc.size;

    auto inserted = cache_.emplace(k, std::move(entry));
    return &inserted.first->second.view;
}

uint64_t TerrainDisplacementTemplatePool::heightCacheKey(const TileKey& key) {
    // 逐瓦片(含 x 列):高度逐瓦片不同,不跨列共享。
    uint64_t h = static_cast<uint64_t>(std::hash<SchemeId>()(key.schemeId));
    auto mix = [&h](uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(static_cast<uint64_t>(key.z));
    mix(static_cast<uint64_t>(key.x));
    mix(static_cast<uint64_t>(key.y));
    return h;
}

const TerrainDisplacementTemplatePool::HeightArray*
TerrainDisplacementTemplatePool::findHeightArray(int gridSize) const {
    auto it = heightArrays_.find(gridSize);
    return it == heightArrays_.end() ? nullptr : &it->second;
}

TerrainDisplacementTemplatePool::HeightArray*
TerrainDisplacementTemplatePool::ensureHeightArray(int gridSize) {
    auto it = heightArrays_.find(gridSize);
    if (it != heightArrays_.end()) {
        return it->second.texture ? &it->second : nullptr;
    }
    const int layers = layersForGridSize(gridSize);
    TextureDesc desc;
    desc.width = gridSize + 1;
    desc.height = gridSize + 1;
    desc.arrayLayers = layers;
    desc.format = TextureDesc::Format::RGBA8;
    desc.mipmap = false;
    desc.minFilter = TextureDesc::Filter::Nearest;
    desc.magFilter = TextureDesc::Filter::Nearest;
    desc.wrapS = TextureDesc::Wrap::Clamp;
    desc.wrapT = TextureDesc::Wrap::Clamp;
    std::unique_ptr<Texture> tex = device_->createTexture(desc);
    if (!tex) {
        return nullptr;
    }
    HeightArray& a = heightArrays_[gridSize];
    a.texture = std::move(tex);
    a.gridSize = gridSize;
    a.layerPool.configure(layers, 1);
    a.layerEpochs.assign(static_cast<size_t>(layers), 0u);
    return &a;
}

const TerrainDisplacementTemplatePool::HeightTexture*
TerrainDisplacementTemplatePool::acquireHeightTexture(
    const TileKey& key, const DecodedHeightmap& heightmap,
    const Rectangle& bounds, int gridSize, uint64_t frameId) {
    if (!device_ || gridSize < 1 || !heightmap.valid()) {
        return nullptr;
    }
    HeightArray* arr = ensureHeightArray(gridSize);
    if (!arr) {
        return nullptr;
    }
    const uint64_t k = heightCacheKey(key);
    auto it = arr->index.find(k);
    if (it != arr->index.end()) {
        arr->layerPool.touch(k, frameId);
        return &it->second;
    }

    // 走到这里 = 该瓦片在本档未驻留,必须现烘 + 现传。dense 档单片最坏 23.6ms
    // (release 实测),故按帧限流;超额者本帧回落 coarse,下一帧再升。
    // 只拦"新建",缓存命中已在上面早退,不受限流影响。
    if (gridSize >= kTerrainDenseGridSize && !tryConsumeDenseBudget(frameId)) {
        return nullptr;
    }

    // 认领一层(LRU;当帧被 touch 的层不淘汰,全满 → 返回 -1 = 本帧放弃,
    // draw 侧回落 P5b 兜底重试)。被淘汰瓦片的旧视图删除 + 层 epoch 自增,
    // 使仍引用旧层的常驻命令在 heightLayerCurrent() 校验时失效自愈。
    uint64_t evicted = 0;
    const int layer = arr->layerPool.acquire(k, frameId, &evicted);
    if (layer < 0) {
        return nullptr;
    }
    if (evicted != 0) {
        arr->index.erase(evicted);
        ++arr->layerEpochs[static_cast<size_t>(layer)];
    }

    // gridN+1 方栅格:texel(i,j) = 顶点(i,j)高度(shader 按栅格下标 texelFetch)。
    // 高度归一化 [0,1] 打进 RG 两通道(16bit):R=高字节,G=低字节。B/A=0。
    // u 用 i/gridSize(mercator-x 线性于经度,精确);v 用 j/gridSize(线性纬度近似
    // mercator-v,细瓦片亚 texel,粗瓦片略偏——Stage B 先验证,必要时补 mercator-v)。
    const int n = gridSize + 1;
    const float minH = heightmap.minHeight;
    const float range = std::max(1e-3f, heightmap.maxHeight - heightmap.minHeight);
    // 先把节点高度采成 scratch:法线要在其上做中心差分,逐节点现采 4 个邻居会
    // 把双线性采样次数翻 4 倍(邻居彼此复用)。
    std::vector<float> nodeH(static_cast<size_t>(n) * n);
    for (int j = 0; j < n; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(gridSize);
        for (int i = 0; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(gridSize);
            nodeH[static_cast<size_t>(j) * n + i] = heightmap.sampleBilinear(u, v);
        }
    }
    // ---- B/A 通道 = 切空间法线(解"竖条"= 逐三角面平法线)----
    // R/G 是 16bit 高度,B/A 空着 → 法线搭车进同一张纹理:不需要新 array、新
    // 纹理槽、第二套 LRU/epoch/staleness,零额外显存。分辨率随高度纹理走,即
    // dense 瓦片 257²(= 目标 256 边长),远处 coarse 65²(那里本就看不出)。
    // 只存 nx,ny —— nz>0 恒成立(高度场是 u,v 的函数,无悬垂),shader 重建。
    constexpr double kEarthRadiusMeters = 6378137.0;
    const double centerLat = 0.5 * (bounds.north() + bounds.south());
    const double widthMeters = std::max(
        1.0, bounds.width() * std::cos(centerLat) * kEarthRadiusMeters);
    const double heightSpanMeters =
        std::max(1.0, bounds.height() * kEarthRadiusMeters);
    const double stepU = widthMeters / gridSize;
    const double stepV = heightSpanMeters / gridSize;

    std::vector<uint8_t> bytes(static_cast<size_t>(n) * n * 4, 0);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const size_t idx = (static_cast<size_t>(j) * n + i) * 4;
            const float hMeters = nodeH[static_cast<size_t>(j) * n + i];
            const float t = std::clamp((hMeters - minH) / range, 0.0f, 1.0f);
            const uint32_t v16 =
                static_cast<uint32_t>(std::lround(t * 65535.0f));
            bytes[idx + 0] = static_cast<uint8_t>((v16 >> 8) & 0xFF);
            bytes[idx + 1] = static_cast<uint8_t>(v16 & 0xFF);

            // 内部中心差分;边界退化单边(分母同步用真实跨度,边界法线不被放大)。
            const int i0 = std::max(0, i - 1), i1 = std::min(n - 1, i + 1);
            const int j0 = std::max(0, j - 1), j1 = std::min(n - 1, j + 1);
            const auto at = [&](int gi, int gj) {
                return static_cast<double>(nodeH[static_cast<size_t>(gj) * n + gi]);
            };
            const double gradU =
                (at(i1, j) - at(i0, j)) / (static_cast<double>(i1 - i0) * stepU);
            const double gradV =
                (at(i, j1) - at(i, j0)) / (static_cast<double>(j1 - j0) * stepV);
            const double inv =
                1.0 / std::sqrt(gradU * gradU + gradV * gradV + 1.0);
            const auto enc = [](double c) {
                return static_cast<uint8_t>(
                    std::lround(std::clamp(c * 0.5 + 0.5, 0.0, 1.0) * 255.0));
            };
            bytes[idx + 2] = enc(-gradU * inv);
            bytes[idx + 3] = enc(-gradV * inv);
        }
    }
    if (!device_->updateTextureRegion(arr->texture.get(), 0, 0, n, n,
                                      bytes.data(),
                                      static_cast<size_t>(n) * 4, layer)) {
        arr->layerPool.release(k);
        return nullptr;
    }

    HeightTexture view;
    view.texture = arr->texture.get();
    view.minHeight = minH;
    view.heightRange = range;
    view.gridSize = gridSize;
    view.layer = layer;
    view.epoch = arr->layerEpochs[static_cast<size_t>(layer)];
    auto inserted = arr->index.emplace(k, view);
    return &inserted.first->second;
}

void TerrainDisplacementTemplatePool::touchHeightTexture(const TileKey& key,
                                                         int gridSize,
                                                         uint64_t frameId) {
    auto it = heightArrays_.find(gridSize);
    if (it == heightArrays_.end()) return;
    it->second.layerPool.touch(heightCacheKey(key), frameId);
}

}  // namespace earth_engine
