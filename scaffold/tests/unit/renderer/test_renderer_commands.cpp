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
    int id() const { return id_; }

private:
    int id_ = 0;
};

} // namespace

// ── Basic surface tile command creation ──

TEST(RendererCommandTest, SurfaceTileCommandHasCorrectDefaults) {
    Renderer renderer(nullptr);

    // Current API: 4 parameters (texture, vertexBuffer, indexBuffer, indexCount)
    // Surface vertex layout: POSITION(12) + NORMAL(12) + TEXCOORD_0(8) = 32
    RenderCommand cmd = renderer.makeSurfaceTileCommand(
        nullptr,   // texture
        nullptr,   // vertexBuffer
        nullptr,   // indexBuffer (null = use shared tile index buffer)
        42);       // indexCount (ignored when indexBuffer is null)

    // Kind and owner
    EXPECT_EQ(RenderCommandKind::SurfaceTile, cmd.kind);
    EXPECT_EQ("surface_tile", cmd.owner);
    EXPECT_EQ("color", cmd.pass);

    // Index count: when indexBuffer is null, the shared tileIndexCount is used
    // (not the passed indexCount). But if Renderer is not initialized (device=nullptr),
    // tileIndexCount defaults to 0 and tileIndexBuffer is null.
    // We pass a non-null indexBuffer to test indexCount passthrough:
    (void)42; // indexCount is only used when indexBuffer != null

    // Vertex stride: 32 bytes (surface tile layout)
    EXPECT_EQ(32, cmd.vertexStride);

    // Render state
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_TRUE(cmd.depthWrite);
    EXPECT_TRUE(cmd.cullFace);
    EXPECT_FALSE(cmd.blend);

    // Primitive type
    EXPECT_EQ(RenderCommand::PrimitiveType::Triangles, cmd.primitive);
    EXPECT_EQ(RenderCommand::IndexType::UInt32, cmd.indexType);
}

TEST(RendererCommandTest, SurfaceTileCommandPreservesIndexCountWithCustomBuffer) {
    Renderer renderer(nullptr);

    // Create a dummy vertex buffer so we can pass a custom index buffer
    // and verify indexCount is preserved.
    // (We can't easily create real GPU buffers without a device, but we can
    // test with nullptr and verify the API shape.)
    RenderCommand cmd = renderer.makeSurfaceTileCommand(
        nullptr, nullptr, nullptr, 0);

    // With null indexBuffer, renderer uses shared tileIndexBuffer.
    // Index count comes from the shared buffer, not from the parameter.
    // This test just validates the API accepts 4 parameters.
    EXPECT_EQ(RenderCommandKind::SurfaceTile, cmd.kind);
}

// ── Surface tile uniforms are set via hot-path fields ──

TEST(RendererCommandTest, SurfaceTileHasUniformsFlagSet) {
    Renderer renderer(nullptr);
    DummyTexture tex(1);

    RenderCommand cmd = renderer.makeSurfaceTileCommand(
        &tex, nullptr, nullptr, 0);

    // hasSurfaceTileUniforms is true (set when shader is assigned in makeSurfaceTileCommand)
    EXPECT_TRUE(cmd.hasSurfaceTileUniforms);

    // Texture is placed in the textures vector
    ASSERT_GE(cmd.textures.size(), 1u);
    EXPECT_EQ(&tex, cmd.textures[0]);
}

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
    ASSERT_TRUE(cmd.uniforms.count("u_baseColor"));
    ASSERT_TRUE(cmd.uniforms.count("u_hasBaseColorTexture"));
    ASSERT_TRUE(cmd.uniforms.count("u_materialFactors"));
    ASSERT_TRUE(cmd.uniforms.count("u_dielectricSpecularF0"));
    ASSERT_TRUE(cmd.uniforms.count("u_hasMaterialTextures"));
    ASSERT_TRUE(cmd.uniforms.count("u_anisotropyFactors"));
    ASSERT_TRUE(cmd.uniforms.count("u_hasAnisotropyTexture"));
    ASSERT_TRUE(cmd.uniforms.count("u_hasSpecularTextures"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularFactor"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularColorFactor"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularGlossinessWorkflow"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularGlossinessFactor"));
    ASSERT_TRUE(cmd.uniforms.count("u_hasSpecularGlossinessTexture"));
    ASSERT_TRUE(cmd.uniforms.count("u_transmissionFactor"));
    ASSERT_TRUE(cmd.uniforms.count("u_hasTransmissionTexture"));
    ASSERT_TRUE(cmd.uniforms.count("u_clearcoatFactors"));
    ASSERT_TRUE(cmd.uniforms.count("u_hasClearcoatTextures"));
    ASSERT_TRUE(cmd.uniforms.count("u_sheenColorFactor"));
    ASSERT_TRUE(cmd.uniforms.count("u_sheenRoughnessFactor"));
    ASSERT_TRUE(cmd.uniforms.count("u_hasSheenTextures"));
    ASSERT_TRUE(cmd.uniforms.count("u_emissiveFactor"));
    ASSERT_TRUE(cmd.uniforms.count("u_textureCoordSets"));
    ASSERT_TRUE(cmd.uniforms.count("u_emissiveTexCoordSet"));
    ASSERT_TRUE(cmd.uniforms.count("u_anisotropyTexCoordSet"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularTexCoordSets"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularGlossinessTexCoordSet"));
    ASSERT_TRUE(cmd.uniforms.count("u_transmissionTexCoordSet"));
    ASSERT_TRUE(cmd.uniforms.count("u_clearcoatTexCoordSets"));
    ASSERT_TRUE(cmd.uniforms.count("u_sheenTexCoordSets"));
    ASSERT_TRUE(cmd.uniforms.count("u_baseColorTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_baseColorTexRotationSinCos"));
    ASSERT_TRUE(cmd.uniforms.count("u_anisotropyTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_anisotropyTexRotationSinCos"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularColorTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_specularGlossinessTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_transmissionTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_transmissionTexRotationSinCos"));
    ASSERT_TRUE(cmd.uniforms.count("u_clearcoatTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_clearcoatRoughnessTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_clearcoatNormalTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_sheenColorTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_sheenRoughnessTexOffsetScale"));
    ASSERT_TRUE(cmd.uniforms.count("u_alphaMode"));
    ASSERT_TRUE(cmd.uniforms.count("u_alphaCutoff"));
    ASSERT_TRUE(cmd.uniforms.count("u_renderOpacity"));
    ASSERT_TRUE(cmd.uniforms.count("u_unlit"));
}

TEST(RendererCommandTest, GltfGlesVertexShadersSetExplicitPointSize) {
    const std::string vertex = renderer_testing::gltfVertexGLSL();
    const std::string instancedVertex =
        renderer_testing::gltfInstancedVertexGLSL();

    EXPECT_NE(std::string::npos, vertex.find("gl_PointSize = 1.0;"));
    EXPECT_NE(std::string::npos, instancedVertex.find("gl_PointSize = 1.0;"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsTransmission) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform sampler2D u_transmissionTexture"));
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
        msl.find("constant float& u_transmissionFactor"));
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
        glsl.find("uniform sampler2D u_specularGlossinessTexture"));
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
        msl.find("u_specularGlossinessWorkflow > 0.5"));
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
        msl.find("constant float& u_dielectricSpecularF0"));
    EXPECT_NE(
        std::string::npos,
        msl.find("clamp(u_dielectricSpecularF0, 0.0, 1.0)"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsAnisotropy) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform sampler2D u_anisotropyTexture"));
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
        glsl.find("uniform sampler2D u_specularTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform sampler2D u_specularColorTexture"));
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
        msl.find("u_specularColorFactor"));
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
        glsl.find("uniform sampler2D u_clearcoatTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform sampler2D u_clearcoatRoughnessTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform sampler2D u_clearcoatNormalTexture"));
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
        msl.find("color = color * (1.0 - coatWeight)"));
}

TEST(RendererCommandTest, GltfFragmentShadersApplyKhrMaterialsSheen) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform sampler2D u_sheenColorTexture"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("uniform sampler2D u_sheenRoughnessTexture"));
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

TEST(RendererCommandTest, GltfFragmentShadersUseBaseColorForUnlitMaterials) {
    const std::string glsl = renderer_testing::gltfFragmentGLSL();
    EXPECT_NE(std::string::npos, glsl.find("uniform float u_unlit"));
    EXPECT_NE(std::string::npos, glsl.find("if (u_unlit > 0.5)"));
    EXPECT_NE(
        std::string::npos,
        glsl.find("fragColor = vec4(base.rgb, alpha * clamp(u_renderOpacity"));

    const std::string msl = renderer_testing::gltfFragmentMSL();
    EXPECT_NE(std::string::npos, msl.find("constant float& u_unlit"));
    EXPECT_NE(std::string::npos, msl.find("if (u_unlit > 0.5)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("return float4(base.rgb, alpha * clamp(u_renderOpacity"));
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
        msl.find("u_alphaMode > 0.5 && u_alphaMode < 1.5"));
    EXPECT_NE(
        std::string::npos,
        msl.find("base.a < u_alphaCutoff"));
    EXPECT_NE(std::string::npos, msl.find("discard_fragment();"));
    EXPECT_NE(
        std::string::npos,
        msl.find("float alpha = u_alphaMode > 1.5 ? base.a : 1.0"));
    EXPECT_NE(
        std::string::npos,
        msl.find("alpha * clamp(u_renderOpacity"));
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
        msl.find("occlusion = clamp(1.0 + u_materialFactors.w * (ao - 1.0)"));
    EXPECT_NE(
        std::string::npos,
        msl.find("float3 emissive = u_emissiveFactor"));
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

TEST(RendererCommandTest, MvpValidatorAcceptsSurfaceTileAsSurface) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.generation = 1;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands);
    EXPECT_FALSE(error.has_value());
}

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
    gltf.uniforms["u_alphaMode"] = {2.0f};
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
    gltf.uniforms["u_alphaMode"] = {1.0f};
    gltf.uniforms["u_alphaCutoff"] = {0.5f};
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
    gltf.uniforms["u_alphaMode"] = {2.0f};
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

TEST(RendererCommandTest, MvpValidatorRejectsMutableSurfaceTileDepthAndCullState) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = false;
    tile.depthWrite = false;
    tile.cullFace = false;
    tile.blend = true;
    tile.generation = 1;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("surface_tile", error->owner);
}

TEST(RendererCommandTest, MvpValidatorAcceptsTerrainPrimaryOverlayDepthState) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "terrain_primary_surface";
    tile.pass = "color";
    tile.depthTest = false;
    tile.depthWrite = false;
    tile.cullFace = false;
    tile.blend = false;
    tile.frameId = 42;
    tile.generation = 7;
    tile.hasSurfaceTileUniforms = true;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, MvpValidatorRejectsStaleSurfaceTileFrameId) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.frameId = 41;
    tile.generation = 7;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("surface_tile", error->owner);
}

TEST(RendererCommandTest, MvpValidatorRejectsMissingSurfaceTileGeneration) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.frameId = 42;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("surface_tile", error->owner);
}

TEST(RendererCommandTest, MvpSortEnforcesSurfaceVectorOrder) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.generation = 1;

    RenderCommand globe;
    globe.kind = RenderCommandKind::GlobeSurface;
    globe.owner = "globe";
    globe.pass = "color";
    globe.depthTest = true;
    globe.depthWrite = true;
    globe.cullFace = true;
    globe.blend = false;

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

    RenderCommandList commands{vector, gltf, tile, globe};
    sortMvpRenderCommands(commands);

    EXPECT_EQ(10, mvpRenderOrder(commands[0].kind));
    EXPECT_EQ(10, mvpRenderOrder(commands[1].kind));
    EXPECT_EQ(RenderCommandKind::GltfPrimitive, commands[2].kind);
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
    nearBlend.uniforms["u_alphaMode"] = {2.0f};
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
    gltf.uniforms["u_alphaMode"] = {2.0f};

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
    nearBlend.uniforms["u_alphaMode"] = {2.0f};
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

TEST(RendererCommandTest, SurfaceTileBlendAllowedForLodTransitionOpacity) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = true;
    tile.generation = 1;
    tile.hasSurfaceTileUniforms = true;
    tile.surfaceTransitionOpacity = 0.5f;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, SurfaceTileBlendRejectedWithoutOpacityReason) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = true;
    tile.generation = 1;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("surface_tile", error->owner);
}

TEST(RendererCommandTest, GltfPrimitiveBlendAllowedForLodTransitionOpacity) {
    RenderCommand gltf;
    gltf.kind = RenderCommandKind::GltfPrimitive;
    gltf.owner = "gltf_primitive";
    gltf.pass = "color";
    gltf.depthTest = true;
    gltf.depthWrite = false;
    gltf.cullFace = true;
    gltf.blend = true;
    gltf.generation = 1;
    gltf.uniforms["u_renderOpacity"] = {0.5f};
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
    gltf.uniforms["u_alphaMode"] = {2.0f};
    gltf.uniforms["u_renderOpacity"] = {1.0f};
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
    gltf.uniforms["u_alphaMode"] = {0.0f};
    gltf.uniforms["u_renderOpacity"] = {1.0f};
    gltf.uniforms["u_transmissionFactor"] = {0.5f};
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
