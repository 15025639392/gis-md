#pragma once

#include "RenderCommand.h"
#include "RenderDevice.h"
#include <memory>

namespace earth_engine {

/// 离屏后处理 pass:场景整帧画进离屏 framebuffer,再全屏采样离屏 color
/// 上屏。这是后处理链(AA / HDR / bloom / 大气)的地基抽象——离屏 FBO
/// 生命周期 + pass 编排是共享的,变化的只是那张全屏采样 shader。
///
/// Effect::Passthrough  纯直通(blit),像素应与直绘一致——RTT 通路的
///                      冒烟验证器,默认关。
/// Effect::Fxaa         FXAA 抗锯齿(NVIDIA/Lottes):采样离屏 color → luma
///                      边缘检测 + 定向模糊 → 写屏。
/// Effect::AerialFog    距离雾(aerial perspective):采样离屏 color + depth
///                      → reverse-Z 线性化视距 → 指数雾混向雾色。填补
///                      AtmosphereBackgroundPass 契约里"地表雾"的空缺。
///
/// 使用方式(Engine 编排):
///   1. initialize(device, effect) 建对应 shader + 全屏 quad
///   2. 每帧 ensureFramebuffer(w,h) 拿离屏目标(尺寸变化惰性重建)
///   3. beginPass(fbo) → 场景 → endPass → beginPass(nullptr) →
///      submit(buildCommand(params)) → endPass
class OffscreenPostProcess {
public:
    enum class Effect { Passthrough, Fxaa, AerialFog };

    /// aerial fog 每帧参数(near/far 视距重建 + 雾色/密度)。非 fog effect
    /// 忽略。density 越大雾越浓;startDistance 前不加雾(米)。
    struct FrameParams {
        float nearPlane = 1.0f;
        float farPlane = 1.0e12f;
        std::array<float, 3> fogColor = {0.62f, 0.82f, 0.94f};
        float fogDensity = 6.0e-6f;
        float fogStartDistance = 0.0f;
    };

    OffscreenPostProcess() = default;
    ~OffscreenPostProcess() = default;

    OffscreenPostProcess(const OffscreenPostProcess&) = delete;
    OffscreenPostProcess& operator=(const OffscreenPostProcess&) = delete;

    /// 创建 GPU 资源。Metal 后端当前返回 false(全屏 shader 仅有 GLSL,
    /// MSL 接线待补——pass API 本身两后端就绪)。
    bool initialize(RenderDevice* device, Effect effect);

    /// 拿离屏 framebuffer,尺寸不符时销毁重建(maplibre 式惰性策略)。
    /// AerialFog 请求可采样深度(depthSampleable)。
    /// 返回 nullptr 表示创建失败(调用方回落直绘主 pass)。
    Framebuffer* ensureFramebuffer(int width, int height);

    /// 全屏后处理命令:采样离屏 color(+ AerialFog 时的 depth)画满默认
    /// 目标。params 仅 AerialFog 用(near/far/雾色/密度);Passthrough/FXAA
    /// 忽略(调用方传默认构造的 FrameParams 即可)。
    RenderCommand buildCommand(const FrameParams& params) const;

    Effect effect() const { return effect_; }
    bool isReady() const { return shader_ != nullptr && quadBuffer_ != nullptr; }

    /// 释放全部 GPU 资源(surface 销毁/context lost 时调用)。
    void dispose();

private:
    RenderDevice* device_ = nullptr;
    Effect effect_ = Effect::Passthrough;
    std::unique_ptr<ShaderProgram> shader_;
    std::unique_ptr<Buffer> quadBuffer_;
    std::unique_ptr<Framebuffer> framebuffer_;
};

} // namespace earth_engine
