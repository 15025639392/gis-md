#include "OffscreenPostProcess.h"

#include "../debug/PlatformLog.h"
#include "../environment/AtmosphereSkyColorGLSL.h"

#include <string>

namespace earth_engine {

namespace {

// GL 侧离屏纹理与默认目标共享 bottom-left 原点,离屏→全屏采样是
// flip-中性的,UV 直传即可(跨后端 y-flip 契约见设计文档 §2.3)。
const char* kFullscreenVertGLSL = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 a_position;
out vec2 v_uv;
void main() {
    v_uv = a_position * 0.5 + 0.5;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// 采样器复用 u_tileTexture 名字:后端 sampler 配置表已把它固定绑 unit 0,
// textures[0] 即离屏 color。
const char* kBlitFragGLSL = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tileTexture;
in vec2 v_uv;
out vec4 fragColor;
void main() {
    fragColor = texture(u_tileTexture, v_uv);
}
)";

// Tonemap(T2 强制终端):采样线性 HDR 场景色 → Khronos PBR-Neutral tonemap
// → sRGB encode → 8bit。highp(mediump 存不下 HDR >1)。曲线 = Cesium 默认
// 同款(hue 稳定、不过饱和)。u_tileTexture=离屏 HDR color(unit 0)。
const char* kTonemapFragGLSL = R"(#version 300 es
precision highp float;
uniform sampler2D u_tileTexture;
in vec2 v_uv;
out vec4 fragColor;
vec3 pbrNeutralToneMapping(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}
void main() {
    vec3 hdr = texture(u_tileTexture, v_uv).rgb;           // 线性 HDR 场景色
    vec3 mapped = pbrNeutralToneMapping(max(hdr, vec3(0.0)));
    fragColor = vec4(pow(mapped, vec3(1.0 / 2.2)), 1.0);   // linear → sRGB
}
)";

// FXAA(Timothy Lottes / NVIDIA 经典版,luma 边缘检测 + 定向 4-tap 模糊)。
// u_inverseResolution = (1/width, 1/height),邻域 texel 偏移用。低对比区
// early-out 保 UI/文字锐利。首版走亮度阈值默认参数,mobile GLES3 稳。
const char* kFxaaFragGLSL = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tileTexture;
uniform vec2 u_inverseResolution;
in vec2 v_uv;
out vec4 fragColor;

const float kEdgeThresholdMin = 0.0312;
const float kEdgeThreshold = 0.125;
const float kSpanMax = 8.0;
const float kReduceMul = 0.125;
const float kReduceMin = 1.0 / 128.0;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 inv = u_inverseResolution;
    vec3 rgbM = texture(u_tileTexture, v_uv).rgb;
    float lM = luma(rgbM);
    float lNW = luma(texture(u_tileTexture, v_uv + vec2(-inv.x, -inv.y)).rgb);
    float lNE = luma(texture(u_tileTexture, v_uv + vec2( inv.x, -inv.y)).rgb);
    float lSW = luma(texture(u_tileTexture, v_uv + vec2(-inv.x,  inv.y)).rgb);
    float lSE = luma(texture(u_tileTexture, v_uv + vec2( inv.x,  inv.y)).rgb);

    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // 平坦区跳过 AA(保 UI/文字锐利,省算力)。
    float range = lMax - lMin;
    if (range < max(kEdgeThresholdMin, lMax * kEdgeThreshold)) {
        fragColor = vec4(rgbM, 1.0);
        return;
    }

    // 边缘方向 = 亮度梯度的垂直方向。
    vec2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));

    float dirReduce =
        max((lNW + lNE + lSW + lSE) * 0.25 * kReduceMul, kReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -kSpanMax, kSpanMax) * inv;

    // 沿边缘方向 2-tap 近芯 + 2-tap 远端平均。
    vec3 rgbA = 0.5 * (
        texture(u_tileTexture, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(u_tileTexture, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(u_tileTexture, v_uv + dir * -0.5).rgb +
        texture(u_tileTexture, v_uv + dir *  0.5).rgb);

    // 远端平均若越出邻域亮度范围(过冲)则退回近芯平均。
    float lB = luma(rgbB);
    fragColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}
)";

// Aerial fog(大气一致的距离雾,Google Earth / Cesium 式):采样离屏 color
// (unit0)+ depth(unit1),reverse-Z 反算 eye-space 视距 d,再:
//   ① 雾色 = 该像素视线方向的天空色(移植大气 pass 天空色近似,与天空
//      同源→远处地形无缝融进它正前方那块天空,随高度/方向/太阳变);
//   ② 密度随相机高度衰减(近地浓、超 maxHeight 关)+ 视线角(朝地平线最
//      浓、朝下几乎无——aerial perspective 主要在地平线可见,同 Cesium Fog.js);
//   ③ 指数-平方雾混合(同 czm_fog)。
// reverse-Z:z_ndc=2*z_win-1(GLES depth range [0,1]),d=near*far/(z_ndc*(far-near)+near)。
// 背景(z_win<0.25,无地形写入)跳过——天空色已在离屏 color 里。
const char* kAerialFogFragHead = R"(#version 300 es
precision highp float;
uniform sampler2D u_tileTexture;   // 离屏 color, unit 0
uniform sampler2D u_depthTexture;  // 离屏 depth,  unit 1
uniform vec2 u_depthNearFar;       // (near, far) 米
uniform vec2 u_fogParams;          // (density, startDistance)
uniform vec3 u_camPos;             // 相机 ECEF
uniform vec3 u_camRight;
uniform vec3 u_camUp;
uniform vec3 u_camForward;
uniform vec3 u_sunDir;             // 太阳方向 ECEF(单位)
uniform vec2 u_fovAspect;          // (tanFovHalf, aspect)
uniform float u_planetRadius;
in vec2 v_uv;
out vec4 fragColor;
)";

// kSkyColorGLSL(computeSkyColor)在此注入:雾色 = 大气 pass 同一天空色
// 模型,逐分量恒等 → 远处地形融进正前方天空、交接无缝。见
// AtmosphereSkyColorGLSL.h。旧的本地 skyColorFor 已删除(那是与大气 pass
// 平行的第二套近似,正是"搭配不完美"的根因)。
const char* kAerialFogFragMain = R"(
void main() {
    vec3 color = texture(u_tileTexture, v_uv).rgb;
    float zWin = texture(u_depthTexture, v_uv).r;
    if (zWin < 0.25) {            // 背景/天空:不加雾。
        fragColor = vec4(color, 1.0);
        return;
    }
    float near = u_depthNearFar.x;
    float far = u_depthNearFar.y;
    float zNdc = 2.0 * zWin - 1.0;
    float d = (near * far) / (zNdc * (far - near) + near);

    // per-pixel 视线(与大气 pass 逐字一致的重建)。
    vec2 ndc = v_uv * 2.0 - 1.0;
    float tanHalf = u_fovAspect.x;
    float aspect = u_fovAspect.y;
    vec3 rayDir = normalize(u_camForward +
                            u_camRight * ndc.x * tanHalf * aspect +
                            u_camUp * ndc.y * tanHalf);
    vec3 up = normalize(u_camPos);
    vec3 sun = normalize(u_sunDir);
    float camHeight = max(length(u_camPos) - u_planetRadius, 0.0);

    // 密度:基础强度 × 视线角(地平线最浓、朝下清)× 高度衰减(近地浓、
    // 超 maxHeight 关)。maxHeight=150km:高空俯瞰基本无雾。
    const float maxHeight = 150000.0;
    float viewWeight = 1.0 - abs(dot(rayDir, up));
    float heightWeight = smoothstep(maxHeight, 0.0, camHeight);
    float density = u_fogParams.x * viewWeight * heightWeight;

    float startDistance = u_fogParams.y;
    float fogDist = max(d - startDistance, 0.0);
    // 平-指数雾(aerial perspective):haze 从近到远**连续**累积,给出纵深/
    // 空间感。指数-平方会让近处极清、只有地平线饱和(二元、无中景过渡、显
    // 平),故用平-exp——中景就开始渐变发蓝,如 Google Earth。
    float fog = clamp(1.0 - exp(-fogDist * density), 0.0, 1.0);
    // 地平线处(视线近水平,viewUp≈0)强制全雾:最远地形距离有限,普通指数
    // 雾只到 ~0.9,残留几%地形色比纯雾天空略深 → 地平线硬轮廓边。这里让近
    // 水平视线的地形完全融进天空(GE 远景本就全雾化),消除交接硬边。
    float viewUp = dot(rayDir, up);
    fog = max(fog, 1.0 - smoothstep(0.0, 0.06, abs(viewUp)));

    // 雾色 = 该视线方向的天空色(与大气 pass 同一 computeSkyColor)。
    // spaceFactor 用与大气 pass 一致的公式(近地 fog 生效区恒≈0,深黑项
    // 不起作用,但保持同源可读)。
    float spaceFactor = smoothstep(120000.0, 900000.0, camHeight);
    vec3 fogColor = computeSkyColor(rayDir, up, sun, spaceFactor);
    fragColor = vec4(mix(color, fogColor, fog), 1.0);
}
)";

// AerialFogTonemap(B0):HDR 路径下 fog + tonemap 合并终端。共用 kAerialFogFragHead
// 的 uniform 集(采样离屏 HDR color + depth,重建视距/视线),先按 kAerialFogFragMain
// 同一套 fog 数学混雾色(在 tonemap 前的线性域),再 PBR-Neutral tonemap + sRGB encode
// → 8bit 上屏。与纯 AerialFog 的唯一区别是收尾多了 tonemap+encode(那条路场景是 LDR
// 已在显示空间,直接上屏)。fog 数学与 tonemap 曲线都逐字复用现有两份,不另立第三套。
// ⚠️ 色彩空间:HDR 下离屏 16F 里地形(kTerrainLightHdrGLSL)与背景天空
// (kAtmosphereComposeHdr)都是**线性**;computeSkyColor 返回 gamma 显示色,故雾色
// 必须 srgbToLinear 后再 mix,否则全雾像素比背景天空亮一个 gamma(地平线雾带过亮)。
const char* kAerialFogTonemapMain = R"(
vec3 srgbToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 pbrNeutralToneMapping(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}
void main() {
    vec3 color = texture(u_tileTexture, v_uv).rgb;   // 线性 HDR 场景色
    float zWin = texture(u_depthTexture, v_uv).r;
    if (zWin >= 0.25) {                              // 前景地形:加雾(背景天空跳过)
        float near = u_depthNearFar.x;
        float far = u_depthNearFar.y;
        float zNdc = 2.0 * zWin - 1.0;
        float d = (near * far) / (zNdc * (far - near) + near);

        vec2 ndc = v_uv * 2.0 - 1.0;
        float tanHalf = u_fovAspect.x;
        float aspect = u_fovAspect.y;
        vec3 rayDir = normalize(u_camForward +
                                u_camRight * ndc.x * tanHalf * aspect +
                                u_camUp * ndc.y * tanHalf);
        vec3 up = normalize(u_camPos);
        vec3 sun = normalize(u_sunDir);
        float camHeight = max(length(u_camPos) - u_planetRadius, 0.0);

        const float maxHeight = 150000.0;
        float viewWeight = 1.0 - abs(dot(rayDir, up));
        float heightWeight = smoothstep(maxHeight, 0.0, camHeight);
        float density = u_fogParams.x * viewWeight * heightWeight;

        float startDistance = u_fogParams.y;
        float fogDist = max(d - startDistance, 0.0);
        float fog = clamp(1.0 - exp(-fogDist * density), 0.0, 1.0);
        float viewUp = dot(rayDir, up);
        fog = max(fog, 1.0 - smoothstep(0.0, 0.06, abs(viewUp)));

        float spaceFactor = smoothstep(120000.0, 900000.0, camHeight);
        // 雾色线性化:与线性地形/背景天空(kAtmosphereComposeHdr 也 srgbToLinear)
        // 同域,mix 才自洽。
        vec3 fogColor = srgbToLinear(computeSkyColor(rayDir, up, sun, spaceFactor));
        color = mix(color, fogColor, fog);
    }
    vec3 mapped = pbrNeutralToneMapping(max(color, vec3(0.0)));
    fragColor = vec4(pow(mapped, vec3(1.0 / 2.2)), 1.0);  // linear → sRGB
}
)";

const char* diagTag(OffscreenPostProcess::Effect effect) {
    switch (effect) {
        case OffscreenPostProcess::Effect::Fxaa:
            return "FXAADIAG";
        case OffscreenPostProcess::Effect::AerialFog:
            return "FOGDIAG";
        case OffscreenPostProcess::Effect::Tonemap:
            return "HDRDIAG";
        case OffscreenPostProcess::Effect::AerialFogTonemap:
            return "HDRFOGDIAG";
        case OffscreenPostProcess::Effect::Passthrough:
        default:
            return "RTTDIAG";
    }
}

std::string fragForEffect(OffscreenPostProcess::Effect effect) {
    switch (effect) {
        case OffscreenPostProcess::Effect::Fxaa:
            return kFxaaFragGLSL;
        case OffscreenPostProcess::Effect::AerialFog:
            // 拼接:头(uniform) + 共享 computeSkyColor + main。雾色与大气
            // pass 同源。
            return std::string(kAerialFogFragHead) + kSkyColorGLSL() +
                   kAerialFogFragMain;
        case OffscreenPostProcess::Effect::Tonemap:
            return kTonemapFragGLSL;
        case OffscreenPostProcess::Effect::AerialFogTonemap:
            // 头(uniform,含 depth)+ 共享 computeSkyColor + fog-then-tonemap main。
            return std::string(kAerialFogFragHead) + kSkyColorGLSL() +
                   kAerialFogTonemapMain;
        case OffscreenPostProcess::Effect::Passthrough:
        default:
            return kBlitFragGLSL;
    }
}

} // anonymous namespace

bool OffscreenPostProcess::initialize(RenderDevice* device, Effect effect) {
    if (!device) return false;
    effect_ = effect;
    const char* tag = diagTag(effect);
    if (!device->supportsOffscreenPostProcess()) {
        // 能力由后端声明(RenderDevice::supportsOffscreenPostProcess),不再
        // 在这里硬编码 backend 判断;Metal 缺 MSL 入口 + submit 侧接线,
        // pass API(beginPass 离屏 attachment)本身两后端就绪。
        platformLog(LogLevel::Warning, tag,
                    "offscreen post-process: backend reports unsupported");
        return false;
    }
    device_ = device;

    ShaderDesc shaderDesc;
    shaderDesc.vertexSource = kFullscreenVertGLSL;
    shaderDesc.fragmentSource = fragForEffect(effect);
    shader_ = device->createShader(shaderDesc);
    if (!shader_) {
        platformLog(LogLevel::Error, tag, "shader create failed");
        return false;
    }

    const float quadVertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    BufferDesc bufferDesc;
    bufferDesc.size = sizeof(quadVertices);
    bufferDesc.data = quadVertices;
    bufferDesc.usage = BufferDesc::Usage::Static;
    bufferDesc.type = BufferDesc::Type::Vertex;
    quadBuffer_ = device->createBuffer(bufferDesc);
    if (!quadBuffer_) {
        platformLog(LogLevel::Error, tag, "quad create failed");
        shader_.reset();
        return false;
    }
    return true;
}

Framebuffer* OffscreenPostProcess::ensureFramebuffer(int width, int height) {
    if (!device_ || width <= 0 || height <= 0) return nullptr;
    if (framebuffer_ && (framebuffer_->width() != width ||
                         framebuffer_->height() != height)) {
        framebuffer_.reset();
    }
    if (!framebuffer_) {
        FramebufferDesc desc;
        desc.width = width;
        desc.height = height;
        desc.hasColor = true;
        desc.hasDepth = true;
        desc.samples = 1;
        // AerialFog / AerialFogTonemap 要采样场景深度重建视距 → 深度须可采样。
        desc.depthSampleable = (effect_ == Effect::AerialFog ||
                                effect_ == Effect::AerialFogTonemap);
        // Tonemap / AerialFogTonemap:场景画进线性 HDR 靶(RGBA16F),本 pass
        // tonemap→8bit。后端探不到 float-color-renderable 时 createFramebuffer
        // 回落 RGBA8(那时输出偏暗=预期回落,见 PipelineConfig.h)。
        desc.colorFormat = (effect_ == Effect::Tonemap ||
                            effect_ == Effect::AerialFogTonemap)
                               ? TextureDesc::Format::RGBA16F
                               : TextureDesc::Format::RGBA8;
        // 场景主 pass 落在本 FBO 上:矢量 stencil 分类(P6)需要 stencil
        // 附件,否则测试恒通过分类失效。
        desc.hasStencil = true;
        framebuffer_ = device_->createFramebuffer(desc);
        if (framebuffer_) {
            platformLog(LogLevel::Info, diagTag(effect_),
                        "offscreen fbo ready %dx%d", width, height);
        }
    }
    return framebuffer_.get();
}

RenderCommand OffscreenPostProcess::buildCommand(
    const FrameParams& params) const {
    RenderCommand cmd;
    // 直接经 device->submit 提交,不进 Scene 主链路,故不占用 MVP kind。
    cmd.kind = RenderCommandKind::Unknown;
    cmd.owner = "offscreen_postprocess";
    cmd.pass = "postprocess";
    cmd.shader = shader_.get();
    cmd.vertexBuffer = quadBuffer_.get();
    cmd.vertexCount = 4;
    cmd.vertexStride = 2 * sizeof(float);
    cmd.primitive = RenderCommand::PrimitiveType::TriangleStrip;
    cmd.depthTest = false;
    cmd.depthWrite = false;
    cmd.blend = false;
    cmd.cullFace = false;
    if (!framebuffer_) {
        return cmd;
    }
    cmd.textures.push_back(framebuffer_->colorTexture());  // unit 0 = color
    if (effect_ == Effect::Fxaa) {
        const float w = static_cast<float>(framebuffer_->width());
        const float h = static_cast<float>(framebuffer_->height());
        cmd.uniforms["u_inverseResolution"] = {1.0f / w, 1.0f / h};
    } else if (effect_ == Effect::AerialFog ||
               effect_ == Effect::AerialFogTonemap) {
        // depth 绑 unit 1(u_depthTexture);后端 sampler 表已把它固定绑 1。
        cmd.textures.push_back(framebuffer_->depthTexture());
        cmd.uniforms["u_depthNearFar"] = {params.nearPlane, params.farPlane};
        cmd.uniforms["u_fogParams"] = {params.fogDensity,
                                       params.fogStartDistance};
        cmd.uniforms["u_camPos"] = {params.camPos[0], params.camPos[1],
                                    params.camPos[2]};
        cmd.uniforms["u_camRight"] = {params.camRight[0], params.camRight[1],
                                      params.camRight[2]};
        cmd.uniforms["u_camUp"] = {params.camUp[0], params.camUp[1],
                                   params.camUp[2]};
        cmd.uniforms["u_camForward"] = {params.camForward[0],
                                        params.camForward[1],
                                        params.camForward[2]};
        cmd.uniforms["u_sunDir"] = {params.sunDir[0], params.sunDir[1],
                                    params.sunDir[2]};
        cmd.uniforms["u_fovAspect"] = {params.tanFovHalf, params.aspect};
        cmd.uniforms["u_planetRadius"] = {params.planetRadius};
    }
    return cmd;
}

void OffscreenPostProcess::dispose() {
    framebuffer_.reset();
    quadBuffer_.reset();
    shader_.reset();
    device_ = nullptr;
}

} // namespace earth_engine
