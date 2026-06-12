#include "AtmosphereBackgroundPass.h"
#include "../core/geodesy/Ellipsoid.h"
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
layout(location = 0) in vec2 a_position;
void main() {
    // Reverse-Z depth is cleared to 0. Draw at the far plane after terrain so
    // this pass only composites through pixels that did not receive ground.
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
    float R = u_bottomRadius;
    float Rtop = u_topRadius;
    float camRadius = length(cam);
    if (camRadius < R + 100.0) {
        cam = normalize(cam) * (R + 100.0);
        camRadius = R + 100.0;
    }

    // ---- Ray-sphere intersection with atmosphere shell ----
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

    // Match OpenGlobus' background atmosphere contract: the independent sky
    // pass never colors ground-facing rays. Ground haze belongs to the surface
    // shader, while depth testing keeps this fullscreen pass behind terrain.
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
    float camHeight = max(length(cam) - R, 0.0);
    float spaceFactor = smoothstep(120000.0, 900000.0, camHeight);
    float viewUp = dot(rayDir, localUp);
    vec3 zenithColor = vec3(0.06, 0.24, 0.55);
    vec3 lowSkyColor = vec3(0.50, 0.74, 0.92);
    float raySkyT = smoothstep(-0.08, 0.85, viewUp);
    float screenSkyT = smoothstep(0.0, 1.0, py / h);
    float skyT = mix(screenSkyT, raySkyT, spaceFactor);
    vec3 baseSky = mix(lowSkyColor, zenithColor, pow(skyT, 0.85));
    baseSky = mix(baseSky, vec3(0.0, 0.005, 0.025), spaceFactor);

    vec3 scatterColor = rayleighColor * r + mieColor * m;
    scatterColor *= u_sunIntensity * 0.85;

    float opticalThickness = rayleighDepth * 0.018 + mieDepth * 0.010;
    float pathScatterAmount =
        clamp(1.0 - exp(-opticalThickness), 0.0, 0.35) * u_opacity;
    vec3 color = mix(baseSky, baseSky + scatterColor, pathScatterAmount * spaceFactor);

    float horizonGlow = pow(1.0 - screenSkyT, 2.4) * (1.0 - spaceFactor);
    vec3 horizonColor = vec3(0.50, 0.74, 0.92);
    color = mix(color, horizonColor, clamp(horizonGlow * 0.36, 0.0, 0.36));

    float horizonAir = (1.0 - smoothstep(0.0, 0.18, abs(viewUp))) * (1.0 - spaceFactor);
    color = mix(color, horizonColor, clamp(horizonAir * 0.22, 0.0, 0.22));

    // ---- Sun disk ----
    float cosTheta = dot(rayDir, sun);
    float sunRadius = max(u_sunAngularRadius, 0.0001);
    float sunAngle = sqrt(max(2.0 * (1.0 - cosTheta), 0.0));
    float sunR = sunAngle / sunRadius;

    float diskMask = 1.0 - smoothstep(0.97, 1.03, sunR);
    float limb = sqrt(max(1.0 - sunR * sunR, 0.0));
    float limbDarkening = mix(0.68, 1.0, pow(limb, 0.42));
    vec3 solarCore = vec3(1.0, 0.985, 0.90);
    vec3 solarEdge = vec3(1.0, 0.72, 0.36);
    vec3 diskColor = mix(solarEdge, solarCore, pow(limb, 0.28)) * limbDarkening;

    float coronaR = max(sunR - 1.0, 0.0);
    float innerCorona = exp(-coronaR * coronaR * 2.6);
    float outerCorona = 1.0 / (1.0 + coronaR * coronaR * 18.0);
    float forwardScatter = smoothstep(0.72, 1.0, cosTheta);
    vec3 coronaColor = vec3(1.0, 0.80, 0.46) * innerCorona * 0.22 +
                       vec3(0.72, 0.84, 1.0) * outerCorona * 0.045;
    coronaColor *= forwardScatter * (0.45 + 0.55 * spaceFactor);

    color += diskMask * diskColor * u_sunIntensity * 1.15;
    color += coronaColor * u_sunIntensity;

    float skyAlpha = mix(1.0, 0.18, spaceFactor);
    float limbAlpha = pathScatterAmount * spaceFactor;
    fragColor = vec4(color, clamp(max(skyAlpha, limbAlpha), 0.0, 1.0));
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
    cmd.depthTest = true;
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
    const Ellipsoid atmosphereEllipsoid(params.equatorialRadius, params.bottomRadius);
    const double localBottomRadius =
        atmosphereEllipsoid.projectToSurface(cameraPos).length();
    cmd.uniforms["u_bottomRadius"] = {static_cast<float>(localBottomRadius)};
    cmd.uniforms["u_topRadius"] = {
        static_cast<float>(localBottomRadius + params.atmosHeight)};
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
