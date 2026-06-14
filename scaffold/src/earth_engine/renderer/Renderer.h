#pragma once

#include "RenderDevice.h"
#include "RenderCommand.h"
#include <memory>
#include <array>

namespace earth_engine {

struct GlobeMesh;
struct FrameState;

/// 平台无关渲染器。
/// 管理共享 GPU 资源（shader、几何 buffer），供 Scene 和各 Layer 使用。
/// 不自行决定渲染什么——由 Scene 收集 RenderCommands 后统一提交。
class Renderer {
public:
    /// @param device 平台渲染设备（生命周期由调用者管理，必须长于 Renderer）
    explicit Renderer(RenderDevice* device);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// 初始化所有共享渲染资源（shader 编译、mesh 上传）。
    /// 必须在 RenderDevice::onSurfaceCreated() 之后调用。
    bool initialize(const GlobeMesh& globeMesh);

    /// 提交渲染命令列表
    void submit(const RenderCommandList& commands);

    /// 释放所有 GPU 资源
    void dispose();

    // ---- 共享资源访问 ----

    /// Globe shader（地球椭球背景）
    ShaderProgram* globeShader() const;

    /// Globe 顶点/索引 buffer
    Buffer* globeVertexBuffer() const;
    Buffer* globeIndexBuffer() const;
    int globeIndexCount() const;

    /// 简单颜色 shader（矢量图层线/面渲染）
    ShaderProgram* colorShader() const;

    /// Tile 共享索引 buffer（64×64 grid，所有 surface tile 共用）
    Buffer* tileIndexBuffer() const;
    int tileIndexCount() const;

    /// 地球模型矩阵（单位球 → ECEF meters）
    static std::array<float, 16> earthModelMatrix();

    /// 构建 globe 背景 RenderCommand
    RenderCommand makeGlobeCommand(const FrameState& frameState) const;

    /// Build surface tile command (unified, cesium-native glTF vertex layout).
    /// vertexStride=32: POSITION(12) + NORMAL(12) + TEXCOORD_0(8)
    RenderCommand makeSurfaceTileCommand(Texture* texture,
                                          Buffer* vertexBuffer,
                                          Buffer* indexBuffer = nullptr,
                                          int indexCount = 0) const;

    /// Compatibility: instanced basemap (to be removed)
    RenderCommand makeInstancedSurfaceTileCommand(Texture* texture,
                                                  Buffer* instanceBuffer,
                                                  int instanceCount) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace earth_engine
