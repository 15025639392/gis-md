#pragma once

#include "../core/math/Vec3.h"
#include "../renderer/RenderCommand.h"
#include "../renderer/RenderDevice.h"
#include "AtmosphereParameters.h"
#include <memory>
#include <array>

namespace earth_engine {

/// 大气散射背景全屏渲染 Pass。
///
/// 对标 openglobus/src/control/SimpleSkyBackground.ts + atmosphere.frag.glsl。
/// 使用全屏 quad + 片段着色器直接积分 Rayleigh + Mie 散射，不依赖 LUT 预计算。
///
/// 使用方式：
///   1. initialize(device) 创建 GPU 资源
///   2. 每帧调用 buildCommand(frameState, skyGradient) 生成 RenderCommand
///   3. 渲染器将命令与主 Pass 合并提交
class AtmosphereBackgroundPass {
public:
    AtmosphereBackgroundPass();
    explicit AtmosphereBackgroundPass(const AtmosphereParameters& params);
    ~AtmosphereBackgroundPass();

    // 禁止拷贝
    AtmosphereBackgroundPass(const AtmosphereBackgroundPass&) = delete;
    AtmosphereBackgroundPass& operator=(const AtmosphereBackgroundPass&) = delete;

    /// 初始化 GPU 资源（shader + 全屏 quad buffer）
    bool initialize(RenderDevice* device);

    /// 设置大气参数（会重建 shader）
    void setParameters(RenderDevice* device, const AtmosphereParameters& params);

    /// 构建渲染命令
    /// @param sunDirECEF ECEF 太阳方向单位向量
    /// @param cameraPos 相机 ECEF 位置（米）
    /// @param viewMatrix 相机视图矩阵（4×4，列主序，16 floats）
    /// @param fovRadians 垂直视场角（弧度）
    /// @param viewportWidth 视口宽度（像素）
    /// @param viewportHeight 视口高度（像素）
    /// @param isOrthographic 是否正交投影
    /// @param frustumLeftRight 正交投影左右范围（仅 ortho 使用）
    /// @param frustumTopBottom 正交投影上下范围（仅 ortho 使用）
    RenderCommand buildCommand(
        const Vec3& sunDirECEF,
        const Vec3& cameraPos,
        const float* viewMatrix,        // 16 floats, column-major
        float fovRadians,
        int viewportWidth,
        int viewportHeight,
        bool isOrthographic = false,
        float frustumLeft = -1.0f,
        float frustumRight = 1.0f,
        float frustumTop = 1.0f,
        float frustumBottom = -1.0f) const;

    /// 是否已初始化
    bool isReady() const { return shader_ != nullptr && quadBuffer_ != nullptr; }

    /// 释放 GPU 资源
    void dispose();

private:
    AtmosphereParameters params_;
    ShaderProgram* shader_ = nullptr;
    Buffer* quadBuffer_ = nullptr;
    RenderDevice* device_ = nullptr;
};

} // namespace earth_engine
