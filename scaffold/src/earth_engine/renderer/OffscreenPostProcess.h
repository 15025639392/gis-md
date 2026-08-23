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
/// Effect::AerialFog    大气光学深度雾(aerial perspective,osgEarth 式):
///                      采样离屏 color + depth → reverse-Z 线性化视距 →
///                      沿视线积分指数密度剖面光学深度 → 混向雾色。填补
///                      AtmosphereBackgroundPass 契约里"地表雾"的空缺。
///
/// 使用方式(Engine 编排):
///   1. initialize(device, effect) 建对应 shader + 全屏 quad
///   2. 每帧 ensureFramebuffer(w,h) 拿离屏目标(尺寸变化惰性重建)
///   3. beginPass(fbo) → 场景 → endPass → beginPass(nullptr) →
///      submit(buildCommand(params)) → endPass
class OffscreenPostProcess {
public:
    /// Tonemap:场景画进线性 HDR(RGBA16F)靶,本 pass 采样 → PBR-Neutral
    /// tonemap → sRGB encode → 8bit 上屏。T2 的强制终端 encode(非可选叠加:
    /// 场景一旦是 HDR,终端必须 tonemap)。见 PipelineConfig.h kEnableHdrPipeline。
    ///
    /// AerialFogTonemap:HDR 路径下的「fog + tonemap 合并终端」(B0)。单 effect
    /// 槽的互斥问题:HDR 开则 fog 被挤掉 → 地平线硬切。解法是把 fog 合进 tonemap
    /// 前的**线性域**(aerial perspective 物理上加在相机响应前),同一 pass 内先
    /// 按视距混雾色(computeSkyColor,与背景天空 + LDR fog 同源)再 PBR-Neutral
    /// tonemap + sRGB encode。省一张全屏 16F 中转 + 一趟带宽(vs 链式两 pass)。
    /// ⚠️ 当前切片场景 16F 实为 gamma 空间常数裹进 16F(T1 真线性未做),fogColor
    /// 与场景同空间故 mix 自洽;T1 落地(B2)时二者须一起线性化。
    enum class Effect { Passthrough, Fxaa, AerialFog, Tonemap, AerialFogTonemap };

    /// aerial fog 每帧参数。非 fog effect 忽略。
    /// 雾色不再是固定常数——shader 每像素按视线方向算天空色作雾色(与大气
    /// pass 同源→天然同调);这里的相机基/太阳/半径喂给光学深度积分。
    /// density 是光学深度积分的消光系数(1/m),近地浓/高空关由指数密度剖面
    /// 自然决定,无角度窗口。
    struct FrameParams {
        float nearPlane = 1.0f;
        float farPlane = 1.0e12f;
        float fogDensity = 3.0e-5f;
        float fogStartDistance = 0.0f;
        // 相机基(单位向量)+ 太阳方向(ECEF)+ 视锥,per-pixel 视线重建用。
        std::array<float, 3> camPos = {0.0f, 0.0f, 0.0f};
        std::array<float, 3> camRight = {1.0f, 0.0f, 0.0f};
        std::array<float, 3> camUp = {0.0f, 1.0f, 0.0f};
        std::array<float, 3> camForward = {0.0f, 0.0f, -1.0f};
        std::array<float, 3> sunDir = {0.0f, 0.0f, 1.0f};
        float tanFovHalf = 0.5f;
        float aspect = 1.0f;
        float planetRadius = 6378137.0f;
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
