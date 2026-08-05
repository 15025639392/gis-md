#pragma once

#include "core/math/Vec3.h"
#include "scene/Diagnostics.h"
#include "scene/FrameState.h"
#include "tiling/TileOcclusionCallback.h"
#include <memory>
#include <cstdint>
#include <vector>
#include <string>

namespace earth_engine {

class Camera;
class CameraController;
class OffscreenPostProcess;
class RenderDevice;
class Scene;
class VirtualTexturePoc;
class TileCompositeBakePoc;
class VtIndirectionSamplePoc;
class TerrainPageStore;
class TerrainPageDecorator;
class Renderer;
class RasterOverlayTileProvider;
class TerrainDisplacementTemplatePool;
class Tileset;
class FeatureRenderLayer;
class VectorLayer;
struct PresentationTrace;
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

    /// 渲染一帧。
    /// 返回 false 表示本帧只推进加载/状态、不呈现新 GPU 帧，应保留上一帧。
    /// @param deltaSeconds 上一帧到现在的秒数（0 = 自动计算）
    bool render(double deltaSeconds = 0.0);

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

    // ---- FeatureStore 渲染桥接层(矢量数据系统 P1) ----

    /// 添加 FeatureStore 渲染层(要素经 layer->store() 写入/编辑)
    void addFeatureRenderLayer(std::unique_ptr<FeatureRenderLayer> layer);

    /// 移除 FeatureStore 渲染层
    std::unique_ptr<FeatureRenderLayer> removeFeatureRenderLayer(
        const std::string& layerId);

    /// 矢量标注字体注入(P5b):应用层读字体文件供字节(引擎不碰文件系统)。
    /// TrueType/ttc 首字体;CFF/OTF 不支持返回 false。渲染线程调用。
    bool setLabelFontData(std::vector<uint8_t> fontData);

    /// 矢量图标位图注入(P6c):应用层解码好 RGBA8 像素供字节(引擎不碰
    /// 文件系统、不做图片解码)。rgba 长度须 = width*height*4;之后样式
    /// 里用 name 作 pointImage 即可画该图标。尺寸非法/图集满返回 false。
    /// 渲染线程调用。
    bool addIconImage(const std::string& name,
                      int width,
                      int height,
                      const std::vector<uint8_t>& rgba);

    /// cesium-native 对齐：设置统一 Tileset。
    void setTileset(std::unique_ptr<Tileset> tileset);
    /// 保持当前地表可交互渲染，直到替代 Tileset 达到接管门槛。
    void stageTilesetReplacement(std::unique_ptr<Tileset> tileset);
    /// 添加并列 3D Tiles / glTF 内容 Tileset；不参与地形采样。
    void addTileset(std::unique_ptr<Tileset> tileset);

    /// cesium-native 对齐：设置 selector 的视图/frustum 列表。
    /// 传入空列表表示本帧没有可选择视图；clear 后回到主相机视图。
    void setSelectorViewOverride(
        std::vector<SelectorView> selectorViews);
    void clearSelectorViewOverride();
    void setOcclusionCallback(TileOcclusionCallback callback);
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
    /// 一帧表现层契约 trace：camera -> selector -> TilePlan -> RenderCommand。
    const PresentationTrace& presentationTrace() const;

    // ---- 访问器 ----

    Camera& camera();
    CameraController& cameraController();
    bool isReady() const;

    /// 相机方位角（弧度，0 = 正北，顺时针为正）。用于指北针。
    double cameraHeadingRadians() const;
    /// 复位到正北朝上（保持俯仰与相机位置）。
    void resetNorthUp();

    /// 离屏后处理能力(passthrough/FXAA/aerial fog 共用):由后端声明
    /// (RenderDevice::supportsOffscreenPostProcess)。下面三个 setter 在
    /// 开启不受支持的效果时**拒绝并返回 false**(打 error 日志),不再
    /// 静默置 initFailed——调用方可据此感知配置未生效。
    bool offscreenPostProcessSupported() const;
    /// 离屏 passthrough(RTT 冒烟通路,默认关):场景画进离屏 FBO 再全屏
    /// blit 上屏,像素应与直绘一致。守住 createFramebuffer+beginPass 通路。
    /// @return 请求是否生效(见 offscreenPostProcessSupported)。
    bool setOffscreenPassthroughEnabled(bool enabled);
    /// FXAA 抗锯齿(默认关):场景画进离屏 FBO,全屏 FXAA 采样上屏消除锯齿。
    /// 与 passthrough 同走离屏后处理通路;两者都开时 FXAA 优先。
    /// @return 请求是否生效(见 offscreenPostProcessSupported)。
    bool setFxaaEnabled(bool enabled);
    /// Aerial fog 距离雾(默认关):场景经离屏 FBO,全屏采样深度重建视距,
    /// 远处地形指数雾混向天空色。同走离屏后处理通路。
    /// @return 请求是否生效(见 offscreenPostProcessSupported)。
    bool setAerialFogEnabled(bool enabled);
    /// Aerial fog 调参:密度(1/米,基础强度)、起雾距离(米)。雾色由
    /// shader 每像素从大气模型算(随视线/高度/太阳),不再是常数,故不在此设。
    /// near/far/相机基/太阳由引擎每帧取。
    void setAerialFogParams(float density, float startDistance);

    /// 北极星 Phase 2b 虚拟纹理 C 方案 PoC(默认关,测量台专用):每帧跑
    /// feedback→回读→页表整链,量移动端固定开销(回读 stall),数报进 EarthPerf。
    void setVirtualTexturePocEnabled(bool enabled);

    /// 北极星 Phase 2b B 方案(逐瓦片合成)PoC(默认关,测量台专用):每帧对当前
    /// 可见瓦片数做 N 个离屏 bake pass,量 B 的每帧烘焙开销,数报进 EarthPerf。
    void setTileCompositeBakePocEnabled(bool enabled);

    /// 北极星 Phase 2b 合成方案「门①」原型(默认关,测量台专用):一屏 fill 量
    /// 逐片元间接采样倍率(baseline vs descent),数报进 EarthPerf 头行。
    void setVtIndirectionSamplePocEnabled(bool enabled);

    /// 北极星 合成方案「门③ Step3」页存储原型(默认关):建一张 texture2DArray
    /// 页存储,挂到一个 capped 真实地形瓦片,terrain 片元按页表 layer 采样。
    /// Step3a = 合成图案(隔离渲染路径),Step3b 换真实高清影像。
    void setTerrainPageStoreEnabled(bool enabled);
    /// C-2c:页上传后的 GPU 叠画钩子(矢量走这条)。页存储可能因 surface 重建而
    /// 重新创建,故指针存在 Engine 上、每次建store时重新挂上。不持有。
    void setTerrainPageDecorator(TerrainPageDecorator* decorator);
    /// C-2c:渲染器(叠画方拿着色器用)。场景未就绪时为 nullptr。
    Renderer* renderer() const;

    /// 北极星 Phase 2c 地形 GPU 位移(默认关,flag-gated A/B):启用后地形瓦片改用
    /// 共享位移模板 VBO/IBO(同 {LOD,row} 复用,§5 有界)+ per-tile 刚体帧。Stage A
    /// 零起伏(贴椭球);起伏由后续高度纹理在 shader 位移。关闭走现 per-tile baked VBO。
    void setTerrainGpuDisplacementEnabled(bool enabled);
    /// 当前档位。环境快照要报它:这是地形几何路径的分岔开关,A/B 两侧不同就
    /// 不是同一个系统,耗时/观感差异无从比较。
    bool terrainGpuDisplacementEnabled() const {
        return terrainGpuDisplacementEnabled_;
    }

private:
    RenderDevice* device_;
    std::unique_ptr<Scene> scene_;
    std::unique_ptr<OffscreenPostProcess> offscreenPostProcess_;
    std::unique_ptr<VirtualTexturePoc> virtualTexturePoc_;
    std::unique_ptr<TileCompositeBakePoc> tileCompositeBakePoc_;
    std::unique_ptr<VtIndirectionSamplePoc> vtIndirectionSamplePoc_;
    std::unique_ptr<TerrainPageStore> terrainPageStore_;
    std::unique_ptr<TerrainDisplacementTemplatePool> terrainDisplacementPool_;
    double lastRenderTime_ = 0.0;
    bool surfaceCreated_ = false;
    // 离屏后处理开关。优先级:AerialFog > FXAA > passthrough 调试直通。
    bool offscreenPassthroughEnabled_ = false;
    bool fxaaEnabled_ = false;
    bool aerialFogEnabled_ = false;
    // Aerial fog 调参(SDK 可配);雾色由 shader 从大气模型算,不在此存。
    float aerialFogDensity_ = 8.0e-6f;
    float aerialFogStartDistance_ = 0.0f;
    // initialize 失败(如 Metal 未接线)后不再逐帧重试。
    bool offscreenPostProcessInitFailed_ = false;
    // 北极星 VT PoC 开关 + 构建失败短路(默认关,不影响生产路径)。
    bool virtualTexturePocEnabled_ = false;
    bool virtualTexturePocInitFailed_ = false;
    // 北极星 B 方案(逐瓦片合成)PoC 开关 + 短路。
    bool tileCompositeBakePocEnabled_ = false;
    bool tileCompositeBakePocInitFailed_ = false;
    // 北极星 合成方案 门① 原型开关 + 短路。
    bool vtIndirectionSamplePocEnabled_ = false;
    bool vtIndirectionSamplePocInitFailed_ = false;
    // 北极星 合成方案 门③ Step3 页存储原型开关 + 短路。
    bool terrainPageStoreEnabled_ = false;
    bool terrainPageStoreInitFailed_ = false;
    // C-1:每帧交给页存储的有序源列表(剔 null 后复用,免逐帧分配)。
    std::vector<RasterOverlayTileProvider*> pageProvidersScratch_;
    TerrainPageDecorator* terrainPageDecorator_ = nullptr;  // C-2c,不持有
    // 北极星 Phase 2c 地形 GPU 位移开关(P5 默认开;仍保留 flag 供运行时 A/B 关闭)。
    bool terrainGpuDisplacementEnabled_ = true;
    // 本帧场景 pass 是否画进了离屏目标(决定帧尾要不要后处理 pass)。
    bool offscreenPassActive_ = false;
    int surfaceWidthPixels_ = 0;
    int surfaceHeightPixels_ = 0;
};

} // namespace earth_engine
