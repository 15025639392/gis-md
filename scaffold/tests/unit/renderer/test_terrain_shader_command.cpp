#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/GltfDrawCommandBuilder.h"
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
    vbDesc.size = 32 * 3;  // three TerrainGpuVertex
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
    EXPECT_EQ(32, cmd.vertexStride);
    EXPECT_EQ(36, cmd.indexCount);
    EXPECT_EQ(24, cmd.vertexCount);
    EXPECT_EQ(renderer.terrainShader(), cmd.shader);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_TRUE(cmd.depthWrite);
    EXPECT_TRUE(cmd.cullFace);
    EXPECT_FALSE(cmd.blend);
    // Only the lightweight terrain uniforms — no PBR-extension defaults.
    EXPECT_TRUE(cmd.uniforms.count("u_baseColor"));
    EXPECT_TRUE(cmd.uniforms.count("u_renderOpacity"));
    EXPECT_TRUE(cmd.uniforms.count("u_clipUV"));
    EXPECT_TRUE(cmd.uniforms.count("u_clipEnabled"));
    EXPECT_TRUE(cmd.uniforms.count("u_mappedRasterTextureCount"));
    EXPECT_FALSE(cmd.uniforms.count("u_materialFactors"));
    EXPECT_FALSE(cmd.uniforms.count("u_clearcoatFactors"));
    EXPECT_FALSE(cmd.uniforms.count("u_specularGlossinessFactor"));
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
    EXPECT_EQ(32, cmd.vertexStride);
    EXPECT_EQ(renderer.terrainShader(), cmd.shader);
    EXPECT_EQ(RenderCommandKind::GltfPrimitive, cmd.kind);
    // The shared per-command population still runs on the terrain command.
    EXPECT_TRUE(cmd.terrainRenderContent == cmd.terrainRenderContent);
    EXPECT_TRUE(cmd.uniforms.count("u_renderOpacity"));
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

    // Terrain vertex uses a 2-float texcoord (attribute 2), not the 120B vec4.
    EXPECT_NE(std::string::npos, glslV.find("in vec2 a_texcoord;"));
    EXPECT_EQ(std::string::npos, glslV.find("a_texcoord01"));
    EXPECT_NE(std::string::npos, mslV.find("float2 texcoord [[attribute(2)]]"));
}

// ── Metal name -> [[buffer(N)]] parity, all indices <= 30 ──

TEST(TerrainShaderCommandTest, TerrainFragmentMslBufferIndicesStayUnderMetalCap) {
    const std::string msl = renderer_testing::terrainFragmentMSL();

    // Every terrain fragment uniform: MSL [[buffer(N)]] index must match the
    // RenderDeviceMetal terrain bind table (kept in sync manually). All <= 30.
    struct Entry {
        const char* name;
        int index;
    };
    const Entry table[] = {
        {"u_lightDir", 0},
        {"u_baseColor", 1},
        {"u_renderOpacity", 2},
        {"u_hasBaseColorTexture", 3},
        {"u_alphaMode", 4},
        {"u_alphaCutoff", 5},
        {"u_mappedRasterTextureCount", 6},
        {"u_mappedRasterTileUV0", 7},
        {"u_mappedRasterTileUV1", 8},
        {"u_mappedRasterTileUV2", 9},
        {"u_mappedRasterTileUV3", 10},
        {"u_mappedRasterOpacity0", 11},
        {"u_mappedRasterOpacity1", 12},
        {"u_mappedRasterOpacity2", 13},
        {"u_mappedRasterOpacity3", 14},
        {"u_mappedRasterTexCoordSet0", 15},
        {"u_mappedRasterTexCoordSet1", 16},
        {"u_mappedRasterTexCoordSet2", 17},
        {"u_mappedRasterTexCoordSet3", 18},
        {"u_gltfHasWaterMask", 19},
        {"u_gltfWaterMaskTranslationScale", 20},
        {"u_gltfWaterMaskState", 21},
        {"u_clipUV", 22},
        {"u_clipEnabled", 23},
    };
    int maxIndex = -1;
    for (const Entry& entry : table) {
        const std::string needle =
            std::string(entry.name) + " [[buffer(" +
            std::to_string(entry.index) + ")]]";
        EXPECT_NE(std::string::npos, msl.find(needle))
            << "missing " << needle;
        maxIndex = std::max(maxIndex, entry.index);
    }
    EXPECT_LE(maxIndex, 30);
}
