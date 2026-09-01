#pragma once

#include "core/math/Vec3.h"
#include "core/resources/SceneFrameResourceArbiter.h"
#include "data/MvtVectorSource.h"
#include "style/AmapClassicRuntime.h"
#include "scene/Diagnostics.h"
#include "scene/FrameState.h"
#include "renderer/GlyphAtlas.h"
#include "threading/CancellationToken.h"
#include "tiling/TileKey.h"
#include "tiling/TileOcclusionCallback.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <cstdint>
#include <optional>
#include <vector>
#include <string>

namespace earth_engine {

class Camera;
class CameraSystem;
class OffscreenPostProcess;
class RenderDevice;
class Scene;
class VirtualTexturePoc;
class TileCompositeBakePoc;
class VtIndirectionSamplePoc;
class TerrainPageStore;
class Renderer;
class RasterOverlayTileProvider;
class TerrainDisplacementTemplatePool;
class Tileset;
class FeatureRenderLayer;
class VectorLayer;
class PlatformBridge;
class ThreadPool;
class EarthEngineSdkFacade;
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

    /// Add/remove a Scene-owned MVT source and its sink-bound render layer.
    bool addMvtVectorSource(std::unique_ptr<MvtVectorSource> source,
                            std::unique_ptr<FeatureRenderLayer> layer);
    bool removeMvtVectorSource(const std::string& layerId);
    size_t mvtVectorSourceCount() const;

    const AmapClassicRuntime* installAmapClassicRuntime(
        PlatformBridge& platformBridge,
        std::shared_ptr<ThreadPool> type1DecodePool,
        std::shared_ptr<ThreadPool> poiDecodePool,
        std::shared_ptr<ThreadPool> tessellationPool,
    AmapClassicRuntime::Options options);
    bool hasAmapClassicRuntime() const;

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
    bool hasIconImage(const std::string& name) const;

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
    void setSunsetTerrainTint(float warmth, float shadowScale);
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
    SceneFrameResourceArbiterSnapshot frameResourceArbiterSnapshot() const;
    /// 一帧表现层契约 trace：camera -> selector -> TilePlan -> RenderCommand。
    const PresentationTrace& presentationTrace() const;

    // ---- 访问器 ----

    Camera& camera();
    CameraSystem& cameraSystem();
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
    bool setVirtualTexturePocEnabled(bool enabled);

    /// 北极星 Phase 2b B 方案(逐瓦片合成)PoC(默认关,测量台专用):每帧对当前
    /// 可见瓦片数做 N 个离屏 bake pass,量 B 的每帧烘焙开销,数报进 EarthPerf。
    bool setTileCompositeBakePocEnabled(bool enabled);

    /// 北极星 Phase 2b 合成方案「门①」原型(默认关,测量台专用):一屏 fill 量
    /// 逐片元间接采样倍率(baseline vs descent),数报进 EarthPerf 头行。
    bool setVtIndirectionSamplePocEnabled(bool enabled);

    /// 北极星 合成方案「门③ Step3」页存储原型(默认关):建一张 texture2DArray
    /// 页存储,挂到一个 capped 真实地形瓦片,terrain 片元按页表 layer 采样。
    /// Step3a = 合成图案(隔离渲染路径),Step3b 换真实高清影像。
    void setTerrainPageStoreEnabled(bool enabled);

    /// GPU 逐区间计时(测量台,默认关):把一帧的 GPU 时间线切成 pass / 命令桶
    /// 若干段,每秒一行 GpuPass 打进 logcat。回答的是"86% GPU busy 花在哪"这类
    /// 问题 —— CPU 侧 EarthPerf 头行对此完全盲目(它只量提交命令的 CPU 成本)。
    /// 返回**实际**开启状态:GLES 无 GL_EXT_disjoint_timer_query 时返回 false。
    /// 读数的三条边界(TBDR 段边界不精确 / MSAA resolve 不在段内 / disjoint 帧
    /// 作废)见 renderer/GpuFrameTiming.h,下结论前必须读。
    bool setGpuPassTimingEnabled(bool enabled);
    bool gpuPassTimingEnabled() const { return gpuPassTimingEnabled_; }

    // ---- 帧级按需渲染(默认关) ----
    //
    // 对齐 maplibre 的三 flag 结构(map.ts `_update` / `triggerRepaint` /
    // `_render` 帧尾续帧判定):**事件型**脏位 + **收敛型**在途判据,帧尾合并
    // 决定要不要排下一帧。两类性质不同,不能合并成一个 bool:
    //   事件型 = 外部输入/API 变更,漏一个 → 该事件不刷新(症状局部、可复现)
    //   收敛型 = 子系统自报"我还没收敛",自己会置位,漏不掉
    // maplibre 之所以敢做这件事,是因为事件型只有一个入口(`_update`),枚举
    // 脏源被压缩成"审一个函数的调用点"。我们照抄这条:事件型只认 requestRender。
    //
    // ⚠️ 停帧的失效方向是最坏的那种:不是画错,是**画面冻住且零报错**。所以
    // 判据一律取保守侧(拿不准 → 继续画),并配机制信号 FrameGate 记录每次
    // 进入/退出空闲的原因。

    /// 开关。关闭时 needsFrame() 恒 true(与接线前逐 vsync 全量重画等价)。
    /// 影子渲染自检(方案 C):gating 判定 idle 后**不立刻睡**,继续渲
    /// kShadowVerifyFrames 帧并逐帧比对帧指纹。指纹在"应该静止"之后还变
    /// = 有异步产物落地却没人置脏位 —— 那正是"画面冻住且零报错"的反面:
    /// 同一个漏洞,一个表现为该更新的不更新,一个表现为不该变的还在变。
    ///
    /// ⚠️ **dev 专用,默认关,严禁在性能测量时开启**:自检帧本身要跑同步
    ///    回读(TBDR 上是管线 flush),会污染同会话的所有帧时读数。
    /// ⚠️ 前提是画面**真的**该静止:时钟必须冻住(demo 用
    ///    kFixedSimulationJulianDate),抖动/jitter 类效果必须关。否则合法的
    ///    逐帧变化会让它一直报警 —— 而"一直报警"比没有守卫更糟,人会学会无视。
    void setShadowVerifyEnabled(bool enabled) { shadowVerifyEnabled_ = enabled; }

    /// 黑块探针(漏底/黑块诊断):swap 前逐帧回读降采样帧,近黑占比超阈值逐帧
    /// 告警,300 帧心跳报活。含同步回读(~1-2ms/帧),仅诊断会话开启。
    void setBlackFrameProbeEnabled(bool enabled) {
        blackFrameProbeEnabled_ = enabled;
    }
    /// 上一帧的近黑像素占比(探针关闭时为 -1)。
    ///
    /// 为什么要这个 getter:排查破洞时「洞在不在」来自截图、「计数」来自 logcat,
    /// 两者时刻对不上 —— 靠截图去猜配对已经导致两次自相矛盾的"证据"。把占比
    /// 挂到同一帧的诊断行上,相关性才是硬的。
    double lastFrameDarkFraction() const { return lastFrameDarkFraction_; }
    bool shadowVerifyEnabled() const { return shadowVerifyEnabled_; }

    void setFrameGatingEnabled(bool enabled);
    bool frameGatingEnabled() const { return frameGatingEnabled_; }

    /// 事件型脏位。**可从任意线程调用**(异步产物落地、输入事件、SDK 变更)。
    /// reason 只进日志,不参与逻辑。
    void requestRender(const char* reason);

    /// Phase B 平台级唤醒钩子(§0)。宿主注入一个"把睡着的渲染循环踹醒"的回调
    /// (Android:ALooper_wake;iOS/macOS:CADisplayLink 触发一帧)。**从任意
    /// 线程调用安全** —— 每次 WorkLedger Landing 令牌释放时触发。
    ///
    /// 这是 WorkLedger 接管 gating(kEnableWorkLedgerGating)的**前提**:
    /// ledger 模式下 Landing 挂着时循环会真睡,没有它落地后无人唤醒=永久冻屏。
    /// 因此 gating 采用**失败安全**:仅当本回调已注入时才走 ledger 判据,否则
    /// 回落旧的 hasConvergingWork(见 needsFrame)。传 nullptr 清除。
    void setFrameRequestCallback(std::function<void()> cb);

    /// 帧尾判定:还需要排下一帧吗?宿主循环据此决定是否投递下一次 vsync 回调。
    /// 未开启 gating 时恒 true。
    bool needsFrame();
    /// C-2c:页上传后的 GPU 叠画钩子(矢量走这条)。页存储可能因 surface 重建而
    /// 重新创建,故指针存在 Engine 上、每次建store时重新挂上。不持有。
    /// C-2c:渲染器(叠画方拿着色器用)。场景未就绪时为 nullptr。
    /// Read-only renderer diagnostics. Resource mutation must go through the
    /// guarded Engine/Scene APIs so a caller cannot replace sealed official
    /// fonts or icon frames through an atlas back door.
    const Renderer* renderer() const;

    /// 北极星 Phase 2c 地形 GPU 位移(默认关,flag-gated A/B):启用后地形瓦片改用
    /// 共享位移模板 VBO/IBO(同 {LOD,row} 复用,§5 有界)+ per-tile 刚体帧。Stage A
    /// 零起伏(贴椭球);起伏由后续高度纹理在 shader 位移。关闭走现 per-tile baked VBO。
    void setTerrainGpuDisplacementEnabled(bool enabled);
    /// 当前档位。环境快照要报它:这是地形几何路径的分岔开关,A/B 两侧不同就
    /// 不是同一个系统,耗时/观感差异无从比较。
    bool terrainGpuDisplacementEnabled() const {
        return terrainGpuDisplacementEnabled_;
    }

    // B:GPU 高度烘焙开关(存储值 + 转发给 pool;out-of-line,pool 类型在 .cpp 完整)。
    void setGpuHeightBakeEnabled(bool enabled);
    bool gpuHeightBakeEnabled() const { return gpuHeightBakeEnabled_; }

private:
    struct FrameWakeGate {
        std::mutex mutex;
        Engine* engine = nullptr;
        std::function<void()> hostCallback;
    };
    friend class AmapClassicAssets;
    friend class AmapClassicRuntime;
    friend class AmapClassicSourceBundle;
    friend class EarthEngineSdkFacade;
    bool addOfficialFeatureRenderLayer(
        std::unique_ptr<FeatureRenderLayer> layer);
    std::unique_ptr<FeatureRenderLayer> removeOfficialFeatureRenderLayer(
        const std::string& layerId);
    void activateAmapClassicOfficialGlyphProvider(
        std::function<void(uint32_t)> demand);
    bool installAmapClassicOfficialGlyphBatch(
        int imageWidth, int imageHeight,
        const std::vector<uint8_t>& grayscale,
        const std::vector<GlyphAtlas::ProviderGlyph>& glyphs);
    bool addOfficialIconImage(const std::string& name, int width, int height,
                              const std::vector<uint8_t>& rgba);
    void clearAmapClassicOfficialAssets();
    bool installAmapClassicTerrainTileset(std::unique_ptr<Tileset> tileset);
    /// Internal teardown bridge used by the SDK facade before releasing the
    /// ActivatedRasterOverlay objects borrowed by the official Tileset.
    void clearAmapClassicTerrainTileset();
    /// Internal SDK bridge: the official runtime remains Scene-owned, but the
    /// facade may borrow its const source bundle while constructing the one
    /// sealed AMap surface-mask overlay. The returned pointer is non-owning
    /// and valid only while hasAmapClassicRuntime() is true.
    AmapClassicRuntime* amapClassicRuntimeForSdk();
    /// Phase B(WorkLedger 接管 gating)的活性判据。仅当 kEnableWorkLedgerGating
    /// 翻转为 true 时被 needsFrame 调用;默认关,当前为死代码骨架(见其定义处注释)。
    bool ledgerGatingNeedsFrame(const char** reason);

    /// 层1 静态省电:时间驱动重画的感知门控。仅当太阳自上次真渲染那帧起移动
    /// 超过感知阈值(~0.1°)才 requestRender("timeChanged");否则不请求 → 静态
    /// 无其它工作时循环真睡(idle 太阳冻结,交互/跳时因 dt 累积越阈即刻追上)。
    /// 替代 advanceTime/setTime 里的无条件 requestRender(那会让活时钟把设备
    /// 永久钉在 60fps,见 docs 发热债)。setTime 跳变=大角差→照常渲染。
    void requestRenderIfSunMoved();
    bool ensureTerrainPageStore();

    /// Phase B 平台级唤醒回调(见 setFrameRequestCallback)。非空 = 宿主已接
    /// 唤醒,ledger gating 方可安全启用。
    std::function<void()> frameRequestCallback_;
    std::shared_ptr<FrameWakeGate> frameWakeGate_ =
        std::make_shared<FrameWakeGate>();

    RenderDevice* device_;
    std::unique_ptr<Scene> scene_;
    bool genericLabelFontInstalled_ = false;
    bool genericIconInstalled_ = false;
    std::unique_ptr<OffscreenPostProcess> offscreenPostProcess_;
    std::unique_ptr<VirtualTexturePoc> virtualTexturePoc_;
    std::unique_ptr<TileCompositeBakePoc> tileCompositeBakePoc_;
    std::unique_ptr<VtIndirectionSamplePoc> vtIndirectionSamplePoc_;
    std::unique_ptr<TerrainPageStore> terrainPageStore_;
    std::unique_ptr<TerrainDisplacementTemplatePool> terrainDisplacementPool_;
    double lastRenderTime_ = 0.0;
    // 层1 时间驱动重画门控的基准:上次真渲染那帧所用的(归一)太阳方向。
    // 见 requestRenderIfSunMoved / Engine::render 末尾快照。
    Vec3 lastRenderedSunDir_{0.0, 0.0, 0.0};
    bool haveRenderedSunDir_ = false;
    bool surfaceCreated_ = false;
    // 离屏后处理开关。优先级:AerialFog > FXAA > passthrough 调试直通。
    bool offscreenPassthroughEnabled_ = false;
    bool fxaaEnabled_ = false;
    bool aerialFogEnabled_ = false;
    // Aerial fog 调参(SDK 可配);雾色由 shader 从大气模型算,不在此存。
    float aerialFogDensity_ = 3.0e-5f;
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
    // GPU 逐区间计时(测量台)。lastLoggedGpuFrameId_ 防同一帧结果重复打行
    // (回读滞后数帧,同一份结果会被连续几帧看到)。
    void logGpuPassTiming();
    bool gpuPassTimingEnabled_ = false;
    uint64_t lastLoggedGpuFrameId_ = 0;
    uint64_t gpuPassResultCount_ = 0;

    // 帧级按需渲染。renderRequested_ 是事件型脏位(任意线程置位,渲染线程消费);
    // settleFrames_ 是停帧前的余量帧 —— 子系统"这一帧报干净"与"画面已经稳定"
    // 之间常差一两帧(上传在本帧末落地、下一帧才画出来),没有余量会停在倒数
    // 第二帧上,表现为"最后一块瓦片永远不出现"。
    std::atomic<bool> renderRequested_{true};
    std::atomic<const char*> renderRequestReason_{"init"};
    bool frameGatingEnabled_ = false;
    /// 上一帧是否真的呈现了(false = 被 presentation hold 扣住)。
    bool lastFramePresented_ = true;
    int settleFrames_ = 0;
    // 黑块探针状态(见 setBlackFrameProbeEnabled)。
    bool blackFrameProbeEnabled_ = false;
    double lastFrameDarkFraction_ = -1.0;
    std::vector<uint8_t> blackProbeScratch_;
    uint64_t blackProbeFrames_ = 0;
    uint64_t blackProbeHits_ = 0;
    double blackProbeWorstFrac_ = 0.0;
    // 影子渲染自检状态(见 setShadowVerifyEnabled)。
    bool shadowVerifyEnabled_ = false;
    int shadowVerifyFramesLeft_ = 0;
    bool shadowVerifyDoneThisIdle_ = false;
    std::vector<uint8_t> shadowVerifyBaseline_;
    std::vector<uint8_t> shadowVerifyScratch_;
    int shadowVerifyMismatches_ = 0;
    int shadowVerifyWorstPixels_ = 0;
    int shadowVerifyWorstDelta_ = 0;
    bool wasIdle_ = false;
    uint64_t framesAwake_ = 0;
    uint64_t framesSinceIdleLog_ = 0;

    // 北极星 合成方案 门③ Step3 页存储原型开关 + 短路。
    bool terrainPageStoreEnabled_ = false;
    bool terrainPageStoreInitFailed_ = false;
    // C-1:每帧交给页存储的有序源列表(剔 null 后复用,免逐帧分配)。
    // 北极星 Phase 2c 地形 GPU 位移开关(P5 默认开;仍保留 flag 供运行时 A/B 关闭)。
    bool terrainGpuDisplacementEnabled_ = true;
    // B:GPU 高度烘焙开关(默认关,CPU 烘焙路径;真机 A/B 用)。
    bool gpuHeightBakeEnabled_ = false;
    // 本帧场景 pass 是否画进了离屏目标(决定帧尾要不要后处理 pass)。
    bool offscreenPassActive_ = false;
    int surfaceWidthPixels_ = 0;
    int surfaceHeightPixels_ = 0;
};

} // namespace earth_engine
