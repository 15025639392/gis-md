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
    vec3 transmittanceFromCameraToSpace = vec3(1.0);

    // Ray-sphere intersection with atmosphere
    float offset = 0.0;
    float distanceToSpace = 0.0;

    vec2 atmHit = intersectSphere(cameraPosition, rayDirection, vec3(0.0), TOP_RADIUS);
    if (atmHit.y > 0.0 && atmHit.x < atmHit.y) {
        offset = max(atmHit.x, 0.0);
        distanceToSpace = atmHit.y;

        vec3 rayOrigin = cameraPosition + rayDirection * offset;

        float height = length(rayOrigin) - BOTTOM_RADIUS;
        float rayAngle = dot(rayOrigin, rayDirection) / length(rayOrigin);

        // Simplified transmittance: approximate as exp(-opticalDepth)
        // For the background pass, we use single-scattering integration

        float phaseAngle = dot(lightDirection, rayDirection);
        float rayleighPhaseVal = rayleighPhase(phaseAngle);
        float miePhaseVal = miePhase(phaseAngle);

        // Earth shadow: check if ray hits the ground
        float distanceToGround = 0.0;
        vec2 groundHit = intersectSphere(cameraPosition, rayDirection, vec3(0.0), BOTTOM_RADIUS);
        bool hitGround = (groundHit.x > 0.0 && groundHit.x < distanceToSpace);

        // If ray hits ground at a close distance, discard (render as black/earth color)
        float earthEdge = BOTTOM_RADIUS - 200000.0;
        vec2 edgeHit = intersectSphere(cameraPosition, rayDirection, vec3(0.0), earthEdge);
        if (edgeHit.x > 0.0 && edgeHit.x < distanceToSpace && hitGround) {
            discard;
        }

        float segmentLength = ((hitGround ? distanceToGround : distanceToSpace) - offset) / 20.0;
        float t = segmentLength * 0.5;

        for (int i = 0; i < 20; i++) {
            vec3 position = rayOrigin + float(i) * segmentLength * rayDirection;
            float h = length(position) - BOTTOM_RADIUS;
            if (h < 0.0) break;

            vec3 up = position / length(position);
            float lightAngle = dot(up, lightDirection);

            // Density at this height
            float rayleighDensity = exp(-h / u_rayleighScaleHeight);
            float mieDensity = exp(-h / u_mieScaleHeight);

            // Optical depth from sun to this point (approximate)
            float sunPathLength = TOP_RADIUS / max(abs(lightAngle), 0.05);
            float rayleighOD = u_rayleighScattering.r * rayleighDensity * sunPathLength;
            float mieOD = u_mieScattering.r * mieDensity * sunPathLength;

            vec3 extinction = vec3(exp(-(rayleighOD + mieOD)));

            // Scattered light at this point
            vec3 scattered = (u_rayleighScattering * rayleighDensity * rayleighPhaseVal +
                              u_mieScattering * mieDensity * miePhaseVal) *
                             extinction * u_sunIntensity;

            // Transmittance from point to camera (simplified)
            float camPathLength = t;
            float camRayleighOD = u_rayleighScattering.r * rayleighDensity * camPathLength;
            float camMieOD = u_mieScattering.r * mieDensity * camPathLength;
            vec3 camTransmittance = vec3(exp(-(camRayleighOD + camMieOD)));

            light += scattered * camTransmittance * segmentLength;
            t += segmentLength;
        }

        light *= u_sunIntensity;

        // Sun disk
        if (!hitGround) {
            float sunAngle = dot(rayDirection, lightDirection);
            float sunAngularRadius = 0.004685;
            if (sunAngle > cos(sunAngularRadius * 3.0)) {
                float sunMask = smoothstep(cos(sunAngularRadius * 3.0),
                                           cos(sunAngularRadius * 1.5),
                                           sunAngle);
                light += vec3(1.0, 0.95, 0.8) * sunMask * u_sunIntensity * 0.5;
            }
        }

        // Ground reflection for horizon
        if (hitGround) {
            vec3 hitPoint = cameraPosition + rayDirection * distanceToGround;
            vec3 up = hitPoint / length(hitPoint);
            float diffuseAngle = max(dot(up, lightDirection), 0.0);
            light += vec3(0.15, 0.12, 0.1) * u_groundAlbedo * diffuseAngle * u_sunIntensity;
        }
    }

    // HDR tone mapping
    light = light / (light + vec3(1.0));

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
    cmd.blend = false;
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
        static_cast<float>(p.rayleigh.r * 1e-6 * p.rayleighSeaLevelScattering),
        static_cast<float>(p.rayleigh.g * 1e-6 * p.rayleighSeaLevelScattering),
        static_cast<float>(p.rayleigh.b * 1e-6 * p.rayleighSeaLevelScattering)
    };
    cmd.uniforms["u_mieScattering"] = {
        static_cast<float>(p.mie.scattering * 1e-6 * p.mieSeaLevelScattering),
        static_cast<float>(p.mie.scattering * 1e-6 * p.mieSeaLevelScattering * 0.8f),
        static_cast<float>(p.mie.scattering * 1e-6 * p.mieSeaLevelScattering * 0.5f)
    };
    cmd.uniforms["u_ozoneAbsorption"] = {
        static_cast<float>(p.ozone.r * 1e-6),
        static_cast<float>(p.ozone.g * 1e-6),
        static_cast<float>(p.ozone.b * 1e-6)
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
