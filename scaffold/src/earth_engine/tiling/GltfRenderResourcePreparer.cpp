#include "GltfRenderResourcePreparer.h"
#include "TerrainTemplateEligibility.h"

#include "GltfRenderGeometryBuilder.h"
#include "GpuReadyData.h"
#include "DirectRasterMapping.h"
#include "TerrainDisplacementTemplatePool.h"  // terrainReliefFade(P5b 判据同源)
#include "TilesetTile.h"
#include "../content/GltfModel.h"
#include "../providers/TerrainProvider.h"  // DecodedHeightmap
#include "../core/async/AsyncSystem.h"
#include "../debug/PerfTimer.h"
#include "../renderer/RenderDevice.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

constexpr int64_t kDeferredCpuReleaseLimitBytes =
    32LL * 1024LL * 1024LL;
std::atomic<int64_t> gDeferredCpuReleasePendingBytes{0};
std::atomic<uint32_t> gDeferredCpuReleasePendingTasks{0};

struct DeferredGpuUploadCpuPayload {
    std::vector<std::vector<uint8_t>> byteBuffers;

    int64_t allocatedBytes() const {
        int64_t bytes = 0;
        for (const std::vector<uint8_t>& buffer : byteBuffers) {
            bytes += static_cast<int64_t>(buffer.capacity());
        }
        return bytes;
    }
};

void deferCpuPayloadRelease(
    std::shared_ptr<DeferredGpuUploadCpuPayload> payload,
    GpuUploadMetrics* metrics) {
    if (!payload || payload->byteBuffers.empty()) {
        return;
    }
    const int64_t payloadBytes = payload->allocatedBytes();
    if (metrics) {
        metrics->deferredCpuBytes = payloadBytes;
    }
    const int64_t previouslyPending =
        gDeferredCpuReleasePendingBytes.fetch_add(
            payloadBytes,
            std::memory_order_acq_rel);
    if (previouslyPending + payloadBytes >
        kDeferredCpuReleaseLimitBytes) {
        gDeferredCpuReleasePendingBytes.fetch_sub(
            payloadBytes,
            std::memory_order_acq_rel);
        payload->byteBuffers.clear();
        if (metrics) {
            metrics->deferredReleaseInlineFallback = true;
            metrics->deferredReleasePendingBytes =
                gDeferredCpuReleasePendingBytes.load(
                    std::memory_order_acquire);
            metrics->deferredReleasePendingTasks =
                gDeferredCpuReleasePendingTasks.load(
                    std::memory_order_acquire);
        }
        return;
    }

    gDeferredCpuReleasePendingTasks.fetch_add(
        1u,
        std::memory_order_acq_rel);
    auto release = [
        payload = std::move(payload),
        payloadBytes]() mutable {
        payload->byteBuffers.clear();
        gDeferredCpuReleasePendingBytes.fetch_sub(
            payloadBytes,
            std::memory_order_acq_rel);
        gDeferredCpuReleasePendingTasks.fetch_sub(
            1u,
            std::memory_order_acq_rel);
    };
    try {
        (void)AsyncSystem::pool().enqueue(release);
    } catch (...) {
        release();
    }
    if (metrics) {
        metrics->deferredReleasePendingBytes =
            gDeferredCpuReleasePendingBytes.load(
                std::memory_order_acquire);
        metrics->deferredReleasePendingTasks =
            gDeferredCpuReleasePendingTasks.load(
                std::memory_order_acquire);
    }
}

TextureDesc::Filter toTextureFilter(GltfTextureFilter filter) {
    return filter == GltfTextureFilter::Nearest
        ? TextureDesc::Filter::Nearest
        : TextureDesc::Filter::Linear;
}

TextureDesc::Wrap toTextureWrap(GltfTextureWrap wrap) {
    switch (wrap) {
        case GltfTextureWrap::ClampToEdge:
            return TextureDesc::Wrap::Clamp;
        case GltfTextureWrap::MirroredRepeat:
            return TextureDesc::Wrap::MirroredRepeat;
        case GltfTextureWrap::Repeat:
        default:
            return TextureDesc::Wrap::Repeat;
    }
}

/// Decode a model's glTF textures to upload-ready RGBA8/R8 once per tile
/// (P2-12): textures are shared across primitives, so the copy lives at
/// GpuReadyData level instead of being duplicated into every primitive.
std::vector<GpuReadyData::TextureData> decodeModelTextures(
    const std::vector<GltfTexture>& modelTextures) {
    std::vector<GpuReadyData::TextureData> textures(modelTextures.size());
    for (size_t textureIndex = 0;
         textureIndex < modelTextures.size();
         ++textureIndex) {
        const GltfTexture& tex = modelTextures[textureIndex];
        GpuReadyData::TextureData& texData = textures[textureIndex];
        texData.minFilter = tex.sampler.minFilter;
        texData.magFilter = tex.sampler.magFilter;
        texData.mipmap = tex.sampler.mipmap;
        texData.wrapS = tex.sampler.wrapS;
        texData.wrapT = tex.sampler.wrapT;
        if (tex.image.width <= 0 || tex.image.height <= 0 ||
            tex.image.pixels.empty()) {
            continue;
        }
        const size_t pixelCount =
            static_cast<size_t>(tex.image.width) *
            static_cast<size_t>(tex.image.height);
        texData.width = tex.image.width;
        texData.height = tex.image.height;

        if (tex.image.channels == 1) {
            if (tex.image.pixels.size() < pixelCount) continue;
            texData.channels = 1;
            texData.pixels.assign(
                tex.image.pixels.begin(),
                tex.image.pixels.begin() + pixelCount);
        } else if (tex.image.channels == 3) {
            if (tex.image.pixels.size() < pixelCount * 3u) continue;
            texData.channels = 4;
            texData.pixels.resize(pixelCount * 4u);
            for (size_t p = 0; p < pixelCount; ++p) {
                texData.pixels[p * 4u + 0] = tex.image.pixels[p * 3u + 0];
                texData.pixels[p * 4u + 1] = tex.image.pixels[p * 3u + 1];
                texData.pixels[p * 4u + 2] = tex.image.pixels[p * 3u + 2];
                texData.pixels[p * 4u + 3] = 255u;
            }
        } else if (tex.image.channels == 4) {
            if (tex.image.pixels.size() < pixelCount * 4u) continue;
            texData.channels = 4;
            texData.pixels = tex.image.pixels;
        } else {
            continue;
        }
    }
    return textures;
}

GltfPrimitiveRenderResources::TextureBinding makeGltfTextureBinding(
    const std::optional<GltfTextureBinding>& modelBinding,
    const std::vector<std::unique_ptr<Texture>>& tileTextures) {
    GltfPrimitiveRenderResources::TextureBinding renderBinding;
    if (!modelBinding ||
        modelBinding->textureIndex >= tileTextures.size() ||
        !tileTextures[modelBinding->textureIndex]) {
        return renderBinding;
    }
    renderBinding.texture = tileTextures[modelBinding->textureIndex].get();
    renderBinding.texCoord = modelBinding->texCoord;
    renderBinding.offsetScale = {
        modelBinding->transform.offset[0],
        modelBinding->transform.offset[1],
        modelBinding->transform.scale[0],
        modelBinding->transform.scale[1]};
    renderBinding.rotationSinCos = {
        static_cast<float>(std::sin(modelBinding->transform.rotation)),
        static_cast<float>(std::cos(modelBinding->transform.rotation))};
    return renderBinding;
}

bool isValidGpuReadyPrimitive(const GpuReadyPrimitive& primitive) {
    // P5b:共享模板几何 primitive 有意不带顶点/索引字节(draw 时换绑共享模板),
    // 只要求计数如实——否则空 bytes 被判无效 → markRenderContentFailedTemporarily
    // → 每帧重试风暴且永不上传。
    if (primitive.metadata.sharedTemplateGeometry) {
        return primitive.vertexCount > 0 && primitive.indexCount > 0 &&
               !primitive.instances;
    }
    if (primitive.vertexStride == 0 ||
        primitive.vertexCount == 0 ||
        primitive.vertexBytes.empty() ||
        primitive.vertexBytes.size() % primitive.vertexStride != 0 ||
        primitive.vertexBytes.size() / primitive.vertexStride !=
            primitive.vertexCount ||
        primitive.indexCount == 0 ||
        (primitive.indexByteSize != sizeof(uint16_t) &&
         primitive.indexByteSize != sizeof(uint32_t)) ||
        primitive.indexBytes.size() !=
            primitive.indexCount * primitive.indexByteSize) {
        return false;
    }
    if (!primitive.instances) {
        return true;
    }
    return primitive.instances->stride > 0 &&
           primitive.instances->count > 0 &&
           !primitive.instances->bytes.empty() &&
           primitive.instances->bytes.size() %
                   primitive.instances->stride ==
               0 &&
           primitive.instances->bytes.size() /
                   primitive.instances->stride ==
               primitive.instances->count;
}

} // namespace

void GltfRenderResourcePreparer::prepare(TilesetTile& tile,
                                         RenderDevice* device,
                                         double currentFrameTimeSeconds,
                                         bool sharedTemplateGeometryActive) {
    const GltfModel* model =
        tile.content.renderContent.gltfModelForRead();
    if (!model) return;
    const bool animated = model->hasRuntimeAnimation();
    if (animated) {
        tile.content.renderContent.updateGltfAnimation(
            currentFrameTimeSeconds);
    }
    size_t expectedResourceCount = 0;
    for (const GltfPrimitive& primitive : model->primitives) {
        expectedResourceCount +=
            GltfRenderGeometryBuilder::primitiveRenderResourceCount(primitive);
    }
    if (expectedResourceCount > 0 &&
        tile.content.renderContent.gltfPrimitiveResourceCount() ==
            expectedResourceCount) {
        const auto& primitiveResources =
            tile.content.renderContent.gltfPrimitiveResourcesForDraw();
        const bool animationResourcesStale =
            animated &&
            std::any_of(
                primitiveResources.begin(),
                primitiveResources.end(),
                [revision = model->currentAnimationRevision()](
                    const GltfPrimitiveRenderResources& resources) {
                    return resources.animationRevision != revision;
                });
        const bool allReady = std::all_of(
            primitiveResources.begin(),
            primitiveResources.end(),
            [sharedTemplateGeometryActive](
                const GltfPrimitiveRenderResources& resources) {
                // P5b:共享模板几何 primitive 的 per-tile buffer 有意为空,
                // 模板活跃时视为就绪(否则每帧误判未就绪→无限重 prepare)。
                // 模板被运行时关闭(pool reset)后不再视为就绪 → 走下方重建
                // 路径补回 legacy VBO(toggle OFF 自愈)。
                if (resources.sharedTemplateGeometry &&
                    sharedTemplateGeometryActive) {
                    return resources.indexCount > 0;
                }
                return resources.vertexBuffer != nullptr &&
                       resources.indexBuffer != nullptr &&
                       resources.indexCount > 0 &&
                       (resources.instanceCount <= 0 ||
                        resources.instanceBuffer != nullptr);
            });
        if (allReady) {
            if (animationResourcesStale) {
                if (!device) {
                    return;
                }
                const Mat4& transform =
                    tile.content.renderContent.gltfTransform();
                const Vec3 localOrigin =
                    GltfRenderGeometryBuilder::localOrigin(
                        *model,
                        transform);
                bool updated = true;
                size_t resourceIndex = 0;
                for (const GltfPrimitive& primitive :
                     model->primitives) {
                    if (primitive.vertices.empty() ||
                        primitive.indices.empty()) {
                        continue;
                    }
                    GltfPrimitiveRenderResources* resources =
                        tile.content.renderContent
                            .gltfPrimitiveResourceForAnimationUpdateAt(
                            resourceIndex++);
                    if (!resources) {
                        updated = false;
                        break;
                    }
                    std::vector<GltfGpuVertex> verts =
                        GltfRenderGeometryBuilder::buildVertices(
                            primitive,
                            transform,
                            localOrigin);
                    bool ok = resources->vertexBuffer &&
                        resources->vertexBuffer->size() ==
                            verts.size() * sizeof(GltfGpuVertex) &&
                        device->updateBuffer(
                            resources->vertexBuffer.get(),
                            0,
                            verts.data(),
                            verts.size() * sizeof(GltfGpuVertex));
                    if (ok && !primitive.instances.empty()) {
                        std::vector<GltfGpuInstance> instances =
                            GltfRenderGeometryBuilder::buildInstances(
                                primitive,
                                transform,
                                localOrigin);
                        ok = resources->instanceBuffer &&
                            resources->instanceCount ==
                                static_cast<int>(instances.size()) &&
                            resources->instanceBuffer->size() ==
                                instances.size() *
                                    sizeof(GltfGpuInstance) &&
                            device->updateBuffer(
                                resources->instanceBuffer.get(),
                                0,
                                instances.data(),
                                instances.size() *
                                    sizeof(GltfGpuInstance));
                    }
                    if (!ok) {
                        updated = false;
                        break;
                    }
                    resources->sortCenterEcef =
                        GltfRenderGeometryBuilder::primitiveSortCenterEcef(
                            primitive,
                            transform);
                    resources->animationRevision =
                        model->currentAnimationRevision();
                }
                if (!updated) {
                    tile.content.renderContent.clearGltfPrimitiveResources();
                } else {
                    tile.content.renderContent.setGltfLocalOrigin(
                        localOrigin);
                    tile.content.renderContent.clearTerrainGpuVertexBytes();
                    tile.markRenderContentDone();
                    return;
                }
            } else {
                tile.content.renderContent.clearTerrainGpuVertexBytes();
                tile.markRenderContentDone();
                return;
            }
        }
    }
    if (!device) {
        return;
    }
    // [interaction-bottleneck-2 诊断] 拆 prepareCpuWork / uploadToGpu 两段墙钟
    // + 顶点数,判 18.7ms terrainUpload 是 CPU 网格构建(H-T1)还是 GPU 上传背压
    // (H-T2)。只打慢段(≥2ms),tag=EarthPerf;判定后可保留为诊断或回滚。
    const double cpuStartMs = perf::nowMs();
    const double cpuStartCpuMs = perf::cpuThreadMs();
    std::optional<GpuReadyData> ready = prepareCpuWork(
        tile, currentFrameTimeSeconds, sharedTemplateGeometryActive);
    const double cpuMs = perf::nowMs() - cpuStartMs;
    const double cpuCpuMs = perf::cpuThreadMs() - cpuStartCpuMs;
    if (cpuMs >= 2.0) {
        const GltfModel* model =
            tile.content.renderContent.gltfModelForRead();
        size_t verts = 0;
        size_t indices = 0;
        if (model) {
            for (const GltfPrimitive& p : model->primitives) {
                verts += p.vertices.size();
                indices += p.indices.size();
            }
        }
        platformLog(LogLevel::Info, "EarthPerf",
                    "GltfPrepare.cpuWork ms=%.2f cpu=%.2f z=%d x=%d y=%d "
                    "verts=%zu indices=%zu",
                    cpuMs, cpuCpuMs, tile.key.z, tile.key.x, tile.key.y,
                    verts, indices);
    }
    if (!ready) {
        tile.content.renderContent.clearGltfGpuResources();
        tile.markRenderContentFailedTemporarily();
        return;
    }
    const double gpuStartMs = perf::nowMs();
    uploadToGpu(tile, device, std::move(*ready));
    const double gpuMs = perf::nowMs() - gpuStartMs;
    if (gpuMs >= 2.0) {
        platformLog(LogLevel::Info, "EarthPerf",
                    "GltfPrepare.uploadToGpu ms=%.2f z=%d x=%d y=%d",
                    gpuMs, tile.key.z, tile.key.x, tile.key.y);
    }
}

// ============================================================
// Phase 1: CPU preparation (Worker Thread)
// ============================================================

std::optional<GpuReadyData> GltfRenderResourcePreparer::prepareCpuWork(
    const TilesetTile& tile,
    double currentFrameTimeSeconds,
    bool sharedTemplateGeometryActive) {
    const GltfModel* model = tile.content.renderContent.gltfModelForRead();
    if (!model) {
        return std::nullopt;
    }
    const Mat4& transform =
        tile.content.renderContent.gltfTransform();
    const Vec3 localOrigin =
        GltfRenderGeometryBuilder::localOrigin(*model, transform);
    // P5b tile 级判据(镜像 GltfDrawCommandBuilder 模板 swap 的 tile 条件,
    // reliefFade 同源 TerrainDisplacementTemplatePool.h):真实地形 + 自有
    // retainedHeightmap + fade>0.001 的瓦片,draw 时必换共享模板 → per-tile
    // baked VBO 从不被绘制,跳过其构建/上传。上采样 z13+(无自有高度图)与
    // coarse z≤6(fade=0)判据为 false,保持 legacy VBO 路径。primitive 级
    // 排除(水面掩码格式/实例化)在 prepareCpuWorkFromModel 内完成。
    const DecodedHeightmap* heightmap =
        tile.content.renderContent.retainedHeightmap().get();
    const bool skipBakedTerrainGeometry =
        TerrainTemplateEligibility::forLoadedTile(
            sharedTemplateGeometryActive,
            tile.key.z,
            tile.content.renderContent.isTerrainRenderContent(),
            heightmap != nullptr && heightmap->valid());
    return prepareCpuWorkFromModel(
        *model,
        transform,
        localOrigin,
        currentFrameTimeSeconds,
        skipBakedTerrainGeometry);
}

std::optional<GpuReadyData> GltfRenderResourcePreparer::prepareCpuWorkFromModel(
    const GltfModel& model,
    const Mat4& transform,
    const Vec3& localOrigin,
    double /*currentFrameTimeSeconds*/,
    bool skipBakedTerrainGeometry) {
    if (model.primitives.empty()) return std::nullopt;

    GpuReadyData ready;
    ready.localOrigin = localOrigin;
    bool hasReferencedTextures = false;
    const bool animated = model.hasRuntimeAnimation();

    for (const GltfPrimitive& primitive : model.primitives) {
        // 无几何地形 primitive(摘 glTF 第一级):CPU 网格根本没造,顶点/索引
        // 为空是**有意**的,不能按「空 primitive」跳过 —— 跳过就没有 primitive
        // 资源、瓦片永远不 ready(症状=地形整片不出且零报错)。
        if (!primitive.templateGeometryOnly &&
            (primitive.vertices.empty() || primitive.indices.empty())) {
            continue;
        }
        if (primitive.templateGeometryOnly) {
            GpuReadyPrimitive templatePrim;
            templatePrim.metadata.sharedTemplateGeometry = true;
            templatePrim.vertexStride = sizeof(GltfGpuVertex);
            templatePrim.vertexCount =
                static_cast<size_t>(primitive.templateVertexCount);
            templatePrim.indexCount =
                static_cast<size_t>(primitive.templateIndexCount);
            templatePrim.sortCenterEcef = primitive.templateSortCenterEcef;
            templatePrim.metadata.primitiveMode = primitive.primitiveMode;
            templatePrim.metadata.doubleSided = primitive.doubleSided;
            templatePrim.metadata.metallicFactor = primitive.metallicFactor;
            templatePrim.metadata.roughnessFactor = primitive.roughnessFactor;
            templatePrim.metadata.unlit = primitive.unlit;
            ready.primitives.push_back(std::move(templatePrim));
            continue;
        }

        GpuReadyPrimitive gpuPrim;
        const bool useTerrainFormat = primitive.hasTerrainWaterMaskMetadata;
        const bool instanced = !primitive.instances.empty();

        // CPU work: build vertex data
        if (useTerrainFormat && !instanced) {
            const size_t expectedPrebuiltBytes =
                primitive.vertices.size() * sizeof(TerrainGpuVertex);
            if (primitive.terrainGpuVertexBytes.size() ==
                expectedPrebuiltBytes) {
                gpuPrim.vertexBytes = primitive.terrainGpuVertexBytes;
            } else {
                auto terrainVerts =
                    GltfRenderGeometryBuilder::buildTerrainVertices(
                        primitive,
                        transform,
                        localOrigin);
                gpuPrim.vertexBytes.resize(
                    terrainVerts.size() * sizeof(TerrainGpuVertex));
                std::memcpy(
                    gpuPrim.vertexBytes.data(),
                    terrainVerts.data(),
                    gpuPrim.vertexBytes.size());
            }
            gpuPrim.vertexStride = sizeof(TerrainGpuVertex);
            gpuPrim.vertexCount = primitive.vertices.size();
        } else if (skipBakedTerrainGeometry && !instanced) {
            // P5b:tile 级判据成立(真实地形+retainedHeightmap+fade>0.001)且
            // primitive 非水面掩码格式、非实例化 → draw 时必换共享位移模板,
            // per-tile baked VBO 从不被绘制。跳过顶点构建(主线程 terrainUpload
            // 尖刺大头 = buildVertices 逐顶点重建)与索引字节;计数仍如实填
            // (供 metadata/命令构建),buffer 留空由 sharedTemplateGeometry
            // 标志向 allReady/uploadToGpu/draw 三方声明「有意为空」。
            gpuPrim.metadata.sharedTemplateGeometry = true;
            gpuPrim.vertexStride = sizeof(GltfGpuVertex);
            gpuPrim.vertexCount = primitive.vertices.size();
            gpuPrim.indexCount = primitive.indices.size();
        } else {
            auto gltfVerts = GltfRenderGeometryBuilder::buildVertices(
                primitive, transform, localOrigin);
            gpuPrim.vertexBytes.resize(gltfVerts.size() * sizeof(GltfGpuVertex));
            memcpy(gpuPrim.vertexBytes.data(), gltfVerts.data(), gpuPrim.vertexBytes.size());
            gpuPrim.vertexStride = sizeof(GltfGpuVertex);
            gpuPrim.vertexCount = gltfVerts.size();

            // Build instance data if needed
            if (instanced) {
                auto instanceData = GltfRenderGeometryBuilder::buildInstances(
                    primitive, transform, localOrigin);
                GpuReadyPrimitive::InstanceData inst;
                inst.bytes.resize(instanceData.size() * sizeof(GltfGpuInstance));
                memcpy(inst.bytes.data(), instanceData.data(), inst.bytes.size());
                inst.count = instanceData.size();
                inst.stride = sizeof(GltfGpuInstance);
                gpuPrim.instances = std::move(inst);
            }
        }

        // Index data: narrow to uint16 whenever the vertex count allows —
        // halves the index upload bytes for virtually every terrain tile.
        // (P5b 共享模板几何 primitive 无索引字节,跳过。)
        if (!gpuPrim.metadata.sharedTemplateGeometry) {
            gpuPrim.assignIndices(primitive.indices, gpuPrim.vertexCount);
        }

        // Sort center
        gpuPrim.sortCenterEcef =
            GltfRenderGeometryBuilder::primitiveSortCenterEcef(primitive, transform);

        gpuPrim.terrainWaterMaskTextureIndex =
            primitive.terrainWaterMaskTextureIndex;
        gpuPrim.baseColorTexture = primitive.baseColorTexture;
        if (!gpuPrim.baseColorTexture &&
            primitive.baseColorTextureIndex) {
            GltfTextureBinding binding;
            binding.textureIndex = *primitive.baseColorTextureIndex;
            gpuPrim.baseColorTexture = binding;
        }
        gpuPrim.metallicRoughnessTexture =
            primitive.metallicRoughnessTexture;
        gpuPrim.anisotropyTexture = primitive.anisotropyTexture;
        gpuPrim.specularTexture = primitive.specularTexture;
        gpuPrim.specularColorTexture = primitive.specularColorTexture;
        gpuPrim.specularGlossinessTexture =
            primitive.specularGlossinessTexture;
        gpuPrim.transmissionTexture = primitive.transmissionTexture;
        gpuPrim.clearcoatTexture = primitive.clearcoatTexture;
        gpuPrim.clearcoatRoughnessTexture =
            primitive.clearcoatRoughnessTexture;
        gpuPrim.clearcoatNormalTexture =
            primitive.clearcoatNormalTexture;
        gpuPrim.sheenColorTexture = primitive.sheenColorTexture;
        gpuPrim.sheenRoughnessTexture =
            primitive.sheenRoughnessTexture;
        gpuPrim.normalTexture = primitive.normalTexture;
        gpuPrim.occlusionTexture = primitive.occlusionTexture;
        gpuPrim.emissiveTexture = primitive.emissiveTexture;

        auto bindingHasTexCoordSet =
            [&](const std::optional<GltfTextureBinding>& binding) {
                return !binding ||
                       GltfRenderGeometryBuilder::primitiveHasTexCoordSet(
                           primitive,
                           binding->texCoord);
            };
        if (!bindingHasTexCoordSet(gpuPrim.baseColorTexture) ||
            !bindingHasTexCoordSet(gpuPrim.metallicRoughnessTexture) ||
            !bindingHasTexCoordSet(gpuPrim.anisotropyTexture) ||
            !bindingHasTexCoordSet(gpuPrim.specularTexture) ||
            !bindingHasTexCoordSet(gpuPrim.specularColorTexture) ||
            !bindingHasTexCoordSet(gpuPrim.specularGlossinessTexture) ||
            !bindingHasTexCoordSet(gpuPrim.transmissionTexture) ||
            !bindingHasTexCoordSet(gpuPrim.clearcoatTexture) ||
            !bindingHasTexCoordSet(gpuPrim.clearcoatRoughnessTexture) ||
            !bindingHasTexCoordSet(gpuPrim.clearcoatNormalTexture) ||
            !bindingHasTexCoordSet(gpuPrim.sheenColorTexture) ||
            !bindingHasTexCoordSet(gpuPrim.sheenRoughnessTexture) ||
            !bindingHasTexCoordSet(gpuPrim.normalTexture) ||
            !bindingHasTexCoordSet(gpuPrim.occlusionTexture) ||
            !bindingHasTexCoordSet(gpuPrim.emissiveTexture)) {
            return std::nullopt;
        }
        hasReferencedTextures =
            hasReferencedTextures ||
            gpuPrim.terrainWaterMaskTextureIndex.has_value() ||
            gpuPrim.baseColorTexture.has_value() ||
            gpuPrim.metallicRoughnessTexture.has_value() ||
            gpuPrim.anisotropyTexture.has_value() ||
            gpuPrim.specularTexture.has_value() ||
            gpuPrim.specularColorTexture.has_value() ||
            gpuPrim.specularGlossinessTexture.has_value() ||
            gpuPrim.transmissionTexture.has_value() ||
            gpuPrim.clearcoatTexture.has_value() ||
            gpuPrim.clearcoatRoughnessTexture.has_value() ||
            gpuPrim.clearcoatNormalTexture.has_value() ||
            gpuPrim.sheenColorTexture.has_value() ||
            gpuPrim.sheenRoughnessTexture.has_value() ||
            gpuPrim.normalTexture.has_value() ||
            gpuPrim.occlusionTexture.has_value() ||
            gpuPrim.emissiveTexture.has_value();

        // Copy all scalar/vector metadata but leave GPU pointers as nullptr.
        GltfPrimitiveRenderResources& meta = gpuPrim.metadata;
        meta.useTerrainVertexFormat = useTerrainFormat;
        meta.vertexCount = static_cast<int>(primitive.vertices.size());
        meta.indexCount = static_cast<int>(primitive.indices.size());
        meta.indexByteSize = static_cast<int>(gpuPrim.indexByteSize);
        meta.instanceCount = instanced
            ? static_cast<int>(primitive.instances.size()) : 0;
        meta.primitiveMode = primitive.primitiveMode;
        meta.sortCenterEcef = gpuPrim.sortCenterEcef;
        meta.baseColorFactor = primitive.baseColorFactor;
        meta.metallicFactor = primitive.metallicFactor;
        meta.roughnessFactor = primitive.roughnessFactor;
        meta.dielectricSpecularF0 = primitive.dielectricSpecularF0;
        meta.specularFactor = primitive.specularFactor;
        meta.specularColorFactor = primitive.specularColorFactor;
        meta.specularGlossinessWorkflow = primitive.specularGlossinessWorkflow;
        meta.specularGlossinessSpecularFactor = primitive.specularGlossinessSpecularFactor;
        meta.specularGlossinessGlossinessFactor = primitive.specularGlossinessGlossinessFactor;
        meta.transmissionFactor = primitive.transmissionFactor;
        meta.anisotropyStrength = primitive.anisotropyStrength;
        meta.anisotropyRotation = primitive.anisotropyRotation;
        meta.clearcoatFactor = primitive.clearcoatFactor;
        meta.clearcoatRoughnessFactor = primitive.clearcoatRoughnessFactor;
        meta.clearcoatNormalTextureScale = primitive.clearcoatNormalTextureScale;
        meta.sheenColorFactor = primitive.sheenColorFactor;
        meta.sheenRoughnessFactor = primitive.sheenRoughnessFactor;
        meta.normalTextureScale = primitive.normalTextureScale;
        meta.occlusionTextureStrength = primitive.occlusionTextureStrength;
        meta.emissiveFactor = primitive.emissiveFactor;
        meta.alphaMode = primitive.alphaMode;
        meta.alphaCutoff = primitive.alphaCutoff;
        meta.doubleSided = primitive.doubleSided;
        meta.unlit = primitive.unlit;
        meta.dynamicVertices = animated;
        meta.animationRevision = model.currentAnimationRevision();
        meta.hasTerrainWaterMaskMetadata = primitive.hasTerrainWaterMaskMetadata;
        meta.terrainOnlyWater = primitive.terrainOnlyWater;
        meta.terrainOnlyLand = primitive.terrainOnlyLand;
        meta.terrainWaterMaskTranslationScale = {
            static_cast<float>(primitive.terrainWaterMaskTranslationX),
            static_cast<float>(primitive.terrainWaterMaskTranslationY),
            static_cast<float>(primitive.terrainWaterMaskScale),
            0.0f};

        ready.primitives.push_back(std::move(gpuPrim));
    }

    if (hasReferencedTextures) {
        ready.textures = decodeModelTextures(model.textures);
    }

    return ready.valid() ? std::make_optional(std::move(ready)) : std::nullopt;
}

// ============================================================
// Phase 2: GPU upload (Main Thread — GL context)
// ============================================================

bool GltfRenderResourcePreparer::uploadToGpu(
    TilesetTile& tile,
    RenderDevice* device,
    GpuReadyData&& ready,
    GpuUploadMetrics* metrics) {
    if (metrics) {
        *metrics = GpuUploadMetrics{};
        metrics->primitiveCount =
            static_cast<uint32_t>(ready.primitives.size());
        for (const GpuReadyPrimitive& primitive : ready.primitives) {
            metrics->vertexBytes +=
                static_cast<int64_t>(primitive.vertexBytes.size());
            metrics->indexBytes +=
                static_cast<int64_t>(primitive.indexBytes.size());
            if (primitive.instances) {
                metrics->instanceBytes += static_cast<int64_t>(
                    primitive.instances->bytes.size());
            }
        }
        for (const GpuReadyData::TextureData& texture : ready.textures) {
            if (texture.valid()) {
                metrics->textureBytes +=
                    static_cast<int64_t>(texture.pixels.size());
                ++metrics->textureCount;
            }
        }
    }

    const bool validPayload =
        ready.valid() &&
        std::all_of(
            ready.primitives.begin(),
            ready.primitives.end(),
            isValidGpuReadyPrimitive);
    if (!device || !validPayload) {
        tile.content.renderContent.asyncGpuUploadPending = false;
        tile.markRenderContentFailedTemporarily();
        return false;
    }

    tile.content.renderContent.setGltfLocalOrigin(ready.localOrigin);
    tile.content.renderContent.beginGltfGpuResourceBuild(
        ready.textures.size(),
        ready.primitives.size());
    auto deferredCpuPayload =
        std::make_shared<DeferredGpuUploadCpuPayload>();
    deferredCpuPayload->byteBuffers.reserve(
        ready.primitives.size() * 3u + ready.textures.size());

    // Upload textures first (they're referenced by index in metadata)
    std::vector<std::unique_ptr<Texture>> gpuTextures;
    bool success = true;
    if (!ready.textures.empty()) {
        gpuTextures.resize(ready.textures.size());
        for (size_t textureIndex = 0;
             textureIndex < ready.textures.size();
             ++textureIndex) {
            GpuReadyData::TextureData& texData =
                ready.textures[textureIndex];
            if (!texData.valid()) {
                continue;
            }
            TextureDesc td;
            td.width = texData.width;
            td.height = texData.height;
            td.format = texData.channels == 1
                ? TextureDesc::Format::R8
                : TextureDesc::Format::RGBA8;
            td.data = texData.pixels.data();
            td.dataSize = texData.pixels.size();
            td.mipmap = texData.mipmap;
            td.minFilter = toTextureFilter(texData.minFilter);
            td.magFilter = toTextureFilter(texData.magFilter);
            td.wrapS = toTextureWrap(texData.wrapS);
            td.wrapT = toTextureWrap(texData.wrapT);
            const double textureStartMs = perf::nowMs();
            gpuTextures[textureIndex] = device->createTexture(td);
            if (metrics) {
                metrics->textureUploadMs +=
                    perf::nowMs() - textureStartMs;
            }
            deferredCpuPayload->byteBuffers.push_back(
                std::move(texData.pixels));
        }
    }

    for (auto& prim : ready.primitives) {
        prim.metadata.vertexCount =
            static_cast<int>(prim.vertexCount);
        prim.metadata.indexCount =
            static_cast<int>(prim.indexCount);
        prim.metadata.indexByteSize =
            static_cast<int>(prim.indexByteSize);
        prim.metadata.instanceCount = prim.instances
            ? static_cast<int>(prim.instances->count)
            : 0;
        prim.metadata.sortCenterEcef = prim.sortCenterEcef;

        // P5b:共享模板几何 primitive 有意不建 per-tile vertex/index buffer
        // (draw 时换绑共享模板,建了也从不绘制 = 每瓦片 ~507KB 废弃显存)。
        if (!prim.metadata.sharedTemplateGeometry) {
        // GPU: create vertex buffer
        BufferDesc vbDesc;
        vbDesc.size = prim.vertexBytes.size();
        vbDesc.data = prim.vertexBytes.data();
        vbDesc.usage = prim.metadata.dynamicVertices
            ? BufferDesc::Usage::Dynamic
            : BufferDesc::Usage::Static;
        vbDesc.type = BufferDesc::Type::Vertex;
        const double vertexBufferStartMs = perf::nowMs();
        prim.metadata.vertexBuffer = device->createBuffer(vbDesc);
        if (metrics) {
            metrics->vertexBufferUploadMs +=
                perf::nowMs() - vertexBufferStartMs;
            ++metrics->vertexBufferCount;
        }
        deferredCpuPayload->byteBuffers.push_back(
            std::move(prim.vertexBytes));

        // GPU: create index buffer
        BufferDesc ibDesc;
        ibDesc.size = prim.indexBytes.size();
        ibDesc.data = prim.indexBytes.data();
        ibDesc.usage = BufferDesc::Usage::Static;
        ibDesc.type = BufferDesc::Type::Index;
        const double indexBufferStartMs = perf::nowMs();
        prim.metadata.indexBuffer = device->createBuffer(ibDesc);
        if (metrics) {
            metrics->indexBufferUploadMs +=
                perf::nowMs() - indexBufferStartMs;
            ++metrics->indexBufferCount;
        }
        deferredCpuPayload->byteBuffers.push_back(
            std::move(prim.indexBytes));
        }  // !sharedTemplateGeometry

        // GPU: create instance buffer if needed
        if (prim.instances) {
            BufferDesc instDesc;
            instDesc.size = prim.instances->bytes.size();
            instDesc.data = prim.instances->bytes.data();
            instDesc.usage = prim.metadata.dynamicVertices
                ? BufferDesc::Usage::Dynamic
                : BufferDesc::Usage::Static;
            instDesc.type = BufferDesc::Type::Vertex;
            const double instanceBufferStartMs = perf::nowMs();
            prim.metadata.instanceBuffer = device->createBuffer(instDesc);
            if (metrics) {
                metrics->instanceBufferUploadMs +=
                    perf::nowMs() - instanceBufferStartMs;
                ++metrics->instanceBufferCount;
            }
            deferredCpuPayload->byteBuffers.push_back(
                std::move(prim.instances->bytes));
        }

        const double resourceCommitStartMs = perf::nowMs();
        // Validate(P5b:共享模板几何 primitive 的 null buffer 是有意的,不算失败)
        if (!prim.metadata.sharedTemplateGeometry &&
            (!prim.metadata.vertexBuffer ||
             !prim.metadata.indexBuffer ||
             (prim.instances && !prim.metadata.instanceBuffer))) {
            success = false;
            if (metrics) {
                metrics->resourceCommitMs +=
                    perf::nowMs() - resourceCommitStartMs;
            }
            continue;
        }

        auto bindTexture =
            [&](GltfPrimitiveRenderResources::TextureBinding& binding,
                const std::optional<GltfTextureBinding>& modelBinding) {
                binding = makeGltfTextureBinding(
                    modelBinding,
                    gpuTextures);
                if (modelBinding && !binding.texture) {
                    success = false;
                }
            };
        bindTexture(
            prim.metadata.baseColorTexture,
            prim.baseColorTexture);
        bindTexture(
            prim.metadata.metallicRoughnessTexture,
            prim.metallicRoughnessTexture);
        bindTexture(
            prim.metadata.anisotropyTexture,
            prim.anisotropyTexture);
        bindTexture(
            prim.metadata.specularTexture,
            prim.specularTexture);
        bindTexture(
            prim.metadata.specularColorTexture,
            prim.specularColorTexture);
        bindTexture(
            prim.metadata.specularGlossinessTexture,
            prim.specularGlossinessTexture);
        bindTexture(
            prim.metadata.transmissionTexture,
            prim.transmissionTexture);
        bindTexture(
            prim.metadata.clearcoatTexture,
            prim.clearcoatTexture);
        bindTexture(
            prim.metadata.clearcoatRoughnessTexture,
            prim.clearcoatRoughnessTexture);
        bindTexture(
            prim.metadata.clearcoatNormalTexture,
            prim.clearcoatNormalTexture);
        bindTexture(
            prim.metadata.sheenColorTexture,
            prim.sheenColorTexture);
        bindTexture(
            prim.metadata.sheenRoughnessTexture,
            prim.sheenRoughnessTexture);
        bindTexture(
            prim.metadata.normalTexture,
            prim.normalTexture);
        bindTexture(
            prim.metadata.occlusionTexture,
            prim.occlusionTexture);
        bindTexture(
            prim.metadata.emissiveTexture,
            prim.emissiveTexture);
        if (prim.terrainWaterMaskTextureIndex) {
            const size_t textureIndex =
                *prim.terrainWaterMaskTextureIndex;
            if (textureIndex >= gpuTextures.size() ||
                !gpuTextures[textureIndex]) {
                success = false;
            } else {
                prim.metadata.terrainWaterMaskTexture =
                    gpuTextures[textureIndex].get();
            }
        }

        tile.content.renderContent.addGltfPrimitiveResource(
            std::move(prim.metadata));
        if (metrics) {
            metrics->resourceCommitMs +=
                perf::nowMs() - resourceCommitStartMs;
        }
    }

    const double finalCommitStartMs = perf::nowMs();
    // Store textures in the tile's texture resource list
    if (!gpuTextures.empty()) {
        for (auto& tex : gpuTextures) {
            tile.content.renderContent.addGltfTextureResource(std::move(tex));
        }
    }

    // Clear async flag
    tile.content.renderContent.asyncGpuUploadPending = false;

    // Mark tile as ready for rendering
    if (success && tile.content.renderContent.hasGltfPrimitiveResources()) {
        std::vector<std::vector<uint8_t>> retainedTerrainBytes =
            tile.content.renderContent
                .takeTerrainGpuVertexBytesForDeferredRelease();
        for (std::vector<uint8_t>& bytes : retainedTerrainBytes) {
            deferredCpuPayload->byteBuffers.push_back(
                std::move(bytes));
        }
        tile.markRenderContentDone();
    } else {
        tile.content.renderContent.clearGltfGpuResources();
        tile.markRenderContentFailedTemporarily();
    }
    if (metrics) {
        metrics->resourceCommitMs +=
            perf::nowMs() - finalCommitStartMs;
    }
    deferCpuPayloadRelease(std::move(deferredCpuPayload), metrics);

    return success;
}

int64_t GltfRenderResourcePreparer::deferredCpuReleasePendingBytes() {
    return gDeferredCpuReleasePendingBytes.load(
        std::memory_order_acquire);
}

uint32_t GltfRenderResourcePreparer::deferredCpuReleasePendingTasks() {
    return gDeferredCpuReleasePendingTasks.load(
        std::memory_order_acquire);
}

int64_t GltfRenderResourcePreparer::deferredCpuReleaseLimitBytes() {
    return kDeferredCpuReleaseLimitBytes;
}

} // namespace earth_engine
