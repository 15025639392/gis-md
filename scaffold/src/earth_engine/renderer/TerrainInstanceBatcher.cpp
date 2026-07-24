#include "TerrainInstanceBatcher.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <unordered_map>

#include "RenderDevice.h"
#include "Renderer.h"

namespace earth_engine {

namespace {

// 资格闸:该逐瓦片命令能否进实例化批(见头注释)。真实地形 + 共享位移模板
// + pageStore 全 cell 驻留 + 无 water mask/baseColor 纹理/blend。
bool isEligible(const RenderCommand& cmd) {
    if (cmd.kind != RenderCommandKind::GltfPrimitive) return false;
    if (!cmd.terrainRenderContent) return false;
    if (cmd.terrainSurfaceSource != TerrainSurfaceCommandSource::RealTerrain) {
        return false;
    }
    if (!cmd.hasTerrainDisplacementFrame) return false;   // 用共享模板
    if (cmd.vertexStride != 32) return false;
    if (cmd.gltfUniforms.pageStoreParams[0] <= 0.5f) return false;  // 页存储开
    if (!cmd.terrainPageStoreFullyResident) return false; // → 丢 mappedRaster
    if (cmd.gltfUniforms.hasWaterMask > 0.5f) return false;
    if (cmd.gltfUniforms.hasBaseColorTexture > 0.5f) return false;
    if (cmd.blend) return false;
    return true;
}

// 从 double 列主序 4x4(rel = inv(frame0)·frame_i)取第 r 行,降 float。
// world 分量 r = Σ_c rel[c][r]·local[c] = dot(rowR, vec4(local,1))。
void extractRow(const glm::dmat4& rel, int r, float out[4]) {
    out[0] = static_cast<float>(rel[0][r]);
    out[1] = static_cast<float>(rel[1][r]);
    out[2] = static_cast<float>(rel[2][r]);
    out[3] = static_cast<float>(rel[3][r]);
}

}  // namespace

Buffer* TerrainInstanceBatcher::acquireInstanceBuffer(
    int slot, const InstanceRecord* records, int count, RenderDevice* device) {
    if (slot >= static_cast<int>(instanceBufferPool_.size())) {
        instanceBufferPool_.resize(slot + 1);
    }
    const size_t needed = static_cast<size_t>(count) * sizeof(InstanceRecord);
    std::unique_ptr<Buffer>& slotBuf = instanceBufferPool_[slot];
    // 容量足够 → 原地 update(glId 稳定,VAO 缓存命中);不足 → 建到统一容量
    // (grow-only,warmup 后不再发生)。updateBuffer=orphan 语义(见 Metal 契约)。
    if (!slotBuf || slotBuf->size() < needed) {
        BufferDesc desc;
        desc.size = static_cast<size_t>(kInstanceBufferCapacity) *
                    sizeof(InstanceRecord);
        desc.data = nullptr;
        desc.usage = BufferDesc::Usage::Dynamic;
        desc.type = BufferDesc::Type::Vertex;
        slotBuf = device->createBuffer(desc);
        if (!slotBuf) return nullptr;
    }
    if (!device->updateBuffer(slotBuf.get(), 0, records, needed)) {
        return nullptr;
    }
    return slotBuf.get();
}

TerrainInstanceBatcher::Stats TerrainInstanceBatcher::assemble(
    RenderCommandList& commands, RenderDevice* device, Renderer& renderer) {
    Stats stats;
    if (!device || commands.empty()) return stats;
    // 实例化 shader 未就绪(Metal 顶点描述表留 Step 4 / 创建失败)→ 不合批,
    // 逐瓦片命令原样保留(零回归)。批命令绝不能带 null shader(→ 瓦片消失)。
    if (!renderer.terrainInstancedShader()) return stats;

    // 分组:资格命令按模板 VBO 身份聚合(P5b 后同 {schemeId,z,row} 共享同一
    // VBO,指针相等即同模板)。保留首见顺序,便于稳定输出。
    std::unordered_map<const Buffer*, std::vector<size_t>> groups;
    std::vector<const Buffer*> groupOrder;
    groups.reserve(32);
    for (size_t i = 0; i < commands.size(); ++i) {
        if (!isEligible(commands[i])) continue;
        ++stats.eligibleCommands;
        const Buffer* vb = commands[i].vertexBuffer;
        auto it = groups.find(vb);
        if (it == groups.end()) {
            groups.emplace(vb, std::vector<size_t>{i});
            groupOrder.push_back(vb);
        } else {
            it->second.push_back(i);
        }
    }

    std::vector<bool> consumed(commands.size(), false);
    std::vector<RenderCommand> batchCommands;
    int slot = 0;

    for (const Buffer* vb : groupOrder) {
        const std::vector<size_t>& members = groups[vb];
        if (members.size() < 2) continue;  // 单例不值得实例化,留逐 draw

        const RenderCommand& first = commands[members[0]];
        // 批参考帧 = 首实例的 ENU→ECEF 刚体帧(double)。
        const glm::dmat4 frame0 =
            glm::make_mat4(first.terrainDisplacementModelMatrix.data());
        const glm::dmat4 invFrame0 = glm::inverse(frame0);

        // 先打包实例流,成功建批后才标 consumed(失败/不足则全组留逐 draw,
        // 绝不丢命令)。溢出(> 容量)截断并回退整组逐 draw(不静默丢瓦片)。
        if (members.size() > static_cast<size_t>(kInstanceBufferCapacity)) {
            continue;  // 极端:单 {z,row} 组超 256 实例,保守留逐 draw
        }
        recordScratch_.clear();
        recordScratch_.reserve(members.size());
        for (size_t mi : members) {
            const RenderCommand& m = commands[mi];
            const glm::dmat4 frameI =
                glm::make_mat4(m.terrainDisplacementModelMatrix.data());
            const glm::dmat4 rel = invFrame0 * frameI;

            InstanceRecord rec;
            extractRow(rel, 0, rec.relRow0);
            extractRow(rel, 1, rec.relRow1);
            extractRow(rel, 2, rec.relRow2);
            rec.dispMorph[0] = m.gltfUniforms.heightDisplace[0];  // minH·fade
            rec.dispMorph[1] = m.gltfUniforms.heightDisplace[1];  // range·fade
            rec.dispMorph[2] = m.gltfUniforms.geomorphUpFactor[3];  // morph
            rec.dispMorph[3] = m.gltfUniforms.pageStoreParams[1];   // gridN
            rec.clipUv[0] = m.gltfUniforms.clipUv[0];
            rec.clipUv[1] = m.gltfUniforms.clipUv[1];
            rec.clipUv[2] = m.gltfUniforms.clipUv[2];
            rec.clipUv[3] = m.gltfUniforms.clipUv[3];
            rec.layers[0] = m.gltfUniforms.terrainLayers[0];  // heightLayer
            rec.layers[1] = m.gltfUniforms.terrainLayers[1];  // indirLayer
            rec.layers[2] = m.gltfUniforms.clipEnabled;
            rec.layers[3] = 0.0f;
            recordScratch_.push_back(rec);
        }
        const int packed = static_cast<int>(recordScratch_.size());

        Buffer* instBuf = acquireInstanceBuffer(
            slot, recordScratch_.data(), packed, device);
        if (!instBuf) {
            continue;  // 缓冲失败:整组留逐 draw(consumed 未标,零丢帧)
        }
        for (size_t mi : members) consumed[mi] = true;

        RenderCommand batch = renderer.makeTerrainInstancedCommand(
            first.vertexBuffer, first.indexBuffer, instBuf,
            first.indexCount, first.vertexCount, packed);
        // 批级不变式:frame0 承载落位(updater 算 mvp=viewProj·frame0);
        // 纹理(高度/页存储/间接 array,均共享)与 baseColor/pageStore enabled
        // 从首实例复制;per-instance 差异全在实例流。
        // frameId/generation 承自首实例(已被 applyPerFrameCommandState 盖成
        // 当前帧)——否则 MVP 校验判「stale frameId」abort。
        batch.frameId = first.frameId;
        batch.generation = first.generation;
        batch.terrainRenderContent = true;
        batch.terrainSurfaceSource = TerrainSurfaceCommandSource::RealTerrain;
        batch.hasTerrainDisplacementFrame = true;
        batch.terrainDisplacementModelMatrix =
            first.terrainDisplacementModelMatrix;
        batch.textures = first.textures;
        // 加载质量诊断承自首实例:资格闸要求成员同 {z,row} 模板 + 页存储全
        // cell 驻留 + 共享同一份纹理状态,故整批影像来源同档。不承的话批命令
        // 的 delta 停在 -1,直方图会把整批漏计(掠视 128 片只数到未合批的 35)。
        batch.surfaceGeometryZoom = first.surfaceGeometryZoom;
        batch.surfaceTextureZoom = first.surfaceTextureZoom;
        batch.imageryAncestorLevelDelta = first.imageryAncestorLevelDelta;
        batch.gltfUniforms.baseColor = first.gltfUniforms.baseColor;
        batch.gltfUniforms.pageStoreParams = {1.0f, 0.0f, 0.0f, 0.0f};
        batch.hasWorldSortCenter = first.hasWorldSortCenter;
        batch.worldSortCenter = first.worldSortCenter;
        batch.cullFace = first.cullFace;
        batchCommands.push_back(std::move(batch));
        ++slot;
        ++stats.batches;
        stats.batchedCommands += packed;
    }

    if (batchCommands.empty()) return stats;

    // 重建命令列表:未消费的原样保留(顺序不变),批命令追加末尾(地形恒
    // opaque,sort 按 kind 归位,批内顺序无关)。
    RenderCommandList rebuilt;
    rebuilt.reserve(commands.size() - stats.batchedCommands +
                    batchCommands.size());
    for (size_t i = 0; i < commands.size(); ++i) {
        if (!consumed[i]) rebuilt.push_back(std::move(commands[i]));
    }
    for (RenderCommand& b : batchCommands) rebuilt.push_back(std::move(b));
    commands.swap(rebuilt);
    return stats;
}

}  // namespace earth_engine
