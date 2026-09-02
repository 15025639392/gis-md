#include "TerrainInstanceBatcher.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <unordered_map>

#include "../debug/Contracts.h"
#include "RenderDevice.h"
#include "Renderer.h"

namespace earth_engine {

namespace {

// 资格闸:该逐瓦片命令能否进实例化批(见头注释)。真实地形 + 共享位移模板
// + pageStore 全 cell 驻留 + 无 water mask/baseColor 纹理/blend。
using RejectReason = TerrainInstanceBatcher::RejectReason;

// 返回**首个**不满足的资格条件;全通过返回 Count(= 合批资格)。
// 改成返回原因而非 bool,是为了让"批为什么不成形"可以一次运行全部投票,而不是
// 逐条注释掉条件试 —— 后者正是退化成 A/B 的典型入口。
RejectReason rejectReasonFor(const RenderCommand& cmd) {
    if (cmd.kind != RenderCommandKind::GltfPrimitive) {
        return RejectReason::NotGltfPrimitive;
    }
    if (!cmd.terrainRenderContent) return RejectReason::NotTerrainContent;
    if (cmd.terrainSurfaceSource != TerrainSurfaceCommandSource::RealTerrain) {
        return RejectReason::NotRealTerrain;
    }
    if (!cmd.hasTerrainDisplacementFrame) {               // 用共享模板
        return RejectReason::NoDisplacementFrame;
    }
    if (cmd.vertexStride != 32) return RejectReason::WrongVertexStride;
    if (cmd.gltfUniforms.pageStoreParams[0] <= 0.5f) {    // 页存储开
        return RejectReason::PageStoreOff;
    }
    if (!cmd.terrainPageStoreFullyResident) {             // → 丢 directComposite
        return RejectReason::NotFullyResident;
    }
    if (cmd.gltfUniforms.hasWaterMask > 0.5f) return RejectReason::HasWaterMask;
    if (cmd.gltfUniforms.hasBaseColorTexture > 0.5f) {
        return RejectReason::HasBaseColorTexture;
    }
    if (cmd.blend) return RejectReason::Blended;
    return RejectReason::Count;  // 全通过
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

const char* TerrainInstanceBatcher::rejectReasonName(RejectReason reason) {
    switch (reason) {
        case RejectReason::NotGltfPrimitive:    return "notGltf";
        case RejectReason::NotTerrainContent:   return "notTerrain";
        case RejectReason::NotRealTerrain:      return "notRealTerrain";
        case RejectReason::NoDisplacementFrame: return "noDispFrame";
        case RejectReason::WrongVertexStride:   return "stride!=32";
        case RejectReason::PageStoreOff:        return "pageStoreOff";
        case RejectReason::NotFullyResident:    return "notFullyResident";
        case RejectReason::HasWaterMask:        return "waterMask";
        case RejectReason::HasBaseColorTexture: return "baseColorTex";
        case RejectReason::Blended:             return "blend";
        case RejectReason::Count:               break;
    }
    return "?";
}

bool TerrainInstanceBatcher::batchTemplateGridIsUniform(
    const InstanceRecord* records, size_t count) {
    if (records == nullptr || count < 2) return true;  // 退化输入不判
    const float grid0 = records[0].layers[3];
    for (size_t i = 1; i < count; ++i) {
        if (records[i].layers[3] != grid0) return false;
    }
    return true;
}

float TerrainInstanceBatcher::firstMismatchedTemplateGrid(
    const InstanceRecord* records, size_t count) {
    if (records == nullptr || count == 0) return -1.0f;
    const float grid0 = records[0].layers[3];
    for (size_t i = 1; i < count; ++i) {
        if (records[i].layers[3] != grid0) return records[i].layers[3];
    }
    return grid0;
}

TerrainInstanceBatcher::Stats TerrainInstanceBatcher::assemble(
    RenderCommandList& commands, RenderDevice* device, Renderer& renderer) {
    Stats stats;
    stats.totalCommands = static_cast<int>(commands.size());
    if (!device || commands.empty()) return stats;
    // 实例化 shader 未就绪(Metal 顶点描述表留 Step 4 / 创建失败)→ 不合批,
    // 逐瓦片命令原样保留(零回归)。批命令绝不能带 null shader(→ 瓦片消失)。
    stats.shaderReady = renderer.terrainInstancedShader() != nullptr;
    if (!stats.shaderReady) return stats;

    // 分组:资格命令按模板 VBO 身份聚合(P5b 后同 {schemeId,z,row} 共享同一
    // VBO,指针相等即同模板)。保留首见顺序,便于稳定输出。
    std::unordered_map<const Buffer*, std::vector<size_t>> groups;
    std::vector<const Buffer*> groupOrder;
    groups.reserve(32);
    for (size_t i = 0; i < commands.size(); ++i) {
        const RejectReason reason = rejectReasonFor(commands[i]);
        if (reason != RejectReason::Count) {
            ++stats.rejects[static_cast<size_t>(reason)];
            continue;
        }
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
        ++stats.groups;
        if (members.size() < 2) {
            ++stats.singletonGroups;  // 单例不值得实例化,留逐 draw
            continue;
        }

        const RenderCommand& first = commands[members[0]];
        // 批参考帧 = 首实例的 ENU→ECEF 刚体帧(double)。
        const glm::dmat4 frame0 =
            glm::make_mat4(first.terrainDisplacementModelMatrix.data());
        const glm::dmat4 invFrame0 = glm::inverse(frame0);

        // 先打包实例流,成功建批后才标 consumed(失败/不足则全组留逐 draw,
        // 绝不丢命令)。溢出(> 容量)截断并回退整组逐 draw(不静默丢瓦片)。
        if (members.size() > static_cast<size_t>(kInstanceBufferCapacity)) {
            ++stats.oversizeGroups;  // 极端:单 {z,row} 组超 256 实例,留逐 draw
            continue;
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
            // cell 网格描述符(cellsX/cellsY/texCoordSet 打包)。逐实例只剩这一个
            // 槽,故打包;解包在实例化 shader 里,见 packPageCellDescriptor。
            rec.dispMorph[3] = packPageCellDescriptor(
                static_cast<int>(m.gltfUniforms.pageStoreParams[1] + 0.5f),
                static_cast<int>(m.gltfUniforms.pageStoreParams[2] + 0.5f),
                static_cast<int>(m.gltfUniforms.pageStoreParams[3]) % 8);
            rec.clipUv[0] = m.gltfUniforms.clipUv[0];
            rec.clipUv[1] = m.gltfUniforms.clipUv[1];
            rec.clipUv[2] = m.gltfUniforms.clipUv[2];
            rec.clipUv[3] = m.gltfUniforms.clipUv[3];
            // params.w 打包了 texSet + 相位,实例侧拆开存(见 pageAux 注释)。
            {
                const float packed = m.gltfUniforms.pageStoreParams[3];
                rec.pageAux[0] = std::fmod(std::floor(packed / 8.0f), 64.0f);
                rec.pageAux[1] = std::floor(packed / 512.0f);
                // [瓦界对齐] zw = 几何仿射 dV(见 pageUv 注释)。
                rec.pageAux[2] = m.terrainPageGeomAffine[4];
                rec.pageAux[3] = m.terrainPageGeomAffine[5];
            }
            // [瓦界对齐] 实例化片元的 psUv 是共享模板的**几何** UV(边到边 0..1),
            // 不是 details 逐顶点 texcoord —— 不能复用 gltfUniforms.pageStoreUv 的
            // origin/span(那是按 details-UV 标定的;GCJ 下差出瓦包围矩形翘曲量,
            // 瓦界错缝 ~30m)。改用 applyToTerrainCommand 给的几何仿射:
            // g = c0 + u·dU + v·dV,相邻瓦共享角点 → 瓦界按构造连续。
            rec.pageUv[0] = m.terrainPageGeomAffine[0];  // c0.x(NW 角)
            rec.pageUv[1] = m.terrainPageGeomAffine[1];  // c0.y
            rec.pageUv[2] = m.terrainPageGeomAffine[2];  // dU.x
            rec.pageUv[3] = m.terrainPageGeomAffine[3];  // dU.y
            rec.layers[0] = m.gltfUniforms.terrainLayers[0];  // heightLayer
            rec.layers[1] = m.gltfUniforms.terrainLayers[1];  // indirLayer
            // clipMode(0/1/2)与边吸附打包共存:z = clipMode + 4·snapPacked
            // (snapPacked ≤ 4095 → 组合 ≤ 16382,float 精确)。shader 端
            // mod(z,4)=clipMode、floor(z/4)=snapPacked。实例流零增长。
            rec.layers[2] = m.gltfUniforms.clipEnabled +
                            4.0f * m.gltfUniforms.terrainLayers[2];
            // 位移模板栅格边长(自适应密度后不再恒 64)。分组按模板 VBO 指针,
            // 而模板按 {schemeId,z,row,gridSize} 缓存 → 同批必然同档,但仍逐实例
            // 携带以免"同批同档"这个隐式前提日后被分组规则改动悄悄破坏。
            rec.layers[3] = m.gltfUniforms.heightDisplace[3];
            recordScratch_.push_back(rec);
        }
        // 上面那句注释以前就是全部的保障。现在把它变成可数信号:分组用的是
        // VBO 指针,判据查的是每实例携带的栅格边长 —— 两条独立数据流。分组规则
        // 改动导致批内混档时当场记一笔,而不是等几何错位被肉眼发现。
        //
        // 每批一次(不是每实例一次):判据本身是集合谓词,且求值计数落在批粒度上
        // 更有意义 —— coverage 直接读作"这一帧合了多少批"。
        GE_CONTRACT(contracts::Id::BatchTemplateGridParity,
                    batchTemplateGridIsUniform(recordScratch_.data(),
                                               recordScratch_.size()),
                    "batchSize=%zu firstGridN=%.1f oddGridN=%.1f",
                    recordScratch_.size(),
                    recordScratch_.empty()
                        ? -1.0
                        : static_cast<double>(recordScratch_.front().layers[3]),
                    static_cast<double>(
                        firstMismatchedTemplateGrid(recordScratch_.data(),
                                                    recordScratch_.size())));
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
        // 底图影像就绪态同样承自首实例,理由与上面那条完全一样(资格闸保证整批
        // 共享同一份纹理状态)。**漏了这两个字段会让整帧永久扣住**:presentation
        // hold 判的是"有没有一条带底图影像的地形命令",批命令带着 0/0 进去就是
        // 一条永远不合格的地形命令,而 hold 那条闸没有活性上限。
        batch.gltfRasterOverlayTextureCount = first.gltfRasterOverlayTextureCount;
        batch.surfaceBaseRasterState = first.surfaceBaseRasterState;
        batch.surfaceBaseUsesDirectRaster = first.surfaceBaseUsesDirectRaster;
        batch.gltfUniforms.baseColor = first.gltfUniforms.baseColor;
        // 批级 params 只留 enabled;cell 网格逐实例给(见 rec.dispMorph[3])。
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
