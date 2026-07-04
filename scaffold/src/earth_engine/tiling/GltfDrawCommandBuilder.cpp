#include "GltfDrawCommandBuilder.h"

#include "RasterMappedToTilesetTile.h"
#include "SurfaceRasterBinding.h"
#include "TilesetTile.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../renderer/Renderer.h"

#include <utility>

namespace earth_engine {
namespace {

float alphaModeUniform(GltfAlphaMode mode) {
    switch (mode) {
        case GltfAlphaMode::Mask:
            return 1.0f;
        case GltfAlphaMode::Blend:
            return 2.0f;
        case GltfAlphaMode::Opaque:
        default:
            return 0.0f;
    }
}

RenderCommand::PrimitiveType renderPrimitiveType(GltfPrimitiveMode mode) {
    switch (mode) {
        case GltfPrimitiveMode::Points:
            return RenderCommand::PrimitiveType::Points;
        case GltfPrimitiveMode::Lines:
            return RenderCommand::PrimitiveType::Lines;
        case GltfPrimitiveMode::LineStrip:
            return RenderCommand::PrimitiveType::LineStrip;
        case GltfPrimitiveMode::TriangleStrip:
            return RenderCommand::PrimitiveType::TriangleStrip;
        case GltfPrimitiveMode::Triangles:
        case GltfPrimitiveMode::TriangleFan:
        default:
            return RenderCommand::PrimitiveType::Triangles;
    }
}

} // namespace

void GltfDrawCommandBuilder::build(
    Renderer& renderer,
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays,
    RenderCommandList& commands,
    const GltfDrawCommandBuildContext& context) {
    if (!tile.content.renderContent.hasGltfResources()) {
        return;
    }
    for (const GltfPrimitiveRenderResources& primitive :
         tile.content.renderContent.gltfPrimitiveResourcesForDraw()) {
        if (!primitive.vertexBuffer || !primitive.indexBuffer ||
            primitive.indexCount <= 0) {
            continue;
        }

        RenderCommand cmd;
        if (primitive.instanceCount > 0) {
            cmd = renderer.makeGltfPrimitiveInstancedCommand(
                primitive.vertexBuffer.get(),
                primitive.indexBuffer.get(),
                primitive.instanceBuffer.get(),
                primitive.indexCount,
                primitive.vertexCount,
                primitive.instanceCount);
        } else if (primitive.useTerrainVertexFormat) {
            // Terrain quantized-mesh primitive: 32-byte TerrainGpuVertex VBO
            // drawn with the dedicated lightweight terrain shader. The
            // per-command population below (raster overlays, water mask, clip,
            // lighting, opacity) stays identical — the terrain shader consumes
            // the subset of uniforms it declares and ignores the rest.
            cmd = renderer.makeTerrainPrimitiveCommand(
                primitive.vertexBuffer.get(),
                primitive.indexBuffer.get(),
                primitive.indexCount,
                primitive.vertexCount);
        } else {
            cmd = renderer.makeGltfPrimitiveCommand(
                primitive.vertexBuffer.get(),
                primitive.indexBuffer.get(),
                primitive.indexCount,
                primitive.vertexCount);
        }
        cmd.frameId = context.frameNumber;
        cmd.generation = context.generation;
        cmd.terrainRenderContent =
            tile.content.renderContent.isTerrainRenderContent();
        cmd.primitive = renderPrimitiveType(primitive.primitiveMode);
        const Vec3& localOrigin = tile.content.renderContent.renderLocalOrigin();
        GltfUniformBlock& u = cmd.gltfUniforms;
        u.modelOrigin = {
            static_cast<float>(localOrigin.x()),
            static_cast<float>(localOrigin.y()),
            static_cast<float>(localOrigin.z())};
        cmd.hasWorldSortCenter = true;
        cmd.worldSortCenter = {
            primitive.sortCenterEcef.x(),
            primitive.sortCenterEcef.y(),
            primitive.sortCenterEcef.z()};
        cmd.surfaceTransitionOpacity = context.transitionOpacity;
        u.renderOpacity = context.transitionOpacity;
        if (cmd.terrainRenderContent && context.surfaceClipUv) {
            cmd.surfaceClipUv = *context.surfaceClipUv;
            cmd.surfaceClipEnabled = 1.0f;
            u.clipUv = *context.surfaceClipUv;
            u.clipEnabled = 1.0f;
        }
        u.baseColor = {
            primitive.baseColorFactor[0],
            primitive.baseColorFactor[1],
            primitive.baseColorFactor[2],
            primitive.baseColorFactor[3]};
        u.hasBaseColorTexture =
            primitive.baseColorTexture.texture ? 1.0f : 0.0f;
        u.materialFactors = {
            primitive.metallicFactor,
            primitive.roughnessFactor,
            primitive.normalTextureScale,
            primitive.occlusionTextureStrength};
        u.dielectricSpecularF0 = primitive.dielectricSpecularF0;
        u.specularFactor = primitive.specularFactor;
        u.specularColorFactor = {
            primitive.specularColorFactor[0],
            primitive.specularColorFactor[1],
            primitive.specularColorFactor[2]};
        u.specularGlossinessWorkflow =
            primitive.specularGlossinessWorkflow ? 1.0f : 0.0f;
        u.specularGlossinessFactor = {
            primitive.specularGlossinessSpecularFactor[0],
            primitive.specularGlossinessSpecularFactor[1],
            primitive.specularGlossinessSpecularFactor[2],
            primitive.specularGlossinessGlossinessFactor};
        u.transmissionFactor = primitive.transmissionFactor;
        u.anisotropyFactors = {
            primitive.anisotropyStrength,
            primitive.anisotropyRotation};
        u.clearcoatFactors = {
            primitive.clearcoatFactor,
            primitive.clearcoatRoughnessFactor,
            primitive.clearcoatNormalTextureScale};
        u.sheenColorFactor = {
            primitive.sheenColorFactor[0],
            primitive.sheenColorFactor[1],
            primitive.sheenColorFactor[2]};
        u.sheenRoughnessFactor = primitive.sheenRoughnessFactor;
        u.hasMaterialTextures = {
            primitive.metallicRoughnessTexture.texture ? 1.0f : 0.0f,
            primitive.normalTexture.texture ? 1.0f : 0.0f,
            primitive.occlusionTexture.texture ? 1.0f : 0.0f,
            primitive.emissiveTexture.texture ? 1.0f : 0.0f};
        u.hasAnisotropyTexture =
            primitive.anisotropyTexture.texture ? 1.0f : 0.0f;
        u.hasSpecularTextures = {
            primitive.specularTexture.texture ? 1.0f : 0.0f,
            primitive.specularColorTexture.texture ? 1.0f : 0.0f};
        u.hasSpecularGlossinessTexture =
            primitive.specularGlossinessTexture.texture ? 1.0f : 0.0f;
        u.hasTransmissionTexture =
            primitive.transmissionTexture.texture ? 1.0f : 0.0f;
        u.hasClearcoatTextures = {
            primitive.clearcoatTexture.texture ? 1.0f : 0.0f,
            primitive.clearcoatRoughnessTexture.texture ? 1.0f : 0.0f,
            primitive.clearcoatNormalTexture.texture ? 1.0f : 0.0f};
        u.hasSheenTextures = {
            primitive.sheenColorTexture.texture ? 1.0f : 0.0f,
            primitive.sheenRoughnessTexture.texture ? 1.0f : 0.0f};
        u.emissiveFactor = {
            primitive.emissiveFactor[0],
            primitive.emissiveFactor[1],
            primitive.emissiveFactor[2]};
        u.textureCoordSets = {
            static_cast<float>(primitive.baseColorTexture.texCoord),
            static_cast<float>(primitive.metallicRoughnessTexture.texCoord),
            static_cast<float>(primitive.normalTexture.texCoord),
            static_cast<float>(primitive.occlusionTexture.texCoord)};
        u.emissiveTexCoordSet =
            static_cast<float>(primitive.emissiveTexture.texCoord);
        u.anisotropyTexCoordSet =
            static_cast<float>(primitive.anisotropyTexture.texCoord);
        u.specularTexCoordSets = {
            static_cast<float>(primitive.specularTexture.texCoord),
            static_cast<float>(primitive.specularColorTexture.texCoord)};
        u.specularGlossinessTexCoordSet =
            static_cast<float>(primitive.specularGlossinessTexture.texCoord);
        u.transmissionTexCoordSet =
            static_cast<float>(primitive.transmissionTexture.texCoord);
        u.clearcoatTexCoordSets = {
            static_cast<float>(primitive.clearcoatTexture.texCoord),
            static_cast<float>(primitive.clearcoatRoughnessTexture.texCoord),
            static_cast<float>(primitive.clearcoatNormalTexture.texCoord)};
        u.sheenTexCoordSets = {
            static_cast<float>(primitive.sheenColorTexture.texCoord),
            static_cast<float>(primitive.sheenRoughnessTexture.texCoord)};
        u.alphaMode = alphaModeUniform(primitive.alphaMode);
        u.alphaCutoff = primitive.alphaCutoff;
        u.unlit = primitive.unlit ? 1.0f : 0.0f;

        auto setTransform = [](
            GltfUniformBlock::TextureTransform& transform,
            const GltfPrimitiveRenderResources::TextureBinding& binding) {
            transform.offsetScale = binding.offsetScale;
            transform.rotationSinCos = binding.rotationSinCos;
        };
        setTransform(u.baseColorTex, primitive.baseColorTexture);
        setTransform(u.metallicRoughnessTex, primitive.metallicRoughnessTexture);
        setTransform(u.anisotropyTex, primitive.anisotropyTexture);
        setTransform(u.specularTex, primitive.specularTexture);
        setTransform(u.specularColorTex, primitive.specularColorTexture);
        setTransform(
            u.specularGlossinessTex, primitive.specularGlossinessTexture);
        setTransform(u.transmissionTex, primitive.transmissionTexture);
        setTransform(u.clearcoatTex, primitive.clearcoatTexture);
        setTransform(
            u.clearcoatRoughnessTex, primitive.clearcoatRoughnessTexture);
        setTransform(u.clearcoatNormalTex, primitive.clearcoatNormalTexture);
        setTransform(u.sheenColorTex, primitive.sheenColorTexture);
        setTransform(u.sheenRoughnessTex, primitive.sheenRoughnessTexture);
        setTransform(u.normalTex, primitive.normalTexture);
        setTransform(u.occlusionTex, primitive.occlusionTexture);
        setTransform(u.emissiveTex, primitive.emissiveTexture);

        cmd.textures.resize(15, nullptr);
        cmd.textures[0] = primitive.baseColorTexture.texture;
        cmd.textures[1] = primitive.metallicRoughnessTexture.texture;
        cmd.textures[2] = primitive.normalTexture.texture;
        cmd.textures[3] = primitive.occlusionTexture.texture;
        cmd.textures[4] = primitive.emissiveTexture.texture;
        cmd.textures[5] = primitive.specularTexture.texture;
        cmd.textures[6] = primitive.specularColorTexture.texture;
        cmd.textures[7] = primitive.clearcoatTexture.texture;
        cmd.textures[8] = primitive.clearcoatRoughnessTexture.texture;
        cmd.textures[9] = primitive.clearcoatNormalTexture.texture;
        cmd.textures[10] = primitive.sheenColorTexture.texture;
        cmd.textures[11] = primitive.sheenRoughnessTexture.texture;
        cmd.textures[12] = primitive.anisotropyTexture.texture;
        cmd.textures[13] = primitive.specularGlossinessTexture.texture;
        cmd.textures[14] = primitive.transmissionTexture.texture;
        const bool primitiveHasWaterMask =
            primitive.hasTerrainWaterMaskMetadata &&
            !primitive.terrainOnlyLand &&
            (primitive.terrainOnlyWater ||
             primitive.terrainWaterMaskTexture != nullptr);
        if (primitiveHasWaterMask) {
            if (cmd.textures.size() <=
                static_cast<size_t>(kGltfWaterMaskTextureSlot)) {
                cmd.textures.resize(
                    static_cast<size_t>(kGltfWaterMaskTextureSlot) + 1u,
                    nullptr);
            }
            cmd.textures[kGltfWaterMaskTextureSlot] =
                primitive.terrainWaterMaskTexture;
            cmd.gltfHasWaterMask = 1.0f;
            cmd.gltfWaterMaskTranslationScale =
                primitive.terrainWaterMaskTranslationScale;
            cmd.gltfWaterMaskState = {
                primitive.terrainOnlyLand ? 1.0f : 0.0f,
                primitive.terrainOnlyWater ? 1.0f : 0.0f,
                primitive.terrainWaterMaskTexture ? 1.0f : 0.0f,
                0.0f};
        }
        u.hasWaterMask = cmd.gltfHasWaterMask;
        u.waterMaskTranslationScale = cmd.gltfWaterMaskTranslationScale;
        u.waterMaskState = cmd.gltfWaterMaskState;
        int rasterOverlayTextureCount = 0;
        for (size_t i = 0;
             i < overlays.size() && i < tile.rasterOverlayState.mappingCount();
             ++i) {
            if (rasterOverlayTextureCount >= kMaxGltfRasterOverlays) {
                break;
            }
            ActivatedRasterOverlay* activeOverlay = overlays[i];
            const RasterMappedToTilesetTile* mapped =
                tile.rasterOverlayState.mappingAt(i);
            const SurfaceRasterBinding binding =
                chooseSurfaceRasterBinding(mapped);
            if (!rasterOverlayBindingAllowedByPolicy(
                    activeOverlay,
                    mapped,
                    binding)) {
                continue;
            }
            const int32_t textureCoordinateID =
                mapped ? mapped->getTextureCoordinateID() : -1;
            if (textureCoordinateID < 0 ||
                textureCoordinateID >= static_cast<int32_t>(kGltfMaxTexCoordSets)) {
                continue;
            }
            Texture* texture = binding.tile->getTexture();
            if (!texture) {
                continue;
            }
            const size_t textureSlot =
                static_cast<size_t>(kGltfRasterOverlayTextureBase +
                                    rasterOverlayTextureCount);
            if (cmd.textures.size() <= textureSlot) {
                cmd.textures.resize(textureSlot + 1u, nullptr);
            }
            cmd.textures[textureSlot] = texture;
            if (binding.tileHandle) {
                cmd.resourceKeepAlive.push_back(binding.tileHandle);
            }
            cmd.gltfRasterOverlayTileUvs[rasterOverlayTextureCount] = {
                binding.offsetU,
                binding.offsetV,
                binding.scaleU,
                binding.scaleV};
            cmd.gltfRasterOverlayOpacities[rasterOverlayTextureCount] =
                activeOverlay ? activeOverlay->opacity() : 1.0f;
            cmd.gltfRasterOverlayTexCoordSets[rasterOverlayTextureCount] =
                static_cast<float>(textureCoordinateID);
            ++rasterOverlayTextureCount;
        }
        cmd.gltfRasterOverlayTextureCount = rasterOverlayTextureCount;
        u.mappedRasterTextureCount =
            static_cast<float>(rasterOverlayTextureCount);
        for (int i = 0; i < kMaxGltfRasterOverlays; ++i) {
            u.mappedRasterTileUv[i] = cmd.gltfRasterOverlayTileUvs[i];
            u.mappedRasterOpacity[i] = cmd.gltfRasterOverlayOpacities[i];
            u.mappedRasterTexCoordSet[i] = cmd.gltfRasterOverlayTexCoordSets[i];
        }
        cmd.cullFace = !primitive.doubleSided;
        if (context.transitionOpacity < 0.999f ||
            primitive.alphaMode == GltfAlphaMode::Blend ||
            primitive.transmissionFactor > 0.0f) {
            cmd.blend = true;
            cmd.depthWrite = false;
            cmd.blendSrc = RenderCommand::BlendFactor::SrcAlpha;
            cmd.blendDst = RenderCommand::BlendFactorDst::OneMinusSrcAlpha;
        }
        commands.push_back(std::move(cmd));
    }
}

} // namespace earth_engine
