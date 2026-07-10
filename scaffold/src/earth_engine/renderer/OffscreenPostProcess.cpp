#include "OffscreenPostProcess.h"

#include "../debug/PlatformLog.h"

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

const char* diagTag(OffscreenPostProcess::Effect effect) {
    return effect == OffscreenPostProcess::Effect::Fxaa ? "FXAADIAG"
                                                        : "RTTDIAG";
}

} // anonymous namespace

bool OffscreenPostProcess::initialize(RenderDevice* device, Effect effect) {
    if (!device) return false;
    effect_ = effect;
    const char* tag = diagTag(effect);
    if (device->backendType() == RenderDevice::Backend::Metal) {
        // TODO: Metal 全屏 shader 需 MSL 入口 + submit 侧纹理/uniform 接线;
        // pass API(beginPass 离屏 attachment)本身 Metal 已就绪。
        platformLog(LogLevel::Warning, tag,
                    "offscreen post-process: Metal shader not wired yet");
        return false;
    }
    device_ = device;

    ShaderDesc shaderDesc;
    shaderDesc.vertexSource = kFullscreenVertGLSL;
    shaderDesc.fragmentSource =
        effect == Effect::Fxaa ? kFxaaFragGLSL : kBlitFragGLSL;
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
        framebuffer_ = device_->createFramebuffer(desc);
        if (framebuffer_) {
            platformLog(LogLevel::Info, diagTag(effect_),
                        "offscreen fbo ready %dx%d", width, height);
        }
    }
    return framebuffer_.get();
}

RenderCommand OffscreenPostProcess::buildCommand() const {
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
    if (framebuffer_) {
        cmd.textures.push_back(framebuffer_->colorTexture());
        if (effect_ == Effect::Fxaa) {
            const float w = static_cast<float>(framebuffer_->width());
            const float h = static_cast<float>(framebuffer_->height());
            cmd.uniforms["u_inverseResolution"] = {1.0f / w, 1.0f / h};
        }
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
