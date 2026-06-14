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
// Unified SurfaceTile Shader — cesium-native glTF vertex layout
// POSITION(vec3) + NORMAL(vec3) + TEXCOORD_0(vec2) = 32 bytes
// ============================================================

static const char* kSurfaceTileVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texcoord;

uniform mat4 u_modelViewProjection;
uniform vec4 u_tileUV;
uniform vec3 u_tileOrigin;

out vec2 v_texcoord;
out vec3 v_normal;
out float v_tileOpacity;
out float v_transitionOpacity;

void main() {
    v_texcoord = u_tileUV.xy + a_texcoord * u_tileUV.zw;
    vec3 worldPos = a_position + u_tileOrigin;
    v_normal = normalize(a_normal);
    v_tileOpacity = 1.0;
    v_transitionOpacity = 1.0;
    gl_Position = u_modelViewProjection * vec4(worldPos, 1.0);
}
)glsl";

static const char* kSurfaceTileFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

in vec2 v_texcoord;
in vec3 v_normal;
in float v_tileOpacity;
in float v_transitionOpacity;
uniform sampler2D u_tileTexture;
uniform vec3 u_lightDir;
out vec4 fragColor;

void main() {
    vec4 color = texture(u_tileTexture, v_texcoord);
    vec3 N = normalize(v_normal);
    vec3 L = normalize(u_lightDir);
    float NdotL = max(dot(N, L), 0.0);

    // cesium-native PBR: metallic=0, roughness=1
    //   Diffuse: baseColor/π * (1 - F) * NdotL ≈ baseColor * 0.318 * (1-F) * NdotL
    //   Fresnel: F = F0 + (1-F0) * (1 - NdotL)^5, with F0 = 0.04 (dielectric)
    //   At grazing angles, Fresnel adds ~4-8% specular
    float F0 = 0.04;
    float F = F0 + (1.0 - F0) * pow(1.0 - NdotL, 5.0);
    float diffuse = NdotL * 0.318;  // 1/π
    float ambient = 0.03;

    color.rgb *= ambient + diffuse * (1.0 - F) + F * 0.5 * NdotL;
    color.a *= clamp(v_tileOpacity, 0.0, 1.0) * clamp(v_transitionOpacity, 0.0, 1.0);
    fragColor = color;
}
)glsl";

// ============================================================
// Deprecated shaders (kept for reference)
// ============================================================

#if 0
static const char* kTileVertexGLSL_deprecated = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 2) in vec2 a_texcoord;

uniform mat4 u_modelViewProjection;
uniform vec4 u_tileUV;
uniform vec3 u_tileOrigin;

out vec2 v_texcoord;
out vec2 v_gridUv;
out vec3 v_normal;

// WGS84 inverse radii squared
const vec3 kInvRadiiSq = vec3(
    1.0 / (6378137.0 * 6378137.0),
    1.0 / (6378137.0 * 6378137.0),
    1.0 / (6356752.314245 * 6356752.314245));

void main() {
    v_texcoord = u_tileUV.xy + a_texcoord * u_tileUV.zw;
    v_gridUv = a_texcoord;
    vec3 worldPos = a_position + u_tileOrigin;
    v_normal = normalize(worldPos * kInvRadiiSq);
    gl_Position = u_modelViewProjection * vec4(worldPos, 1.0);
}
)glsl";

static const char* kTileFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

in vec2 v_texcoord;
in vec2 v_gridUv;
in vec3 v_normal;
uniform sampler2D u_tileTexture;
uniform sampler2D u_overlayTexture0;
uniform sampler2D u_overlayTexture1;
uniform sampler2D u_overlayTexture2;
uniform sampler2D u_overlayTexture3;
uniform sampler2D u_waterMask;
uniform vec3 u_lightDir;
uniform vec3 u_fogColor;
uniform float u_fogDensity;
uniform float u_tileOpacity;
uniform float u_transitionOpacity;
uniform int u_overlayTextureCount;
uniform float u_hasWaterMask;
uniform vec4 u_overlayTileUV0;
uniform vec4 u_overlayTileUV1;
uniform vec4 u_overlayTileUV2;
uniform vec4 u_overlayTileUV3;
uniform float u_overlayOpacity0;
uniform float u_overlayOpacity1;
uniform float u_overlayOpacity2;
uniform float u_overlayOpacity3;
out vec4 fragColor;

vec4 alphaOver(vec4 base, vec4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

void main() {
    vec4 color = texture(u_tileTexture, v_texcoord);
    color.a = 1.0;
    color.a *= clamp(u_tileOpacity, 0.0, 1.0) * clamp(u_transitionOpacity, 0.0, 1.0);
    if (u_overlayTextureCount > 0) {
        vec2 overlayUv = u_overlayTileUV0.xy + v_gridUv * u_overlayTileUV0.zw;
        color = alphaOver(color, texture(u_overlayTexture0, overlayUv), u_overlayOpacity0);
    }
    if (u_overlayTextureCount > 1) {
        vec2 overlayUv = u_overlayTileUV1.xy + v_gridUv * u_overlayTileUV1.zw;
        color = alphaOver(color, texture(u_overlayTexture1, overlayUv), u_overlayOpacity1);
    }
    if (u_overlayTextureCount > 2) {
        vec2 overlayUv = u_overlayTileUV2.xy + v_gridUv * u_overlayTileUV2.zw;
        color = alphaOver(color, texture(u_overlayTexture2, overlayUv), u_overlayOpacity2);
    }
    if (u_overlayTextureCount > 3) {
        vec2 overlayUv = u_overlayTileUV3.xy + v_gridUv * u_overlayTileUV3.zw;
        color = alphaOver(color, texture(u_overlayTexture3, overlayUv), u_overlayOpacity3);
    }
    fragColor = color;
}
)glsl";
#endif

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
#include <metal_stdlib>
using namespace metal;
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
    // normal removed — GPU computes geodetic normal from position
    float2 texcoord [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 texcoord;
    float3 normal;
};

vertex VertexOut tileVertex(VertexIn in [[stage_in]],
                             constant float4x4& u_modelViewProjection [[buffer(1)]],
                             constant float4& u_tileUV [[buffer(3)]]) {
    VertexOut out;
    out.position = u_modelViewProjection *
        float4(in.position, 1.0);
    out.texcoord = u_tileUV.xy + in.texcoord * u_tileUV.zw;
    // WGS84 geodetic normal from ECEF position
    constexpr float3 kInvRadiiSq = float3(
        1.0f / (6378137.0f * 6378137.0f),
        1.0f / (6378137.0f * 6378137.0f),
        1.0f / (6356752.314245f * 6356752.314245f));
    out.normal = normalize(in.position * kInvRadiiSq);
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
#include <metal_stdlib>
using namespace metal;

fragment float4 tileFragment(VertexOut in [[stage_in]],
                             texture2d<float> u_tileTexture [[texture(0)]],
                             sampler u_sampler [[sampler(0)]],
                             constant float3& u_lightDir [[buffer(0)]],
                             constant float& u_tileOpacity [[buffer(1)]],
                             constant float& u_transitionOpacity [[buffer(2)]]) {
    float4 color = u_tileTexture.sample(u_sampler, in.texcoord);
    float luma = dot(color.rgb, float3(0.299, 0.587, 0.114));
    color.rgb = mix(float3(luma), color.rgb, 1.08);
    color.rgb = (color.rgb - float3(0.5)) * 1.06 + float3(0.5);
    color.rgb = clamp(color.rgb, 0.0, 1.0);

    float3 n = normalize(in.normal);
    float diffuse = max(dot(n, normalize(u_lightDir)), 0.0);
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

    // Surface tile (unified, cesium-native glTF layout)
    std::unique_ptr<ShaderProgram> surfaceTileShader;
    std::unique_ptr<Buffer> tileIndexBuffer;  // shared 64×64 grid IBO
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
    if (!impl_->globeShader) { fprintf(stderr, "[Renderer] globeShader failed\n"); return false; }

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

    // ---- Unified SurfaceTile shader (cesium-native glTF layout) ----
    if (!isMetal) {
        ShaderDesc surfaceTileSd;
        surfaceTileSd.vertexSource = kSurfaceTileVertexGLSL;
        surfaceTileSd.fragmentSource = kSurfaceTileFragmentGLSL;
        impl_->surfaceTileShader = dev->createShader(surfaceTileSd);
        if (!impl_->surfaceTileShader) {
            fprintf(stderr, "[Renderer] surfaceTileShader failed\n");
            return false;
        }
    }

    // Shared index buffer for surface tiles (64×64 grid)
    auto [tileVerts, tileIndices] = makeTileGeometry(64);
    (void)tileVerts;  // VBOs are per-tile now

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
    impl_->surfaceTileShader.reset();
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

ShaderProgram* Renderer::colorShader() const { return impl_->colorShader.get(); }
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
                                                Buffer* vertexBuffer,
                                                Buffer* indexBuffer,
                                                int indexCount) const {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::SurfaceTile;
    cmd.owner = "surface_tile";
    cmd.pass = "color";
    cmd.shader = impl_->surfaceTileShader.get();
    cmd.vertexBuffer = vertexBuffer;
    cmd.indexBuffer = indexBuffer ? indexBuffer : impl_->tileIndexBuffer.get();
    cmd.indexCount = indexBuffer ? indexCount : impl_->tileIndexCount;
    cmd.vertexStride = 32;  // POSITION(12) + NORMAL(12) + TEXCOORD_0(8)
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    cmd.depthTest = true;
    cmd.depthWrite = true;
    cmd.blend = false;
    cmd.cullFace = true;
    cmd.hasSurfaceTileUniforms = true;
    cmd.surfaceHasWaterMask = 0.0f;
    if (texture) {
        cmd.textures.push_back(texture);
    }
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
