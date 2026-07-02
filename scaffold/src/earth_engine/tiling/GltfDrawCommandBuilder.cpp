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
        cmd.uniforms["u_modelOrigin"] = {
            static_cast<float>(localOrigin.x()),
            static_cast<float>(localOrigin.y()),
            static_cast<float>(localOrigin.z())};
        cmd.hasWorldSortCenter = true;
        cmd.worldSortCenter = {
            primitive.sortCenterEcef.x(),
            primitive.sortCenterEcef.y(),
            primitive.sortCenterEcef.z()};
        cmd.surfaceTransitionOpacity = context.transitionOpacity;
        cmd.uniforms["u_renderOpacity"] = {context.transitionOpacity};
        if (cmd.terrainRenderContent && context.surfaceClipUv) {
            cmd.surfaceClipUv = *context.surfaceClipUv;
            cmd.surfaceClipEnabled = 1.0f;
            cmd.uniforms["u_clipUV"] = {
                (*context.surfaceClipUv)[0],
                (*context.surfaceClipUv)[1],
                (*context.surfaceClipUv)[2],
                (*context.surfaceClipUv)[3]};
            cmd.uniforms["u_clipEnabled"] = {1.0f};
        }
        cmd.uniforms["u_baseColor"] = {
            primitive.baseColorFactor[0],
            primitive.baseColorFactor[1],
            primitive.baseColorFactor[2],
            primitive.baseColorFactor[3]};
        cmd.uniforms["u_hasBaseColorTexture"] = {
            primitive.baseColorTexture.texture ? 1.0f : 0.0f};
        cmd.uniforms["u_materialFactors"] = {
            primitive.metallicFactor,
            primitive.roughnessFactor,
            primitive.normalTextureScale,
            primitive.occlusionTextureStrength};
        cmd.uniforms["u_dielectricSpecularF0"] = {
            primitive.dielectricSpecularF0};
        cmd.uniforms["u_specularFactor"] = {primitive.specularFactor};
        cmd.uniforms["u_specularColorFactor"] = {
            primitive.specularColorFactor[0],
            primitive.specularColorFactor[1],
            primitive.specularColorFactor[2]};
        cmd.uniforms["u_specularGlossinessWorkflow"] = {
            primitive.specularGlossinessWorkflow ? 1.0f : 0.0f};
        cmd.uniforms["u_specularGlossinessFactor"] = {
            primitive.specularGlossinessSpecularFactor[0],
            primitive.specularGlossinessSpecularFactor[1],
            primitive.specularGlossinessSpecularFactor[2],
            primitive.specularGlossinessGlossinessFactor};
        cmd.uniforms["u_transmissionFactor"] = {
            primitive.transmissionFactor};
        cmd.uniforms["u_anisotropyFactors"] = {
            primitive.anisotropyStrength,
            primitive.anisotropyRotation};
        cmd.uniforms["u_clearcoatFactors"] = {
            primitive.clearcoatFactor,
            primitive.clearcoatRoughnessFactor,
            primitive.clearcoatNormalTextureScale};
        cmd.uniforms["u_sheenColorFactor"] = {
            primitive.sheenColorFactor[0],
            primitive.sheenColorFactor[1],
            primitive.sheenColorFactor[2]};
        cmd.uniforms["u_sheenRoughnessFactor"] = {
            primitive.sheenRoughnessFactor};
        cmd.uniforms["u_hasMaterialTextures"] = {
            primitive.metallicRoughnessTexture.texture ? 1.0f : 0.0f,
            primitive.normalTexture.texture ? 1.0f : 0.0f,
            primitive.occlusionTexture.texture ? 1.0f : 0.0f,
            primitive.emissiveTexture.texture ? 1.0f : 0.0f};
        cmd.uniforms["u_hasAnisotropyTexture"] = {
            primitive.anisotropyTexture.texture ? 1.0f : 0.0f};
        cmd.uniforms["u_hasSpecularTextures"] = {
            primitive.specularTexture.texture ? 1.0f : 0.0f,
            primitive.specularColorTexture.texture ? 1.0f : 0.0f};
        cmd.uniforms["u_hasSpecularGlossinessTexture"] = {
            primitive.specularGlossinessTexture.texture ? 1.0f : 0.0f};
        cmd.uniforms["u_hasTransmissionTexture"] = {
            primitive.transmissionTexture.texture ? 1.0f : 0.0f};
        cmd.uniforms["u_hasClearcoatTextures"] = {
            primitive.clearcoatTexture.texture ? 1.0f : 0.0f,
            primitive.clearcoatRoughnessTexture.texture ? 1.0f : 0.0f,
            primitive.clearcoatNormalTexture.texture ? 1.0f : 0.0f};
        cmd.uniforms["u_hasSheenTextures"] = {
            primitive.sheenColorTexture.texture ? 1.0f : 0.0f,
            primitive.sheenRoughnessTexture.texture ? 1.0f : 0.0f};
        cmd.uniforms["u_emissiveFactor"] = {
            primitive.emissiveFactor[0],
            primitive.emissiveFactor[1],
            primitive.emissiveFactor[2]};
        cmd.uniforms["u_textureCoordSets"] = {
            static_cast<float>(primitive.baseColorTexture.texCoord),
            static_cast<float>(primitive.metallicRoughnessTexture.texCoord),
            static_cast<float>(primitive.normalTexture.texCoord),
            static_cast<float>(primitive.occlusionTexture.texCoord)};
        cmd.uniforms["u_emissiveTexCoordSet"] = {
            static_cast<float>(primitive.emissiveTexture.texCoord)};
        cmd.uniforms["u_anisotropyTexCoordSet"] = {
            static_cast<float>(primitive.anisotropyTexture.texCoord)};
        cmd.uniforms["u_specularTexCoordSets"] = {
            static_cast<float>(primitive.specularTexture.texCoord),
            static_cast<float>(primitive.specularColorTexture.texCoord)};
        cmd.uniforms["u_specularGlossinessTexCoordSet"] = {
            static_cast<float>(primitive.specularGlossinessTexture.texCoord)};
        cmd.uniforms["u_transmissionTexCoordSet"] = {
            static_cast<float>(primitive.transmissionTexture.texCoord)};
        cmd.uniforms["u_clearcoatTexCoordSets"] = {
            static_cast<float>(primitive.clearcoatTexture.texCoord),
            static_cast<float>(primitive.clearcoatRoughnessTexture.texCoord),
            static_cast<float>(primitive.clearcoatNormalTexture.texCoord)};
        cmd.uniforms["u_sheenTexCoordSets"] = {
            static_cast<float>(primitive.sheenColorTexture.texCoord),
            static_cast<float>(primitive.sheenRoughnessTexture.texCoord)};
        cmd.uniforms["u_alphaMode"] = {
            alphaModeUniform(primitive.alphaMode)};
        cmd.uniforms["u_alphaCutoff"] = {primitive.alphaCutoff};
        cmd.uniforms["u_unlit"] = {primitive.unlit ? 1.0f : 0.0f};

        auto setTransformUniforms = [&cmd](
            const char* offsetScaleName,
            const char* rotationName,
            const GltfPrimitiveRenderResources::TextureBinding& binding) {
            cmd.uniforms[offsetScaleName] = {
                binding.offsetScale[0],
                binding.offsetScale[1],
                binding.offsetScale[2],
                binding.offsetScale[3]};
            cmd.uniforms[rotationName] = {
                binding.rotationSinCos[0],
                binding.rotationSinCos[1]};
        };
        setTransformUniforms(
            "u_baseColorTexOffsetScale",
            "u_baseColorTexRotationSinCos",
            primitive.baseColorTexture);
        setTransformUniforms(
            "u_metallicRoughnessTexOffsetScale",
            "u_metallicRoughnessTexRotationSinCos",
            primitive.metallicRoughnessTexture);
        setTransformUniforms(
            "u_anisotropyTexOffsetScale",
            "u_anisotropyTexRotationSinCos",
            primitive.anisotropyTexture);
        setTransformUniforms(
            "u_specularTexOffsetScale",
            "u_specularTexRotationSinCos",
            primitive.specularTexture);
        setTransformUniforms(
            "u_specularColorTexOffsetScale",
            "u_specularColorTexRotationSinCos",
            primitive.specularColorTexture);
        setTransformUniforms(
            "u_specularGlossinessTexOffsetScale",
            "u_specularGlossinessTexRotationSinCos",
            primitive.specularGlossinessTexture);
        setTransformUniforms(
            "u_transmissionTexOffsetScale",
            "u_transmissionTexRotationSinCos",
            primitive.transmissionTexture);
        setTransformUniforms(
            "u_clearcoatTexOffsetScale",
            "u_clearcoatTexRotationSinCos",
            primitive.clearcoatTexture);
        setTransformUniforms(
            "u_clearcoatRoughnessTexOffsetScale",
            "u_clearcoatRoughnessTexRotationSinCos",
            primitive.clearcoatRoughnessTexture);
        setTransformUniforms(
            "u_clearcoatNormalTexOffsetScale",
            "u_clearcoatNormalTexRotationSinCos",
            primitive.clearcoatNormalTexture);
        setTransformUniforms(
            "u_sheenColorTexOffsetScale",
            "u_sheenColorTexRotationSinCos",
            primitive.sheenColorTexture);
        setTransformUniforms(
            "u_sheenRoughnessTexOffsetScale",
            "u_sheenRoughnessTexRotationSinCos",
            primitive.sheenRoughnessTexture);
        setTransformUniforms(
            "u_normalTexOffsetScale",
            "u_normalTexRotationSinCos",
            primitive.normalTexture);
        setTransformUniforms(
            "u_occlusionTexOffsetScale",
            "u_occlusionTexRotationSinCos",
            primitive.occlusionTexture);
        setTransformUniforms(
            "u_emissiveTexOffsetScale",
            "u_emissiveTexRotationSinCos",
            primitive.emissiveTexture);

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
        cmd.uniforms["u_gltfHasWaterMask"] = {cmd.gltfHasWaterMask};
        cmd.uniforms["u_gltfWaterMaskTranslationScale"] = {
            cmd.gltfWaterMaskTranslationScale[0],
            cmd.gltfWaterMaskTranslationScale[1],
            cmd.gltfWaterMaskTranslationScale[2],
            cmd.gltfWaterMaskTranslationScale[3]};
        cmd.uniforms["u_gltfWaterMaskState"] = {
            cmd.gltfWaterMaskState[0],
            cmd.gltfWaterMaskState[1],
            cmd.gltfWaterMaskState[2],
            cmd.gltfWaterMaskState[3]};
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
        cmd.uniforms["u_mappedRasterTextureCount"] = {
            static_cast<float>(rasterOverlayTextureCount)};
        for (int i = 0; i < kMaxGltfRasterOverlays; ++i) {
            cmd.uniforms[
                "u_mappedRasterTileUV" + std::to_string(i)] = {
                    cmd.gltfRasterOverlayTileUvs[i][0],
                    cmd.gltfRasterOverlayTileUvs[i][1],
                    cmd.gltfRasterOverlayTileUvs[i][2],
                    cmd.gltfRasterOverlayTileUvs[i][3]};
            cmd.uniforms[
                "u_mappedRasterOpacity" + std::to_string(i)] = {
                    cmd.gltfRasterOverlayOpacities[i]};
            cmd.uniforms[
                "u_mappedRasterTexCoordSet" + std::to_string(i)] = {
                    cmd.gltfRasterOverlayTexCoordSets[i]};
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
