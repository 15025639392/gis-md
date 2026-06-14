#pragma once

#include "core/math/Vec3.h"
#include "layers/BasemapLayerStack.h"
#include "scene/FrameState.h"
#include <memory>
#include <cstdint>
#include <vector>
#include <string>

namespace earth_engine {

class Camera;
class RenderDevice;
class Scene;
class BasemapLayer;
class VectorLayer;
class Tileset;
class TerrainLayer;
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

    // ---- 图层 ----

    /// 添加底图图层
    void addLayer(std::unique_ptr<BasemapLayer> layer);

    /// 移除图层（返回所有权）
    std::unique_ptr<BasemapLayer> removeLayer(const std::string& layerId);

    /// 移动图层位置
    void moveLayer(const std::string& layerId, size_t index);

    /// 图层数量
    size_t layerCount() const;
    int visibleTileCount() const;
    int cachedTileCount() const;

    /// 控制点验证（跨图层/跨 scheme）
    std::vector<BasemapLayerStack::ControlPointResult>
    verifyControlPoint(double lngRad, double latRad, int zoom = 10) const;

    // ---- 矢量图层 ----

    /// 添加矢量图层
    void addVectorLayer(std::unique_ptr<VectorLayer> layer);

    /// 移除矢量图层
    std::unique_ptr<VectorLayer> removeVectorLayer(const std::string& layerId);

    /// 矢量图层数量
    size_t vectorLayerCount() const;

    // ---- 地形 ----

    /// 设置地形图层
    void setTerrainLayer(std::unique_ptr<TerrainLayer> layer);

    /// cesium-native 对齐：设置统一 Tileset（替代 BasemapLayer + TerrainLayer）
    void setTileset(std::unique_ptr<Tileset> tileset);

    /// 启用/禁用地形
    void setTerrainEnabled(bool enabled);

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

    // ---- 调试 ----

    /// 开关调试叠加层（瓦片边框）
    void setDebugOverlayEnabled(bool enabled);
    bool debugOverlayEnabled() const;

    /// 开关法线贴图调试渲染（作用于所有底图图层）
    void setNormalMapDebugEnabled(bool enabled);

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
