#include "DebugOverlay.h"
#include "../renderer/RenderDevice.h"
#include "../tiling/TileScheme.h"
#include "../core/math/Rectangle.h"

#include <array>

namespace earth_engine {

// ============================================================
// Debug Line Shader — GLSL ES 3.0
// ============================================================

static const char* kDebugLineVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec2 a_texcoord;

uniform mat4 u_modelViewProjection;
uniform vec4 u_tileBounds;

out vec2 v_texcoord;

vec3 geoToECEF(vec2 lngLat) {
    const float a = 6378137.0;
    const float e2 = 0.00669437999014;
    float lng = lngLat.x;
    float lat = lngLat.y;
    float sinLat = sin(lat);
    float cosLat = cos(lat);
    float N = a / sqrt(1.0 - e2 * sinLat * sinLat);
    return vec3(N * cosLat * cos(lng),
                N * cosLat * sin(lng),
                N * (1.0 - e2) * sinLat);
}

void main() {
    float lng = mix(u_tileBounds.x, u_tileBounds.z, a_texcoord.x);
    float lat = mix(u_tileBounds.w, u_tileBounds.y, a_texcoord.y);
    vec2 geo = vec2(lng, lat);
    vec3 ecef = geoToECEF(geo);
    v_texcoord = a_texcoord;
    gl_Position = u_modelViewProjection * vec4(ecef, 1.0);
}
)glsl";

static const char* kDebugLineFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

uniform vec4 u_color;
out vec4 fragColor;

void main() {
    fragColor = u_color;
}
)glsl";

// ============================================================
// Debug Line Shader — MSL
// ============================================================

static const char* kDebugLineVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 texcoord [[attribute(0)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 texcoord;
};

constant float a = 6378137.0;
constant float e2 = 0.00669437999014;

float3 geoToECEF(float2 lngLat) {
    float lng = lngLat.x;
    float lat = lngLat.y;
    float sinLat = sin(lat);
    float cosLat = cos(lat);
    float N = a / sqrt(1.0 - e2 * sinLat * sinLat);
    return float3(N * cosLat * cos(lng),
                  N * cosLat * sin(lng),
                  N * (1.0 - e2) * sinLat);
}

vertex VertexOut debugLineVertex(VertexIn in [[stage_in]],
                                  constant float4x4& u_modelViewProjection [[buffer(1)]],
                                  constant float4& u_tileBounds [[buffer(2)]]) {
    VertexOut out;
    float lng = mix(u_tileBounds.x, u_tileBounds.z, in.texcoord.x);
    float lat = mix(u_tileBounds.w, u_tileBounds.y, in.texcoord.y);
    float2 geo = float2(lng, lat);
    out.position = u_modelViewProjection * float4(geoToECEF(geo), 1.0);
    out.texcoord = in.texcoord;
    return out;
}
)msl";

static const char* kDebugLineFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

fragment float4 debugLineFragment(constant float4& u_color [[buffer(0)]]) {
    return u_color;
}
)msl";

// ============================================================
// Border geometry: unit square perimeter (LINE_STRIP)
// ============================================================

struct BorderVertex {
    float texcoord[2];
};

static const BorderVertex kBorderVerts[] = {
    {{0.0f, 0.0f}},
    {{1.0f, 0.0f}},
    {{1.0f, 1.0f}},
    {{0.0f, 1.0f}},
    {{0.0f, 0.0f}},  // 闭合回环
};

// ============================================================
// DebugOverlay
// ============================================================

DebugOverlay::DebugOverlay() = default;

DebugOverlay::~DebugOverlay() {
    dispose();
}

bool DebugOverlay::initialize(RenderDevice* device) {
    device_ = device;
    if (!device) return false;

    // 编译 line shader
    ShaderDesc sd;
    bool isMetal = (device->backendType() == RenderDevice::Backend::Metal);
    sd.vertexSource = isMetal ? kDebugLineVertexMSL : kDebugLineVertexGLSL;
    sd.fragmentSource = isMetal ? kDebugLineFragmentMSL : kDebugLineFragmentGLSL;
    lineShader_ = device->createShader(sd);
    if (!lineShader_) return false;

    // 边框几何
    BufferDesc bd;
    bd.size = sizeof(kBorderVerts);
    bd.data = kBorderVerts;
    bd.usage = BufferDesc::Usage::Static;
    bd.type = BufferDesc::Type::Vertex;
    borderVertexBuffer_ = device->createBuffer(bd);
    if (!borderVertexBuffer_) return false;

    return true;
}

void DebugOverlay::buildCommands(const std::vector<TileKey>& tileKeys,
                                  const TileScheme& tileScheme,
                                  RenderCommandList& commands) {
    if (!enabled_ || !lineShader_ || !borderVertexBuffer_) return;

    for (const auto& key : tileKeys) {
        Rectangle bounds = tileScheme.tileToRectangle(key);

        // 基于 z/x/y 的确定性颜色
        uint32_t h = static_cast<uint32_t>(key.z * 2654435761u) ^
                     static_cast<uint32_t>(key.x * 0x9e3779b9u) ^
                     static_cast<uint32_t>(key.y * 0x517cc1b7u);
        float r = ((h >> 16) & 0xFF) / 255.0f;
        float g = ((h >> 8) & 0xFF) / 255.0f;
        float b = (h & 0xFF) / 255.0f;

        RenderCommand cmd;
        cmd.owner = "debug-overlay";
        cmd.pass = "color";
        cmd.shader = lineShader_.get();
        cmd.vertexBuffer = borderVertexBuffer_.get();
        cmd.indexBuffer = nullptr;    // 无索引缓冲：使用 glDrawArrays
        cmd.vertexCount = 5;          // 5 个顶点 = LINE_STRIP 矩形
        cmd.primitive = RenderCommand::PrimitiveType::LineStrip;
        cmd.indexType = RenderCommand::IndexType::UInt16;
        cmd.depthTest = true;
        cmd.depthWrite = false;
        cmd.blend = true;             // 半透明边框

        cmd.uniforms["u_tileBounds"] = {
            static_cast<float>(bounds.west()),
            static_cast<float>(bounds.south()),
            static_cast<float>(bounds.east()),
            static_cast<float>(bounds.north())
        };
        cmd.uniforms["u_color"] = {r, g, b, 0.6f};

        commands.push_back(std::move(cmd));
    }
}

void DebugOverlay::dispose() {
    lineShader_.reset();
    borderVertexBuffer_.reset();
    device_ = nullptr;
}

} // namespace earth_engine
