#include "TerrainDisplacementTemplatePool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "../content/TerrainDisplacementTemplate.h"
#include "../providers/TerrainProvider.h"  // DecodedHeightmap
#include "../renderer/RenderCommand.h"
#include "../renderer/RenderDevice.h"
#include "../renderer/TerrainHeightBakeShader.h"  // B:GPU 烘焙 shader
#include "GltfRenderGeometryBuilder.h"  // TerrainGpuVertex
#include "SchemeId.h"

namespace earth_engine {

namespace {
// 经度跨度(反经线安全):east<west 说明跨了反经线,补一圈。
double boundsLongitudeSpan(const Rectangle& b) {
    double span = b.east() - b.west();
    if (span < 0.0) span += 2.0 * 3.14159265358979323846;
    return std::abs(span);
}
}  // namespace

uint64_t TerrainDisplacementTemplatePool::cacheKey(const TileKey& key,
                                                   int gridSize,
                                                   double latSpan,
                                                   double lonSpan) {
    // 列无关：只用 schemeId/z/y(row)/gridSize，跨 x(列)复用同一模板。
    // schemeId 是 interned handle → 折其 std::hash 进 key（O(1)）。
    uint64_t h = static_cast<uint64_t>(std::hash<SchemeId>()(key.schemeId));
    auto mix = [&h](uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(static_cast<uint64_t>(key.z));
    mix(static_cast<uint64_t>(key.y));
    mix(static_cast<uint64_t>(gridSize));
    // 跨度量化到 1e-9 rad(约 6mm 地面)后并入:同一 {z,row} 下跨度相同的瓦片
    // 仍然共享(量化吸收 f64 末位噪声),跨度不同的则各建各的。
    auto quantize = [](double v) {
        return static_cast<uint64_t>(std::llround(std::abs(v) * 1e9));
    };
    mix(quantize(latSpan));
    mix(quantize(lonSpan));
    return h;
}

const TerrainDisplacementTemplatePool::TemplateBuffers*
TerrainDisplacementTemplatePool::acquire(const TileKey& key,
                                         const Rectangle& bounds,
                                         int gridSize) {
    if (!device_ || gridSize < 1) {
        return nullptr;
    }

    const double wantLat = std::abs(bounds.north() - bounds.south());
    const double wantLon = boundsLongitudeSpan(bounds);
    const uint64_t k = cacheKey(key, gridSize, wantLat, wantLon);
    auto it = cache_.find(k);
    if (it != cache_.end()) {
        // 直接量症状:共享的前提是「同 {z,row} 跨度相同」。模板由第一个来要的
        // 瓦片决定并永久缓存,前提一旦被破坏,整行都用错尺寸几何且不自愈 ——
        // 屏幕上就是「瓦片画大/画小,四周露背景」。不猜成因,当场比。
        const Entry& e = it->second;
        const double latRatio =
            e.builtLatSpan > 0.0 ? wantLat / e.builtLatSpan : 1.0;
        const double lonRatio =
            e.builtLonSpan > 0.0 ? wantLon / e.builtLonSpan : 1.0;
        if (std::abs(latRatio - 1.0) > 1e-6 ||
            std::abs(lonRatio - 1.0) > 1e-6) {
            ++heightFrameStats_.templateSpanMismatch;
            if (heightFrameStats_.mismatchZ < 0) {
                heightFrameStats_.mismatchZ = key.z;
                heightFrameStats_.mismatchX = key.x;
                heightFrameStats_.mismatchY = key.y;
                heightFrameStats_.mismatchLatRatio = latRatio;
                heightFrameStats_.mismatchLonRatio = lonRatio;
            }
        }
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
    entry.builtLatSpan = std::abs(bounds.north() - bounds.south());
    entry.builtLonSpan = boundsLongitudeSpan(bounds);

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
    // 末尾多 kEdgeLutRows 行给边吸附的邻居高度差表(①-1),见头文件说明。
    desc.height = heightLayerRows(gridSize);
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

std::vector<uint8_t> bakeTerrainHeightNormalTexels(
    const DecodedHeightmap& heightmap, const Rectangle& bounds, int gridSize,
    float minHeight, float heightRange) {
    // gridN+1 方栅格:texel(i,j) = 顶点(i,j)高度(shader 按栅格下标 texelFetch)。
    // 高度归一化 [0,1] 打进 RG 两通道(16bit):R=高字节,G=低字节。
    const int n = gridSize + 1;
    const float range = std::max(1e-3f, heightRange);
    // 先把节点高度采成 scratch:法线要在其上做中心差分,逐节点现采 4 个邻居会
    // 把双线性采样次数翻 4 倍(邻居彼此复用)。
    //
    // scratch 是 (n+2)² —— 四周各多一圈**瓦片外**样本(u = -reach 与 1+reach),
    // 取自重叠环像素 = 真实邻瓦数据。没有这圈的话边界节点只能退化单边差分,
    // 而相邻两瓦对同一条边界各自向内取单边(一个前差一个后差),差值 = 网格步长
    // × 二阶导 —— 真实粗糙度下实测法线夹角达 25°(z8/grid64/9.75km 波长
    // 300m 起伏),即每条瓦片边一道明暗带。见 test_terrain_edge_normal_seam。
    const double reach = static_cast<double>(heightmap.overscanReach());
    const int ns = n + 2;
    // scratch 下标 k ↔ 归一化坐标:k=0 → -reach,k=1..n → (k-1)/gridSize,
    // k=n+1 → 1+reach。无重叠环的源(reach=0)首尾与相邻节点重合,下面的
    // 不等臂公式自动退化回单边差分(与本次改动前逐位一致)。
    std::vector<double> axis(static_cast<size_t>(ns));
    axis[0] = -reach;
    for (int k = 1; k <= n; ++k) {
        axis[static_cast<size_t>(k)] =
            static_cast<double>(k - 1) / static_cast<double>(gridSize);
    }
    axis[static_cast<size_t>(ns - 1)] = 1.0 + reach;

    std::vector<float> nodeH(static_cast<size_t>(ns) * ns);
    for (int j = 0; j < ns; ++j) {
        const float v = static_cast<float>(axis[static_cast<size_t>(j)]);
        for (int i = 0; i < ns; ++i) {
            const float u = static_cast<float>(axis[static_cast<size_t>(i)]);
            const float sampled = heightmap.sampleBilinearUnclamped(u, v);
            // 四角全 nodata 时 sampleBilinear 原样回传哨兵(保留"此处无数据"
            // 信号);烘焙侧落 0(海平面,Mapbox 缺数据语义),不能让哨兵进高度
            // 量化与法线差分。
            nodeH[static_cast<size_t>(j) * ns + i] =
                heightmap.isNoData(sampled) ? 0.0f : sampled;
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
    // 三点不等臂一阶导:臂长相等时逐位退化为原中心差分 (f₊−f₋)/2d,
    // 故内部节点结果不变;只有边界节点(一臂 = 重叠环余量 reach)走新分支。
    // 该公式对二次函数精确 → 相邻两瓦在共享边界上虽用镜像臂长,仍收敛到同一
    // 导数(残差 O(d_l·d_r·f'''),实测降到量化噪声底)。
    const auto derivative3pt = [](double fL, double fC, double fR, double dL,
                                  double dR) -> double {
        if (dL <= 0.0) return dR > 0.0 ? (fR - fC) / dR : 0.0;
        if (dR <= 0.0) return (fC - fL) / dL;
        return (-dR * dR * fL + (dR * dR - dL * dL) * fC + dL * dL * fR) /
               (dL * dR * (dL + dR));
    };

    std::vector<uint8_t> bytes(static_cast<size_t>(n) * n * 4, 0);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const size_t idx = (static_cast<size_t>(j) * n + i) * 4;
            // 节点 (i,j) 在 scratch 里是 (i+1, j+1)。
            const int si = i + 1, sj = j + 1;
            const auto at = [&](int gi, int gj) {
                return static_cast<double>(
                    nodeH[static_cast<size_t>(gj) * ns + gi]);
            };
            const float hMeters =
                static_cast<float>(at(si, sj));
            const float t = std::clamp((hMeters - minHeight) / range, 0.0f, 1.0f);
            const uint32_t v16 =
                static_cast<uint32_t>(std::lround(t * 65535.0f));
            bytes[idx + 0] = static_cast<uint8_t>((v16 >> 8) & 0xFF);
            bytes[idx + 1] = static_cast<uint8_t>(v16 & 0xFF);

            // 臂长按 scratch 轴的真实间距换算成米(边界臂 = reach × 瓦片跨度)。
            const double dLu =
                (axis[static_cast<size_t>(si)] -
                 axis[static_cast<size_t>(si - 1)]) * widthMeters;
            const double dRu =
                (axis[static_cast<size_t>(si + 1)] -
                 axis[static_cast<size_t>(si)]) * widthMeters;
            const double dLv =
                (axis[static_cast<size_t>(sj)] -
                 axis[static_cast<size_t>(sj - 1)]) * heightSpanMeters;
            const double dRv =
                (axis[static_cast<size_t>(sj + 1)] -
                 axis[static_cast<size_t>(sj)]) * heightSpanMeters;
            const double gradU = derivative3pt(at(si - 1, sj), at(si, sj),
                                               at(si + 1, sj), dLu, dRu);
            const double gradV = derivative3pt(at(si, sj - 1), at(si, sj),
                                               at(si, sj + 1), dLv, dRv);
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
    return bytes;
}

const TerrainDisplacementTemplatePool::HeightTexture*
TerrainDisplacementTemplatePool::acquireHeightTexture(
    const TileKey& key, const DecodedHeightmap& heightmap,
    const Rectangle& bounds, int gridSize, uint64_t frameId) {
    beginHeightStatsFrame(frameId);
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
        ++heightFrameStats_.denseBudgetRejected;
        return nullptr;
    }

    // 认领一层(LRU;当帧被 touch 的层不淘汰,全满 → 返回 -1 = 本帧放弃,
    // draw 侧回落 P5b 兜底重试)。被淘汰瓦片的旧视图删除 + 层 epoch 自增,
    // 使仍引用旧层的常驻命令在 heightLayerCurrent() 校验时失效自愈。
    uint64_t evicted = 0;
    const int layer = arr->layerPool.acquire(k, frameId, &evicted);
    if (layer < 0) {
        ++heightFrameStats_.layerFull;
        return nullptr;
    }
    if (evicted != 0) {
        ++heightFrameStats_.evicted;
        arr->index.erase(evicted);
        ++arr->layerEpochs[static_cast<size_t>(layer)];
    }

    // u 用 i/gridSize(mercator-x 线性于经度,精确);v 用 j/gridSize(线性纬度近似
    // mercator-v,细瓦片亚 texel,粗瓦片略偏——Stage B 先验证,必要时补 mercator-v)。
    const int n = gridSize + 1;
    const float minH = heightmap.minHeight;
    const float range = std::max(1e-3f, heightmap.maxHeight - heightmap.minHeight);
    // 后端守卫:bake shader 目前只有 GLSL(kTerrainBakeFragGLSL),仅 OpenGLES 可用。
    // Metal 需 MSL(未写)、Vulkan 需 SPIR-V —— 那两个后端强行走 GPU 路径会因
    // createShader 失败使高度层永不烘 → 地形变平。故非 GLES 一律回退 CPU 烘焙。
    const bool gpuBakeUsable =
        gpuBakeEnabled_ &&
        device_->backendType() == RenderDevice::Backend::OpenGLES;
    if (gpuBakeUsable) {
        // B:登记 GPU bake 请求(源按值打包自持),主区内容由 flushHeightBakes 在
        // build 之后、depth prepass 之前以片元 pass 填。edge-LUT 行仍在下面 CPU 初始化。
        PendingHeightBake req;
        req.targetTexture = arr->texture.get();
        req.layer = layer;
        req.gridSize = gridSize;
        req.srcSize = heightmap.tileSize;
        req.srcInset = heightmap.borderInset;
        req.quantBase = static_cast<float>(heightmap.quantBase);
        req.quantStep = DecodedHeightmap::kQuantStep;
        req.minH = minH;
        req.range = range;
        const double centerLat = 0.5 * (bounds.north() + bounds.south());
        req.widthM = static_cast<float>(std::max(
            1.0, bounds.width() * std::cos(centerLat) * 6378137.0));
        req.heightM =
            static_cast<float>(std::max(1.0, bounds.height() * 6378137.0));
        req.reach = heightmap.overscanReach();
        // quantizedHeights(uint16 码)→ RGBA8:R=码高字节 G=码低字节,BA 空。
        const size_t px = static_cast<size_t>(req.srcSize) * req.srcSize;
        req.srcPacked.assign(px * 4, 0);
        for (size_t p = 0; p < px; ++p) {
            const uint16_t code = heightmap.quantizedHeights[p];
            req.srcPacked[p * 4 + 0] = static_cast<uint8_t>(code >> 8);
            req.srcPacked[p * 4 + 1] = static_cast<uint8_t>(code & 0xFF);
        }
        pendingBakes_.push_back(std::move(req));
    } else {
        const std::vector<uint8_t> bytes =
            bakeTerrainHeightNormalTexels(heightmap, bounds, gridSize, minH, range);
        if (!device_->updateTextureRegion(arr->texture.get(), 0, 0, n, n,
                                          bytes.data(),
                                          static_cast<size_t>(n) * 4, layer)) {
            arr->layerPool.release(k);
            return nullptr;
        }
    }
    // ①-1:把本层的边 LUT 行初始化为「差值 0」。层是 LRU 复用的,不初始化就会
    // 读到上一个租户留下的差值;而 0 恰好等于改前的自纹理吸附,是安全默认。
    // ⚠️ 全零字节**不是**差值 0 —— 差值走 ±R 的归一化,0 落在量程中点。
    {
        std::vector<uint8_t> lut(static_cast<size_t>(n) * kEdgeLutRows * 4, 0);
        const uint16_t q = encodeEdgeLutDelta(0.0f);
        for (size_t i = 0; i < lut.size(); i += 4) {
            lut[i] = static_cast<uint8_t>(q >> 8);
            lut[i + 1] = static_cast<uint8_t>(q & 0xFF);
        }
        device_->updateTextureRegion(arr->texture.get(), 0, n, n, kEdgeLutRows,
                                     lut.data(), static_cast<size_t>(n) * 4,
                                     layer);
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

bool TerrainDisplacementTemplatePool::ensureBakeResources() {
    if (!device_) return false;
    if (bakeShader_ && bakeQuad_) return true;
    ShaderDesc sd;
    sd.vertexSource = kTerrainBakeVertGLSL();
    sd.fragmentSource = kTerrainBakeFragGLSL();
    bakeShader_ = device_->createShader(sd);
    const float quad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    BufferDesc bd;
    bd.size = sizeof(quad);
    bd.data = quad;
    bd.usage = BufferDesc::Usage::Static;
    bd.type = BufferDesc::Type::Vertex;
    bakeQuad_ = device_->createBuffer(bd);
    return bakeShader_ && bakeQuad_;
}

void TerrainDisplacementTemplatePool::flushHeightBakes() {
    if (pendingBakes_.empty()) return;
    if (!ensureBakeResources()) {
        pendingBakes_.clear();  // 建不出(如 Metal 缺 MSL)→ 丢弃,层留空由 P5b 兜底
        return;
    }
    for (auto& req : pendingBakes_) {
        const int n = req.gridSize + 1;
        // 按 n 建 bake FBO:viewport=fbo 尺寸(RenderDeviceGLES beginPass),取 n×n
        // 恰好只覆盖主区、不碰底部 edge-LUT 行。externalColorTarget 建时给一个,
        // 逐请求 setFramebufferColorLayer 改绑同 array 的目标层。
        auto& fbo = bakeFbos_[n];
        if (!fbo) {
            FramebufferDesc fd;
            fd.width = n;
            fd.height = n;
            fd.hasColor = true;
            fd.hasDepth = false;
            fd.externalColorTarget = req.targetTexture;
            fd.externalColorLayer = req.layer;
            fbo = device_->createFramebuffer(fd);
            if (!fbo) continue;
        }
        // 按 srcSize 复用源 scratch 纹理(NEAREST,含重叠环)。
        auto& src = bakeSrcTex_[req.srcSize];
        if (!src) {
            TextureDesc td;
            td.width = req.srcSize;
            td.height = req.srcSize;
            td.format = TextureDesc::Format::RGBA8;
            td.mipmap = false;
            td.minFilter = TextureDesc::Filter::Nearest;
            td.magFilter = TextureDesc::Filter::Nearest;
            td.data = req.srcPacked.data();
            td.dataSize = req.srcPacked.size();
            src = device_->createTexture(td);
            if (!src) continue;
        } else {
            device_->updateTextureRegion(
                src.get(), 0, 0, req.srcSize, req.srcSize, req.srcPacked.data(),
                static_cast<size_t>(req.srcSize) * 4, 0);
        }
        // 改绑目标层。false = 后端不支持 → 短路(决不画到错误层)。
        if (!device_->setFramebufferColorLayer(fbo.get(), req.targetTexture,
                                               req.layer)) {
            continue;
        }
        if (!device_->beginPass(fbo.get(), /*clearTarget*/ false)) continue;
        RenderCommand cmd;
        cmd.kind = RenderCommandKind::Unknown;
        cmd.owner = "terrain_height_bake";
        cmd.pass = "bake";
        cmd.shader = bakeShader_.get();
        cmd.vertexBuffer = bakeQuad_.get();
        cmd.vertexCount = 4;
        cmd.vertexStride = 2 * sizeof(float);
        cmd.primitive = RenderCommand::PrimitiveType::TriangleStrip;
        cmd.depthTest = false;
        cmd.depthWrite = false;
        cmd.blend = false;
        cmd.cullFace = false;
        cmd.textures.push_back(src.get());  // unit 0 = 源(shader u_tileTexture)
        cmd.uniforms["u_srcSize"] = {static_cast<float>(req.srcSize)};
        cmd.uniforms["u_srcInset"] = {req.srcInset};
        cmd.uniforms["u_gridSize"] = {static_cast<float>(req.gridSize)};
        cmd.uniforms["u_quantBase"] = {req.quantBase};
        cmd.uniforms["u_quantStep"] = {req.quantStep};
        cmd.uniforms["u_minH"] = {req.minH};
        cmd.uniforms["u_range"] = {req.range};
        cmd.uniforms["u_widthM"] = {req.widthM};
        cmd.uniforms["u_heightM"] = {req.heightM};
        cmd.uniforms["u_reach"] = {req.reach};
        RenderCommandList cmds;
        cmds.push_back(std::move(cmd));
        device_->submit(cmds);
        device_->endPass();
    }
    pendingBakes_.clear();
}

bool TerrainDisplacementTemplatePool::updateEdgeLutRows(const TileKey& key,
                                                        int gridSize,
                                                        const uint8_t* bytes) {
    if (!device_ || !bytes) return false;
    auto arrIt = heightArrays_.find(gridSize);
    if (arrIt == heightArrays_.end() || !arrIt->second.texture) return false;
    HeightArray* arr = &arrIt->second;
    auto it = arr->index.find(heightCacheKey(key));
    if (it == arr->index.end() || it->second.layer < 0) return false;
    const int n = gridSize + 1;
    return device_->updateTextureRegion(arr->texture.get(), 0, n, n,
                                        kEdgeLutRows, bytes,
                                        static_cast<size_t>(n) * 4,
                                        it->second.layer);
}

void TerrainDisplacementTemplatePool::touchHeightTexture(const TileKey& key,
                                                         int gridSize,
                                                         uint64_t frameId) {
    auto it = heightArrays_.find(gridSize);
    if (it == heightArrays_.end()) return;
    it->second.layerPool.touch(heightCacheKey(key), frameId);
}

// 黑带排查(2026-08-15):帧号变化即清零并重拍占用快照。resident==capacity
// (池饱和)是「一旦出现就容易复现」这一粘滞特征的直接候选解释。
void TerrainDisplacementTemplatePool::beginHeightStatsFrame(uint64_t frameId) {
    if (heightStatsFrameId_ == frameId) {
        return;
    }
    heightStatsFrameId_ = frameId;
    heightFrameStats_ = HeightFrameStats{};
    for (const auto& entry : heightArrays_) {
        const HeightArray& arr = entry.second;
        const int resident = static_cast<int>(arr.index.size());
        const int capacity = layersForGridSize(arr.gridSize);
        if (arr.gridSize >= kTerrainDenseGridSize) {
            heightFrameStats_.denseResident += resident;
            heightFrameStats_.denseCapacity += capacity;
        } else {
            heightFrameStats_.coarseResident += resident;
            heightFrameStats_.coarseCapacity += capacity;
        }
    }
}

}  // namespace earth_engine
