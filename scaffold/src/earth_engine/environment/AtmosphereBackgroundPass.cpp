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

// Aligned with openglobus SimpleSkyBackground fragment shader

#define MAX_DIST 1e10
#define PI 3.14159265359
#define ZERO vec3(0.0)

uniform vec3 u_camPos;
uniform vec2 u_resolution;
uniform float u_fov;
uniform float u_isOrthographic;
uniform vec4 u_frustumParams;
uniform mat4 u_viewMatrix;
uniform float u_earthRadius;
uniform mat3 u_normalMatrix;
uniform vec3 u_colorZenith;   // SkyGradient zenith color (top)
uniform vec3 u_colorHorizon;  // SkyGradient horizon color (bottom)

out vec4 fragColor;

vec2 sphIntersect(vec3 ro, vec3 rd, vec3 ce, float ra) {
    vec3 oc = ro - ce;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - ra * ra;
    float h = b * b - c;
    if (h < 0.0) return vec2(MAX_DIST);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

void main() {
    vec3 cameraPosition = u_camPos;

    // Compute view ray from screen coordinates
    vec2 uv = (2.0 * gl_FragCoord.xy - u_resolution.xy) / u_resolution.y;
    vec3 rayDirection;

    if (u_isOrthographic > 0.5) {
        float px = uv.x * u_resolution.y / u_resolution.x;
        float py = uv.y;
        float dx = 0.5 * u_frustumParams.x * px;
        float dy = 0.5 * u_frustumParams.y * py;

        mat4 viewT = transpose(u_viewMatrix);
        vec3 right = normalize(viewT[0].xyz);
        vec3 up = normalize(viewT[1].xyz);
        vec3 backward = normalize(viewT[2].xyz);
        vec3 forward = -backward;

        rayDirection = forward;
        cameraPosition = cameraPosition + right * dx + up * dy;
    } else {
        float z = 1.0 / tan(u_fov * 0.5);
        rayDirection = normalize(vec3(uv, -z));
        vec4 rd = transpose(u_viewMatrix) * vec4(rayDirection, 1.0);
        rayDirection = rd.xyz;
    }

    // Compute closest approach of ray to Earth center
    vec3 oc = -cameraPosition;
    float b = dot(oc, rayDirection);
    float c = dot(oc, oc);
    float closestDist2 = c - b * b;
    float closestDist = sqrt(max(0.0, closestDist2));
    float tangentHeight = closestDist - u_earthRadius;

    // Discard pixels deep inside Earth (covered by globe/tiles)
    // Keep a thin ring (~200km) at the limb for atmosphere glow
    if (tangentHeight < -200000.0) {
        discard;
    }

    vec3 color;

    if (tangentHeight < 0.0) {
        // Ray grazes the atmosphere limb — bright blue glow
        float glow = 1.0 + tangentHeight / 200000.0; // 0..1 from -200km to 0
        color = u_colorZenith * glow;
    } else {
        // Above atmosphere — use big sphere blend for sky gradient
        float bigRadius = u_earthRadius * 2.5;
        vec3 bigCenter = normalize(cameraPosition) * bigRadius * 1.3;

        vec2 bigHit = sphIntersect(cameraPosition, rayDirection, bigCenter, bigRadius);

        vec3 exitPoint = cameraPosition + rayDirection * bigHit.y;
        float distToEarthCenter = length(exitPoint);

        float maxDist = sqrt(bigRadius * bigRadius + bigRadius * bigRadius);
        float t = distToEarthCenter / maxDist;

        color = mix(u_colorZenith, u_colorHorizon, t);
    }

    fragColor = vec4(color, 1.0);
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
    const Vec3& cameraPos,
    const float* viewMatrix,
    float fovRadians,
    int viewportWidth,
    int viewportHeight,
    bool isOrthographic,
    const std::array<float, 3>& zenithColor,
    const std::array<float, 3>& horizonColor,
    float earthRadius,
    const float* normalMatrix) const {

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

    // Camera position
    cmd.uniforms["u_camPos"] = {
        static_cast<float>(cameraPos.x()),
        static_cast<float>(cameraPos.y()),
        static_cast<float>(cameraPos.z())
    };

    // Resolution
    cmd.uniforms["u_resolution"] = {
        static_cast<float>(viewportWidth),
        static_cast<float>(viewportHeight)
    };

    // FOV
    cmd.uniforms["u_fov"] = {fovRadians};
    cmd.uniforms["u_isOrthographic"] = {isOrthographic ? 1.0f : 0.0f};

    // Frustum (placeholder, not used in perspective)
    cmd.uniforms["u_frustumParams"] = {0.0f, 0.0f, 0.0f, 0.0f};

    // View matrix
    cmd.uniforms["u_viewMatrix"] = std::vector<float>(viewMatrix, viewMatrix + 16);

    // Earth radius (meters)
    cmd.uniforms["u_earthRadius"] = {earthRadius};

    // Normal matrix (mat3, 9 floats)
    cmd.uniforms["u_normalMatrix"] = std::vector<float>(normalMatrix, normalMatrix + 9);

    // Sky colors from SkyGradient
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
