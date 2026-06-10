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

float rayleighPhase(float mu) {
    return 0.75 * (1.0 + mu * mu);
}

float miePhase(float mu) {
    // Henyey-Greenstein, softened for a background pass so the horizon
    // does not collapse into a white outline on mobile displays.
    const float g = 0.76;
    float g2 = g * g;
    return 0.18 * (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * mu, 0.05), 1.5);
}

void main() {
    // ---- View ray in world (ECEF) space from camera basis ----
    float px = gl_FragCoord.x;
    float py = gl_FragCoord.y;
    float w = u_resolution.x;
    float h = u_resolution.y;
    float ndcX = (2.0 * px / w - 1.0) * u_aspect;
    float ndcY = 2.0 * py / h - 1.0;
    float tanFovHalf = tan(u_fov * 0.5);
    vec3 rayDir = normalize(u_camForward + u_camRight * ndcX * tanFovHalf + u_camUp * ndcY * tanFovHalf);

    vec3 sun = normalize(u_sunDir);
    vec3 cam = u_camPos;

    // ---- Ray-sphere intersection with atmosphere shell ----
    float R = u_bottomRadius;
    float Rtop = u_topRadius;
    float b = -dot(cam, rayDir);
    float c = dot(cam, cam);
    float dInner = b * b - (c - R * R);
    float dOuter = b * b - (c - Rtop * Rtop);

    float pathLen = 0.0;   // distance through atmosphere (m)
    float pathStart = 0.0;
    float pathEnd = 0.0;
    bool hitPlanet = false;

    if (dInner > 0.0) {
        float tHit = b - sqrt(dInner);
        if (tHit > 0.0) {
            // Ray from camera to planet surface — atmosphere path in between
            float tEnter = 0.0;
            if (dOuter > 0.0) {
                float tOutEntry = b - sqrt(dOuter);
                tEnter = max(tOutEntry, 0.0);
            }
            if (tEnter < tHit) {
                pathStart = tEnter;
                pathEnd = tHit;
                pathLen = tHit - tEnter;
            }
            hitPlanet = true;
        }
    } else if (dOuter > 0.0) {
        // Ray passes through atmosphere without hitting planet
        float tOutEntry = b - sqrt(dOuter);
        float tExit = b + sqrt(dOuter);
        float tEnter = max(tOutEntry, 0.0);
        if (tExit > tEnter) {
            pathStart = tEnter;
            pathEnd = tExit;
            pathLen = tExit - tEnter;
        }
    }

    // Ground-facing rays are left for the globe/terrain pass. For every
    // open-sky ray, this pass still emits a base sky color; otherwise the
    // top-atmosphere tangent shows up as a hard line against glClearColor.
    if (hitPlanet) {
        discard;
    }

    // ---- Scattering approximation ----
    // OpenGlobus integrates optical density along the view ray. Keep this
    // pass lightweight, but use the same idea instead of mapping raw path
    // length linearly to white alpha.
    const int SAMPLE_COUNT = 8;
    const float rayleighScaleHeight = 8000.0;
    const float mieScaleHeight = 1200.0;
    float segmentLen = max(pathEnd - pathStart, 0.0) / float(SAMPLE_COUNT);
    float t = pathStart + segmentLen * 0.5;
    float rayleighDepth = 0.0;
    float mieDepth = 0.0;

    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        vec3 p = cam + rayDir * t;
        float height = max(length(p) - R, 0.0);
        rayleighDepth += exp(-height / rayleighScaleHeight) * segmentLen;
        mieDepth += exp(-height / mieScaleHeight) * segmentLen;
        t += segmentLen;
    }

    rayleighDepth *= 0.001; // km-equivalent optical depth
    mieDepth *= 0.001;

    float mu = dot(rayDir, sun);
    float rPhase = rayleighPhase(mu);
    float mPhase = miePhase(mu);

    vec3 rayleighColor = vec3(0.20, 0.42, 1.0);
    vec3 mieColor = vec3(0.72, 0.80, 0.92);
    float r = rayleighDepth * 0.055 * rPhase;
    float m = mieDepth * 0.020 * mPhase;

    vec3 localUp = normalize(cam);
    float viewUp = clamp(dot(rayDir, localUp), 0.0, 1.0);
    vec3 zenithColor = vec3(0.08, 0.28, 0.58);
    vec3 lowSkyColor = vec3(0.18, 0.42, 0.82);
    vec3 baseSky = mix(lowSkyColor, zenithColor, pow(viewUp, 0.65));

    vec3 scatterColor = rayleighColor * r + mieColor * m;
    scatterColor *= u_sunIntensity * 0.85;

    float opticalThickness = rayleighDepth * 0.018 + mieDepth * 0.010;
    float scatterAmount = clamp(1.0 - exp(-opticalThickness), 0.0, 0.35) * u_opacity;
    vec3 color = mix(baseSky, baseSky + scatterColor, scatterAmount);

    // ---- Sun disk ----
    float minSunCos = cos(u_sunAngularRadius);
    float cosTheta = dot(rayDir, sun);
    float sunDisk = 0.0;
    if (cosTheta >= minSunCos) {
        sunDisk = 1.0;
    } else {
        float off = minSunCos - cosTheta;
        sunDisk = exp(-off * 5000.0) * 0.5 + 1.0 / (0.05 + off * 120.0) * 0.015;
    }
    sunDisk = smoothstep(0.002, 1.0, sunDisk);
    color += sunDisk * vec3(1.0, 0.95, 0.7) * u_sunIntensity * 0.4;

    fragColor = vec4(color, 1.0);
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
        cmd.uniforms[name] = {
            static_cast<float>(v.x()),
            static_cast<float>(v.y()),
            static_cast<float>(v.z())
        };
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
