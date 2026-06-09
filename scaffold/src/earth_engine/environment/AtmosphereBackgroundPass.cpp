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

#define PI 3.14159265359
#define SQRT_SAMPLE_COUNT 100.0

uniform vec3 u_sunDir;
uniform vec3 u_camPos;
uniform vec2 u_resolution;
uniform float u_fov;
uniform float u_isOrthographic;
uniform vec4 u_frustumParams; // x=left-right, y=top-bottom, z=right, w=top
uniform mat4 u_viewMatrix;

// Atmosphere parameters
uniform float u_atmosHeight;
uniform float u_bottomRadius;
uniform float u_topRadius;
uniform vec3 u_rayleighScattering;   // RGB at sea level
uniform vec3 u_mieScattering;        // RGB at sea level  
uniform vec3 u_ozoneAbsorption;      // RGB at sea level
uniform float u_rayleighScaleHeight;
uniform float u_mieScaleHeight;
uniform float u_sunIntensity;
uniform float u_groundAlbedo;
uniform float u_ozoneDensityHeight;
uniform float u_ozoneDensityWidth;

out vec4 fragColor;

// Rayleigh phase function
float rayleighPhase(float cosTheta) {
    return 3.0 / (16.0 * PI) * (1.0 + cosTheta * cosTheta);
}

// Mie phase function (Henyey-Greenstein, g=0.76)
float miePhase(float cosTheta) {
    float g = 0.76;
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    denom = max(denom, 1e-6);
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

// intersect sphere: return vec2(t1, t2) or vec2(1e10) if no hit
vec2 intersectSphere(vec3 ro, vec3 rd, vec3 center, float radius) {
    vec3 oc = ro - center;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - radius * radius;
    float h = b * b - c;
    if (h < 0.0) return vec2(1e10);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

void main() {
    vec3 lightDirection = normalize(u_sunDir);
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

    float BOTTOM_RADIUS = u_bottomRadius;
    float TOP_RADIUS = u_topRadius;

    vec3 light = vec3(0.0);

    // Check ray intersection with atmosphere and Earth
    vec2 atmHit = intersectSphere(cameraPosition, rayDirection, vec3(0.0), TOP_RADIUS);
    vec2 groundHit = intersectSphere(cameraPosition, rayDirection, vec3(0.0), BOTTOM_RADIUS);

    bool hitsAtmosphere = (atmHit.y > 0.0 && atmHit.x < atmHit.y);
    bool hitsGround = (groundHit.x > 0.0 && groundHit.x < atmHit.y);

    // Discard pixels that hit the Earth (covered by globe/tiles)
    if (hitsGround) {
        discard;
    }

    if (hitsAtmosphere) {
        // Compute the closest approach of the ray to Earth's center
        vec3 oc = -cameraPosition;
        float b = dot(oc, rayDirection);
        float c = dot(oc, oc);
        float closestDist2 = max(0.0, c - b * b);
        float closestDist = sqrt(closestDist2);
        float tangentHeight = closestDist - BOTTOM_RADIUS;

        // Only glow if ray passes through atmosphere
        if (tangentHeight > 0.0 && tangentHeight < u_atmosHeight) {
            // Exponential falloff based on tangent height
            float density = exp(-tangentHeight / u_rayleighScaleHeight);

            // Compute scattering angle between view and sun
            float cosTheta = dot(rayDirection, lightDirection);

            // Rayleigh: strong forward+backward, blue-rich
            float rayleigh = 1.0 + cosTheta * cosTheta;
            vec3 rayleighColor = vec3(0.3, 0.6, 1.0);

            // Mie: strong forward (sun direction)
            float mie = 1.0 / max(1.0 - 0.9 * cosTheta, 0.01);
            vec3 mieColor = vec3(1.0, 0.9, 0.7);

            // Path length through atmosphere (longer at tangent, shorter at limb)
            float pathLength = sqrt(max(0.0, TOP_RADIUS * TOP_RADIUS - closestDist * closestDist));

            // Glow intensity
            float intensity = density * pathLength * 0.00001 * u_sunIntensity;

            light = (rayleighColor * rayleigh + mieColor * mie * 0.3) * intensity;

            // Sun disk in atmosphere
            if (cosTheta > 0.9998) {
                light += vec3(1.0, 0.95, 0.8) * 0.5 * u_sunIntensity;
            }
        }
    }

    // Tone mapping
    light = light / (light + vec3(1.0));

    // Discard pixels with negligible atmosphere glow
    if (dot(light, light) < 0.0001) {
        discard;
    }

    fragColor = vec4(light, 1.0);
}
)";

} // anonymous namespace

// ============================================================
// AtmosphereBackgroundPass
// ============================================================

AtmosphereBackgroundPass::AtmosphereBackgroundPass()
    : params_(earthAtmosphereDefaults()) {}

AtmosphereBackgroundPass::AtmosphereBackgroundPass(const AtmosphereParameters& params)
    : params_(params) {
    params_.validate();
}

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

void AtmosphereBackgroundPass::setParameters(RenderDevice* device,
                                              const AtmosphereParameters& params) {
    params_ = params;
    params_.validate();
    // Recreate shader with new parameters if needed
    // (currently parameters are passed as uniforms, so no rebuild needed)
    (void)device;
}

RenderCommand AtmosphereBackgroundPass::buildCommand(
    const Vec3& sunDirECEF,
    const Vec3& cameraPos,
    const float* viewMatrix,
    float fovRadians,
    int viewportWidth,
    int viewportHeight,
    bool isOrthographic,
    float frustumLeft,
    float frustumRight,
    float frustumTop,
    float frustumBottom) const {

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

    // Sun direction uniform
    cmd.uniforms["u_sunDir"] = {
        static_cast<float>(sunDirECEF.x()),
        static_cast<float>(sunDirECEF.y()),
        static_cast<float>(sunDirECEF.z())
    };

    // Camera position uniform
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

    // FOV (radians)
    cmd.uniforms["u_fov"] = {fovRadians};
    cmd.uniforms["u_isOrthographic"] = {isOrthographic ? 1.0f : 0.0f};

    // Frustum params
    cmd.uniforms["u_frustumParams"] = {
        frustumLeft - frustumRight,  // x: total width
        frustumTop - frustumBottom,  // y: total height
        frustumRight,
        frustumTop
    };

    // View matrix (16 floats, column-major)
    cmd.uniforms["u_viewMatrix"] = std::vector<float>(viewMatrix, viewMatrix + 16);

    // Atmosphere parameters as uniforms
    const auto& p = params_;
    cmd.uniforms["u_atmosHeight"] = {static_cast<float>(p.atmosHeight)};
    cmd.uniforms["u_bottomRadius"] = {static_cast<float>(p.bottomRadius)};
    cmd.uniforms["u_topRadius"] = {static_cast<float>(p.topRadius())};
    cmd.uniforms["u_rayleighScattering"] = {
        static_cast<float>(p.rayleigh.r * 5e-5),
        static_cast<float>(p.rayleigh.g * 5e-5),
        static_cast<float>(p.rayleigh.b * 5e-5)
    };
    cmd.uniforms["u_mieScattering"] = {
        static_cast<float>(p.mie.scattering * 5e-5),
        static_cast<float>(p.mie.scattering * 5e-5 * 0.8f),
        static_cast<float>(p.mie.scattering * 5e-5 * 0.5f)
    };
    cmd.uniforms["u_ozoneAbsorption"] = {
        static_cast<float>(p.ozone.r * 5e-5),
        static_cast<float>(p.ozone.g * 5e-5),
        static_cast<float>(p.ozone.b * 5e-5)
    };
    cmd.uniforms["u_rayleighScaleHeight"] = {static_cast<float>(p.rayleighScaleHeight)};
    cmd.uniforms["u_mieScaleHeight"] = {static_cast<float>(p.mieScaleHeight)};
    cmd.uniforms["u_sunIntensity"] = {static_cast<float>(p.sunIntensity)};
    cmd.uniforms["u_groundAlbedo"] = {static_cast<float>(p.groundAlbedo)};
    cmd.uniforms["u_ozoneDensityHeight"] = {static_cast<float>(p.ozoneDensityHeight)};
    cmd.uniforms["u_ozoneDensityWidth"] = {static_cast<float>(p.ozoneDensityWidth)};

    return cmd;
}

void AtmosphereBackgroundPass::dispose() {
    // Resources owned by RenderDevice; just clear pointers
    shader_ = nullptr;
    quadBuffer_ = nullptr;
    // Note: actual GPU resource deletion is handled by RenderDevice::onSurfaceDestroyed()
}

} // namespace earth_engine
