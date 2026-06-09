#pragma once

#include "../core/math/Vec3.h"
#include "../renderer/RenderCommand.h"
#include "../renderer/RenderDevice.h"
#include <memory>
#include <array>
#include <string>

namespace earth_engine {

/// 天空盒 — 对标 openglobus/src/scene/SkyBox.ts + src/shaders/skybox.ts。
///
/// 两种模式：
///   1. Cubemap — 六面立方体贴图（白天天空）
///   2. Procedural starfield — 程序化星空（夜间）
///
/// 使用 gl_Position.xyww 深度技巧保证天空盒渲染在最远。
///
/// 使用方式：
///   1. initialize(device) 创建 cubemap 纹理 + shader + 顶点 buffer
///   2. 每帧调用 buildCommand() 生成 RenderCommand
class SkyBox {
public:
    /// Cubemap 六面纹理路径（可选，nullptr 表示使用 procedural 模式）
    struct CubemapPaths {
        const char* positiveX = nullptr;  // right
        const char* negativeX = nullptr;  // left
        const char* positiveY = nullptr;  // top
        const char* negativeY = nullptr;  // bottom
        const char* positiveZ = nullptr;  // front
        const char* negativeZ = nullptr;  // back
    };

    SkyBox();
    ~SkyBox();

    SkyBox(const SkyBox&) = delete;
    SkyBox& operator=(const SkyBox&) = delete;

    /// 使用 cubemap 纹理路径初始化
    void setCubemapPaths(const CubemapPaths& paths);

    /// 设置天空盒尺寸（世界单位），默认 10000m
    void setSize(float size) { size_ = size; }

    /// 初始化 GPU 资源
    bool initialize(RenderDevice* device);

    /// 构建渲染命令
    /// @param viewMatrix 相机视图矩阵（16 floats, column-major, 仅旋转部分）
    /// @param projMatrix 相机投影矩阵（16 floats, column-major）
    /// @param isOrthographic 是否正交投影
    RenderCommand buildCommand(
        const float* viewMatrix,       // 16 floats
        const float* projMatrix,       // 16 floats
        bool isOrthographic = false) const;

    /// 是否已初始化
    bool isReady() const { return shader_ != nullptr && vertexBuffer_ != nullptr; }

    /// 释放 GPU 资源
    void dispose();

private:
    CubemapPaths cubemapPaths_;
    float size_ = 10000.0f;
    bool useCubemap_ = false;

    ShaderProgram* shader_ = nullptr;
    Buffer* vertexBuffer_ = nullptr;
    Texture* cubemapTexture_ = nullptr;
    int vertexCount_ = 0;
};

} // namespace earth_engine
