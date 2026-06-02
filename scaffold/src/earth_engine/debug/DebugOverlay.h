#pragma once

#include "../renderer/RenderCommand.h"
#include "../tiling/TileKey.h"
#include <vector>

namespace earth_engine {

class RenderDevice;
class Renderer;
class TileScheme;

/// 调试叠加层。
/// 为每个可见瓦片绘制彩色边框，表示瓦片状态（已加载/加载中/缺失）。
/// 可在运行时开关。
class DebugOverlay {
public:
    DebugOverlay();
    ~DebugOverlay();

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    /// 初始化 GPU 资源（shader + border geometry）
    bool initialize(RenderDevice* device);

    /// 生成当前帧的调试渲染命令
    /// @param tileKeys 可见瓦片列表
    /// @param tileScheme 瓦片体系（用于计算 tile bounds）
    /// @param commands 输出命令列表
    void buildCommands(const std::vector<TileKey>& tileKeys,
                       const TileScheme& tileScheme,
                       RenderCommandList& commands);

    /// 是否启用
    bool enabled() const { return enabled_; }
    void setEnabled(bool e) { enabled_ = e; }

    /// 释放 GPU 资源
    void dispose();

private:
    bool enabled_ = true;
    RenderDevice* device_ = nullptr;
    std::unique_ptr<ShaderProgram> lineShader_;
    std::unique_ptr<Buffer> borderVertexBuffer_;
};

} // namespace earth_engine
