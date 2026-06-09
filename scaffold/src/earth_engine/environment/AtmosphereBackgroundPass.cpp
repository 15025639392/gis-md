#include "AtmosphereBackgroundPass.h"
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>

#ifdef ANDROID
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AtmosPass", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AtmosPass", __VA_ARGS__)
#else
#define LOGI(...)
#define LOGE(...)
#endif

namespace earth_engine {

// ============================================================
// GLSL ES 3.0 Atmosphere Background Shader
// 对标 openglobus/src/shaders/atmos/atmosphere.frag.glsl
// ============================================================

namespace {

const char* kAtmosphereBackgroundVert = R"(#version 300 es
precision highp float;

in vec2 a_position;

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

const char* kAtmosphereBackgroundFrag = R"(#version 300 es
precision highp float;

// Thin-shell atmosphere limb glow — computed in CAMERA SPACE.
// Earth center derived from view matrix translation (no normalMatrix needed).

#define MAX_DIST 1e10

uniform vec2 u_resolution;
uniform float u_fov;
uniform float u_isOrthographic;
uniform vec4 u_frustumParams;
uniform mat4 u_viewMatrix;       // world→camera
uniform float u_earthRadius;
uniform float u_atmosHeight;
uniform float u_scaleHeight;
uniform vec3 u_colorZenith;
uniform vec3 u_colorHorizon;

out vec4 fragColor;

void main() {
    // View ray in camera space: camera at (0,0,0), looking along -Z
    vec2 uv = (2.0 * gl_FragCoord.xy - u_resolution.xy) / u_resolution.y;
    vec3 rayDir;

    if (u_isOrthographic > 0.5) {
        // Ortho: ray is forward, origin offset by screen pos
        float px = uv.x * u_resolution.y / u_resolution.x;
        float py = uv.y;
        float dx = 0.5 * u_frustumParams.x * px;
        float dy = 0.5 * u_frustumParams.y * py;
        mat4 viewT = transpose(u_viewMatrix);
        vec3 right = normalize(viewT[0].xyz);
        vec3 up = normalize(viewT[1].xyz);
        vec3 backward = normalize(viewT[2].xyz);
        rayDir = -backward;
        // Ortho branch not fully implemented for limb glow
        fragColor = vec4(0.0);
        return;
    } else {
        float z = 1.0 / tan(u_fov * 0.5);
        rayDir = normalize(vec3(uv, -z));
    }

    // Earth center in CAMERA SPACE = viewMatrix * (0,0,0,1)
    vec3 earthCenter = (u_viewMatrix * vec4(0.0, 0.0, 0.0, 1.0)).xyz;

    // Camera at origin in camera space
    vec3 camPos = vec3(0.0);

    // Closest approach of ray to Earth center
    // Use the vector projection to avoid catastrophic cancellation
    // (c - b*b loses all precision when ray nearly hits center)
    vec3 oc = earthCenter - camPos;
    float b = dot(oc, rayDir);
    vec3 closestPoint = oc - b * rayDir;
    float closestDist = length(closestPoint);
    float tangentHeight = closestDist - u_earthRadius;

    float TOP_RADIUS = u_earthRadius + u_atmosHeight;
    vec3 color = vec3(0.0);

    if (closestDist < u_earthRadius) {
        if (tangentHeight > -u_atmosHeight) {
            float pathLen = 2.0 * sqrt(max(0.0, TOP_RADIUS * TOP_RADIUS - closestDist * closestDist));
            float density = exp(-max(tangentHeight, 0.0) / u_scaleHeight);
            float glow = density * pathLen * 0.0000008;
            color = mix(u_colorZenith, vec3(0.6, 0.85, 1.0), clamp(tangentHeight / 50000.0 + 0.5, 0.0, 1.0)) * glow;
        } else {
            discard;
        }
    } else if (closestDist < TOP_RADIUS) {
        float pathLen = 2.0 * sqrt(max(0.0, TOP_RADIUS * TOP_RADIUS - closestDist * closestDist));
        float density = exp(-tangentHeight / u_scaleHeight);
        float glow = density * pathLen * 0.0000008;
        float blueShift = clamp(tangentHeight / u_scaleHeight, 0.0, 1.0);
        vec3 atmosphereColor = mix(vec3(0.8, 0.9, 1.0), vec3(0.2, 0.4, 1.0), blueShift);
        color = atmosphereColor * glow;
    } else {
        color = u_colorHorizon * 0.01;
    }

    color = color / (color + vec3(1.0));
    float alpha = dot(color, vec3(0.299, 0.587, 0.114));
    if (alpha < 0.0005) discard;
    fragColor = vec4(color, alpha);
}
)";

} // anonymous namespace

// ============================================================
// AtmosphereBackgroundPass
// ============================================================

AtmosphereBackgroundPass::AtmosphereBackgroundPass() = default;

AtmosphereBackgroundPass::~AtmosphereBackgroundPass() {
    dispose();
}

bool AtmosphereBackgroundPass::initialize(RenderDevice* device) {
    if (!device) {
        LOGE("initialize: null device");
        return false;
    }
    device_ = device;
    LOGI("initialize: creating shader...");

    ShaderDesc shaderDesc;
    shaderDesc.vertexSource = kAtmosphereBackgroundVert;
    shaderDesc.fragmentSource = kAtmosphereBackgroundFrag;
    auto shaderPtr = device->createShader(shaderDesc);
    if (!shaderPtr) {
        LOGE("initialize: createShader returned null");
        return false;
    }
    shader_ = shaderPtr.release();
    LOGI("initialize: shader created ok");

    // Full-screen quad: two triangles covering NDC [-1,1]²
    // Positioned as triangle strip: [-1,-1], [1,-1], [-1,1], [1,1]
    float quadVertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };

    BufferDesc bufferDesc;
    bufferDesc.size = sizeof(quadVertices);
    bufferDesc.data = quadVertices;
    bufferDesc.usage = BufferDesc::Usage::Static;
    bufferDesc.type = BufferDesc::Type::Vertex;
    auto bufPtr = device->createBuffer(bufferDesc);
    if (!bufPtr) {
        LOGE("initialize: createBuffer returned null");
        return false;
    }
    quadBuffer_ = bufPtr.release();
    LOGI("initialize: buffer created ok, ready");

    return true;
}

RenderCommand AtmosphereBackgroundPass::buildCommand(
    const float* viewMatrix,
    float fovRadians,
    int viewportWidth,
    int viewportHeight,
    bool isOrthographic,
    const std::array<float, 3>& zenithColor,
    const std::array<float, 3>& horizonColor,
    float earthRadius) const {

    RenderCommand cmd;
    cmd.kind = RenderCommandKind::AtmosphereBackground;
    cmd.owner = "atmosphere_background";
    cmd.pass = "color";
    cmd.shader = shader_;
    cmd.vertexBuffer = quadBuffer_;
    cmd.indexBuffer = nullptr;
    cmd.vertexCount = 4;
    cmd.vertexStride = 2 * sizeof(float);
    cmd.primitive = RenderCommand::PrimitiveType::TriangleStrip;
    cmd.depthTest = false;
    cmd.depthWrite = false;
    cmd.blend = true;
    cmd.blendSrc = RenderCommand::BlendFactor::SrcAlpha;
    cmd.blendDst = RenderCommand::BlendFactorDst::OneMinusSrcAlpha;
    cmd.cullFace = false;

    // Resolution
    cmd.uniforms["u_resolution"] = {
        static_cast<float>(viewportWidth),
        static_cast<float>(viewportHeight)
    };

    // FOV
    cmd.uniforms["u_fov"] = {fovRadians};
    cmd.uniforms["u_isOrthographic"] = {isOrthographic ? 1.0f : 0.0f};

    // Frustum (placeholder)
    cmd.uniforms["u_frustumParams"] = {0.0f, 0.0f, 0.0f, 0.0f};

    // View matrix (world→camera, 16 floats)
    cmd.uniforms["u_viewMatrix"] = std::vector<float>(viewMatrix, viewMatrix + 16);

    // Earth radius (meters)
    cmd.uniforms["u_earthRadius"] = {earthRadius};

    // Atmosphere geometry (meters)
    cmd.uniforms["u_atmosHeight"] = {100000.0f};
    cmd.uniforms["u_scaleHeight"] = {7994.0f};

    // Sky colors
    cmd.uniforms["u_colorZenith"] = {zenithColor[0], zenithColor[1], zenithColor[2]};
    cmd.uniforms["u_colorHorizon"] = {horizonColor[0], horizonColor[1], horizonColor[2]};

    return cmd;
}

void AtmosphereBackgroundPass::dispose() {
    // Resources owned by RenderDevice; just clear pointers
    shader_ = nullptr;
    quadBuffer_ = nullptr;
    // Note: actual GPU resource deletion is handled by RenderDevice::onSurfaceDestroyed()
}

} // namespace earth_engine
