#include "AtmosphereBackgroundPass.h"
#include "AtmosphereSkyColorGLSL.h"
#include "../core/geodesy/Ellipsoid.h"
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <cstring>
#include <string>

#include "../debug/PlatformLog.h"
// LOGI/LOGE 现为统一 platformLog 的薄别名（Android -> logcat，其它平台 -> stderr）。
#define LOGI(...) ::earth_engine::platformLog(::earth_engine::LogLevel::Info, "AtmosPass", __VA_ARGS__)
#define LOGE(...) ::earth_engine::platformLog(::earth_engine::LogLevel::Error, "AtmosPass", __VA_ARGS__)

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

const char* kAtmosphereBackgroundFragHead = R"(#version 300 es
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
uniform float u_sunDiskEnabled;
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
)";

// kSkyColorGLSL(computeSkyColor)在此注入:大气 pass 与 aerial fog 共享
// 同一天空色模型,雾霾色逐分量恒等天空色。见 AtmosphereSkyColorGLSL.h。
const char* kAtmosphereBackgroundFragMain = R"(
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

    // 命中/掠射星球的光线:地形通常覆盖它们(本 pass depthTest 已把 fragment
    // 挡在地形之后,terrain 写过深度的像素这里必被丢弃)。但地形几何到 limb
    // 边缘会留一两像素缝没覆盖 —— 旧版 discard 让这些缝露出离屏黑底,fog pass
    // 又因 zWin<0.25 当背景跳过 → 地平线上一条纯黑虚线。改为输出该方向的地平线
    // 雾霾色(与 fog 同一 computeSkyColor):缝隙填霾无缝,terrain 覆盖处仍被
    // 深度测试丢弃,故只影响真正空缺的像素。
    if (hitPlanet) {
        vec3 gapUp = normalize(cam);
        float gapHeight = max(length(cam) - R, 0.0);
        float gapSpace = smoothstep(120000.0, 900000.0, gapHeight);
        fragColor = vec4(computeSkyColor(rayDir, gapUp, sun, gapSpace), 1.0);
        return;
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
    vec3 mieColor = vec3(0.2000, 0.4196, 1.0); // #336BFF
    float r = rayleighDepth * 0.055 * rPhase;
    float m = mieDepth * 0.020 * mPhase;

    vec3 localUp = normalize(cam);
    float camHeight = max(length(cam) - R, 0.0);
    float spaceFactor = smoothstep(120000.0, 900000.0, camHeight);

    // 天空底色 = 统一散射模型(与 aerial fog 逐分量同源)。仰角渐变 +
    // 太空压黑 + 太阳侧地平线微暖都在 computeSkyColor 里,雾色调它同一函数
    // → 天空↔地形交接恒等无缝。旧的 screen-y 参数化 + 独立 horizon band 常数
    // 对齐已删除(那是"三套模型凑近似"的病根)。
    vec3 color = computeSkyColor(rayDir, localUp, sun, spaceFactor);

    vec3 scatterColor = rayleighColor * r + mieColor * m;
    scatterColor *= u_sunIntensity * 0.85;

    float opticalThickness = rayleighDepth * 0.018 + mieDepth * 0.010;
    float pathScatterAmount =
        clamp(1.0 - exp(-opticalThickness), 0.0, 0.35) * u_opacity;
    color = mix(color, color + scatterColor, pathScatterAmount * spaceFactor);

    // ---- 太阳侧大气 rim light（从太空看地球的蓝色大气环）----
    // 只有掠过地球边缘的天空光线才有 rim：ray 到地心的最近距离 perpDist>R(擦边而
    // 过,未命中星球——命中的已 discard)。limbHeight = perpDist-R 在贴着地表处最小、
    // 越往高空越大,用指数壳做出贴地最亮、向上淡出的薄环。sunlit 让 rim 在朝阳的一
    // 侧最亮、跨过晨昏线向夜面淡出——即"太阳光照亮地球边缘"的真实感。仅太空可见
    // (spaceFactor 门控),近地时地球边缘不在画面里。
    float perpDist = sqrt(max(c - b * b, 0.0));
    float limbHeight = perpDist - R;
    vec3 limbPoint = cam + rayDir * b;            // 光线到地心的最近点
    float sunlit = smoothstep(-0.15, 0.35, dot(normalize(limbPoint), sun));
    float rim = exp(-max(limbHeight, 0.0) / 26000.0) *
                step(0.0, limbHeight) * spaceFactor * sunlit;
    // 朝阳最亮处偏冷白,向晨昏线偏天蓝——大气瑞利散射的观感。
    vec3 rimColor = mix(vec3(0.28, 0.52, 1.0), vec3(0.72, 0.86, 1.0), sunlit);
    color += rimColor * rim * u_sunIntensity * 1.15;

    // ---- Sun disk + halo：真实日面尺寸 + 大气门控的绘画风放射霞光 ----
    // u_sunAngularRadius 现为真实值(≈0.00465rad)，从太空看是远处小亮盘=真实日地
    // 距离感。暖金大光晕与放射霞光是**大气散射**效果，故按 atmoScatter(=1-spaceFactor)
    // 门控：近地满、太空里淡出，只留紧致亮芯——太空中即为写实的小太阳。所有项另受
    // forwardScatter 门控，太阳在背面/侧面时自然淡出。系数均为可调旋钮。
    float cosTheta = dot(rayDir, sun);
    float baseSunRadius = max(u_sunAngularRadius, 0.0001);
    float sunAngle = sqrt(max(2.0 * (1.0 - cosTheta), 0.0));
    float atmoScatter = 1.0 - spaceFactor;  // 大气效果强度:近地=1,太空=0
    // d: 归一化角距，1.0 落在圆盘边缘。
    float discRadius = baseSunRadius * 0.72;
    float d = sunAngle / discRadius;
    float dClamp = min(d, 1.0);

    // 柔边圆盘：边缘用宽 smoothstep 融进光晕，不再是硬切。
    float core = 1.0 - smoothstep(0.55, 1.10, d);
    float limb = sqrt(max(1.0 - dClamp * dClamp, 0.0));
    // 暖色绘画调色板：近白的芯 → 暖金 → 琥珀边。
    vec3 sunHeart = vec3(1.00, 0.97, 0.86);
    vec3 sunGold  = vec3(1.00, 0.86, 0.52);
    vec3 sunAmber = vec3(1.00, 0.66, 0.30);
    vec3 discColor = mix(sunAmber, sunGold, smoothstep(0.0, 0.7, limb));
    discColor = mix(discColor, sunHeart, pow(limb, 1.6));

    // 暖金大光晕：参考日出照片的金色光团——加大加暖，向天空平滑晕开。三层壳:
    // 贴边亮芯 bloom / 中层暖金 / 宽阔琥珀拖尾。
    float forwardScatter = smoothstep(0.5, 1.0, cosTheta);
    float halo1 = exp(-max(d - 0.6, 0.0) * 2.1);   // 贴边的紧致 bloom(镜头/眼睛过曝)
    float halo2 = exp(-max(d - 1.0, 0.0) * 0.55);  // 中层暖金晕染(大气)
    float halo3 = 1.0 / (1.0 + d * d * 0.085);     // 宽阔的金色大光团(大气)
    // halo1 太空里也保留(亮源过曝 bloom);halo2/halo3 是大气金色光晕,随 atmoScatter
    // 淡出——太空中只剩紧致亮芯,符合远处小太阳的真实观感。
    vec3 haloColor = sunHeart * halo1 * 0.74 +
                     (sunGold * halo2 * 0.54 + sunAmber * halo3 * 0.30) * atmoScatter;
    haloColor *= forwardScatter;

    // 放射状霞光(crepuscular rays / sunburst)：屏幕空间里从太阳向外辐射的光束。
    // 用三组不同频率+相位的角向脉冲叠加，得到粗细不匀、不规则的自然光束(避免
    // 机械十字星),近太阳最亮、随距离拉长淡出。参考图二的放射霞光。
    vec3 sunView = vec3(
        dot(sun, u_camRight),
        dot(sun, u_camUp),
        dot(sun, u_camForward));
    float sunInFront = smoothstep(0.0, 0.05, sunView.z);
    vec2 screenUv = vec2(ndcX, ndcY);
    vec2 sunScreenUv = sunView.xy / max(sunView.z * tanFovHalf, 0.0001);
    vec2 screenDelta = screenUv - sunScreenUv;
    float screenDist = length(screenDelta);
    float rayAngle = atan(screenDelta.y, screenDelta.x);
    float rays = pow(0.5 + 0.5 * cos(rayAngle * 13.0), 7.0) * 1.0 +
                 pow(0.5 + 0.5 * cos(rayAngle * 24.0 + 1.7), 11.0) * 0.55 +
                 pow(0.5 + 0.5 * cos(rayAngle * 7.0 - 0.9), 5.0) * 0.6;
    // 起点略在盘外(smoothstep)避免正中心堆亮点。小衰减系数=光束拉得很长,
    // 能从太阳一路伸到地球上("太阳光射线直接打到地球")。
    float sunburst = rays * exp(-screenDist * 1.25) *
                     smoothstep(0.015, 0.06, screenDist) * 0.38;
    // 光束在太空里也保留大部分(0.7 floor),这样从太空看太阳的光线也能射到地球;
    // 近地时(大气)再补足到满。
    sunburst *= sunInFront * forwardScatter * (0.7 + 0.3 * atmoScatter);

    vec3 sunColor = discColor * (core + limb * 0.35) +
                    haloColor +
                    mix(sunGold, sunHeart, 0.3) * sunburst;

    float sunEnabled = step(0.5, u_sunDiskEnabled);
    color += sunEnabled * sunColor * u_sunIntensity * 1.4;

    float skyAlpha = mix(1.0, 0.18, spaceFactor);
    float limbAlpha = pathScatterAmount * spaceFactor;
    float rimAlpha = clamp(rim, 0.0, 1.0);   // 让蓝色 rim 在深色太空天中显现
    float sunAlpha = sunEnabled * forwardScatter *
        clamp(core + halo1 * 0.6 +
                  (halo2 * 0.35 + halo3 * 0.2) * atmoScatter + sunburst,
              0.0,
              1.0);
    float alpha = max(max(max(skyAlpha, limbAlpha), sunAlpha), rimAlpha);
    // 打破平滑天空渐变的 8-bit 量化条带(banding):按屏幕位置加 ±0.5/255
    // 的有序抖动,把台阶散成噪声(GE 天空是平滑的)。仅影响最低有效位。
    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)))
                         * 43758.5453);
    color += (dither - 0.5) / 255.0;
    fragColor = vec4(color, clamp(alpha, 0.0, 1.0));
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
    // 拼接:头(uniform+phase 函数) + 共享 computeSkyColor + main。
    shaderDesc.fragmentSource = std::string(kAtmosphereBackgroundFragHead) +
                                kSkyColorGLSL + kAtmosphereBackgroundFragMain;
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
    cmd.uniforms["u_sunDiskEnabled"] = {params.disableSunDisk ? 0.0f : 1.0f};
    cmd.uniforms["u_opacity"] = {opacity};
    cmd.uniforms["u_resolution"] = {static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)};

    return cmd;
}

void AtmosphereBackgroundPass::dispose() {
    shader_ = nullptr;
    quadBuffer_ = nullptr;
}

} // namespace earth_engine
