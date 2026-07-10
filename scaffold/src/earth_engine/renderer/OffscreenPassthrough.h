#pragma once

#include "RenderCommand.h"
#include "RenderDevice.h"
#include <memory>

namespace earth_engine {

/// 离屏 passthrough(RTT 冒烟通路):场景整帧画进离屏 framebuffer,再全屏
/// blit 上屏。像素结果应与直绘一致——它的价值是点亮并守住
/// createFramebuffer + beginPass 离屏通路(后处理链的地基),默认关闭。
///
/// 使用方式(Engine 编排):
///   1. initialize(device) 建 blit shader + 全屏 quad
///   2. 每帧 ensureFramebuffer(w,h) 拿离屏目标(尺寸变化惰性重建)
///   3. beginPass(fbo) → 场景 → endPass → beginPass(nullptr) →
///      submit(buildBlitCommand()) → endPass
class OffscreenPassthrough {
public:
    OffscreenPassthrough() = default;
    ~OffscreenPassthrough() = default;

    OffscreenPassthrough(const OffscreenPassthrough&) = delete;
    OffscreenPassthrough& operator=(const OffscreenPassthrough&) = delete;

    /// 创建 GPU 资源。Metal 后端当前返回 false(blit shader 仅有 GLSL,
    /// MSL 接线待补——pass API 本身两后端就绪)。
    bool initialize(RenderDevice* device);

    /// 拿离屏 framebuffer,尺寸不符时销毁重建(maplibre 式惰性策略)。
    /// 返回 nullptr 表示创建失败(调用方回落直绘主 pass)。
    Framebuffer* ensureFramebuffer(int width, int height);

    /// 全屏 blit 命令:采样离屏 color 纹理画满默认目标。
    RenderCommand buildBlitCommand() const;

    bool isReady() const { return shader_ != nullptr && quadBuffer_ != nullptr; }

    /// 释放全部 GPU 资源(surface 销毁/context lost 时调用)。
    void dispose();

private:
    RenderDevice* device_ = nullptr;
    std::unique_ptr<ShaderProgram> shader_;
    std::unique_ptr<Buffer> quadBuffer_;
    std::unique_ptr<Framebuffer> framebuffer_;
};

} // namespace earth_engine
