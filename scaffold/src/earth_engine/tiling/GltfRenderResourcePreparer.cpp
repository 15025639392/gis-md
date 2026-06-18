#include "GltfRenderResourcePreparer.h"

#include "GltfRenderGeometryBuilder.h"
#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "../renderer/RenderDevice.h"

#include <algorithm>
#include <cmath>
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
    if (texture.image.channels == 3) {
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
    textureDesc.format = TextureDesc::Format::RGBA8;
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
    if (!tile.gltfModel) return;
    const bool animated = tile.gltfModel->hasRuntimeAnimation();
    const bool animationChanged =
        animated && tile.gltfModel->updateAnimation(currentFrameTimeSeconds);
    size_t expectedResourceCount = 0;
    for (const GltfPrimitive& primitive : tile.gltfModel->primitives) {
        expectedResourceCount +=
            GltfRenderGeometryBuilder::primitiveRenderResourceCount(primitive);
    }
    const bool splitBlendInstances =
        GltfRenderGeometryBuilder::modelUsesSplitBlendInstances(
            *tile.gltfModel);
    if (expectedResourceCount > 0 &&
        tile.gltfPrimitiveResources.size() == expectedResourceCount) {
        const bool allReady = std::all_of(
            tile.gltfPrimitiveResources.begin(),
            tile.gltfPrimitiveResources.end(),
            [](const GltfPrimitiveRenderResources& resources) {
                return resources.vertexBuffer != nullptr &&
                       resources.indexBuffer != nullptr &&
                       resources.indexCount > 0 &&
                       (resources.instanceCount <= 0 ||
                        resources.instanceBuffer != nullptr);
            });
        if (allReady) {
            if (animated && animationChanged && splitBlendInstances) {
                tile.gltfPrimitiveResources.clear();
            } else if (animated && animationChanged && device) {
                tile.localOrigin = GltfRenderGeometryBuilder::localOrigin(
                    *tile.gltfModel,
                    tile.gltfContentTransform);
                bool updated = true;
                size_t resourceIndex = 0;
                for (const GltfPrimitive& primitive :
                     tile.gltfModel->primitives) {
                    if (primitive.vertices.empty() ||
                        primitive.indices.empty()) {
                        continue;
                    }
                    if (resourceIndex >= tile.gltfPrimitiveResources.size()) {
                        updated = false;
                        break;
                    }
                    GltfPrimitiveRenderResources& resources =
                        tile.gltfPrimitiveResources[resourceIndex++];
                    std::vector<GltfGpuVertex> verts =
                        GltfRenderGeometryBuilder::buildVertices(
                            primitive,
                            tile.gltfContentTransform,
                            tile.localOrigin);
                    const bool ok = resources.vertexBuffer &&
                        resources.vertexBuffer->size() ==
                            verts.size() * sizeof(GltfGpuVertex) &&
                        device->updateBuffer(
                            resources.vertexBuffer.get(),
                            0,
                            verts.data(),
                            verts.size() * sizeof(GltfGpuVertex));
                    if (!ok) {
                        updated = false;
                        break;
                    }
                    resources.sortCenterEcef =
                        GltfRenderGeometryBuilder::primitiveSortCenterEcef(
                            primitive,
                            tile.gltfContentTransform);
                    resources.animationRevision =
                        tile.gltfModel->currentAnimationRevision();
                }
                if (!updated) {
                    tile.gltfPrimitiveResources.clear();
                } else {
                    tile.meshReady = true;
                    tile.loadState = TileLoadState::Done;
                    tile.contentKind = TileContentKind::Render;
                    return;
                }
            } else {
                tile.meshReady = true;
                tile.loadState = TileLoadState::Done;
                tile.contentKind = TileContentKind::Render;
                return;
            }
        }
    }
    if (!device) return;
    tile.localOrigin = GltfRenderGeometryBuilder::localOrigin(
        *tile.gltfModel,
        tile.gltfContentTransform);

    tile.gltfTextureResources.clear();
    tile.gltfTextureResources.reserve(tile.gltfModel->textures.size());
    for (const GltfTexture& texture : tile.gltfModel->textures) {
        tile.gltfTextureResources.push_back(
            createGltfGpuTexture(device, texture));
    }

    tile.gltfPrimitiveResources.clear();
    tile.gltfPrimitiveResources.reserve(expectedResourceCount);
    bool resourceFailure = false;

    for (const GltfPrimitive& primitive : tile.gltfModel->primitives) {
        if (primitive.vertices.empty() || primitive.indices.empty()) {
            continue;
        }

        const bool instanced = !primitive.instances.empty();

        auto appendPrimitiveResource =
            [&](std::vector<GltfGpuVertex>&& verts,
                const Vec3& sortCenter,
                const std::vector<GltfGpuInstance>* instanceData) {
                GltfPrimitiveRenderResources resources;
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
                    tile.gltfModel->currentAnimationRevision();
                std::optional<GltfTextureBinding> baseColorBinding =
                    primitive.baseColorTexture;
                if (!baseColorBinding && primitive.baseColorTextureIndex) {
                    GltfTextureBinding binding;
                    binding.textureIndex = *primitive.baseColorTextureIndex;
                    baseColorBinding = binding;
                }
                resources.baseColorTexture = makeGltfTextureBinding(
                    baseColorBinding,
                    tile.gltfTextureResources);
                resources.metallicRoughnessTexture = makeGltfTextureBinding(
                    primitive.metallicRoughnessTexture,
                    tile.gltfTextureResources);
                resources.anisotropyTexture = makeGltfTextureBinding(
                    primitive.anisotropyTexture,
                    tile.gltfTextureResources);
                resources.specularTexture = makeGltfTextureBinding(
                    primitive.specularTexture,
                    tile.gltfTextureResources);
                resources.specularColorTexture = makeGltfTextureBinding(
                    primitive.specularColorTexture,
                    tile.gltfTextureResources);
                resources.specularGlossinessTexture = makeGltfTextureBinding(
                    primitive.specularGlossinessTexture,
                    tile.gltfTextureResources);
                resources.transmissionTexture = makeGltfTextureBinding(
                    primitive.transmissionTexture,
                    tile.gltfTextureResources);
                resources.clearcoatTexture = makeGltfTextureBinding(
                    primitive.clearcoatTexture,
                    tile.gltfTextureResources);
                resources.clearcoatRoughnessTexture = makeGltfTextureBinding(
                    primitive.clearcoatRoughnessTexture,
                    tile.gltfTextureResources);
                resources.clearcoatNormalTexture = makeGltfTextureBinding(
                    primitive.clearcoatNormalTexture,
                    tile.gltfTextureResources);
                resources.sheenColorTexture = makeGltfTextureBinding(
                    primitive.sheenColorTexture,
                    tile.gltfTextureResources);
                resources.sheenRoughnessTexture = makeGltfTextureBinding(
                    primitive.sheenRoughnessTexture,
                    tile.gltfTextureResources);
                resources.normalTexture = makeGltfTextureBinding(
                    primitive.normalTexture,
                    tile.gltfTextureResources);
                resources.occlusionTexture = makeGltfTextureBinding(
                    primitive.occlusionTexture,
                    tile.gltfTextureResources);
                resources.emissiveTexture = makeGltfTextureBinding(
                    primitive.emissiveTexture,
                    tile.gltfTextureResources);
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

                tile.gltfPrimitiveResources.push_back(std::move(resources));
                return true;
            };

        if (GltfRenderGeometryBuilder::primitiveUsesSplitBlendInstances(
                primitive)) {
            const Vec3 centroid =
                GltfRenderGeometryBuilder::primitiveCentroid(primitive);
            for (const GltfInstance& instance : primitive.instances) {
                const Mat4 instanceTransform(
                    tile.gltfContentTransform.raw() *
                    instance.transform.raw());
                std::vector<GltfGpuVertex> verts =
                    GltfRenderGeometryBuilder::buildVertices(
                        primitive,
                        instanceTransform,
                        tile.localOrigin,
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

        std::vector<GltfGpuVertex> verts =
            GltfRenderGeometryBuilder::buildVertices(
                primitive,
                tile.gltfContentTransform,
                tile.localOrigin);

        std::vector<GltfGpuInstance> instances;
        if (instanced) {
            instances = GltfRenderGeometryBuilder::buildInstances(
                primitive,
                tile.gltfContentTransform,
                tile.localOrigin);
        }

        const std::vector<GltfGpuInstance>* instanceData =
            instanced ? &instances : nullptr;
        if (!appendPrimitiveResource(
                std::move(verts),
                GltfRenderGeometryBuilder::primitiveSortCenterEcef(
                    primitive,
                    tile.gltfContentTransform),
                instanceData)) {
            resourceFailure = true;
            break;
        }
    }

    if (resourceFailure) {
        tile.gltfPrimitiveResources.clear();
        tile.gltfTextureResources.clear();
    }

    tile.meshReady =
        !resourceFailure && !tile.gltfPrimitiveResources.empty();
    tile.loadState = tile.meshReady
        ? TileLoadState::Done
        : TileLoadState::FailedTemporarily;
    tile.contentKind = tile.meshReady
        ? TileContentKind::Render
        : TileContentKind::Unknown;
}


} // namespace earth_engine
