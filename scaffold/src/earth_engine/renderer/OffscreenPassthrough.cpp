#include "OffscreenPassthrough.h"

#include "../debug/PlatformLog.h"

namespace earth_engine {

namespace {

// GL 侧离屏纹理与默认目标共享 bottom-left 原点,离屏→全屏采样是
// flip-中性的,UV 直传即可(跨后端 y-flip 契约见设计文档 §2.3)。
const char* kBlitVertGLSL = R"(#version 300 es
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

} // anonymous namespace

bool OffscreenPassthrough::initialize(RenderDevice* device) {
    if (!device) return false;
    if (device->backendType() == RenderDevice::Backend::Metal) {
        // TODO: Metal blit 需 MSL 入口 + submit 侧纹理/uniform 接线;
        // pass API(beginPass 离屏 attachment)本身 Metal 已就绪。
        platformLog(LogLevel::Warning, "RTTDIAG",
                    "offscreen passthrough: Metal blit shader not wired yet");
        return false;
    }
    device_ = device;

    ShaderDesc shaderDesc;
    shaderDesc.vertexSource = kBlitVertGLSL;
    shaderDesc.fragmentSource = kBlitFragGLSL;
    shader_ = device->createShader(shaderDesc);
    if (!shader_) {
        platformLog(LogLevel::Error, "RTTDIAG", "blit shader create failed");
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
        platformLog(LogLevel::Error, "RTTDIAG", "blit quad create failed");
        shader_.reset();
        return false;
    }
    return true;
}

Framebuffer* OffscreenPassthrough::ensureFramebuffer(int width, int height) {
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
            platformLog(LogLevel::Info, "RTTDIAG",
                        "offscreen fbo ready %dx%d", width, height);
        }
    }
    return framebuffer_.get();
}

RenderCommand OffscreenPassthrough::buildBlitCommand() const {
    RenderCommand cmd;
    // 直接经 device->submit 提交,不进 Scene 主链路,故不占用 MVP kind。
    cmd.kind = RenderCommandKind::Unknown;
    cmd.owner = "offscreen_passthrough";
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
    }
    return cmd;
}

void OffscreenPassthrough::dispose() {
    framebuffer_.reset();
    quadBuffer_.reset();
    shader_.reset();
    device_ = nullptr;
}

} // namespace earth_engine
