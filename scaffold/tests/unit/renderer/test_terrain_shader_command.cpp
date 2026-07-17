#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/GltfDrawCommandBuilder.h"
#include "earth_engine/tiling/GltfRenderGeometryBuilder.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"

#include "../../helpers/MockRenderDevice.h"

#include <memory>
#include <string>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
namespace renderer_testing {
const char* terrainVertexGLSL();
const char* terrainFragmentGLSL();
const char* terrainVertexMSL();
const char* terrainFragmentMSL();
} // namespace renderer_testing
} // namespace earth_engine

namespace {

// Build a primitive with real (dummy) GPU buffers so GltfDrawCommandBuilder
// treats it as drawable (vertexBuffer && indexBuffer && indexCount > 0).
GltfPrimitiveRenderResources makePrimitive(RenderDevice& device,
                                           bool terrainVertexFormat) {
    GltfPrimitiveRenderResources primitive;
    BufferDesc vbDesc;
    vbDesc.size = sizeof(TerrainGpuVertex) * 3;  // three TerrainGpuVertex
    vbDesc.type = BufferDesc::Type::Vertex;
    primitive.vertexBuffer = device.createBuffer(vbDesc);
    BufferDesc ibDesc;
    ibDesc.size = sizeof(uint32_t) * 3;
    ibDesc.type = BufferDesc::Type::Index;
    primitive.indexBuffer = device.createBuffer(ibDesc);
    primitive.indexCount = 3;
    primitive.vertexCount = 3;
    primitive.useTerrainVertexFormat = terrainVertexFormat;
    return primitive;
}

} // namespace

// ── makeTerrainPrimitiveCommand shape ──

TEST(TerrainShaderCommandTest, MakeTerrainPrimitiveCommandHasCorrectDefaults) {
    earth_engine::testing::MockRenderDevice device;
    Renderer renderer(&device);
    ASSERT_TRUE(renderer.initialize());
    ASSERT_NE(nullptr, renderer.terrainShader());

    RenderCommand cmd = renderer.makeTerrainPrimitiveCommand(
        nullptr, nullptr, 36, 24);

    EXPECT_EQ(RenderCommandKind::GltfPrimitive, cmd.kind);
    EXPECT_EQ("terrain_primitive", cmd.owner);
    EXPECT_EQ("color", cmd.pass);
    EXPECT_EQ(static_cast<int>(sizeof(TerrainGpuVertex)), cmd.vertexStride);
    EXPECT_EQ(36, cmd.indexCount);
    EXPECT_EQ(24, cmd.vertexCount);
    EXPECT_EQ(renderer.terrainShader(), cmd.shader);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_TRUE(cmd.depthWrite);
    EXPECT_TRUE(cmd.cullFace);
    EXPECT_FALSE(cmd.blend);
    // Uniforms now live in the fixed-size block; the string map stays empty
    // (hot-path zero-allocation contract). Defaults come from the block's
    // member initializers.
    EXPECT_TRUE(cmd.hasGltfUniforms);
    EXPECT_TRUE(cmd.uniforms.empty());
    EXPECT_FLOAT_EQ(0.82f, cmd.gltfUniforms.baseColor[0]);
    EXPECT_FLOAT_EQ(0.84f, cmd.gltfUniforms.baseColor[1]);
    EXPECT_FLOAT_EQ(0.88f, cmd.gltfUniforms.baseColor[2]);
    EXPECT_FLOAT_EQ(1.0f, cmd.gltfUniforms.baseColor[3]);
    EXPECT_FLOAT_EQ(1.0f, cmd.gltfUniforms.renderOpacity);
    EXPECT_EQ((std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f}),
              cmd.gltfUniforms.clipUv);
    EXPECT_FLOAT_EQ(0.0f, cmd.gltfUniforms.clipEnabled);
    EXPECT_FLOAT_EQ(0.0f, cmd.gltfUniforms.mappedRasterTextureCount);
    // PBR-extension fields still exist in the block but stay at defaults
    // (the "absent from the map" semantics no longer apply).
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              cmd.gltfUniforms.materialFactors);
    EXPECT_EQ((std::array<float, 3>{0.0f, 0.0f, 1.0f}),
              cmd.gltfUniforms.clearcoatFactors);
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              cmd.gltfUniforms.specularGlossinessFactor);
}

// ── GltfDrawCommandBuilder branches on useTerrainVertexFormat ──

TEST(TerrainShaderCommandTest, TerrainPrimitiveUsesTerrainShaderAndStride) {
    earth_engine::testing::MockRenderDevice device;
    Renderer renderer(&device);
    ASSERT_TRUE(renderer.initialize());
    ASSERT_NE(nullptr, renderer.terrainShader());
    ASSERT_NE(nullptr, renderer.gltfShader());

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    auto model = std::make_unique<GltfModel>();
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        makePrimitive(device, /*terrainVertexFormat=*/true));
    tile.content.renderContent.setGltfResourcesReady(true);

    std::vector<ActivatedRasterOverlay*> overlays;
    RenderCommandList commands;
    GltfDrawCommandBuildContext context;
    context.frameNumber = 7;
    context.generation = 3;
    GltfDrawCommandBuilder::build(renderer, tile, overlays, commands, context);

    ASSERT_EQ(1u, commands.size());
    const RenderCommand& cmd = commands.front();
    EXPECT_EQ(static_cast<int>(sizeof(TerrainGpuVertex)), cmd.vertexStride);
    EXPECT_EQ(renderer.terrainShader(), cmd.shader);
    EXPECT_EQ(RenderCommandKind::GltfPrimitive, cmd.kind);
    // The shared per-command population still runs on the terrain command.
    EXPECT_TRUE(cmd.terrainRenderContent == cmd.terrainRenderContent);
    EXPECT_TRUE(cmd.hasGltfUniforms);
    EXPECT_FLOAT_EQ(1.0f, cmd.gltfUniforms.renderOpacity);
}

TEST(TerrainShaderCommandTest, NonTerrainPrimitiveUsesGltfShaderAndStride) {
    earth_engine::testing::MockRenderDevice device;
    Renderer renderer(&device);
    ASSERT_TRUE(renderer.initialize());

    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0));
    auto model = std::make_unique<GltfModel>();
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.addGltfPrimitiveResource(
        makePrimitive(device, /*terrainVertexFormat=*/false));
    tile.content.renderContent.setGltfResourcesReady(true);

    std::vector<ActivatedRasterOverlay*> overlays;
    RenderCommandList commands;
    GltfDrawCommandBuildContext context;
    GltfDrawCommandBuilder::build(renderer, tile, overlays, commands, context);

    ASSERT_EQ(1u, commands.size());
    const RenderCommand& cmd = commands.front();
    EXPECT_EQ(120, cmd.vertexStride);
    EXPECT_EQ(renderer.gltfShader(), cmd.shader);
    EXPECT_NE(renderer.terrainShader(), cmd.shader);
}

// ── Terrain shader source hygiene (parity with the design constraints) ──

TEST(TerrainShaderCommandTest, TerrainShadersDropPbrExtensionUniforms) {
    const std::string glslV = renderer_testing::terrainVertexGLSL();
    const std::string glslF = renderer_testing::terrainFragmentGLSL();
    const std::string mslV = renderer_testing::terrainVertexMSL();
    const std::string mslF = renderer_testing::terrainFragmentMSL();

    // Kept: base color, raster overlays, water mask, clip, lighting, opacity.
    EXPECT_NE(std::string::npos, glslF.find("u_baseColor"));
    EXPECT_NE(std::string::npos, glslF.find("u_mappedRasterTextureCount"));
    EXPECT_NE(std::string::npos, glslF.find("u_gltfWaterMaskState"));
    EXPECT_NE(std::string::npos, glslF.find("u_clipEnabled"));
    EXPECT_NE(std::string::npos, glslF.find("u_lightDir"));
    EXPECT_NE(std::string::npos, glslF.find("u_renderOpacity"));
    EXPECT_NE(std::string::npos, mslF.find("terrainFragment"));
    EXPECT_NE(std::string::npos, mslV.find("terrainVertex"));

    // Dropped: PBR-extension uniforms must not appear in either shader.
    for (const std::string& source : {glslF, mslF}) {
        EXPECT_EQ(std::string::npos, source.find("u_materialFactors"));
        EXPECT_EQ(std::string::npos, source.find("u_clearcoatFactors"));
        EXPECT_EQ(std::string::npos, source.find("u_sheenColorFactor"));
        EXPECT_EQ(std::string::npos, source.find("u_specularGlossinessFactor"));
        EXPECT_EQ(std::string::npos, source.find("u_anisotropyFactors"));
        EXPECT_EQ(std::string::npos, source.find("u_transmissionFactor"));
    }

    // Terrain keeps both supported projection UV sets in one packed attribute.
    EXPECT_NE(
        std::string::npos,
        glslV.find("in vec4 a_texcoord01;"));
    EXPECT_NE(
        std::string::npos,
        glslF.find(
            "setIndex == 1 ? v_texcoord01.zw : v_texcoord01.xy"));
    EXPECT_NE(
        std::string::npos,
        mslV.find("float4 texcoord01 [[attribute(2)]]"));
    EXPECT_NE(
        std::string::npos,
        mslF.find(
            "setIndex == 1 ? in.texcoord01.zw : in.texcoord01.xy"));
}

// ── Metal name -> [[buffer(N)]] parity, all indices <= 30 ──

TEST(TerrainShaderCommandTest, TerrainFragmentMslBufferIndicesStayUnderMetalCap) {
    const std::string msl = renderer_testing::terrainFragmentMSL();

    // The per-name bind table is gone: the terrain fragment now consumes the
    // whole GltfUniformBlock mirror struct in a single buffer(0) binding.
    EXPECT_NE(
        std::string::npos,
        msl.find("constant GltfUniforms& u [[buffer(0)]]"));

    // The fields the terrain shader consumes must remain in the mirror struct
    // (same set as the old bind table, spelled as struct members).
    for (const char* field : {
             "packed_float3 lightDir;",
             "packed_float4 baseColor;",
             "float renderOpacity;",
             "float hasBaseColorTexture;",
             "float alphaMode;",
             "float alphaCutoff;",
             "float mappedRasterTextureCount;",
             "packed_float4 mappedRasterTileUV[4];",
             "float mappedRasterOpacity[4];",
             "float mappedRasterTexCoordSet[4];",
             "float hasWaterMask;",
             "packed_float4 waterMaskTranslationScale;",
             "packed_float4 waterMaskState;",
             "packed_float4 clipUV;",
             "float clipEnabled;"}) {
        EXPECT_NE(std::string::npos, msl.find(field))
            << "missing GltfUniforms field: " << field;
    }

    // Preserve the original intent: every [[buffer(N)]] index in the terrain
    // fragment source must stay <= 30 (Metal argument-table cap is 31).
    const std::string marker = "[[buffer(";
    int maxIndex = -1;
    for (size_t pos = msl.find(marker);
         pos != std::string::npos;
         pos = msl.find(marker, pos + marker.size())) {
        const size_t start = pos + marker.size();
        const size_t end = msl.find(")]]", start);
        ASSERT_NE(std::string::npos, end);
        maxIndex = std::max(maxIndex, std::stoi(msl.substr(start, end - start)));
    }
    EXPECT_GE(maxIndex, 0) << "terrain fragment MSL binds no buffers";
    EXPECT_LE(maxIndex, 30);
}
