#include "AtmosphereBackgroundPass.h"
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <cstring>

#ifdef ANDROID
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AtmosPass", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AtmosPass", __VA_ARGS__)
#else
#define LOGI(...)
#define LOGE(...)
#endif

namespace earth_engine {

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

uniform vec3 u_camPos;
uniform vec3 u_camRight;
uniform vec3 u_camUp;
uniform vec3 u_camForward;
uniform vec3 u_sunDir;
uniform float u_bottomRadius;
uniform float u_topRadius;
uniform float u_fov;
uniform float u_aspect;
uniform float u_sunIntensity;
uniform float u_sunAngularRadius;
uniform float u_opacity;
uniform vec2 u_resolution;

out vec4 fragColor;

const float PI = 3.1415926538;

void main() {
    // NDC from gl_FragCoord
    float px = gl_FragCoord.x;
    float py = gl_FragCoord.y;
    float w = u_resolution.x;
    float h = u_resolution.y;
    float ndcX = (2.0 * px / w - 1.0) * u_aspect;
    float ndcY = 2.0 * py / h - 1.0;

    float tanFovHalf = tan(u_fov * 0.5);

    // View ray in world (ECEF) space from camera basis
    vec3 rayDir = normalize(u_camForward + u_camRight * ndcX * tanFovHalf + u_camUp * ndcY * tanFovHalf);

    vec3 sun = normalize(u_sunDir);
    vec3 cam = u_camPos;
    float sunAngle = dot(rayDir, sun);

    // Tangent height: distance from ray's closest approach to ellipse center (origin), minus radius
    float ct = -dot(cam, rayDir);
    float dist = length(cam + ct * rayDir);
    float tanH = dist - u_bottomRadius;

    // Discard pixels clearly inside Earth
    if (tanH < -100.0) {
        discard;
    }

    float atmosH = u_topRadius - u_bottomRadius;

    // Rayleigh phase function
    float rayleighPhaseFunc = 3.0 / (16.0 * PI) * (1.0 + sunAngle * sunAngle);

    // Horizon glow: strongest at tanH=0, fades over scale height
    float scaleH = atmosH * 0.06;
    float limb = exp(-max(tanH, 0.0) / scaleH);

    // Sky color: brighter zenith, warm near sun
    vec3 zenith = vec3(0.25, 0.45, 0.9);
    vec3 sunCol = vec3(1.0, 0.95, 0.7);
    vec3 horizon = vec3(0.85, 0.88, 0.95);
    float sunWeight = max(0.0, sunAngle) * rayleighPhaseFunc * 3.0;
    vec3 color = mix(zenith, sunCol, clamp(sunWeight, 0.0, 1.0));

    // Horizon fade: near horizon brighter, far from horizon darker (but not black)
    float horizonFactor = 1.0 - clamp(tanH / (atmosH * 0.4), 0.0, 1.0);
    color = mix(color, horizon, horizonFactor * 0.5);

    // Limb brightening at the very edge
    color = mix(color, vec3(1.0, 0.95, 0.85), limb * 0.3);

    // Space glow: very faint blue even at high altitude (prevents black space)
    float spaceFade = exp(-max(tanH, 0.0) / (atmosH * 2.0));
    color = mix(vec3(0.02, 0.02, 0.08), color, clamp(spaceFade * 3.0, 0.0, 1.0));

    // Sun disk with Gaussian bloom (wider search)
    float minSunCos = cos(u_sunAngularRadius);
    float cosTheta = dot(rayDir, sun);
    float sunDisk = 0.0;
    if (cosTheta >= minSunCos) {
        sunDisk = 1.0;
    } else {
        float offset = minSunCos - cosTheta;
        sunDisk = exp(-offset * 5000.0) * 0.5 + 1.0 / (0.05 + offset * 100.0) * 0.02;
    }
    // Wider bloom tolerance
    sunDisk = smoothstep(0.001, 1.0, sunDisk);

    vec3 outColor = color + sunDisk * vec3(1.0, 0.95, 0.7) * u_sunIntensity * 0.5;
    float alpha = clamp(0.35 + horizonFactor * 0.3 + limb * 0.2 + spaceFade * 0.2, 0.0, 1.0);

    fragColor = vec4(outColor * u_opacity, alpha);
}
)";

} // anonymous namespace

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
    float fovRadians,
    int viewportWidth,
    int viewportHeight,
    const Vec3& camRight,
    const Vec3& camUp,
    const Vec3& camForward,
    const Vec3& sunDir,
    const AtmosphereParameters& params,
    float opacity) const {

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

    auto set3 = [&](const char* name, const Vec3& v) {
        cmd.uniforms[name] = {static_cast<float>(v.x()), static_cast<float>(v.y()), static_cast<float>(v.z())};
    };

    set3("u_camPos", cameraPos);
    set3("u_camRight", camRight);
    set3("u_camUp", camUp);
    set3("u_camForward", camForward);
    set3("u_sunDir", sunDir);
    cmd.uniforms["u_bottomRadius"] = {static_cast<float>(params.bottomRadius)};
    cmd.uniforms["u_topRadius"] = {static_cast<float>(params.topRadius())};
    cmd.uniforms["u_fov"] = {fovRadians};
    cmd.uniforms["u_aspect"] = {static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight)};
    cmd.uniforms["u_sunIntensity"] = {static_cast<float>(params.sunIntensity)};
    cmd.uniforms["u_sunAngularRadius"] = {static_cast<float>(params.sunAngularRadius)};
    cmd.uniforms["u_opacity"] = {opacity};
    cmd.uniforms["u_resolution"] = {static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)};

    return cmd;
}

void AtmosphereBackgroundPass::dispose() {
    shader_ = nullptr;
    quadBuffer_ = nullptr;
}

} // namespace earth_engine
