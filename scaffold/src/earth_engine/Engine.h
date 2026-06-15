#pragma once

#include "core/math/Vec3.h"
#include "scene/FrameState.h"
#include "tiling/Tileset.h"
#include <memory>
#include <cstdint>
#include <vector>
#include <string>

namespace earth_engine {

class Camera;
class RenderDevice;
class Scene;
class VectorLayer;
struct InputEvent;
struct PickResult;

/// 地球引擎顶层 API。
/// 管理渲染生命周期和输入事件路由。
///
/// 使用方式：
///   1. 平台代码创建 RenderDevice（Metal / GLES）
///   2. 创建 Engine 并传入 RenderDevice
///   3. 调用 onSurfaceCreated() → onSurfaceChanged()
///   4. 每帧调用 render()
///   5. 输入事件通过 onInputEvent(InputEvent) 转发（或向后兼容的 onDrag*）
class Engine {
public:
    /// @param device 平台渲染设备（Engine 不拥有所有权）
    explicit Engine(RenderDevice* device);
    ~Engine();

    // 禁止拷贝/移动
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- 生命周期 ----

    /// Surface 首次创建或 context lost 后重建
    void onSurfaceCreated();

    /// Surface 尺寸变化
    void onSurfaceChanged(int widthPixels, int heightPixels, float dpr = 1.0f);

    /// Surface 销毁前调用
    void onSurfaceDestroyed();

    // ---- 渲染 ----

    /// 渲染一帧
    /// @param deltaSeconds 上一帧到现在的秒数（0 = 自动计算）
    void render(double deltaSeconds = 0.0);

    // ---- 输入事件 ----

    /// 归一化输入事件（推荐使用，携带时间戳和修饰键）
    void onInputEvent(const InputEvent& event);

    /// 原始输入方法（内部转为 InputEvent 调用 onInputEvent）
    void onDragStart(float xPixels, float yPixels);
    void onDragMove(float xPixels, float yPixels);
    void onDragEnd();

    // ---- 矢量图层 ----

    /// 添加矢量图层
    void addVectorLayer(std::unique_ptr<VectorLayer> layer);

    /// 移除矢量图层
    std::unique_ptr<VectorLayer> removeVectorLayer(const std::string& layerId);

    /// 矢量图层数量
    size_t vectorLayerCount() const;

    /// cesium-native 对齐：设置统一 Tileset。
    void setTileset(std::unique_ptr<Tileset> tileset);
    /// 添加并列 3D Tiles / glTF 内容 Tileset；不参与地形采样。
    void addTileset(std::unique_ptr<Tileset> tileset);

    /// cesium-native 对齐：设置 selector 的视图/frustum 列表。
    /// 传入空列表表示本帧没有可选择视图；clear 后回到主相机视图。
    void setSelectorViewOverride(
        std::vector<FrameState::SelectorView> selectorViews);
    void clearSelectorViewOverride();
    void setOcclusionCallback(Tileset::OcclusionCallback callback);
    void clearOcclusionCallback();

    /// 是否有地形数据
    bool hasTerrain() const;

    // ---- 拾取与选择 ----

    /// 拾取屏幕坐标下的对象
    PickResult pick(float screenX, float screenY) const;

    /// 处理悬停
    void onHover(const PickResult& result);

    /// 处理选择
    void onSelect(const PickResult& result);

    /// 清除选择
    void clearSelection();

    // ---- 环境系统 ----

    /// 设置模拟时间（Julian Date）
    void setTime(double julianDate);
    /// 获取当前模拟时间
    double time() const;
    /// 时间步进（秒）
    void advanceTime(double seconds);
    /// 当前 ECEF 太阳方向
    Vec3 sunDirection() const;
    /// 天空 clear color（RGBA，环境系统计算）
    void getClearColor(float& r, float& g, float& b, float& a) const;

    /// 运行时诊断（FPS、draw calls、visible tiles 等）
    const Diagnostics& diagnostics() const;

    // ---- 访问器 ----

    Camera& camera();
    bool isReady() const;

private:
    RenderDevice* device_;
    std::unique_ptr<Scene> scene_;
    double lastRenderTime_ = 0.0;
    bool surfaceCreated_ = false;
};

} // namespace earth_engine
