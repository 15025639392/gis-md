#include "Renderer.h"
#include "../globe/Globe.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../core/math/Vec3.h"
#include "../core/math/Mat4.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace earth_engine {

// ============================================================
// Globe Shader — GLSL ES 3.0
// ============================================================

static const char* kGlobeVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texcoord;

uniform mat4 u_modelViewProjection;
uniform mat4 u_model;

out vec3 v_normal;
out vec2 v_texcoord;

void main() {
    v_normal = normalize(mat3(u_model) * a_normal);
    v_texcoord = a_texcoord;
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kGlobeFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

in vec3 v_normal;
in vec2 v_texcoord;

uniform vec3 u_lightDir;

out vec4 fragColor;

void main() {
    vec3 n = normalize(v_normal);
    float diffuse = max(dot(n, normalize(u_lightDir)), 0.0);
    vec3 ocean = vec3(0.05, 0.26, 0.58);
    vec3 land = vec3(0.18, 0.48, 0.24);
    float band = smoothstep(0.42, 0.58,
        sin(v_texcoord.x * 37.0) * 0.5 + sin(v_texcoord.y * 23.0) * 0.5 + 0.5);
    vec3 base = mix(ocean, land, band * 0.45);
    vec3 color = base * (0.22 + diffuse * 0.88);
    fragColor = vec4(color, 1.0);
}
)glsl";

// ============================================================
// SurfaceTile Shader — GLSL ES 3.0
// ============================================================

static const char* kTileVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texcoord;

uniform mat4 u_modelViewProjection;
uniform vec4 u_tileUV;     // offsetU, offsetV, scaleU, scaleV
uniform vec3 u_cameraRelativeOrigin;

out vec2 v_texcoord;
out vec3 v_normal;

void main() {
    v_texcoord = u_tileUV.xy + a_texcoord * u_tileUV.zw;
    v_normal = normalize(a_normal);
    gl_Position = u_modelViewProjection * vec4(a_position - u_cameraRelativeOrigin, 1.0);
}
)glsl";

static const char* kTileFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

in vec2 v_texcoord;
in vec3 v_normal;
uniform sampler2D u_tileTexture;
uniform sampler2D u_normalMap;
uniform vec3 u_lightDir;
uniform float u_useNormalMap;
uniform float u_debugNormalMap;
uniform float u_tileOpacity;
uniform float u_transitionOpacity;
out vec4 fragColor;

void main() {
    vec4 color = texture(u_tileTexture, v_texcoord);
    vec3 normalSample = texture(u_normalMap, v_texcoord).rgb;
    if (u_debugNormalMap > 0.5) {
        fragColor = vec4(normalSample, 1.0);
        return;
    }
    vec3 mapNormal = normalize((normalSample - 0.5) * 2.0);
    vec3 normal = normalize(mix(normalize(v_normal), mapNormal, clamp(u_useNormalMap, 0.0, 1.0)));
    float diffuse = max(dot(normal, normalize(u_lightDir)), 0.0);
    color.rgb *= 0.45 + diffuse * 0.55;
    color.a *= clamp(u_tileOpacity, 0.0, 1.0) * clamp(u_transitionOpacity, 0.0, 1.0);
    fragColor = color;
}
)glsl";

// ============================================================
// Metal Shading Language 源码
// ============================================================

static const char* kGlobeVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 texcoord [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float2 texcoord;
};

vertex VertexOut globeVertex(VertexIn in [[stage_in]],
                             constant float4x4& u_modelViewProjection [[buffer(1)]],
                             constant float4x4& u_model [[buffer(2)]]) {
    VertexOut out;
    out.position = u_modelViewProjection * float4(in.position, 1.0);
    out.normal = normalize((u_model * float4(in.normal, 0.0)).xyz);
    out.texcoord = in.texcoord;
    return out;
}
)msl";

static const char* kGlobeFragmentMSL = R"msl(
fragment float4 globeFragment(VertexOut in [[stage_in]],
                              constant float3& u_lightDir [[buffer(0)]]) {
    float3 n = normalize(in.normal);
    float diffuse = max(dot(n, normalize(u_lightDir)), 0.0f);
    float3 ocean = float3(0.05, 0.26, 0.58);
    float3 land = float3(0.18, 0.48, 0.24);
    float band = smoothstep(0.42, 0.58,
        sin(in.texcoord.x * 37.0) * 0.5 + sin(in.texcoord.y * 23.0) * 0.5 + 0.5);
    float3 base = mix(ocean, land, band * 0.45);
    float3 color = base * (0.22 + diffuse * 0.88);
    return float4(color, 1.0);
}
)msl";

static const char* kTileVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 texcoord [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 texcoord;
    float3 normal;
};

vertex VertexOut tileVertex(VertexIn in [[stage_in]],
                             constant float4x4& u_modelViewProjection [[buffer(1)]],
                             constant float4& u_tileUV [[buffer(3)]],
                             constant float3& u_cameraRelativeOrigin [[buffer(4)]]) {
    VertexOut out;
    out.position = u_modelViewProjection *
        float4(in.position - u_cameraRelativeOrigin, 1.0);
    out.texcoord = u_tileUV.xy + in.texcoord * u_tileUV.zw;
    out.normal = normalize(in.normal);
    return out;
}
)msl";

// ============================================================
// Color Shader (Vector Layers) — GLSL ES 3.0
// ============================================================

static const char* kColorVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;

uniform mat4 u_modelViewProjection;

void main() {
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kColorFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

uniform vec4 u_color;
out vec4 fragColor;

void main() {
    fragColor = u_color;
}
)glsl";

// ============================================================
// Color Shader (Vector Layers) — MSL
// ============================================================

static const char* kColorVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexOut {
    float4 position [[position]];
};

vertex VertexOut colorVertex(VertexIn in [[stage_in]],
                              constant float4x4& u_modelViewProjection [[buffer(1)]]) {
    VertexOut out;
    out.position = u_modelViewProjection * float4(in.position, 1.0);
    return out;
}
)msl";

static const char* kColorFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

fragment float4 colorFragment(constant float4& u_color [[buffer(0)]]) {
    return u_color;
}
)msl";

static const char* kTileFragmentMSL = R"msl(
fragment float4 tileFragment(VertexOut in [[stage_in]],
                             texture2d<float> u_tileTexture [[texture(0)]],
                             texture2d<float> u_normalMap [[texture(1)]],
                             sampler u_sampler [[sampler(0)]],
                             constant float3& u_lightDir [[buffer(0)]],
                             constant float& u_useNormalMap [[buffer(1)]],
                             constant float& u_debugNormalMap [[buffer(2)]],
                             constant float& u_tileOpacity [[buffer(3)]],
                             constant float& u_transitionOpacity [[buffer(4)]]) {
    float4 color = u_tileTexture.sample(u_sampler, in.texcoord);
    float3 normalSample = u_normalMap.sample(u_sampler, in.texcoord).rgb;
    if (u_debugNormalMap > 0.5) {
        return float4(normalSample, 1.0);
    }
    float3 mapNormal = normalize((normalSample - 0.5) * 2.0);
    float3 normal = normalize(mix(normalize(in.normal), mapNormal, clamp(u_useNormalMap, 0.0, 1.0)));
    float diffuse = max(dot(normal, normalize(u_lightDir)), 0.0);
    color.rgb *= 0.45 + diffuse * 0.55;
    color.a *= clamp(u_tileOpacity, 0.0, 1.0) * clamp(u_transitionOpacity, 0.0, 1.0);
    return color;
}
)msl";

// ============================================================
// Shared SurfaceTile geometry: unit grid mesh
// ============================================================

struct TileVertex {
    float texcoord[2];
};

static std::pair<std::vector<TileVertex>, std::vector<uint32_t>>
makeTileGeometry(int gridSize) {
    std::vector<TileVertex> verts;
    std::vector<uint32_t> indices;

    int n = gridSize + 1;
    verts.reserve(static_cast<size_t>(n * n));

    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(gridSize);
            float v = static_cast<float>(y) / static_cast<float>(gridSize);
            verts.push_back({{u, v}});
        }
    }

    indices.reserve(static_cast<size_t>(gridSize * gridSize * 6));
    for (int y = 0; y < gridSize; ++y) {
        for (int x = 0; x < gridSize; ++x) {
            uint32_t a = static_cast<uint32_t>(y * n + x);
            uint32_t b = static_cast<uint32_t>(y * n + x + 1);
            uint32_t c = static_cast<uint32_t>((y + 1) * n + x);
            uint32_t d = static_cast<uint32_t>((y + 1) * n + x + 1);
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }

    return {verts, indices};
}

// ============================================================
// Renderer::Impl
// ============================================================

struct Renderer::Impl {
    RenderDevice* device = nullptr;

    // Globe
    std::unique_ptr<ShaderProgram> globeShader;
    std::unique_ptr<Buffer> globeVertexBuffer;
    std::unique_ptr<Buffer> globeIndexBuffer;
    int globeIndexCount = 0;

    // Surface tile
    std::unique_ptr<ShaderProgram> tileShader;
    std::unique_ptr<Buffer> tileVertexBuffer;
    std::unique_ptr<Buffer> tileIndexBuffer;
    int tileIndexCount = 0;

    // Color (vector)
    std::unique_ptr<ShaderProgram> colorShader;

    bool initialized = false;
};

// ============================================================
// Renderer
// ============================================================

Renderer::Renderer(RenderDevice* device)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
}

Renderer::~Renderer() {
    dispose();
}

bool Renderer::initialize(const GlobeMesh& mesh) {
    if (impl_->initialized) dispose();
    auto* dev = impl_->device;
    if (!dev) return false;

    bool isMetal = (dev->backendType() == RenderDevice::Backend::Metal);

    // ---- Globe shader ----
    ShaderDesc globeSd;
    globeSd.vertexSource = isMetal ? kGlobeVertexMSL : kGlobeVertexGLSL;
    globeSd.fragmentSource = isMetal ? kGlobeFragmentMSL : kGlobeFragmentGLSL;
    impl_->globeShader = dev->createShader(globeSd);
    if (!impl_->globeShader) return false;

    // Globe vertex buffer
    BufferDesc vbDesc;
    vbDesc.size = mesh.vertices.size() * sizeof(GlobeVertex);
    vbDesc.data = mesh.vertices.data();
    vbDesc.usage = BufferDesc::Usage::Static;
    vbDesc.type = BufferDesc::Type::Vertex;
    impl_->globeVertexBuffer = dev->createBuffer(vbDesc);
    if (!impl_->globeVertexBuffer) return false;

    // Globe index buffer
    BufferDesc ibDesc;
    ibDesc.size = mesh.indices.size() * sizeof(uint32_t);
    ibDesc.data = mesh.indices.data();
    ibDesc.usage = BufferDesc::Usage::Static;
    ibDesc.type = BufferDesc::Type::Index;
    impl_->globeIndexBuffer = dev->createBuffer(ibDesc);
    if (!impl_->globeIndexBuffer) return false;
    impl_->globeIndexCount = static_cast<int>(mesh.indices.size());

    // ---- Tile shader ----
    ShaderDesc tileSd;
    tileSd.vertexSource = isMetal ? kTileVertexMSL : kTileVertexGLSL;
    tileSd.fragmentSource = isMetal ? kTileFragmentMSL : kTileFragmentGLSL;
    impl_->tileShader = dev->createShader(tileSd);
    if (!impl_->tileShader) return false;

    // Tile shared geometry. Web Mercator tile bounds are converted to ECEF on a curved
    // ellipsoid, so low subdivision visibly warps large low-zoom tiles.
    auto [tileVerts, tileIndices] = makeTileGeometry(32);

    BufferDesc tvbDesc;
    tvbDesc.size = tileVerts.size() * sizeof(TileVertex);
    tvbDesc.data = tileVerts.data();
    tvbDesc.usage = BufferDesc::Usage::Static;
    tvbDesc.type = BufferDesc::Type::Vertex;
    impl_->tileVertexBuffer = dev->createBuffer(tvbDesc);
    if (!impl_->tileVertexBuffer) return false;

    BufferDesc tibDesc;
    tibDesc.size = tileIndices.size() * sizeof(uint32_t);
    tibDesc.data = tileIndices.data();
    tibDesc.usage = BufferDesc::Usage::Static;
    tibDesc.type = BufferDesc::Type::Index;
    impl_->tileIndexBuffer = dev->createBuffer(tibDesc);
    if (!impl_->tileIndexBuffer) return false;
    impl_->tileIndexCount = static_cast<int>(tileIndices.size());

    // ---- Color shader (vector layers) ----
    ShaderDesc colorSd;
    colorSd.vertexSource = isMetal ? kColorVertexMSL : kColorVertexGLSL;
    colorSd.fragmentSource = isMetal ? kColorFragmentMSL : kColorFragmentGLSL;
    impl_->colorShader = dev->createShader(colorSd);
    // colorShader failure is non-fatal (vector layers won't render but globe still works)

    impl_->initialized = true;
    return true;
}

void Renderer::submit(const RenderCommandList& commands) {
    if (!impl_->initialized || !impl_->device) return;
    impl_->device->submit(commands);
}

void Renderer::dispose() {
    impl_->globeShader.reset();
    impl_->globeVertexBuffer.reset();
    impl_->globeIndexBuffer.reset();
    impl_->tileShader.reset();
    impl_->tileVertexBuffer.reset();
    impl_->tileIndexBuffer.reset();
    impl_->colorShader.reset();
    impl_->globeIndexCount = 0;
    impl_->tileIndexCount = 0;
    impl_->initialized = false;
}

// ---- 共享资源访问 ----

ShaderProgram* Renderer::globeShader() const { return impl_->globeShader.get(); }
Buffer* Renderer::globeVertexBuffer() const { return impl_->globeVertexBuffer.get(); }
Buffer* Renderer::globeIndexBuffer() const { return impl_->globeIndexBuffer.get(); }
int Renderer::globeIndexCount() const { return impl_->globeIndexCount; }

ShaderProgram* Renderer::tileShader() const { return impl_->tileShader.get(); }
ShaderProgram* Renderer::colorShader() const { return impl_->colorShader.get(); }
Buffer* Renderer::tileVertexBuffer() const { return impl_->tileVertexBuffer.get(); }
Buffer* Renderer::tileIndexBuffer() const { return impl_->tileIndexBuffer.get(); }
int Renderer::tileIndexCount() const { return impl_->tileIndexCount; }

// ---- Command builders ----

RenderCommand Renderer::makeGlobeCommand(const FrameState& frameState) const {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::GlobeSurface;
    cmd.owner = "globe";
    cmd.pass = "color";
    cmd.shader = impl_->globeShader.get();
    cmd.vertexBuffer = impl_->globeVertexBuffer.get();
    cmd.indexBuffer = impl_->globeIndexBuffer.get();
    cmd.indexCount = impl_->globeIndexCount;
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    cmd.depthTest = true;
    cmd.depthWrite = true;
    cmd.blend = false;
    cmd.cullFace = true;

    if (frameState.camera) {
        const Camera& cam = *frameState.camera;
        float vpW = static_cast<float>(frameState.viewportWidthPixels);
        float vpH = static_cast<float>(frameState.viewportHeightPixels);

        glm::mat4 model = glm::make_mat4(earthModelMatrix().data());
        glm::mat4 view(cam.viewMatrix().raw());
        glm::mat4 proj(cam.projectionMatrix(vpW, vpH).raw());
        glm::mat4 mvp = proj * view * model;

        auto& mvpU = cmd.uniforms["u_modelViewProjection"];
        mvpU.resize(16);
        std::memcpy(mvpU.data(), glm::value_ptr(mvp), 16 * sizeof(float));

        auto& modelU = cmd.uniforms["u_model"];
        modelU.resize(16);
        std::memcpy(modelU.data(), glm::value_ptr(model), 16 * sizeof(float));
    }

    cmd.uniforms["u_lightDir"] = {
        frameState.lightDir.x,
        frameState.lightDir.y,
        frameState.lightDir.z
    };
    return cmd;
}

RenderCommand Renderer::makeSurfaceTileCommand(Texture* texture,
                                                Texture* normalMapTexture,
                                                Buffer* vertexBuffer,
                                                Buffer* indexBuffer,
                                                int indexCount,
                                                float uvOffsetX,
                                                float uvOffsetY,
                                                float uvScaleX,
                                                float uvScaleY) const {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::SurfaceTile;
    cmd.owner = "surface_tile";
    cmd.pass = "color";
    cmd.shader = impl_->tileShader.get();
    cmd.vertexBuffer = vertexBuffer;
    cmd.indexBuffer = indexBuffer;
    cmd.indexCount = indexCount;
    cmd.vertexStride = 32;
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    // SurfaceTile is the authoritative MVP globe surface and writes depth.
    // Imagery is attached to this surface instead of competing with a globe mesh.
    cmd.depthTest = true;
    cmd.depthWrite = true;
    cmd.blend = false;
    cmd.cullFace = true;

    if (texture) {
        cmd.textures.push_back(texture);
    }
    if (normalMapTexture) {
        cmd.textures.push_back(normalMapTexture);
    }

    cmd.uniforms["u_tileUV"] = {uvOffsetX, uvOffsetY, uvScaleX, uvScaleY};
    cmd.uniforms["u_useNormalMap"] = {normalMapTexture ? 1.0f : 0.0f};
    cmd.uniforms["u_tileOpacity"] = {1.0f};
    cmd.uniforms["u_transitionOpacity"] = {1.0f};
    return cmd;
}

std::array<float, 16> Renderer::earthModelMatrix() {
    constexpr float kEarthRadius = 6378137.0f;
    glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(kEarthRadius));
    std::array<float, 16> result;
    std::memcpy(result.data(), glm::value_ptr(m), 16 * sizeof(float));
    return result;
}

} // namespace earth_engine
