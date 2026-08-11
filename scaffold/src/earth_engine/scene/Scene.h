#pragma once

#include "EngineTimingScope.h"
#include "FrameState.h"
#include "SceneFrameRuntime.h"
#include "../tiling/TileOcclusionCallback.h"
#include <memory>
#include <vector>
#include <string>

namespace earth_engine {

class Camera;
class CameraSystem;
struct Diagnostics;
struct InputEvent;
struct PickResult;
struct PresentationTrace;
class Framebuffer;
class RenderDevice;
class Renderer;
class SceneEnvironmentCoordinator;
class SceneLayerCoordinator;
struct SceneInteractionContext;
class SceneInteractionCoordinator;
class SceneRenderPipeline;
class SceneTelemetryCoordinator;
class SceneTilesetCoordinator;
class SkyGradient;
class TerrainPageStore;
class TerrainDisplacementTemplatePool;
class Tileset;
class FeatureRenderLayer;
class VectorLayer;
class Vec3;

/// 3D 场景管理器。
class Scene {
public:
    using EngineTimingScope = earth_engine::EngineTimingScope;

    Scene();
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    bool setRenderDevice(RenderDevice* device);
    bool isReady() const { return renderer_ != nullptr; }
    /// C-2c:页存储叠画方要用它拿着色器(渲染线程,场景就绪后非空)。
    Renderer* renderer() const { return renderer_.get(); }

    /// 门③ Step3:把 Engine 拥有的页存储挂到内部 Renderer(可空=未启用)。
    void setTerrainPageStore(TerrainPageStore* store);

    /// 北极星 Phase 2c:把 Engine 拥有的地形位移模板池挂到内部 Renderer(可空)。
    void setTerrainDisplacementPool(TerrainDisplacementTemplatePool* pool);

    Camera& camera() { return *camera_; }
    CameraSystem& cameraSystem() { return *cameraSystem_; }

    void setViewport(int widthPixels, int heightPixels, float dpr = 1.0f);
    void update(double deltaSeconds);
    bool render();

    /// T2:Engine 在 beginPass 之后、render() 之前推入场景 pass 的目标与像素
    /// 尺寸。地形深度 prepass 会临时切走 pass,靠这个切回来。
    void setSceneRenderTarget(Framebuffer* target, int widthPixels,
                              int heightPixels) {
        sceneRenderTarget_ = target;
        sceneSurfaceWidthPixels_ = widthPixels;
        sceneSurfaceHeightPixels_ = heightPixels;
    }
    // 非 const:内部要累计"已连续扣住多少帧"以保证活性(见 .cpp 实现注释)。
    bool shouldHoldPresentationFrame();

    /// 场景是否仍有"会让下一帧长得不一样"的在途工作。帧级按需渲染的**收敛型**
    /// 判据(事件型脏位在 Engine 侧)。
    ///
    /// ⚠️ 只读**权威**状态,绝不读 Diagnostics —— 那套聚合每 15 帧才刷新一次
    /// (见 SceneRenderPipeline::aggregateDiagnostics),且注释写明"纯诊断消费、
    /// 无功能逻辑读取"。拿一个最多陈旧 15 帧的 pending 计数去判停帧,失效方向
    /// 恰好是最坏的那个:它会在真的还有在途时报 0 → 停帧 → 到货没人消费 →
    /// 画面永久停在半成品且无任何报错。
    ///
    /// outReason 非空时写入首个命中的原因(机制信号用,勿据此做逻辑分支)。
    bool hasConvergingWork(const char** outReason) const;

    /// 并行验证期的对拍(见实现注释)。零行为影响,只在不一致时打 ERROR。
    void auditWorkLedger() const;

    /// CPU 常驻账量测:每 300 帧把主 Tileset 的分类目字节打一行 Info
    /// (tag=CpuAcct)。回答"heightmap/幽灵网格常驻推算是否成立"。
    void logCpuResidentAccount();
    void setSelectorViewOverride(
        std::vector<SelectorView> selectorViews);
    void clearSelectorViewOverride();
    void setOcclusionCallback(TileOcclusionCallback callback);
    void clearOcclusionCallback();

    const FrameState& frameState() const { return frameRuntime_.frameState(); }

    /// 运行时诊断（FPS、draw calls、visible tiles 等）
    const Diagnostics& diagnostics() const;
    void recordEngineTiming(EngineTimingScope scope, double elapsedMs);
    void finishEngineFrame(double elapsedMs);
    const PresentationTrace& presentationTrace() const;

    // ---- 矢量图层管理 ----
    void addVectorLayer(std::unique_ptr<VectorLayer> layer);
    std::unique_ptr<VectorLayer> removeVectorLayer(const std::string& layerId);
    size_t vectorLayerCount() const;

    // ---- FeatureStore 渲染桥接层(矢量数据系统 P1) ----
    void addFeatureRenderLayer(std::unique_ptr<FeatureRenderLayer> layer);
    std::unique_ptr<FeatureRenderLayer> removeFeatureRenderLayer(
        const std::string& layerId);

    /// 矢量标注字体注入(P5b;TrueType 字节,渲染线程调用)。
    bool setLabelFontData(std::vector<uint8_t> fontData);

    /// 矢量图标位图注入(P6c;RGBA8 紧排字节,渲染线程调用)。
    bool addIconImage(const std::string& name,
                      int width,
                      int height,
                      const std::vector<uint8_t>& rgba);

    // ---- 统一 Tileset（cesium-native 对齐） ----
    void setTileset(std::unique_ptr<Tileset> tileset);
    void stageTilesetReplacement(std::unique_ptr<Tileset> tileset);
    void addTileset(std::unique_ptr<Tileset> tileset);
    Tileset* tileset() const;
    size_t additionalTilesetCount() const;
    bool hasTerrain() const;

    // ---- 输入事件（归一化） ----
    void onInputEvent(const InputEvent& event);

    // ---- 拾取与选择 ----
    PickResult pick(float screenX, float screenY) const;
    void onHover(const PickResult& result);
    void onSelect(const PickResult& result);
    void clearSelection();

    // ---- 环境系统 ----

    void setTime(double julianDate);
    double time() const;
    void advanceTime(double seconds);
    Vec3 sunDirection() const;
    const SkyGradient& skyGradient() const;

private:
    void configureCameraSurfacePicker();
    SceneInteractionContext interactionContext() const;
    void updatePresentationTrace();

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraSystem> cameraSystem_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<SceneRenderPipeline> renderPipeline_;
    SceneFrameRuntime frameRuntime_;
    RenderDevice* renderDevice_ = nullptr;
    // T2:场景 pass 的目标(离屏 FBO;直绘为 nullptr)与其像素尺寸,由
    // Engine 在 beginPass 后、render() 前推入。地形深度 prepass 跑完要靠
    // 它把场景 pass 重新 begin 回来。
    Framebuffer* sceneRenderTarget_ = nullptr;
    int sceneSurfaceWidthPixels_ = 0;
    int sceneSurfaceHeightPixels_ = 0;

    // CPU 常驻账量测的限频计数(见 logCpuResidentAccount)
    uint64_t cpuAcctFrameCounter_ = 0;

    // 矢量图层
    std::unique_ptr<SceneLayerCoordinator> layers_;

    // 统一 Tileset（cesium-native 对齐）
    std::unique_ptr<SceneTilesetCoordinator> tilesets_;

    // 交互
    std::unique_ptr<SceneInteractionCoordinator> interaction_;

    // 环境系统
    std::unique_ptr<SceneEnvironmentCoordinator> environment_;

    // 诊断与 presentation trace
    std::unique_ptr<SceneTelemetryCoordinator> telemetry_;

    // presentation hold 的活性兜底计数(见 shouldHoldPresentationFrame)。
    int consecutiveHeldFrames_ = 0;
};

} // namespace earth_engine
