#include "GltfRenderResourcePreparer.h"

#include "GltfRenderGeometryBuilder.h"
#include "GpuReadyData.h"
#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "../content/GltfModel.h"
#include "../renderer/RenderDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

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

std::unique_ptr<Texture> createGltfGpuTexture(RenderDevice* device,
                                              const GltfTexture& texture) {
    if (!device ||
        texture.image.width <= 0 ||
        texture.image.height <= 0 ||
        texture.image.pixels.empty()) {
        return nullptr;
    }

    const size_t pixelCount =
        static_cast<size_t>(texture.image.width) *
        static_cast<size_t>(texture.image.height);
    std::vector<uint8_t> rgbaPixels;
    const uint8_t* texturePixels = texture.image.pixels.data();
    size_t texturePixelBytes = texture.image.pixels.size();
    TextureDesc::Format textureFormat = TextureDesc::Format::RGBA8;
    if (texture.image.channels == 1) {
        if (texture.image.pixels.size() < pixelCount) {
            return nullptr;
        }
        texturePixelBytes = pixelCount;
        textureFormat = TextureDesc::Format::R8;
    } else if (texture.image.channels == 3) {
        if (texture.image.pixels.size() < pixelCount * 3u) {
            return nullptr;
        }
        rgbaPixels.resize(pixelCount * 4u);
        for (size_t i = 0; i < pixelCount; ++i) {
            rgbaPixels[i * 4u + 0u] = texture.image.pixels[i * 3u + 0u];
            rgbaPixels[i * 4u + 1u] = texture.image.pixels[i * 3u + 1u];
            rgbaPixels[i * 4u + 2u] = texture.image.pixels[i * 3u + 2u];
            rgbaPixels[i * 4u + 3u] = 255u;
        }
        texturePixels = rgbaPixels.data();
        texturePixelBytes = rgbaPixels.size();
    } else if (texture.image.channels == 4) {
        if (texture.image.pixels.size() < pixelCount * 4u) {
            return nullptr;
        }
    } else {
        return nullptr;
    }

    TextureDesc textureDesc;
    textureDesc.width = texture.image.width;
    textureDesc.height = texture.image.height;
    textureDesc.format = textureFormat;
    textureDesc.data = texturePixels;
    textureDesc.dataSize = texturePixelBytes;
    textureDesc.mipmap = texture.sampler.mipmap;
    textureDesc.minFilter = toTextureFilter(texture.sampler.minFilter);
    textureDesc.magFilter = toTextureFilter(texture.sampler.magFilter);
    textureDesc.wrapS = toTextureWrap(texture.sampler.wrapS);
    textureDesc.wrapT = toTextureWrap(texture.sampler.wrapT);
    return device->createTexture(textureDesc);
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


} // namespace

void GltfRenderResourcePreparer::prepare(TilesetTile& tile,
                                         RenderDevice* device,
                                         double currentFrameTimeSeconds) {
    GltfModel* model = tile.content.renderContent.gltfContent();
    if (!model) return;
    const bool animated = model->hasRuntimeAnimation();
    const bool animationChanged =
        animated && model->updateAnimation(currentFrameTimeSeconds);
    size_t expectedResourceCount = 0;
    for (const GltfPrimitive& primitive : model->primitives) {
        expectedResourceCount +=
            GltfRenderGeometryBuilder::primitiveRenderResourceCount(primitive);
    }
    const bool splitBlendInstances =
        GltfRenderGeometryBuilder::modelUsesSplitBlendInstances(*model);
    if (expectedResourceCount > 0 &&
        tile.content.renderContent.gltfPrimitiveResourceCount() ==
            expectedResourceCount) {
        const auto& primitiveResources =
            tile.content.renderContent.gltfPrimitiveResourcesForDraw();
        const bool allReady = std::all_of(
            primitiveResources.begin(),
            primitiveResources.end(),
            [](const GltfPrimitiveRenderResources& resources) {
                return resources.vertexBuffer != nullptr &&
                       resources.indexBuffer != nullptr &&
                       resources.indexCount > 0 &&
                       (resources.instanceCount <= 0 ||
                        resources.instanceBuffer != nullptr);
            });
        if (allReady) {
            if (animated && animationChanged && splitBlendInstances) {
                tile.content.renderContent.clearGltfPrimitiveResources();
            } else if (animated && animationChanged && device) {
                tile.content.renderContent.setGltfLocalOrigin(
                    GltfRenderGeometryBuilder::localOrigin(
                        *model,
                        tile.content.renderContent.gltfTransform()));
                bool updated = true;
                size_t resourceIndex = 0;
                for (const GltfPrimitive& primitive :
                     model->primitives) {
                    if (primitive.vertices.empty() ||
                        primitive.indices.empty()) {
                        continue;
                    }
                    GltfPrimitiveRenderResources* resources =
                        tile.content.renderContent.gltfPrimitiveResourceForBuildAt(
                            resourceIndex++);
                    if (!resources) {
                        updated = false;
                        break;
                    }
                    std::vector<GltfGpuVertex> verts =
                        GltfRenderGeometryBuilder::buildVertices(
                            primitive,
                            tile.content.renderContent.gltfTransform(),
                            tile.content.renderContent.renderLocalOrigin());
                    const bool ok = resources->vertexBuffer &&
                        resources->vertexBuffer->size() ==
                            verts.size() * sizeof(GltfGpuVertex) &&
                        device->updateBuffer(
                            resources->vertexBuffer.get(),
                            0,
                            verts.data(),
                            verts.size() * sizeof(GltfGpuVertex));
                    if (!ok) {
                        updated = false;
                        break;
                    }
                    resources->sortCenterEcef =
                        GltfRenderGeometryBuilder::primitiveSortCenterEcef(
                            primitive,
                            tile.content.renderContent.gltfTransform());
                    resources->animationRevision =
                        model->currentAnimationRevision();
                }
                if (!updated) {
                    tile.content.renderContent.clearGltfPrimitiveResources();
                } else {
                    tile.markRenderContentDone();
                    return;
                }
            } else {
                tile.markRenderContentDone();
                return;
            }
        }
    }
    if (!device) return;
    tile.content.renderContent.setGltfLocalOrigin(
        GltfRenderGeometryBuilder::localOrigin(
            *model,
            tile.content.renderContent.gltfTransform()));

    tile.content.renderContent.beginGltfGpuResourceBuild(
        model->textures.size(),
        expectedResourceCount);
    for (const GltfTexture& texture : model->textures) {
        tile.content.renderContent.addGltfTextureResource(
            createGltfGpuTexture(device, texture));
    }

    bool resourceFailure = false;

    for (const GltfPrimitive& primitive : model->primitives) {
        if (primitive.vertices.empty() || primitive.indices.empty()) {
            continue;
        }

        const bool instanced = !primitive.instances.empty();

        // Determine if this primitive should use terrain-specific lightweight vertex format
        const bool useTerrainFormat = primitive.hasTerrainWaterMaskMetadata;

        auto appendPrimitiveResource =
            [&](std::vector<GltfGpuVertex>&& verts,
                const Vec3& sortCenter,
                const std::vector<GltfGpuInstance>* instanceData) {
                GltfPrimitiveRenderResources resources;
                resources.useTerrainVertexFormat = false;
                BufferDesc vbDesc;
                vbDesc.size = verts.size() * sizeof(GltfGpuVertex);
                vbDesc.data = verts.data();
                vbDesc.usage = animated
                    ? BufferDesc::Usage::Dynamic
                    : BufferDesc::Usage::Static;
                vbDesc.type = BufferDesc::Type::Vertex;
                resources.vertexBuffer = device->createBuffer(vbDesc);

                BufferDesc ibDesc;
                ibDesc.size = primitive.indices.size() * sizeof(uint32_t);
                ibDesc.data = primitive.indices.data();
                ibDesc.usage = BufferDesc::Usage::Static;
                ibDesc.type = BufferDesc::Type::Index;
                resources.indexBuffer = device->createBuffer(ibDesc);
                resources.vertexCount =
                    static_cast<int>(primitive.vertices.size());
                resources.indexCount =
                    static_cast<int>(primitive.indices.size());
                resources.primitiveMode = primitive.primitiveMode;
                resources.sortCenterEcef = sortCenter;
                resources.baseColorFactor = primitive.baseColorFactor;
                resources.metallicFactor = primitive.metallicFactor;
                resources.roughnessFactor = primitive.roughnessFactor;
                resources.dielectricSpecularF0 =
                    primitive.dielectricSpecularF0;
                resources.specularFactor = primitive.specularFactor;
                resources.specularColorFactor =
                    primitive.specularColorFactor;
                resources.specularGlossinessWorkflow =
                    primitive.specularGlossinessWorkflow;
                resources.specularGlossinessSpecularFactor =
                    primitive.specularGlossinessSpecularFactor;
                resources.specularGlossinessGlossinessFactor =
                    primitive.specularGlossinessGlossinessFactor;
                resources.transmissionFactor = primitive.transmissionFactor;
                resources.anisotropyStrength =
                    primitive.anisotropyStrength;
                resources.anisotropyRotation =
                    primitive.anisotropyRotation;
                resources.clearcoatFactor = primitive.clearcoatFactor;
                resources.clearcoatRoughnessFactor =
                    primitive.clearcoatRoughnessFactor;
                resources.clearcoatNormalTextureScale =
                    primitive.clearcoatNormalTextureScale;
                resources.sheenColorFactor = primitive.sheenColorFactor;
                resources.sheenRoughnessFactor =
                    primitive.sheenRoughnessFactor;
                resources.normalTextureScale = primitive.normalTextureScale;
                resources.occlusionTextureStrength =
                    primitive.occlusionTextureStrength;
                resources.emissiveFactor = primitive.emissiveFactor;
                resources.alphaMode = primitive.alphaMode;
                resources.alphaCutoff = primitive.alphaCutoff;
                resources.doubleSided = primitive.doubleSided;
                resources.unlit = primitive.unlit;
                resources.dynamicVertices = animated;
                resources.animationRevision =
                    model->currentAnimationRevision();
                resources.hasTerrainWaterMaskMetadata =
                    primitive.hasTerrainWaterMaskMetadata;
                resources.terrainOnlyWater = primitive.terrainOnlyWater;
                resources.terrainOnlyLand = primitive.terrainOnlyLand;
                resources.terrainWaterMaskTranslationScale = {
                    static_cast<float>(primitive.terrainWaterMaskTranslationX),
                    static_cast<float>(primitive.terrainWaterMaskTranslationY),
                    static_cast<float>(primitive.terrainWaterMaskScale),
                    0.0f};
                if (primitive.terrainWaterMaskTextureIndex &&
                    *primitive.terrainWaterMaskTextureIndex <
                        tile.content.renderContent
                            .gltfTextureResourcesForBinding()
                            .size()) {
                    resources.terrainWaterMaskTexture =
                        tile.content.renderContent
                            .gltfTextureResourcesForBinding()
                            [*primitive.terrainWaterMaskTextureIndex]
                                .get();
                }
                std::optional<GltfTextureBinding> baseColorBinding =
                    primitive.baseColorTexture;
                if (!baseColorBinding && primitive.baseColorTextureIndex) {
                    GltfTextureBinding binding;
                    binding.textureIndex = *primitive.baseColorTextureIndex;
                    baseColorBinding = binding;
                }
                resources.baseColorTexture = makeGltfTextureBinding(
                    baseColorBinding,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.metallicRoughnessTexture = makeGltfTextureBinding(
                    primitive.metallicRoughnessTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.anisotropyTexture = makeGltfTextureBinding(
                    primitive.anisotropyTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.specularTexture = makeGltfTextureBinding(
                    primitive.specularTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.specularColorTexture = makeGltfTextureBinding(
                    primitive.specularColorTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.specularGlossinessTexture = makeGltfTextureBinding(
                    primitive.specularGlossinessTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.transmissionTexture = makeGltfTextureBinding(
                    primitive.transmissionTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.clearcoatTexture = makeGltfTextureBinding(
                    primitive.clearcoatTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.clearcoatRoughnessTexture = makeGltfTextureBinding(
                    primitive.clearcoatRoughnessTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.clearcoatNormalTexture = makeGltfTextureBinding(
                    primitive.clearcoatNormalTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.sheenColorTexture = makeGltfTextureBinding(
                    primitive.sheenColorTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.sheenRoughnessTexture = makeGltfTextureBinding(
                    primitive.sheenRoughnessTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.normalTexture = makeGltfTextureBinding(
                    primitive.normalTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.occlusionTexture = makeGltfTextureBinding(
                    primitive.occlusionTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                resources.emissiveTexture = makeGltfTextureBinding(
                    primitive.emissiveTexture,
                    tile.content.renderContent.gltfTextureResourcesForBinding());
                auto bindingHasTexCoordSet =
                    [&](const std::optional<GltfTextureBinding>& binding) {
                        return !binding ||
                               GltfRenderGeometryBuilder::primitiveHasTexCoordSet(
                                   primitive,
                                   binding->texCoord);
                    };
                if ((baseColorBinding &&
                     !resources.baseColorTexture.texture) ||
                    (primitive.metallicRoughnessTexture &&
                     !resources.metallicRoughnessTexture.texture) ||
                    (primitive.anisotropyTexture &&
                     !resources.anisotropyTexture.texture) ||
                    (primitive.specularTexture &&
                     !resources.specularTexture.texture) ||
                    (primitive.specularColorTexture &&
                     !resources.specularColorTexture.texture) ||
                    (primitive.specularGlossinessTexture &&
                     !resources.specularGlossinessTexture.texture) ||
                    (primitive.transmissionTexture &&
                     !resources.transmissionTexture.texture) ||
                    (primitive.clearcoatTexture &&
                     !resources.clearcoatTexture.texture) ||
                    (primitive.clearcoatRoughnessTexture &&
                     !resources.clearcoatRoughnessTexture.texture) ||
                    (primitive.clearcoatNormalTexture &&
                     !resources.clearcoatNormalTexture.texture) ||
                    (primitive.sheenColorTexture &&
                     !resources.sheenColorTexture.texture) ||
                    (primitive.sheenRoughnessTexture &&
                     !resources.sheenRoughnessTexture.texture) ||
                    (primitive.normalTexture &&
                     !resources.normalTexture.texture) ||
                    (primitive.occlusionTexture &&
                     !resources.occlusionTexture.texture) ||
                    (primitive.emissiveTexture &&
                     !resources.emissiveTexture.texture) ||
                    !bindingHasTexCoordSet(baseColorBinding) ||
                    !bindingHasTexCoordSet(
                        primitive.metallicRoughnessTexture) ||
                    !bindingHasTexCoordSet(primitive.anisotropyTexture) ||
                    !bindingHasTexCoordSet(primitive.specularTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.specularColorTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.specularGlossinessTexture) ||
                    !bindingHasTexCoordSet(primitive.transmissionTexture) ||
                    !bindingHasTexCoordSet(primitive.clearcoatTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.clearcoatRoughnessTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.clearcoatNormalTexture) ||
                    !bindingHasTexCoordSet(primitive.sheenColorTexture) ||
                    !bindingHasTexCoordSet(
                        primitive.sheenRoughnessTexture) ||
                    !bindingHasTexCoordSet(primitive.normalTexture) ||
                    !bindingHasTexCoordSet(primitive.occlusionTexture) ||
                    !bindingHasTexCoordSet(primitive.emissiveTexture)) {
                    return false;
                }

                if (instanceData) {
                    BufferDesc instanceDesc;
                    instanceDesc.size =
                        instanceData->size() * sizeof(GltfGpuInstance);
                    instanceDesc.data = instanceData->data();
                    instanceDesc.usage = BufferDesc::Usage::Static;
                    instanceDesc.type = BufferDesc::Type::Vertex;
                    resources.instanceBuffer =
                        device->createBuffer(instanceDesc);
                    resources.instanceCount =
                        static_cast<int>(instanceData->size());
                }

                if (!resources.vertexBuffer ||
                    !resources.indexBuffer ||
                    (instanceData && !resources.instanceBuffer)) {
                    return false;
                }

                tile.content.renderContent.addGltfPrimitiveResource(
                    std::move(resources));
                return true;
            };

        if (GltfRenderGeometryBuilder::primitiveUsesSplitBlendInstances(
                primitive)) {
            const Vec3 centroid =
                GltfRenderGeometryBuilder::primitiveCentroid(primitive);
            for (const GltfInstance& instance : primitive.instances) {
                const Mat4 instanceTransform(
                    tile.content.renderContent.gltfTransform().raw() *
                    instance.transform.raw());
                std::vector<GltfGpuVertex> verts =
                    GltfRenderGeometryBuilder::buildVertices(
                        primitive,
                        instanceTransform,
                        tile.content.renderContent.renderLocalOrigin(),
                        false);
                const Vec3 sortCenter =
                    GltfRenderGeometryBuilder::transformPoint(
                        instanceTransform.raw(),
                        centroid);
                if (!appendPrimitiveResource(
                        std::move(verts),
                        sortCenter,
                        nullptr)) {
                    resourceFailure = true;
                    break;
                }
            }
            if (resourceFailure) {
                break;
            }
            continue;
        }

        // Use lightweight terrain vertex format for terrain content
        if (useTerrainFormat && !instanced) {
            std::vector<TerrainGpuVertex> terrainVerts =
                GltfRenderGeometryBuilder::buildTerrainVertices(
                    primitive,
                    tile.content.renderContent.gltfTransform(),
                    tile.content.renderContent.renderLocalOrigin());

            GltfPrimitiveRenderResources resources;
            resources.useTerrainVertexFormat = true;
            BufferDesc vbDesc;
            vbDesc.size = terrainVerts.size() * sizeof(TerrainGpuVertex);
            vbDesc.data = terrainVerts.data();
            vbDesc.usage = BufferDesc::Usage::Static;
            vbDesc.type = BufferDesc::Type::Vertex;
            resources.vertexBuffer = device->createBuffer(vbDesc);

            BufferDesc ibDesc;
            ibDesc.size = primitive.indices.size() * sizeof(uint32_t);
            ibDesc.data = primitive.indices.data();
            ibDesc.usage = BufferDesc::Usage::Static;
            ibDesc.type = BufferDesc::Type::Index;
            resources.indexBuffer = device->createBuffer(ibDesc);
            resources.vertexCount = static_cast<int>(primitive.vertices.size());
            resources.indexCount = static_cast<int>(primitive.indices.size());
            resources.primitiveMode = primitive.primitiveMode;
            resources.sortCenterEcef = GltfRenderGeometryBuilder::primitiveSortCenterEcef(
                primitive, tile.content.renderContent.gltfTransform());
            // Set minimal material properties for terrain
            resources.baseColorFactor = primitive.baseColorFactor;
            resources.metallicFactor = 0.0f;
            resources.roughnessFactor = 1.0f;
            resources.unlit = false;
            resources.hasTerrainWaterMaskMetadata = true;
            resources.terrainOnlyWater = primitive.terrainOnlyWater;
            resources.terrainOnlyLand = primitive.terrainOnlyLand;
            resources.terrainWaterMaskTranslationScale = {
                static_cast<float>(primitive.terrainWaterMaskTranslationX),
                static_cast<float>(primitive.terrainWaterMaskTranslationY),
                static_cast<float>(primitive.terrainWaterMaskScale),
                0.0f};
            if (primitive.terrainWaterMaskTextureIndex &&
                *primitive.terrainWaterMaskTextureIndex <
                    tile.content.renderContent.gltfTextureResourcesForBinding().size()) {
                resources.terrainWaterMaskTexture =
                    tile.content.renderContent.gltfTextureResourcesForBinding()
                    [*primitive.terrainWaterMaskTextureIndex].get();
            }
            // Set base color texture for terrain
            std::optional<GltfTextureBinding> baseColorBinding = primitive.baseColorTexture;
            if (!baseColorBinding && primitive.baseColorTextureIndex) {
                GltfTextureBinding binding;
                binding.textureIndex = *primitive.baseColorTextureIndex;
                baseColorBinding = binding;
            }
            resources.baseColorTexture = makeGltfTextureBinding(
                baseColorBinding,
                tile.content.renderContent.gltfTextureResourcesForBinding());

            if (!resources.vertexBuffer || !resources.indexBuffer) {
                resourceFailure = true;
                break;
            }
            tile.content.renderContent.addGltfPrimitiveResource(std::move(resources));
            continue;
        }

        std::vector<GltfGpuVertex> verts =
            GltfRenderGeometryBuilder::buildVertices(
                primitive,
                tile.content.renderContent.gltfTransform(),
                tile.content.renderContent.renderLocalOrigin());

        std::vector<GltfGpuInstance> instances;
        if (instanced) {
            instances = GltfRenderGeometryBuilder::buildInstances(
                primitive,
                tile.content.renderContent.gltfTransform(),
                tile.content.renderContent.renderLocalOrigin());
        }

        const std::vector<GltfGpuInstance>* instanceData =
            instanced ? &instances : nullptr;
        if (!appendPrimitiveResource(
                std::move(verts),
                GltfRenderGeometryBuilder::primitiveSortCenterEcef(
                    primitive,
                    tile.content.renderContent.gltfTransform()),
                instanceData)) {
            resourceFailure = true;
            break;
        }
    }

    if (resourceFailure) {
        tile.content.renderContent.clearGltfGpuResources();
    }

    if (!resourceFailure && tile.content.renderContent.hasGltfPrimitiveResources()) {
        tile.markRenderContentDone();
    } else {
        tile.markRenderContentFailedTemporarily();
    }
}

// ============================================================
// Phase 1: CPU preparation (Worker Thread)
// ============================================================

std::optional<GpuReadyData> GltfRenderResourcePreparer::prepareCpuWork(
    const TilesetTile& tile,
    double currentFrameTimeSeconds) {
    const GltfModel* model = tile.content.renderContent.gltfModelForRead();
    if (!model || model->primitives.empty()) return std::nullopt;

    GpuReadyData ready;
    const Mat4& transform = tile.content.renderContent.gltfTransform();
    const Vec3& localOrigin = tile.content.renderContent.renderLocalOrigin();

    for (const GltfPrimitive& primitive : model->primitives) {
        if (primitive.vertices.empty() || primitive.indices.empty()) {
            continue;
        }

        GpuReadyPrimitive gpuPrim;
        const bool useTerrainFormat = primitive.hasTerrainWaterMaskMetadata;
        const bool instanced = !primitive.instances.empty();

        // CPU work: build vertex data
        if (useTerrainFormat && !instanced) {
            auto terrainVerts = GltfRenderGeometryBuilder::buildTerrainVertices(
                primitive, transform, localOrigin);
            gpuPrim.vertexBytes.resize(terrainVerts.size() * sizeof(TerrainGpuVertex));
            memcpy(gpuPrim.vertexBytes.data(), terrainVerts.data(), gpuPrim.vertexBytes.size());
            gpuPrim.vertexStride = sizeof(TerrainGpuVertex);
            gpuPrim.vertexCount = terrainVerts.size();
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

        // Index data (just copy)
        gpuPrim.indices = primitive.indices;
        gpuPrim.indexCount = primitive.indices.size();

        // Sort center
        gpuPrim.sortCenterEcef =
            GltfRenderGeometryBuilder::primitiveSortCenterEcef(primitive, transform);

        // CPU work: decode textures (only for non-terrain, terrain textures
        // are already uploaded by the raster overlay system)
        if (!useTerrainFormat) {
            for (const GltfTexture& tex : model->textures) {
                GpuReadyPrimitive::TextureData texData;
                if (tex.image.width <= 0 || tex.image.height <= 0 ||
                    tex.image.pixels.empty()) {
                    continue;
                }
                const size_t pixelCount =
                    static_cast<size_t>(tex.image.width) *
                    static_cast<size_t>(tex.image.height);
                texData.width = tex.image.width;
                texData.height = tex.image.height;
                texData.mipmap = tex.sampler.mipmap;

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
                gpuPrim.textures.push_back(std::move(texData));
            }
        }

        // Extract metadata (material properties, etc.)
        // This copies all the scalar/vector properties but leaves
        // GPU pointers (vertexBuffer, indexBuffer, texture) as nullptr.
        GltfPrimitiveRenderResources& meta = gpuPrim.metadata;
        meta.useTerrainVertexFormat = useTerrainFormat;
        meta.vertexCount = static_cast<int>(primitive.vertices.size());
        meta.indexCount = static_cast<int>(primitive.indices.size());
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

    return ready.valid() ? std::make_optional(std::move(ready)) : std::nullopt;
}

std::optional<GpuReadyData> GltfRenderResourcePreparer::prepareCpuWorkFromModel(
    const GltfModel& model,
    const Mat4& transform,
    const Vec3& localOrigin,
    double /*currentFrameTimeSeconds*/) {
    if (model.primitives.empty()) return std::nullopt;

    GpuReadyData ready;

    for (const GltfPrimitive& primitive : model.primitives) {
        if (primitive.vertices.empty() || primitive.indices.empty()) {
            continue;
        }

        GpuReadyPrimitive gpuPrim;
        const bool useTerrainFormat = primitive.hasTerrainWaterMaskMetadata;
        const bool instanced = !primitive.instances.empty();

        // CPU work: build vertex data
        if (useTerrainFormat && !instanced) {
            auto terrainVerts = GltfRenderGeometryBuilder::buildTerrainVertices(
                primitive, transform, localOrigin);
            gpuPrim.vertexBytes.resize(terrainVerts.size() * sizeof(TerrainGpuVertex));
            memcpy(gpuPrim.vertexBytes.data(), terrainVerts.data(), gpuPrim.vertexBytes.size());
            gpuPrim.vertexStride = sizeof(TerrainGpuVertex);
            gpuPrim.vertexCount = terrainVerts.size();
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

        // Index data (just copy)
        gpuPrim.indices = primitive.indices;
        gpuPrim.indexCount = primitive.indices.size();

        // Sort center
        gpuPrim.sortCenterEcef =
            GltfRenderGeometryBuilder::primitiveSortCenterEcef(primitive, transform);

        // CPU work: decode textures (only for non-terrain)
        if (!useTerrainFormat) {
            for (const GltfTexture& tex : model.textures) {
                GpuReadyPrimitive::TextureData texData;
                if (tex.image.width <= 0 || tex.image.height <= 0 ||
                    tex.image.pixels.empty()) {
                    continue;
                }
                const size_t pixelCount =
                    static_cast<size_t>(tex.image.width) *
                    static_cast<size_t>(tex.image.height);
                texData.width = tex.image.width;
                texData.height = tex.image.height;
                texData.mipmap = tex.sampler.mipmap;

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
                gpuPrim.textures.push_back(std::move(texData));
            }
        }

        // Copy all scalar/vector metadata but leave GPU pointers as nullptr.
        GltfPrimitiveRenderResources& meta = gpuPrim.metadata;
        meta.useTerrainVertexFormat = useTerrainFormat;
        meta.vertexCount = static_cast<int>(primitive.vertices.size());
        meta.indexCount = static_cast<int>(primitive.indices.size());
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

    return ready.valid() ? std::make_optional(std::move(ready)) : std::nullopt;
}

// ============================================================
// Phase 2: GPU upload (Main Thread — GL context)
// ============================================================

bool GltfRenderResourcePreparer::uploadToGpu(
    TilesetTile& tile,
    RenderDevice* device,
    GpuReadyData&& ready) {
    if (!device || !ready.valid()) return false;

    // Upload textures first (they're referenced by index in metadata)
    std::vector<std::unique_ptr<Texture>> gpuTextures;
    if (!ready.primitives.empty() && !ready.primitives[0].textures.empty()) {
        gpuTextures.reserve(ready.primitives[0].textures.size());
        for (const auto& texData : ready.primitives[0].textures) {
            TextureDesc td;
            td.width = texData.width;
            td.height = texData.height;
            td.format = texData.channels == 1
                ? TextureDesc::Format::R8
                : TextureDesc::Format::RGBA8;
            td.data = texData.pixels.data();
            td.dataSize = texData.pixels.size();
            td.mipmap = texData.mipmap;
            td.minFilter = texData.mipmap
                ? TextureDesc::Filter::Linear
                : TextureDesc::Filter::Nearest;
            td.magFilter = TextureDesc::Filter::Linear;
            gpuTextures.push_back(device->createTexture(td));
        }
    }

    bool success = true;
    for (auto& prim : ready.primitives) {
        // GPU: create vertex buffer
        BufferDesc vbDesc;
        vbDesc.size = prim.vertexBytes.size();
        vbDesc.data = prim.vertexBytes.data();
        vbDesc.usage = prim.metadata.dynamicVertices
            ? BufferDesc::Usage::Dynamic
            : BufferDesc::Usage::Static;
        vbDesc.type = BufferDesc::Type::Vertex;
        prim.metadata.vertexBuffer = device->createBuffer(vbDesc);

        // GPU: create index buffer
        BufferDesc ibDesc;
        ibDesc.size = prim.indices.size() * sizeof(uint32_t);
        ibDesc.data = prim.indices.data();
        ibDesc.usage = BufferDesc::Usage::Static;
        ibDesc.type = BufferDesc::Type::Index;
        prim.metadata.indexBuffer = device->createBuffer(ibDesc);

        // GPU: create instance buffer if needed
        if (prim.instances) {
            BufferDesc instDesc;
            instDesc.size = prim.instances->bytes.size();
            instDesc.data = prim.instances->bytes.data();
            instDesc.usage = BufferDesc::Usage::Static;
            instDesc.type = BufferDesc::Type::Vertex;
            prim.metadata.instanceBuffer = device->createBuffer(instDesc);
        }

        // Validate
        if (!prim.metadata.vertexBuffer || !prim.metadata.indexBuffer) {
            success = false;
            continue;
        }

        // Set texture bindings (raw pointers into gpuTextures)
        if (!gpuTextures.empty()) {
            auto bindTexture = [&](GltfPrimitiveRenderResources::TextureBinding& binding,
                                   const std::optional<GltfTextureBinding>& modelBinding) {
                if (modelBinding && modelBinding->textureIndex < gpuTextures.size()) {
                    binding.texture = gpuTextures[modelBinding->textureIndex].get();
                    binding.texCoord = modelBinding->texCoord;
                    binding.offsetScale = {
                        modelBinding->transform.offset[0],
                        modelBinding->transform.offset[1],
                        modelBinding->transform.scale[0],
                        modelBinding->transform.scale[1]};
                    binding.rotationSinCos = {
                        static_cast<float>(std::sin(modelBinding->transform.rotation)),
                        static_cast<float>(std::cos(modelBinding->transform.rotation))};
                }
            };
            // For terrain, only bind base color
            if (prim.metadata.useTerrainVertexFormat) {
                std::optional<GltfTextureBinding> baseColorBinding;
                // Terrain textures are handled by raster overlay system,
                // not by the GLTF texture system. Skip texture binding.
            }
        }

        tile.content.renderContent.addGltfPrimitiveResource(
            std::move(prim.metadata));
    }

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
        tile.markRenderContentDone();
    } else {
        tile.markRenderContentFailedTemporarily();
    }

    return success;
}

} // namespace earth_engine
