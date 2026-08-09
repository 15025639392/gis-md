#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"

#include <string>

using namespace earth_engine;

namespace earth_engine {
namespace renderer_testing {
const char* gltfVertexGLSL();
const char* gltfFragmentGLSL();
const char* gltfFragmentMSL();
const char* gltfInstancedVertexGLSL();
const char* gltfInstancedVertexMSL();
} // namespace renderer_testing
} // namespace earth_engine

namespace {

class DummyTexture final : public Texture {
public:
    explicit DummyTexture(int id) : id_(id) {}
    int width() const override { return 256; }
    int height() const override { return 256; }
    size_t sizeBytes() const override { return 256u * 256u * 4u; }
    int id() const { return id_; }

private:
    int id_ = 0;
};

} // namespace

// ── Basic surface tile command creation ──

// ── Surface tile uniforms are set via hot-path fields ──

TEST(RendererCommandTest, GltfPrimitiveCommandHasCorrectDefaults) {
    Renderer renderer(nullptr);

    RenderCommand cmd = renderer.makeGltfPrimitiveCommand(
        nullptr,
        nullptr,
        36,
        24);

    EXPECT_EQ(RenderCommandKind::GltfPrimitive, cmd.kind);
    EXPECT_EQ("gltf_primitive", cmd.owner);
    EXPECT_EQ("color", cmd.pass);
    EXPECT_EQ(120, cmd.vertexStride);
    EXPECT_EQ(36, cmd.indexCount);
    EXPECT_EQ(24, cmd.vertexCount);
    EXPECT_EQ(RenderCommand::PrimitiveType::Triangles, cmd.primitive);
    EXPECT_EQ(RenderCommand::IndexType::UInt32, cmd.indexType);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_TRUE(cmd.depthWrite);
    EXPECT_TRUE(cmd.cullFace);
    EXPECT_FALSE(cmd.blend);
    // Uniforms live in the fixed-size block; the string map stays empty
    // (hot-path zero-allocation contract). Every former map default is now
    // a member initializer of GltfUniformBlock — assert those values.
    ASSERT_TRUE(cmd.hasGltfUniforms);
    EXPECT_TRUE(cmd.uniforms.empty());
    const GltfUniformBlock& u = cmd.gltfUniforms;
    EXPECT_EQ((std::array<float, 4>{0.82f, 0.84f, 0.88f, 1.0f}), u.baseColor);
    EXPECT_FLOAT_EQ(0.0f, u.hasBaseColorTexture);
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              u.materialFactors);
    EXPECT_FLOAT_EQ(0.04f, u.dielectricSpecularF0);
    EXPECT_EQ((std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
              u.hasMaterialTextures);
    EXPECT_EQ((std::array<float, 2>{0.0f, 0.0f}), u.anisotropyFactors);
    EXPECT_FLOAT_EQ(0.0f, u.hasAnisotropyTexture);
    EXPECT_EQ((std::array<float, 2>{0.0f, 0.0f}), u.hasSpecularTextures);
    EXPECT_FLOAT_EQ(1.0f, u.specularFactor);
    EXPECT_EQ((std::array<float, 3>{1.0f, 1.0f, 1.0f}), u.specularColorFactor);
    EXPECT_FLOAT_EQ(0.0f, u.specularGlossinessWorkflow);
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              u.specularGlossinessFactor);
    EXPECT_FLOAT_EQ(0.0f, u.hasSpecularGlossinessTexture);
    EXPECT_FLOAT_EQ(0.0f, u.transmissionFactor);
    EXPECT_FLOAT_EQ(0.0f, u.hasTransmissionTexture);
    EXPECT_EQ((std::array<float, 3>{0.0f, 0.0f, 1.0f}), u.clearcoatFactors);
    EXPECT_EQ((std::array<float, 3>{0.0f, 0.0f, 0.0f}),
              u.hasClearcoatTextures);
    EXPECT_EQ((std::array<float, 3>{0.0f, 0.0f, 0.0f}), u.sheenColorFactor);
    EXPECT_FLOAT_EQ(0.0f, u.sheenRoughnessFactor);
    EXPECT_EQ((std::array<float, 2>{0.0f, 0.0f}), u.hasSheenTextures);
    EXPECT_EQ((std::array<float, 3>{0.0f, 0.0f, 0.0f}), u.emissiveFactor);
    EXPECT_EQ((std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
              u.textureCoordSets);
    EXPECT_FLOAT_EQ(0.0f, u.emissiveTexCoordSet);
    EXPECT_FLOAT_EQ(0.0f, u.anisotropyTexCoordSet);
    EXPECT_EQ((std::array<float, 2>{0.0f, 0.0f}), u.specularTexCoordSets);
    EXPECT_FLOAT_EQ(0.0f, u.specularGlossinessTexCoordSet);
    EXPECT_FLOAT_EQ(0.0f, u.transmissionTexCoordSet);
    EXPECT_EQ((std::array<float, 3>{0.0f, 0.0f, 0.0f}),
              u.clearcoatTexCoordSets);
    EXPECT_EQ((std::array<float, 2>{0.0f, 0.0f}), u.sheenTexCoordSets);
    const std::array<float, 4> identityOffsetScale{0.0f, 0.0f, 1.0f, 1.0f};
    const std::array<float, 2> identityRotation{0.0f, 1.0f};
    for (const GltfUniformBlock::TextureTransform* transform :
         {&u.baseColorTex, &u.anisotropyTex, &u.specularTex,
          &u.specularColorTex, &u.specularGlossinessTex, &u.transmissionTex,
          &u.clearcoatTex, &u.clearcoatRoughnessTex, &u.clearcoatNormalTex,
          &u.sheenColorTex, &u.sheenRoughnessTex}) {
        EXPECT_EQ(identityOffsetScale, transform->offsetScale);
        EXPECT_EQ(identityRotation, transform->rotationSinCos);
    }
    EXPECT_FLOAT_EQ(0.0f, u.alphaMode);
    EXPECT_FLOAT_EQ(0.5f, u.alphaCutoff);
    EXPECT_FLOAT_EQ(1.0f, u.renderOpacity);
    EXPECT_FLOAT_EQ(0.0f, u.unlit);
    EXPECT_EQ((std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f}), u.clipUv);
    EXPECT_FLOAT_EQ(0.0f, u.clipEnabled);
}

TEST(RendererCommandTest, GltfGlesVertexShadersSetExplicitPointSize) {
    const std::string vertex = renderer_testing::gltfVertexGLSL();
    const std::string instancedVertex =
        renderer_testing::gltfInstancedVertexGLSL();

    EXPECT_NE(std::string::npos, vertex.find("gl_PointSize = 1.0;"));
    EXPECT_NE(std::string::npos, instancedVertex.find("gl_PointSize = 1.0;"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyTerrainFallbackClip) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(std::string::npos, glsl.find("uniform vec4 u_clipUV"));
    EXPECT_NE(std::string::npos, glsl.find("uniform float u_clipEnabled"));
    EXPECT_NE(std::string::npos, glsl.find("vec2 terrainUv = uvFromSet(0.0);"));
    EXPECT_NE(std::string::npos, glsl.find("if (u_clipEnabled > 0.5 &&"));
    EXPECT_NE(std::string::npos, glsl.find("discard;"));

    // Metal consumes the whole GltfUniformBlock as one struct at buffer(0);
    // clip uniforms are fields of that struct.
    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("constant GltfUniforms& u [[buffer(0)]]"));
    EXPECT_NE(std::string::npos, msl.find("packed_float4 clipUV;"));
    EXPECT_NE(std::string::npos, msl.find("float clipEnabled;"));
    EXPECT_NE(
        std::string::npos,
        msl.find("float2 terrainUv = gltfUvFromSet(in, 0.0);"));
    EXPECT_NE(std::string::npos, msl.find("if (u.clipEnabled > 0.5 &&"));
    EXPECT_NE(std::string::npos, msl.find("discard_fragment();"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyCesiumTextureTransformFormula) {
    const std::string expectedBody =
        "vec2 scaled = uv * offsetScale.zw;\n"
        "    return vec2(\n"
        "        scaled.x * sinCos.y + scaled.y * sinCos.x,\n"
        "        scaled.y * sinCos.y - scaled.x * sinCos.x) + offsetScale.xy;";

    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(std::string::npos, glsl.find("vec2 transformUv("));
    EXPECT_NE(std::string::npos, glsl.find(expectedBody));

    const std::string expectedMetalBody =
        "float2 scaled = uv * offsetScale.zw;\n"
        "    return float2(\n"
        "        scaled.x * sinCos.y + scaled.y * sinCos.x,\n"
        "        scaled.y * sinCos.y - scaled.x * sinCos.x) + offsetScale.xy;";

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(std::string::npos, msl.find("float2 gltfTransformUv("));
    EXPECT_NE(std::string::npos, msl.find(expectedMetalBody));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsTransmission) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_transmissionTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform float u_transmissionFactor"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("texture(u_transmissionTexture, transmissionUv).r"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("diffuseColor *= 1.0 - transmission"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("alpha *= 1.0 - transmission"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_transmissionTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("clamp(u.transmissionFactor, 0.0, 1.0)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("u_transmissionTexture.sample"));
    EXPECT_NE(
        std::string::npos,
        msl.find("diffuseColor *= 1.0 - transmission"));
    EXPECT_NE(
        std::string::npos,
        msl.find("alpha *= 1.0 - transmission"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsPbrSpecularGlossiness) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_specularGlossinessTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("u_specularGlossinessWorkflow > 0.5"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("roughness = clamp(1.0 - specGloss.a"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("specularColor = specGlossSpecularColor"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_specularGlossinessTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("u.specularGlossinessWorkflow > 0.5"));
    EXPECT_NE(
        std::string::npos,
        msl.find("roughness = clamp(1.0 - specGloss.a"));
    EXPECT_NE(
        std::string::npos,
        msl.find("specularColor = specGlossSpecularColor"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyDielectricSpecularF0) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform float u_dielectricSpecularF0"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("clamp(u_dielectricSpecularF0, 0.0, 1.0)"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("float dielectricSpecularF0;"));
    EXPECT_NE(
        std::string::npos,
        msl.find("clamp(u.dielectricSpecularF0, 0.0, 1.0)"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsAnisotropy) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_anisotropyTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("anisotropySample.rg * 2.0 - 1.0"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("anisotropyStrength *= anisotropySample.b"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("specular = anisotropicSpecular"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_anisotropyTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("anisotropySample.rg * 2.0 - 1.0"));
    EXPECT_NE(
        std::string::npos,
        msl.find("anisotropyStrength *= anisotropySample.b"));
    EXPECT_NE(
        std::string::npos,
        msl.find("specular = gltfAnisotropicSpecular"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsSpecular) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_specularTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_specularColorTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("specularStrength *= texture(u_specularTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("u_specularColorFactor"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("dielectricSpecular = clamp(dielectricSpecular, 0.0, 1.0)"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("mix(dielectricSpecular, base.rgb, metallic)"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_specularTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_specularColorTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("specularStrength *= u_specularTexture.sample"));
    EXPECT_NE(
        std::string::npos,
        msl.find("u.specularColorFactor"));
    EXPECT_NE(
        std::string::npos,
        msl.find("dielectricSpecular = clamp(dielectricSpecular, 0.0, 1.0)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("mix(dielectricSpecular, base.rgb, metallic)"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsClearcoat) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_clearcoatTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_clearcoatRoughnessTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_clearcoatNormalTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("clearcoat *= texture(u_clearcoatTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("texture(u_clearcoatRoughnessTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("perturbClearcoatNormal"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("clearcoatNormal = perturbClearcoatNormal(\n"
                  "            geometryN,\n"
                  "            clearcoatNormalUv,\n"
                  "            v_tangent,\n"
                  "            u_clearcoatFactors.z);"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("color = color * (1.0 - coatWeight)"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_clearcoatTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_clearcoatRoughnessTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_clearcoatNormalTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("clearcoat *= u_clearcoatTexture.sample"));
    EXPECT_NE(
        std::string::npos,
        msl.find("u_clearcoatRoughnessTexture.sample"));
    EXPECT_NE(
        std::string::npos,
        msl.find("u_clearcoatNormalTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("clearcoatNormal = gltfPerturbNormal(\n"
                 "            geometryN,\n"
                 "            clearcoatNormalUv,\n"
                 "            in.localPosition,\n"
                 "            in.tangent,\n"
                 "            u.clearcoatFactors.z,\n"
                 "            u_clearcoatNormalTexture,\n"
                 "            u_clearcoatNormalSampler);"));
    EXPECT_NE(
        std::string::npos,
        msl.find("color = color * (1.0 - coatWeight)"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsSheen) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_sheenColorTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("#define u_sheenRoughnessTexture u_baseColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("sheenColor *= texture(u_sheenColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("texture(u_sheenRoughnessTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("color += sheenColor * sheen"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_sheenColorTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("texture2d<float> u_sheenRoughnessTexture"));
    EXPECT_NE(
        std::string::npos,
        msl.find("sheenColor *= u_sheenColorTexture.sample"));
    EXPECT_NE(
        std::string::npos,
        msl.find("u_sheenRoughnessTexture.sample"));
    EXPECT_NE(
        std::string::npos,
        msl.find("color += sheenColor * sheen"));
}

// GLES fragment shaders may declare at most GL_MAX_TEXTURE_IMAGE_UNITS
// sampler2D (spec floor 16; Adreno enforces exactly 16). The full PBR material
// needs 20, so the GLES glTF shader aliases its advanced KHR-extension textures
// to u_baseColorTexture (keeping factor-based extension math) and compacts the
// live samplers to 10 — base color, metallic-roughness, normal, occlusion,
// emissive, four raster overlays and the water mask. This guard fails loudly if
// a future edit reintroduces enough sampler2D to overflow a 16-unit device.
TEST(RendererCommandTest, GltfFragmentShaderStaysWithinGlesSamplerLimit) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();

    const std::string kSamplerDecl = "uniform sampler2D ";
    size_t samplerCount = 0;
    for (size_t pos = glsl.find(kSamplerDecl);
         pos != std::string::npos;
         pos = glsl.find(kSamplerDecl, pos + kSamplerDecl.size())) {
        ++samplerCount;
    }

    EXPECT_LE(samplerCount, 16u)
        << "GLES glTF fragment shader declares " << samplerCount
        << " sampler2D; must stay <= 16 to link on GL_MAX_TEXTURE_IMAGE_UNITS==16";
    EXPECT_EQ(samplerCount, 10u);

    // The samplers the engine actually feeds must remain real declarations.
    for (const char* kept : {
             "uniform sampler2D u_baseColorTexture",
             "uniform sampler2D u_metallicRoughnessTexture",
             "uniform sampler2D u_normalTexture",
             "uniform sampler2D u_occlusionTexture",
             "uniform sampler2D u_emissiveTexture",
             "uniform sampler2D u_mappedRasterTexture0",
             "uniform sampler2D u_mappedRasterTexture1",
             "uniform sampler2D u_mappedRasterTexture2",
             "uniform sampler2D u_mappedRasterTexture3",
             "uniform sampler2D u_gltfWaterMaskTexture"}) {
        EXPECT_NE(std::string::npos, glsl.find(kept))
            << "missing required GLES glTF sampler: " << kept;
    }
}

TEST(RendererCommandTest, GltfFragmentShadersUseBaseColorForUnlitMaterials) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(std::string::npos, glsl.find("uniform float u_unlit"));
    EXPECT_NE(std::string::npos, glsl.find("if (u_unlit > 0.5)"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("fragColor = vec4(base.rgb, alpha * clamp(u_renderOpacity"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(std::string::npos, msl.find("float unlit;"));
    EXPECT_NE(std::string::npos, msl.find("if (u.unlit > 0.5)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("return float4(base.rgb, alpha * clamp(u.renderOpacity"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyAlphaModesConsistently) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("u_alphaMode > 0.5 && u_alphaMode < 1.5"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("base.a < u_alphaCutoff"));
    EXPECT_NE(std::string::npos, glsl.find("discard;"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("float alpha = u_alphaMode > 1.5 ? base.a : 1.0"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("alpha * clamp(u_renderOpacity"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("u.alphaMode > 0.5 && u.alphaMode < 1.5"));
    EXPECT_NE(
        std::string::npos,
        msl.find("base.a < u.alphaCutoff"));
    EXPECT_NE(std::string::npos, msl.find("discard_fragment();"));
    EXPECT_NE(
        std::string::npos,
        msl.find("float alpha = u.alphaMode > 1.5 ? base.a : 1.0"));
    EXPECT_NE(
        std::string::npos,
        msl.find("alpha * clamp(u.renderOpacity"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyOcclusionAndEmissive) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("occlusion = clamp(1.0 + u_materialFactors.w * (ao - 1.0)"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("vec3 emissive = u_emissiveFactor"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("emissive *= texture(u_emissiveTexture"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("occlusion = clamp(1.0 + u.materialFactors.w * (ao - 1.0)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("float3 emissive = float3(u.emissiveFactor)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("emissive *= u_emissiveTexture.sample"));
}

TEST(RendererCommandTest, GltfFragmentShadersFlipBackFaceNormals) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(std::string::npos, glsl.find("gl_FrontFacing ? 1.0 : -1.0"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("normalize(v_normal) * faceSign"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(std::string::npos, msl.find("[[front_facing]]"));
    EXPECT_NE(
        std::string::npos,
        msl.find("normalize(in.normal) * faceSign"));
}

TEST(RendererCommandTest, GltfFragmentShadersGuardNormalMapDegenerates) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("float mapNormalLenSq = dot(mapNormal, mapNormal)"));
    EXPECT_NE(std::string::npos, glsl.find("if (mapNormalLenSq < 1e-8)"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("float perturbedLenSq = dot(perturbed, perturbed)"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("float tangentLenSq = dot(tangent, tangent)"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("float bitangentLenSq = dot(bitangent, bitangent)"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(
        std::string::npos,
        msl.find("float mapNormalLenSq = dot(mapNormal, mapNormal)"));
    EXPECT_NE(std::string::npos, msl.find("if (mapNormalLenSq < 1e-8)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("float perturbedLenSq = dot(perturbed, perturbed)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("float tangentLenSq = dot(tangent, tangent)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("float bitangentLenSq = dot(bitangent, bitangent)"));
}

TEST(RendererCommandTest, GltfInstancedVertexShadersTransformTangentsWithModelMatrix) {
    const std::string glsl = renderer_testing::gltfInstancedVertexGLSL();
    EXPECT_NE(std::string::npos, glsl.find("mat3 instanceTangent = mat3(instanceModel)"));
    EXPECT_NE(std::string::npos, glsl.find("instanceTangent * tangent"));

    const std::string msl = renderer_testing::gltfInstancedVertexMSL();
    EXPECT_NE(std::string::npos, msl.find("float3x3 instanceTangent = float3x3"));
    EXPECT_NE(std::string::npos, msl.find("instanceTangent * tangent"));
}

TEST(RendererCommandTest, GltfPrimitiveInstancedCommandHasCorrectDefaults) {
    Renderer renderer(nullptr);

    RenderCommand cmd = renderer.makeGltfPrimitiveInstancedCommand(
        nullptr,
        nullptr,
        reinterpret_cast<Buffer*>(0x1),
        36,
        24,
        7);

    EXPECT_EQ(RenderCommandKind::GltfPrimitiveInstanced, cmd.kind);
    EXPECT_EQ("gltf_primitive_instanced", cmd.owner);
    EXPECT_EQ("color", cmd.pass);
    EXPECT_EQ(120, cmd.vertexStride);
    EXPECT_EQ(kGltfInstanceMatrixStride, cmd.instanceStride);
    EXPECT_EQ(7, cmd.instanceCount);
    EXPECT_EQ(reinterpret_cast<Buffer*>(0x1), cmd.instanceBuffer);
    EXPECT_EQ(RenderCommand::PrimitiveType::Triangles, cmd.primitive);
    EXPECT_EQ(RenderCommand::IndexType::UInt32, cmd.indexType);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_TRUE(cmd.depthWrite);
    EXPECT_FALSE(cmd.blend);
}

// ── MVP validation tests ──

TEST(RendererCommandTest, MvpValidatorAcceptsGltfPrimitive) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = true;
    gltf.cullFace = true;
    gltf.blend = false;
    gltf.frameId = 42;
    gltf.generation = 7;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, MvpValidatorAcceptsGltfPointAndLinePrimitives) {
    const RenderCommand::PrimitiveType primitiveTypes[] = {
        RenderCommand::PrimitiveType::Points,
        RenderCommand::PrimitiveType::Lines,
        RenderCommand::PrimitiveType::LineStrip};

    for (RenderCommand::PrimitiveType primitiveType : primitiveTypes) {
        RenderCommand gltf;
        gltf.kind = RenderCommandKind::GltfPrimitive;
        gltf.owner = "gltf_primitive";
        gltf.pass = "color";
        gltf.depthTest = true;
        gltf.depthWrite = true;
        gltf.cullFace = true;
        gltf.blend = false;
        gltf.frameId = 42;
        gltf.generation = 7;
        gltf.primitive = primitiveType;

        RenderCommandList commands{gltf};
        auto error = validateMvpRenderCommands(commands, 42);
        EXPECT_FALSE(error.has_value());
    }
}

TEST(RendererCommandTest, MvpValidatorAcceptsBlendedGltfWithReadOnlyDepth) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = false;
    gltf.cullFace = true;
    gltf.blend = true;
    gltf.hasGltfUniforms = true;
    gltf.gltfUniforms.alphaMode = 2.0f;
    gltf.hasTranslucentSortDepth = true;
    gltf.translucentSortDepth = 10.0;
    gltf.frameId = 42;
    gltf.generation = 7;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, MvpValidatorKeepsAlphaMaskGltfOpaque) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_mask_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = true;
    gltf.cullFace = true;
    gltf.blend = false;
    gltf.hasGltfUniforms = true;
    gltf.gltfUniforms.alphaMode = 1.0f;
    gltf.gltfUniforms.alphaCutoff = 0.5f;
    gltf.frameId = 42;
    gltf.generation = 7;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, MvpValidatorRejectsBlendedGltfDepthWrites) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = true;
    gltf.cullFace = true;
    gltf.blend = true;
    gltf.hasGltfUniforms = true;
    gltf.gltfUniforms.alphaMode = 2.0f;
    gltf.hasTranslucentSortDepth = true;
    gltf.translucentSortDepth = 10.0;
    gltf.frameId = 42;
    gltf.generation = 7;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("gltf_primitive", error->owner);
}

TEST(RendererCommandTest, MvpValidatorAcceptsInstancedGltfPrimitive) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitiveInstanced;
    gltf.owner = "gltf_primitive_instanced";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = true;
    gltf.cullFace = true;
    gltf.blend = false;
    gltf.frameId = 42;
    gltf.generation = 7;
    gltf.instanceBuffer = reinterpret_cast<Buffer*>(0x1);
    gltf.instanceCount = 3;
    gltf.instanceStride = kGltfInstanceMatrixStride;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, MvpValidatorRejectsInstancedGltfWithoutBuffer) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitiveInstanced;
    gltf.owner = "gltf_primitive_instanced";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = false;
    gltf.cullFace = true;
    gltf.blend = false;
    gltf.frameId = 42;
    gltf.generation = 7;
    gltf.instanceCount = 3;
    gltf.instanceStride = kGltfInstanceMatrixStride;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("gltf_primitive_instanced", error->owner);
}

TEST(RendererCommandTest, MvpSortPutsVectorLast) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::GltfPrimitive;
    tile.owner = "terrain_primitive";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.generation = 1;

    RenderCommand tile2;
    tile2.kind = RenderCommandKind::GltfPrimitive;
    tile2.owner = "terrain_primitive";
    tile2.pass = "color";
    tile2.depthTest = true;
    tile2.depthWrite = true;
    tile2.cullFace = true;
    tile2.blend = false;
    tile2.generation = 1;

    RenderCommand vector;
    vector.kind = RenderCommandKind::VectorOverlay;
    vector.owner = "vector";
    vector.pass = "color";

    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = true;
    gltf.cullFace = true;
    gltf.blend = false;
    gltf.generation = 1;

    RenderCommandList commands{vector, gltf, tile, tile2};
    sortMvpRenderCommands(commands);

    // SurfaceTile(order 10)删除后地表与 glTF 同为 GltfPrimitive(order 15),
    // 二者之间不再有次序区分;仍然成立且是本用例真正要钉的是「矢量恒在最后」。
    EXPECT_EQ(15, mvpRenderOrder(commands[0].kind));
    EXPECT_EQ(15, mvpRenderOrder(commands[1].kind));
    EXPECT_EQ(15, mvpRenderOrder(commands[2].kind));
    EXPECT_EQ(RenderCommandKind::VectorOverlay, commands[3].kind);
    EXPECT_FALSE(validateMvpRenderCommands(commands).has_value());
}

TEST(RendererCommandTest, MvpSortDrawsOpaqueGltfBeforeTranslucentBackToFront) {
    RenderCommand nearBlend;
    nearBlend.kind = RenderCommandKind::GltfPrimitive;
    nearBlend.owner = "near_blend";
    nearBlend.pass = "color";
    nearBlend.depthTest = true;
    nearBlend.depthWrite = false;
    nearBlend.cullFace = true;
    nearBlend.blend = true;
    nearBlend.generation = 1;
    nearBlend.hasGltfUniforms = true;
    nearBlend.gltfUniforms.alphaMode = 2.0f;
    nearBlend.hasTranslucentSortDepth = true;
    nearBlend.translucentSortDepth = 5.0;

    RenderCommand opaque;
    opaque.kind = RenderCommandKind::GltfPrimitive;
    opaque.owner = "opaque";
    opaque.pass = "color";
    opaque.depthTest = true;
    opaque.depthWrite = true;
    opaque.cullFace = true;
    opaque.blend = false;
    opaque.generation = 1;

    RenderCommand farBlend = nearBlend;
    farBlend.owner = "far_blend";
    farBlend.translucentSortDepth = 25.0;

    RenderCommandList commands{nearBlend, opaque, farBlend};
    sortMvpRenderCommands(commands);

    ASSERT_EQ(3u, commands.size());
    EXPECT_EQ("opaque", commands[0].owner);
    EXPECT_EQ("far_blend", commands[1].owner);
    EXPECT_EQ("near_blend", commands[2].owner);
    EXPECT_FALSE(validateMvpRenderCommands(commands).has_value());
}

TEST(RendererCommandTest, MvpValidatorRejectsTranslucentGltfWithoutSortDepth) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = false;
    gltf.cullFace = true;
    gltf.blend = true;
    gltf.generation = 1;
    gltf.hasGltfUniforms = true;
    gltf.gltfUniforms.alphaMode = 2.0f;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("gltf_primitive", error->owner);
}

TEST(RendererCommandTest, MvpValidatorRejectsTranslucentGltfFrontToBack) {
    RenderCommand nearBlend;
    nearBlend.kind = RenderCommandKind::GltfPrimitive;
    nearBlend.owner = "near_blend";
    nearBlend.pass = "color";
    nearBlend.depthTest = true;
    nearBlend.depthWrite = false;
    nearBlend.cullFace = true;
    nearBlend.blend = true;
    nearBlend.generation = 1;
    nearBlend.hasGltfUniforms = true;
    nearBlend.gltfUniforms.alphaMode = 2.0f;
    nearBlend.hasTranslucentSortDepth = true;
    nearBlend.translucentSortDepth = 5.0;

    RenderCommand farBlend = nearBlend;
    farBlend.owner = "far_blend";
    farBlend.translucentSortDepth = 25.0;

    RenderCommandList commands{nearBlend, farBlend};
    auto error = validateMvpRenderCommands(commands);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("far_blend", error->owner);
}

// ── Blend state tests ──

TEST(RendererCommandTest, GltfPrimitiveBlendAllowedForPartialOpacity) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = false;
    gltf.cullFace = true;
    gltf.blend = true;
    gltf.generation = 1;
    gltf.hasGltfUniforms = true;
    gltf.gltfUniforms.renderOpacity = 0.5f;
    gltf.hasTranslucentSortDepth = true;
    gltf.translucentSortDepth = 10.0;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, GltfPrimitiveBlendAllowedForAlphaModeBlend) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = false;
    gltf.cullFace = true;
    gltf.blend = true;
    gltf.frameId = 42;
    gltf.generation = 7;
    gltf.hasGltfUniforms = true;
    gltf.gltfUniforms.alphaMode = 2.0f;
    gltf.gltfUniforms.renderOpacity = 1.0f;
    gltf.hasTranslucentSortDepth = true;
    gltf.translucentSortDepth = 10.0;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, GltfPrimitiveBlendAllowedForTransmission) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_transmission";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = false;
    gltf.cullFace = true;
    gltf.blend = true;
    gltf.frameId = 42;
    gltf.generation = 7;
    gltf.hasGltfUniforms = true;
    gltf.gltfUniforms.alphaMode = 0.0f;
    gltf.gltfUniforms.renderOpacity = 1.0f;
    gltf.gltfUniforms.transmissionFactor = 0.5f;
    gltf.hasTranslucentSortDepth = true;
    gltf.translucentSortDepth = 10.0;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, GltfPrimitiveBlendRejectedWithoutOpacityReason) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = false;
    gltf.cullFace = true;
    gltf.blend = true;
    gltf.generation = 1;
    gltf.hasTranslucentSortDepth = true;
    gltf.translucentSortDepth = 10.0;

    RenderCommandList commands{gltf};
    auto error = validateMvpRenderCommands(commands);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("gltf_primitive", error->owner);
}

// ---- stencil 分类色 pass 的覆盖面契约 ----
//
// 色 pass 光栅化的是挤出体、着色的却是 stencil 选中的地形像素,所以覆盖面只需
// 「每个选中像素被盖到一次」。水密体的背面单独就覆盖整个轮廓 → 画双面是白烧一倍
// 光栅化。取背面而非正面:相机进体内时正面被近平面切掉,背面永远在 —— 剔错面的
// 症状是「走进这片区域时它整片消失」,静态截图查不出来,故写死在校验里。
namespace {

RenderCommand makeStencilVolume() {
    RenderCommand vol;
    vol.kind = RenderCommandKind::VectorStencil;
    vol.stencilPhase = StencilPhase::ClassifyVolume;
    vol.owner = "stencil";
    vol.pass = "color";
    vol.depthTest = true;
    vol.depthWrite = false;
    vol.cullFace = false;  // 两侧 z-fail 计数 = 必须双面
    vol.blend = false;
    vol.frameId = 42;
    vol.generation = 1;
    return vol;
}

RenderCommand makeStencilColor() {
    RenderCommand col = makeStencilVolume();
    col.stencilPhase = StencilPhase::ClassifyColor;
    col.depthTest = false;
    col.cullFace = true;
    col.cullMode = RenderCommand::CullMode::Front;
    col.blend = true;
    return col;
}

}  // namespace

TEST(RendererCommandTest, VectorStencilColorPassCullsFrontFaces) {
    RenderCommandList commands{makeStencilVolume(), makeStencilColor()};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, VectorStencilColorPassRejectsDoubleSided) {
    RenderCommand col = makeStencilColor();
    col.cullFace = false;  // 旧行为:双面全画
    RenderCommandList commands{makeStencilVolume(), col};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->message.find("cullFace"), std::string::npos);
}

TEST(RendererCommandTest, VectorStencilColorPassRejectsCullingBackFaces) {
    RenderCommand col = makeStencilColor();
    col.cullMode = RenderCommand::CullMode::Back;  // 剔错面
    RenderCommandList commands{makeStencilVolume(), col};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->message.find("back faces"), std::string::npos);
}

TEST(RendererCommandTest, VectorStencilVolumePassStaysDoubleSided) {
    // 体 pass 单面 = 失去 z-fail 分类语义(两侧计数是它的定义)。
    RenderCommand vol = makeStencilVolume();
    vol.cullFace = true;
    RenderCommandList commands{vol, makeStencilColor()};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->message.find("cullFace"), std::string::npos);
}
