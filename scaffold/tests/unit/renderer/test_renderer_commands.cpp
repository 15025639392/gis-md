#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/renderer/VectorUniformBlock.h"

#include <algorithm>
#include <string>

using namespace earth_engine;

namespace earth_engine {
namespace renderer_testing {
const char* vectorFillFragmentMSL();
const char* gltfVertexGLSL();
const char* gltfFragmentGLSL();
const char* gltfFragmentMSL();
const char* gltfInstancedVertexGLSL();
const char* gltfInstancedVertexMSL();
const char* vectorLineFragmentGLSL();
const char* vectorLineFragmentMSL();
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

TEST(RendererCommandTest, VectorFillMetalColorUsesFragmentSlotZero) {
    const std::string msl = renderer_testing::vectorFillFragmentMSL();
    EXPECT_NE(std::string::npos,
              msl.find("constant float4& u_color [[buffer(0)]]"));
    EXPECT_EQ(std::string::npos, msl.find("u_color [[buffer(2)]]"));
}

TEST(RendererCommandTest, VectorDashCapsUsePerEndpointCircularDistance) {
    const std::string glsl = renderer_testing::vectorLineFragmentGLSL();
    const std::string msl = renderer_testing::vectorLineFragmentMSL();
    for (const std::string* source : {&glsl, &msl}) {
        EXPECT_NE(std::string::npos, source->find("ds = min(ds, period - ds)"));
        EXPECT_NE(std::string::npos, source->find("de = min(de, period - de)"));
        EXPECT_NE(std::string::npos, source->find("float d = min(ds, de)"));
    }

    auto circularDistance = [](float p, float endpoint, float period) {
        const float direct = std::abs(p - endpoint);
        return std::min(direct, period - direct);
    };
    const float period = 12.0f;
    const float p = 11.8f;
    const float d = std::min(circularDistance(p, 0.0f, period),
                             circularDistance(p, 6.0f, period));
    EXPECT_NEAR(0.2f, d, 1e-5f);
    EXPECT_LE(d, 0.5f);  // next-cycle leading square/round cap is visible
}

TEST(RendererCommandTest, SolidEndpointPrimitivesAreCommandTimeGated) {
    const std::string glsl = renderer_testing::vectorLineFragmentGLSL();
    const std::string msl = renderer_testing::vectorLineFragmentMSL();
    for (const std::string* source : {&glsl, &msl}) {
        EXPECT_NE(std::string::npos,
                  source->find("solidCapStyle < 0.5"));
        EXPECT_NE(std::string::npos,
                  source->find("solidCapStyle > 1.5"));
    }
    const auto& table = vectorUniformTable();
    EXPECT_NE(table.end(), std::find_if(
                               table.begin(), table.end(), [](const auto& e) {
                                   return std::string(e.name) ==
                                          "u_solidCapStyle";
                               }));
}

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

TEST(RendererCommandTest, MvpValidatorRejectsDualFixedUniformBlocks) {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::GltfPrimitive;
    cmd.owner = "malformed_dual_uniform_command";
    cmd.pass = "color";
    cmd.depthTest = true;
    cmd.depthWrite = true;
    cmd.cullFace = true;
    cmd.generation = 1;
    cmd.hasGltfUniforms = true;
    cmd.hasVectorUniforms = true;

    const auto error = validateMvpRenderCommands({cmd});
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(cmd.owner, error->owner);
    EXPECT_NE(std::string::npos, error->message.find("simultaneously"));
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
    // Ten remains below the GLES 3.0 fragment-stage floor of 16 texture units.
    EXPECT_EQ(samplerCount, 10u);

    // The samplers the engine actually feeds must remain real declarations.
    for (const char* kept : {
             "uniform sampler2D u_baseColorTexture",
             "uniform sampler2D u_metallicRoughnessTexture",
             "uniform sampler2D u_normalTexture",
             "uniform sampler2D u_occlusionTexture",
             "uniform sampler2D u_emissiveTexture",
             "uniform sampler2D u_directRasterTexture0",
             "uniform sampler2D u_directRasterTexture1",
             "uniform sampler2D u_directRasterTexture2",
             "uniform sampler2D u_directRasterTexture3",
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

TEST(RendererCommandTest, MvpSortUsesVectorPaintOrderAcrossSameMvpPass) {
    auto makeFill = [](const char* owner, int paintOrder) {
        RenderCommand cmd;
        cmd.kind = RenderCommandKind::VectorFill;
        cmd.owner = owner;
        cmd.pass = "color";
        cmd.vectorPaintOrder = paintOrder;
        cmd.depthTest = true;
        cmd.depthWrite = false;
        cmd.blend = true;
        cmd.cullFace = false;
        return cmd;
    };
    RenderCommandList commands{makeFill("water", 50), makeFill("land", 30),
                               makeFill("green", 10)};
    EXPECT_TRUE(mvpRenderCommandsNeedSort(commands));
    EXPECT_TRUE(validateMvpRenderCommands(commands).has_value());
    sortMvpRenderCommands(commands);
    EXPECT_FALSE(mvpRenderCommandsNeedSort(commands));
    ASSERT_EQ(3u, commands.size());
    EXPECT_EQ("green", commands[0].owner);
    EXPECT_EQ("land", commands[1].owner);
    EXPECT_EQ("water", commands[2].owner);
    EXPECT_FALSE(validateMvpRenderCommands(commands).has_value());
}

TEST(RendererCommandTest, MvpSortUsesVectorPaintSubOrderAcrossTiles) {
    RenderCommand center;
    center.kind = RenderCommandKind::VectorLine;
    center.owner = "tile-a-center";
    center.vectorPaintOrder = 80;
    center.vectorPaintSubOrder = 1;

    RenderCommand casing;
    casing.kind = RenderCommandKind::VectorLine;
    casing.owner = "tile-b-casing";
    casing.vectorPaintOrder = 80;
    casing.vectorPaintSubOrder = 0;

    RenderCommand centerB = center;
    centerB.owner = "tile-b-center";
    RenderCommand casingA = casing;
    casingA.owner = "tile-a-casing";

    RenderCommandList commands;
    commands.push_back(std::move(casingA));
    commands.push_back(std::move(center));
    commands.push_back(std::move(casing));
    commands.push_back(std::move(centerB));
    EXPECT_TRUE(mvpRenderCommandsNeedSort(commands));
    sortMvpRenderCommands(commands);

    ASSERT_EQ(4u, commands.size());
    EXPECT_EQ(0, commands[0].vectorPaintSubOrder);
    EXPECT_EQ(0, commands[1].vectorPaintSubOrder);
    EXPECT_EQ(1, commands[2].vectorPaintSubOrder);
    EXPECT_EQ(1, commands[3].vectorPaintSubOrder);
    EXPECT_FALSE(mvpRenderCommandsNeedSort(commands));
}

TEST(RendererCommandTest, MvpSortPreservesOfficialOrderBeforeStrokeSubOrder) {
    auto makeRoad = [](int paintOrder, int subOrder, const char* owner) {
        RenderCommand cmd;
        cmd.kind = RenderCommandKind::VectorLine;
        cmd.owner = owner;
        cmd.vectorPaintOrder = paintOrder;
        cmd.vectorPaintSubOrder = subOrder;
        return cmd;
    };
    RenderCommandList commands;
    commands.push_back(makeRoad(79, 1, "minor-center"));
    commands.push_back(makeRoad(82, 0, "major-casing"));
    commands.push_back(makeRoad(82, 1, "major-center"));
    commands.push_back(makeRoad(79, 0, "minor-casing"));
    sortMvpRenderCommands(commands);

    ASSERT_EQ(4u, commands.size());
    EXPECT_EQ("minor-casing", commands[0].owner);
    EXPECT_EQ("minor-center", commands[1].owner);
    EXPECT_EQ("major-casing", commands[2].owner);
    EXPECT_EQ("major-center", commands[3].owner);
}

TEST(RendererCommandTest, MvpSortIsTransitiveAcrossMixedVectorKinds) {
    RenderCommand casing;
    casing.kind = RenderCommandKind::VectorLine;
    casing.owner = "major-casing";
    casing.vectorPaintOrder = 82;
    casing.vectorPaintSubOrder = 0;

    RenderCommand center;
    center.kind = RenderCommandKind::VectorLine;
    center.owner = "minor-center";
    center.vectorPaintOrder = 79;
    center.vectorPaintSubOrder = 1;

    RenderCommand fill;
    fill.kind = RenderCommandKind::VectorFill;
    fill.owner = "fill";
    fill.vectorPaintOrder = 80;

    std::array<RenderCommand, 3> source{casing, center, fill};
    for (auto& cmd : source) {
        cmd.pass = "color";
        cmd.frameId = 7;
        cmd.depthTest = true;
        cmd.depthWrite = false;
        cmd.blend = true;
        cmd.cullFace = false;
    }
    std::array<int, 3> order{0, 1, 2};
    do {
        RenderCommandList commands;
        for (int i : order) commands.push_back(source[i]);
        sortMvpRenderCommands(commands);
        ASSERT_EQ(3u, commands.size());
        EXPECT_EQ("minor-center", commands[0].owner);
        EXPECT_EQ("fill", commands[1].owner);
        EXPECT_EQ("major-casing", commands[2].owner);
        EXPECT_FALSE(mvpRenderCommandsNeedSort(commands));
        EXPECT_FALSE(validateMvpRenderCommands(commands, 7).has_value());
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(RendererCommandTest, MvpValidatorRejectsVectorPaintSubOrderRegression) {
    RenderCommand center;
    center.kind = RenderCommandKind::VectorLine;
    center.owner = "center";
    center.pass = "color";
    center.frameId = 7;
    center.vectorPaintOrder = 80;
    center.vectorPaintSubOrder = 1;
    center.depthTest = true;
    center.depthWrite = false;
    center.blend = true;
    center.cullFace = false;

    RenderCommand casing = center;
    casing.owner = "casing";
    casing.vectorPaintSubOrder = 0;
    RenderCommandList commands{center, casing};

    const auto error = validateMvpRenderCommands(commands, 7);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(std::string::npos, error->message.find("sub-order"));
}

TEST(RendererCommandTest, MvpSortKeepsStableTiesAndHeavyPayloads) {
    auto makeFill = [](const char* owner) {
        RenderCommand cmd;
        cmd.kind = RenderCommandKind::VectorFill;
        cmd.owner = owner;
        cmd.pass = "color";
        cmd.vectorPaintOrder = 30;
        cmd.depthTest = true;
        cmd.depthWrite = false;
        cmd.blend = true;
        cmd.cullFace = false;
        cmd.textures.push_back(nullptr);
        cmd.uniforms["u_modelViewProjection"] =
            std::vector<float>(16, owner[0] == 'a' ? 1.0f : 2.0f);
        return cmd;
    };
    RenderCommand late;
    late.kind = RenderCommandKind::VectorLabel;
    late.owner = "label";
    late.vectorPaintOrder = 30;
    late.vectorPaintSubOrder = 3;
    late.pass = "color";
    late.depthTest = false;
    late.depthWrite = false;
    late.blend = true;
    late.cullFace = false;

    RenderCommandList commands{late, makeFill("alpha"), makeFill("beta")};
    sortMvpRenderCommands(commands);

    ASSERT_EQ(3u, commands.size());
    EXPECT_EQ("alpha", commands[0].owner);
    EXPECT_EQ("beta", commands[1].owner);
    EXPECT_EQ("label", commands[2].owner);
    ASSERT_EQ(16u, commands[0].uniforms.at("u_modelViewProjection").size());
    EXPECT_EQ(1.0f,
              commands[0].uniforms.at("u_modelViewProjection").front());
    EXPECT_EQ(2.0f,
              commands[1].uniforms.at("u_modelViewProjection").front());
    EXPECT_FALSE(validateMvpRenderCommands(commands).has_value());
}

TEST(RendererCommandTest, MvpSortKeepsNanDepthTiesStable) {
    auto makeBlend = [](const char* owner, double depth) {
        RenderCommand cmd;
        cmd.kind = RenderCommandKind::GltfPrimitive;
        cmd.owner = owner;
        cmd.pass = "color";
        cmd.depthTest = true;
        cmd.depthWrite = false;
        cmd.cullFace = true;
        cmd.blend = true;
        cmd.generation = 1;
        cmd.hasGltfUniforms = true;
        cmd.gltfUniforms.alphaMode = 2.0f;
        cmd.hasTranslucentSortDepth = true;
        cmd.translucentSortDepth = depth;
        return cmd;
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    RenderCommandList commands{makeBlend("nan-first", nan),
                               makeBlend("finite", 10.0),
                               makeBlend("nan-last", nan)};
    sortMvpRenderCommands(commands);
    ASSERT_EQ(3u, commands.size());
    EXPECT_EQ("nan-first", commands[0].owner);
    EXPECT_EQ("finite", commands[1].owner);
    EXPECT_EQ("nan-last", commands[2].owner);
}

TEST(RendererCommandTest, MvpSortInterleavesStencilWithVectorPaintOrder) {
    auto makeFill = [](int paintOrder) {
        RenderCommand cmd;
        cmd.kind = RenderCommandKind::VectorFill;
        cmd.owner = "land";
        cmd.pass = "color";
        cmd.vectorPaintOrder = paintOrder;
        cmd.depthTest = true;
        cmd.depthWrite = false;
        cmd.blend = true;
        cmd.cullFace = false;
        return cmd;
    };
    auto makeStencil = [](StencilPhase phase, int paintOrder) {
        RenderCommand cmd;
        cmd.kind = RenderCommandKind::VectorStencil;
        cmd.owner = "water";
        cmd.pass = "color";
        cmd.stencilPhase = phase;
        cmd.vectorPaintOrder = paintOrder;
        cmd.depthTest = phase == StencilPhase::ClassifyVolume;
        cmd.depthWrite = false;
        cmd.blend = phase == StencilPhase::ClassifyColor;
        cmd.cullFace = phase == StencilPhase::ClassifyColor;
        if (cmd.cullFace) cmd.cullMode = RenderCommand::CullMode::Front;
        return cmd;
    };

    // 低 ordinal 的普通用地必须先画，高 ordinal 的贴地水面随后覆盖；
    // VectorStencil 不能因固定在独立 MVP pass 而整体跑到普通 fill 前。
    RenderCommandList commands{
        makeStencil(StencilPhase::ClassifyVolume, 50),
        makeStencil(StencilPhase::ClassifyColor, 50),
        makeFill(30),
    };
    sortMvpRenderCommands(commands);
    ASSERT_EQ(3u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorFill, commands[0].kind);
    EXPECT_EQ(RenderCommandKind::VectorStencil, commands[1].kind);
    EXPECT_EQ(RenderCommandKind::VectorStencil, commands[2].kind);
    EXPECT_EQ(StencilPhase::ClassifyVolume, commands[1].stencilPhase);
    EXPECT_EQ(StencilPhase::ClassifyColor, commands[2].stencilPhase);
    EXPECT_FALSE(validateMvpRenderCommands(commands).has_value());
}

TEST(RendererCommandTest, OfficialDrawOrderPrecedesVectorKind) {
    RenderCommand extrusion;
    extrusion.kind = RenderCommandKind::VectorExtrusion;
    extrusion.owner = "building";
    extrusion.pass = "color";
    extrusion.vectorPaintOrder = 60;
    extrusion.depthTest = true;
    extrusion.depthWrite = true;
    extrusion.blend = false;
    extrusion.cullFace = false;

    RenderCommand label;
    label.kind = RenderCommandKind::VectorLabel;
    label.owner = "label";
    label.pass = "color";
    label.vectorPaintOrder = 59;
    label.vectorPaintSubOrder = 3;
    label.depthTest = false;
    label.depthWrite = false;
    label.blend = true;
    label.cullFace = false;

    RenderCommandList commands{label, extrusion};
    sortMvpRenderCommands(commands);
    ASSERT_EQ(2u, commands.size());
    EXPECT_EQ(RenderCommandKind::VectorLabel, commands[0].kind);
    EXPECT_EQ(RenderCommandKind::VectorExtrusion, commands[1].kind);
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

// 符号命令固定状态:**深度测试必须关**。billboard 四角共用锚点深度,逐像素
// 深度测试只会把 quad 切一块(那道切口是不存在的形状边界),遮挡改由锚点
// 判定整符号决定。
//
// ⚠️ 这条契约此前**只在真机上生效**:校验器有这个分支,但主机测试没有一条
// 构造过 VectorPoint/VectorLabel 命令,于是把状态改错时 host 188/188 全绿、
// 一上真机就 abort。补这两条把契约拉回主机。
TEST(RendererCommandTest, MvpValidatorRequiresSymbolDepthTestOff) {
    RenderCommand sym;
    sym.kind = RenderCommandKind::VectorPoint;
    sym.owner = "mvt-basemap";
    sym.pass = "color";
    sym.depthTest = false;
    sym.depthWrite = false;
    sym.cullFace = false;
    sym.blend = true;
    sym.generation = 1;

    RenderCommandList commands{sym};
    EXPECT_FALSE(validateMvpRenderCommands(commands, 1).has_value());

    commands[0].depthTest = true;  // 回到"逐像素切 quad"的错误状态
    auto error = validateMvpRenderCommands(commands, 1);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(std::string::npos, error->message.find("depthTest"));
}

// 面/线相反:它们是贴地几何,像素与 3D 位置一一对应,逐像素深度测试语义
// 正确,必须保持开 —— 两条契约分道,别一起改。
TEST(RendererCommandTest, MvpValidatorKeepsFillLineDepthTestOn) {
    RenderCommand fill;
    fill.kind = RenderCommandKind::VectorFill;
    fill.owner = "mvt-basemap";
    fill.pass = "color";
    fill.depthTest = true;
    fill.depthWrite = false;
    fill.cullFace = false;
    fill.blend = true;
    fill.generation = 1;

    RenderCommandList commands{fill};
    EXPECT_FALSE(validateMvpRenderCommands(commands, 1).has_value());

    commands[0].depthTest = false;
    EXPECT_TRUE(validateMvpRenderCommands(commands, 1).has_value());
}
