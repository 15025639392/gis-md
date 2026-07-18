#pragma once

#include "../core/math/Vec3.h"
#include "../renderer/RenderCommand.h"
#include "../renderer/RenderDevice.h"
#include "AtmosphereParameters.h"
#include <memory>
#include <array>

namespace earth_engine {

/// 天空背景全屏渲染 Pass。
///
/// 对齐 openglobus/src/control/atmosphere/Atmosphere.ts + atmosphere.frag.glsl —
/// 使用 Rayleigh + Mie + Ozone 物理散射，透射率实时计算（16 样本光学深度积分），
/// 含 sun disk + Gaussian bloom。暂不依赖预计算 LUT（后期优化可加）。
///
/// 使用方式：
///   1. initialize(device) 创建 GPU 资源
///   2. 每帧调用 buildCommand(...) 生成 RenderCommand
///   3. 渲染器将命令与主 Pass 合并提交
class AtmosphereBackgroundPass {
public:
    AtmosphereBackgroundPass();
    ~AtmosphereBackgroundPass();

    AtmosphereBackgroundPass(const AtmosphereBackgroundPass&) = delete;
    AtmosphereBackgroundPass& operator=(const AtmosphereBackgroundPass&) = delete;

    /// 初始化 GPU 资源（shader + 全屏 quad buffer）
    bool initialize(RenderDevice* device);

    /// 构建渲染命令 — 视线通过相机基向量直接在 ECEF 中重建
    RenderCommand buildCommand(
        const Vec3& cameraPos,
        float fovRadians,
        int viewportWidth,
        int viewportHeight,
        const Vec3& camRight,
        const Vec3& camUp,
        const Vec3& camForward,
        const Vec3& sunDir,
        const AtmosphereParameters& params,
        float opacity = 1.0f) const;

    bool isReady() const { return shader_ != nullptr && quadBuffer_ != nullptr; }
    void dispose();

private:
    std::unique_ptr<ShaderProgram> shader_;
    std::unique_ptr<Buffer> quadBuffer_;
    RenderDevice* device_ = nullptr;
};

} // namespace earth_engine
