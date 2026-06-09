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
/// 对齐 openglobus/src/control/SimpleSkyBackground.ts — 使用 SkyGradient
/// 计算的天顶/地平线颜色，片段着色器通过 big sphere intersection 混合。
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

    /// 构建渲染命令（薄壳大气模型，world space 计算）
    /// @param cameraPos 相机 ECEF 位置（米）
    /// @param fovRadians 垂直 FOV
    /// @param viewportWidth/Height 视口像素
    /// @param normalMatrix 相机→世界旋转（9 floats, mat3 column-major）
    /// @param zenithColor 天顶颜色（RGB, 0..1）
    /// @param horizonColor 地平线颜色（RGB, 0..1）
    /// @param earthRadius 地球半径（米）
    RenderCommand buildCommand(
        const Vec3& cameraPos,
        float fovRadians,
        int viewportWidth,
        int viewportHeight,
        const float* normalMatrix,
        const std::array<float, 3>& zenithColor,
        const std::array<float, 3>& horizonColor,
        float earthRadius) const;

    bool isReady() const { return shader_ != nullptr && quadBuffer_ != nullptr; }
    void dispose();

private:
    ShaderProgram* shader_ = nullptr;
    Buffer* quadBuffer_ = nullptr;
    RenderDevice* device_ = nullptr;
};

} // namespace earth_engine
