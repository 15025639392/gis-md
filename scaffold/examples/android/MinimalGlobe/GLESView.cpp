#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include <sched.h>
#include <sys/system_properties.h>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <iterator>
#include <optional>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/renderer/GlyphAtlas.h"
#include "earth_engine/Engine.h"
#include "earth_engine/camera/CameraSystem.h"
#include "earth_engine/camera/CameraPose.h"
#include "earth_engine/camera/Viewpoint.h"
#include "earth_engine/camera/controllers/TetheredController.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/core/async/AsyncSystem.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/platform/bridge/CurlMultiRequestScheduler.h"
#include "earth_engine/data/AmapVectorSource.h"
#include <nlohmann/json.hpp>
#include "earth_engine/style/AmapClassicRuntime.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/platform/android/RenderDeviceGLES.h"
#include "earth_engine/platform/android/AndroidPlatformBridge.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/sdk/EarthSceneConfig.h"
#include "earth_engine/sdk/EarthEngineSdkFacade.h"
#include "earth_engine/threading/RenderThreadPlacement.h"

#include "MinimalGlobeDiagnostics.h"
#include "MinimalGlobeDemoConfig.h"
#include "AmapVectorConfig.h"

#define LOG_TAG "MinimalGlobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/// Android 测量台布尔属性。未设置时保持编译期默认；显式 0/1 覆盖，其他值
/// 视为未设置。所有开关只在进程启动建场时读取，A/B 前 force-stop 重启即可。
static bool startupBoolProperty(const char* name, bool fallback) {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get(name, prop);
    if (prop[0] == '0' && prop[1] == '\0') return false;
    if (prop[0] == '1' && prop[1] == '\0') return true;
    return fallback;
}

static size_t startupSizeProperty(const char* name, size_t fallback) {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get(name, prop);
    if (!prop[0]) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(prop, &end, 10);
    if (end == prop || *end != '\0' || parsed == 0) return fallback;
    return static_cast<size_t>(parsed);
}

static std::optional<double> startupDoubleProperty(const char* name) {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get(name, prop);
    if (!prop[0]) return std::nullopt;
    char* end = nullptr;
    const double parsed = std::strtod(prop, &end);
    if (end == prop || *end != '\0' || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

static void applyStartupCameraOverride(earth_engine::EarthSceneConfig& config) {
    const std::optional<double> lon =
        startupDoubleProperty("debug.ee.camlon");
    const std::optional<double> lat =
        startupDoubleProperty("debug.ee.camlat");
    const std::optional<double> height =
        startupDoubleProperty("debug.ee.camh");
    const std::optional<double> pitch =
        startupDoubleProperty("debug.ee.campitch");
    if (!lon || !lat || !height || !pitch) {
        return;
    }

    config.initialCamera.longitudeDegrees = *lon;
    config.initialCamera.latitudeDegrees = *lat;
    config.initialCamera.heightMeters = *height;
    // CamPose 日志里 pitch 是向下为负；SceneCameraConfig 需要地平线上方仰角。
    config.initialCamera.obliqueElevationDegrees =
        std::clamp(std::abs(*pitch), 0.1, 89.9);
    config.initialCamera.obliqueAzimuthDegrees =
        startupDoubleProperty("debug.ee.camheading").value_or(0.0);
    config.initialCamera.freezeCamera =
        startupBoolProperty("debug.ee.camfreeze", false);

    LOGI("Startup camera override: center=%.5f,%.5f camH=%.1f "
         "pitchDeg=%.2f headingDeg=%.2f freeze=%d",
         *lon, *lat, *height, *pitch,
         config.initialCamera.obliqueAzimuthDegrees,
         config.initialCamera.freezeCamera ? 1 : 0);
}
// 设备侧 amap-vector.json(getFilesDir)。缺文件/解析失败 → 回落 sealed 默认
// 并记日志；fail-loud:未知键整份拒收,不静默吞。
static minimal_globe_demo::AmapVectorConfig loadAmapVectorConfig(
    earth_engine::PlatformBridge& bridge) {
    minimal_globe_demo::AmapVectorConfig config;
    const std::string dir = bridge.documentsDirectory();
    if (dir.empty()) {
        LOGI("AmapVectorConfig: no files dir, using sealed defaults");
        return config;
    }
    const std::string path = dir + "/amap-vector.json";
    std::ifstream file(path);
    if (!file.is_open()) {
        LOGI("AmapVectorConfig: %s not found, using sealed defaults",
             path.c_str());
        return config;
    }
    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    const std::string err = parseAmapVectorConfig(text, config);
    if (!err.empty()) {
        LOGE("AmapVectorConfig: rejecting %s: %s (using defaults)",
             path.c_str(), err.c_str());
        return {};
    }
    LOGI("AmapVectorConfig: loaded %s endpoints=%d terrain=%d zooms=%d "
         "style=%d",
         path.c_str(), config.hasAmapEndpoints ? 1 : 0,
         config.hasTerrain ? 1 : 0, config.hasZooms ? 1 : 0,
         config.hasStyle ? 1 : 0);
    return config;
}


// ============================================================
// 阶段 3/4/5 真机验证钩子(数字键 7/8/9)
// ============================================================
//
// 这三个阶段(飞行/系留/正交)在 demo 里**没有任何产品入口**,不接出来就只能
// 靠 host 判据。每个钩子都配一条机制信号日志 —— 画面"看着像对的"分不清
// "真跑了"和"根本没走到那条路"。

// 阶段 3:飞行目标(重庆 → 北京)。
constexpr double kFlightDestLng = 116.397;
constexpr double kFlightDestLat = 39.908;
constexpr double kFlightDestAlt = 3000.0;
struct FlightProbe {
    bool armed = false;
    uint64_t clampsAtStart = 0;
    double maxProgress = 0.0;
    int frames = 0;
};
FlightProbe gFlightProbe;

// 阶段 4:假载体 —— 绕重庆做匀速圆周运动并自转,驱动 tether 的两个 provider。
struct FakeCarrier {
    bool active = false;
    double angleRad = 0.0;          // 圆周相位
    double radiusDeg = 0.02;        // ~2km 半径
    double centerLng = 106.508;
    double centerLat = 29.617;
    double altMeters = 1200.0;
    bool useOrientation = false;    // true = 接 orientationProvider(座舱/roll 跟随)
    glm::dvec3 position{0.0};
    glm::dmat3 orientation{1.0};

    void step(double dt) {
        if (!active) return;
        angleRad += dt * 0.5;       // ~12s 一圈
        const double lng = centerLng + radiusDeg * std::cos(angleRad);
        const double lat = centerLat + radiusDeg * std::sin(angleRad);
        position = earth_engine::Ellipsoid::WGS84()
                       .cartographicToCartesian(
                           earth_engine::Cartographic::fromDegrees(lng, lat, altMeters))
                       .raw();
        // 机体系:绕本地垂直轴按航向转,并随相位横滚(验 roll 跟随)。
        const glm::dmat3 enu = earth_engine::CameraPose::enuFrameAt(position);
        const double heading = angleRad + 1.5707963;   // 切向
        const double roll = 0.5 * std::sin(angleRad * 2.0);
        const glm::dquat q = glm::angleAxis(-heading, enu[2]) *
                             glm::angleAxis(roll, enu[1]);
        orientation = glm::dmat3(q * enu[0], q * enu[1], q * enu[2]);
    }
};
FakeCarrier gCarrier;

// 阶段 5:正交开关。宽度取切换瞬间"透视在地面处的足迹",两者画面才可比。
bool gOrthographic = false;


using namespace earth_engine;

// 渲染线程放置策略(ADPF → uclamp_min → 性能核亲和,三级降级)已下沉成 SDK helper,
// 成因、实测数据与各级细节见 earth_engine/threading/RenderThreadPlacement.h。宿主自建
// 的渲染线程都要走一遍,否则 EAS 会把这条匿名的裸 std::thread 扔进小核簇。
namespace {
RenderThreadPlacement gRenderThreadPlacement;
}  // namespace

// ============================================================
// EGL / GL ES 3.0 上下文
// ============================================================

static EGLDisplay gDisplay = EGL_NO_DISPLAY;
static EGLSurface gSurface = EGL_NO_SURFACE;
static EGLContext gContext = EGL_NO_CONTEXT;
static ANativeWindow* gWindow = nullptr;
// 宽高被 UI 线程（触摸事件整形）与渲染线程（EGL/引擎）两侧读写，用原子避免撕裂
static std::atomic<int> gWidth{0}, gHeight{0};
static std::atomic<float> gDisplayDensity{1.0f};

// 每帧发布相机方位角(弧度),UI 指北针无锁读取。
static std::atomic<float> gHeadingRadians{0.0f};
// P6 分段:Scene 托管的 MVT source 更新耗时通过 Diagnostics 发布。

// Engine + RenderDevice
static std::unique_ptr<RenderDeviceGLES> gRenderDevice;
static std::unique_ptr<Engine> gEngine;
static std::unique_ptr<AndroidPlatformBridge> gPlatformBridge;
static std::unique_ptr<EarthEngineSdkFacade> gSdkFacade;
static const AmapClassicRuntime* gAmapOfficialRuntime = nullptr;
static bool gEngineReady = false;

// JNI_OnLoad — 存储 JavaVM 引用
static JavaVM* gJvm = nullptr;
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    gJvm = vm;
    AndroidPlatformBridge_InitJvm(vm);
    return JNI_VERSION_1_6;
}

// Application Context 的 global ref（GLESView 构造时经 nativeInit 传入），
// 供 AndroidPlatformBridge 查询目录 / 设备信息。
static jobject gAppContext = nullptr;

// Touch state
static bool gTouching = false;
static bool gDragStarted = false;
static bool gTouchMoved = false;

// Debug panel state
static bool gDebugPinchActive = false;

// ---- 官方高德矢量源。regions/main 共享 request-type-1 缓存，
// POI 独立消费 request-type-2；三者都只灌注 FeatureRenderLayer。 ----
static const FeatureRenderLayer* gAmapRegionsLayer = nullptr;  // Engine 持有
static const FeatureRenderLayer* gAmapMainLayer = nullptr;  // Engine 持有
static const FeatureRenderLayer* gAmapPoiLayer = nullptr;  // Engine 持有
// 官方高德数据使用独立 decode/tessellation 后台通道。此前二者
// 共用严格 FIFO 池，全球
// z3 首批 regions/main 镶嵌会把后来的 POI decode 挡在队尾；网络 8 秒已
// 完成，画面却要 20-38 秒才收敛。拆池消除队头阻塞，线程总数仍按设备
// 内存/核心有界，不减少瓦片或可见细节。
static std::shared_ptr<ThreadPool> gAmapType1DecodePool;
static std::shared_ptr<ThreadPool> gAmapPoiDecodePool;
static std::shared_ptr<ThreadPool> gAmapTessellationPool;
static minimal_globe_demo::AmapWorkerBudget gAmapWorkerBudget;

static void ensureAmapWorkerPools() {
    if (gAmapType1DecodePool && gAmapPoiDecodePool &&
        gAmapTessellationPool) return;

    minimal_globe_demo::AmapWorkerBudget budget{
        minimal_globe_demo::kAmapType1DecodeThreadsFallback,
        1,
        minimal_globe_demo::kAmapTessellationThreadsFallback};
    DeviceInfo device;
    if (gPlatformBridge) {
        device = gPlatformBridge->deviceInfo();
        budget = minimal_globe_demo::chooseAmapWorkerBudget(
            device.cpuCores, device.totalMemoryBytes);
    }
    budget.decodeThreads = std::clamp<size_t>(
        startupSizeProperty("debug.ee.amaptype1decode", budget.decodeThreads),
        1, 4);
    budget.poiDecodeThreads = std::clamp<size_t>(
        startupSizeProperty("debug.ee.amapdecode", budget.poiDecodeThreads),
        1, 4);
    budget.tessellationThreads = std::clamp<size_t>(
        startupSizeProperty("debug.ee.amaptess", budget.tessellationThreads),
        1, 8);
    gAmapWorkerBudget = budget;
    gAmapType1DecodePool = std::make_shared<ThreadPool>(budget.decodeThreads);
    gAmapPoiDecodePool = std::make_shared<ThreadPool>(budget.poiDecodeThreads);
    gAmapTessellationPool =
        std::make_shared<ThreadPool>(budget.tessellationThreads);
    LOGI("AmapWorkers split type1Decode=%zu poiDecode=%zu tess=%zu cores=%d "
         "memory=%lldMB model=%s",
         budget.decodeThreads, budget.poiDecodeThreads,
         budget.tessellationThreads, device.cpuCores,
         static_cast<long long>(device.totalMemoryBytes / (1024 * 1024)),
         device.model.empty() ? "unknown" : device.model.c_str());
}

static double androidUptimeSeconds();
static void postInputEvent(const InputEvent& event);

// UI 线程调用：投递 Cancel 事件到渲染线程，并复位 UI 侧触摸状态。
static void cancelInputIfNeeded() {
    InputEvent event;
    event.type = InputEvent::Type::Cancel;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
    gTouching = false;
    gDragStarted = false;
    gTouchMoved = false;
    gDebugPinchActive = false;
}

static void clearDemoEngineObjects() {
    // SDK facade 销毁时先停 Scene-owned MVT bundles；其 HttpRequest 句柄
    // 随 source-specific fetch state 的 RAII 析构取消。
    gAmapOfficialRuntime = nullptr;
    gAmapRegionsLayer = nullptr;  // Engine 持有,随 runtime 一起销毁
    gAmapMainLayer = nullptr;     // Engine 持有,随 runtime 一起销毁
    gAmapPoiLayer = nullptr;      // Engine 持有,随 runtime 一起销毁
    gSdkFacade.reset();
    gEngine.reset();
    gRenderDevice.reset();
    gAmapType1DecodePool.reset();
    gAmapPoiDecodePool.reset();
    gAmapTessellationPool.reset();
    gAmapWorkerBudget = {};
    gPlatformBridge.reset();
    gEngineReady = false;
}

static bool initEGL(ANativeWindow* window) {
    gDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gDisplay == EGL_NO_DISPLAY) return false;

    EGLint major, minor;
    if (!eglInitialize(gDisplay, &major, &minor)) return false;

    // 优先请求 4x MSAA(默认帧缓冲多重采样,eglSwapBuffers 自动 resolve,无需
    // 改 shader/离屏帧缓冲);驱动不支持则回退无 MSAA。消除地形/海岸线/建筑轮廓
    // 边缘爬行。
    // stencil 8 位:P6 矢量 stencil 分类贴地(阴影体计数)需要。
    const EGLint msaaAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 1, EGL_SAMPLES, 4,
        EGL_NONE
    };
    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs = 0;
    // MSAA A/B(2026-08-21 GPU swap 专项):4x MSAA 在 1240×2772 × 2 pass 下
    // 是 GPU 帧时大头候选(swap 10-14ms)。运行时 `adb shell setprop
    // debug.ee.msaa 0` 关,重进 app 生效。
    char msaaProp[4] = {0};
    __system_property_get("debug.ee.msaa", msaaProp);
    const bool wantMsaa = msaaProp[0] != '0';
    const EGLint* chosenAttribs = wantMsaa ? msaaAttribs : attribs;
    if (!eglChooseConfig(gDisplay, chosenAttribs, &config, 1, &numConfigs) ||
        numConfigs < 1) {
        if (!eglChooseConfig(gDisplay, attribs, &config, 1, &numConfigs)) {
            return false;
        }
        if (numConfigs < 1) return false;
    }
    EGLint chosenSamples = 0, chosenStencil = 0, chosenDepth = 0;
    eglGetConfigAttrib(gDisplay, config, EGL_SAMPLES, &chosenSamples);
    eglGetConfigAttrib(gDisplay, config, EGL_STENCIL_SIZE, &chosenStencil);
    eglGetConfigAttrib(gDisplay, config, EGL_DEPTH_SIZE, &chosenDepth);
    __android_log_print(ANDROID_LOG_INFO, "GLESView",
                        "EGL config MSAA samples=%d depth=%d stencil=%d",
                        chosenSamples, chosenDepth, chosenStencil);

    gSurface = eglCreateWindowSurface(gDisplay, config, window, nullptr);
    if (gSurface == EGL_NO_SURFACE) return false;

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    gContext = eglCreateContext(gDisplay, config, EGL_NO_CONTEXT, ctxAttribs);
    if (gContext == EGL_NO_CONTEXT) return false;

    if (!eglMakeCurrent(gDisplay, gSurface, gSurface, gContext)) return false;

    EGLint surfaceWidth = 0, surfaceHeight = 0;
    eglQuerySurface(gDisplay, gSurface, EGL_WIDTH, &surfaceWidth);
    eglQuerySurface(gDisplay, gSurface, EGL_HEIGHT, &surfaceHeight);
    gWidth = surfaceWidth;
    gHeight = surfaceHeight;

    LOGI("EGL initialized: %dx%d, GL: %s, GLSL: %s",
         surfaceWidth, surfaceHeight,
         glGetString(GL_VERSION),
         glGetString(GL_SHADING_LANGUAGE_VERSION));

    return true;
}

// C-V8 late-latch:上一帧 GPU 完成栅栏(仅渲染线程访问)。声明于此因 destroyEGL
// 在 teardown 处先引用它;创建/等待逻辑见 renderFrame 前的 late-latch 段。
static GLsync gPrevFrameFence = nullptr;
/// 有待处理的输入事件(UI 线程置位,渲染线程 onFrame 顶部消费)。C-V8 的
/// fence 等待只为 latch 新鲜输入;惯性/无输入帧没有 latch 收益,跳过等待
/// 让 CPU 与 GPU 重叠,避免帧率塌陷(2026-08-20 PHK110:GPU≈16ms 贴预算,
/// 无条件等待+CPU 4.4ms=20ms>16.7ms → 30fps)。
static std::atomic<bool> gInputPending{false};

static void destroyEGL() {
    clearDemoEngineObjects();

    // C-V8 late-latch fence 随 context 失效,趁 context 仍 current 回收。
    if (gPrevFrameFence) {
        glDeleteSync(gPrevFrameFence);
        gPrevFrameFence = nullptr;
    }

    eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gContext != EGL_NO_CONTEXT) eglDestroyContext(gDisplay, gContext);
    if (gSurface != EGL_NO_SURFACE) eglDestroySurface(gDisplay, gSurface);
    if (gDisplay != EGL_NO_DISPLAY) eglTerminate(gDisplay);
    gContext = EGL_NO_CONTEXT;
    gSurface = EGL_NO_SURFACE;
    gDisplay = EGL_NO_DISPLAY;
}

static bool createEngine() {
    clearDemoEngineObjects();
    gRenderDevice = std::make_unique<RenderDeviceGLES>();
    gEngine = std::make_unique<Engine>(gRenderDevice.get());

    gEngine->onSurfaceCreated();
    gEngine->onSurfaceChanged(gWidth, gHeight, gDisplayDensity.load());

    gEngineReady = gEngine->isReady();
    if (gEngineReady) {
        LOGI("Engine initialized successfully, camera pos: %.1f,%.1f,%.1f",
             gEngine->camera().position().x(),
             gEngine->camera().position().y(),
             gEngine->camera().position().z());

        // 创建 Android 平台桥接；网络由 native curl scheduler 调度。
        gPlatformBridge = std::make_unique<AndroidPlatformBridge>(gJvm, gAppContext);
        gSdkFacade =
            std::make_unique<EarthEngineSdkFacade>(
                *gEngine,
                *gRenderDevice,
                *gPlatformBridge);
        EarthSceneConfig sceneConfig =
            minimal_globe_demo::makeDefaultDemoSceneConfig();
        applyStartupCameraOverride(sceneConfig);
        const minimal_globe_demo::AmapVectorConfig amapCfg =
            loadAmapVectorConfig(*gPlatformBridge);
        if (amapCfg.hasTerrain) {
            // 设备侧地形源覆盖(amap-vector.json sources.terrain)。
            sceneConfig.terrain.kind = TerrainSourceKind::Heightmap;
            sceneConfig.terrain.urlTemplate = amapCfg.terrainUrlTemplate;
            sceneConfig.terrain.minimumZoom = amapCfg.terrainMinZoom;
            sceneConfig.terrain.maximumZoom = amapCfg.terrainMaxZoom;
            sceneConfig.terrain.heightmapMaxNativeZoom = amapCfg.terrainMaxZoom;
            sceneConfig.terrain.tileSize = amapCfg.terrainTileSize;
            sceneConfig.terrain.heightmapBorderInset = amapCfg.terrainBorderInset;
        }
        sceneConfig.gpuPassTiming = startupBoolProperty(
            "debug.ee.gputiming", sceneConfig.gpuPassTiming);
        LOGI("RuntimeAB amapVector=official-only aerialFog=%d gpuTiming=%d",
             sceneConfig.aerialFog ? 1 : 0,
             sceneConfig.gpuPassTiming ? 1 : 0);
        // ---- 高德矢量:type2 面 VectorFill(z10)垫底,再上路网/建筑/POI。----
        {
            // 只为 20004/20003/20002/20001 四级主要道路画外壳。
            // 轨道、铁路和低等级街巷使用单线，避免整座城市出现白色
            // 双边并减少无意义的片元提交。
            // Official classic normal supplies casing stops from each road
            // class onset.  Data availability already gates the class, so do
            // not add a later app-only casing threshold.
            // Transit 20015 uses one shared ordinary-route geometry group;
            // route identity selects the official Road field-3 color, while
            // Semantic styleGroup expressions provide zoom widths. This
            // keeps draw topology constant as high-zoom widths expand.
            // fill 按 classic-normal 官方 field-7 表分流。30001/30002 的
            // 颜色键都是 amap_subkey；amap_kind 是几何解码类别，不能继续
            // 被误当作样式 subKey。
            //   type 3 → 建筑 roof 基色；未在官方 surface 表里的面透明。
            // Production FeatureMulti.drawOrder is the official renderer
            // zIndex. Keep that source identity intact instead of maintaining
            // a second class-level ordering table in the demo.
            // Provider drawOrder controls label ordering only. Official road
            // field-5 style is keyed by source class/subKey, including the
            // official type-1 road-name payload.
            // Road-name labels are owned exclusively by the official
            // request-type-2 dedicated road_name payload below. The main
            // geometry source must not create a second, non-colliding copy.
            // Official 55001 records own building admission, color, zoom and
            // height. No app-local hide/planar fallback is retained.
            ensureAmapWorkerPools();
            // 粗源 z10 type2 → VectorFill(V30 地球网格)。无地形时 drape
            // 不出画,这条才是水/绿地的上屏路。先挂垫底。
            // 粗源必须有自己的 surface 配色。主源 as 会把错位的
            // 30001 设透明，不能复用，否则 z10 的连续水/绿底实际不出画。
            // Official RegionLayer type-2 payload is not polygon-only: its
            // `lines` array carries low-zoom railway/guide/boundary records
            // through the same handlerTile dispatch. Consume those identities
            // with the exact official road contract; do not silently discard
            // them or revive a generic stroke.
            // z10 大面若不细分，二维 CDT 三角映到 ECEF 后会成为穿过球内的长
            // 弦，缺口/地平线处表现为黑色跨球射线。使用公里级上限恢复球面
            // 贴合；不回到曾触发碎网格和 worker 爆量的 400m 密度。
            // z10 粗源只在 display zoom < 12 时显示。VectorTileTree 对
            // canonical zoom 取 floor，主源到 12.0 才真正切入 z12 type2；
            // 旧 11.5 交接会让 30002 地块在 [11.5,12) 整体空窗。
            // 近景让位给主源 z12-14 细面，避免
            // 粗像素块盖在细面上 = 双源叠加「破破烂烂」。
            // 高德并不是只有 z10 区域档。canonical view zoom 经
            // amapDataZoom 映射到服务端的 3/6/8/10/12/14 离散档位。
            // 粗区域层只消费到 z10：高空用 z3/6/8
            // 覆盖全球可见范围，不能把全国视野截成重庆中心最近 256 张
            // z10 瓦片。
            // 主源:路网/建筑/轨道与 30002 地块 z12-14 网格。
            // regions/main 共享 official source bundle 持有的完整
            // type1 解码 cache，平台层不再持有第二套 source 状态。
            // 高德数据是离散档位，不是只有重庆验证过的 z12/z14。
            // amapDataZoom 负责 canonical → 服务端档位：全球视野 z3，
            // 逐级进入 z6/8/10/12/14。这样扩大视野时降数据 LOD，而
            // 不是提高 maxTilesPerView 硬拉全球细瓦。
            // 近景 z14 视口约 84 瓦(1.5km 高)，默认 64 会继续降到
            // z12；抬高闸让 z14 完整进入。远景不靠这个上限硬撑，前面的
            // 离散档位会先降到 z10/8/6/3。
            // POI 源:type 0 通用 POI 点标签 + type 1 官方道路文字几何。
            // Administrative labels use a distinct official neutral text
            // family. One semantic split is bounded and extensible; unlike a
            // subKey-per-paintOrder palette it adds at most one label/point
            // range per tile.
            // POI request type-1 carries official road-name
            // polylines.  They exist only to provide an along-road anchor;
            // road geometry itself is already rendered by amap-main.
            // This source contains point POIs and official road-name lines.
            // No line stroke contract is installed here: all line geometry
            // therefore fails closed after an admitted official label.
            // The POI stream also carries official road-name polylines. Install
            // Install the centralized official field-5 contracts as the
            // single source of truth while preserving point/range batching.
            AmapClassicRuntime::Options runtimeOptions;
            runtimeOptions.credentials.webKey =
                minimal_globe_demo::kAmapWebKey;
            runtimeOptions.sources.decodedCacheTiles =
                minimal_globe_demo::kAmapTileCacheDecoded;
            runtimeOptions.sources.rawCacheTiles =
                minimal_globe_demo::kAmapTileCacheRaw;
            // amap-vector.json sources.amap / zooms / style 穿进 sealed runtime。
            if (!amapCfg.apiBase.empty())
                runtimeOptions.endpoints.apiBase = amapCfg.apiBase;
            if (!amapCfg.initBase.empty())
                runtimeOptions.endpoints.initBase = amapCfg.initBase;
            if (!amapCfg.iconBase.empty())
                runtimeOptions.endpoints.iconBase = amapCfg.iconBase;
            if (!amapCfg.sdfBase.empty())
                runtimeOptions.endpoints.sdfBase = amapCfg.sdfBase;
            runtimeOptions.sources.zoomSelection.minZoom =
                amapCfg.zoomMinZoom;
            runtimeOptions.sources.zoomSelection.regionsMaxZoom =
                amapCfg.zoomRegionsMaxZoom;
            runtimeOptions.sources.zoomSelection.mainMaxZoom =
                amapCfg.zoomMainMaxZoom;
            runtimeOptions.sources.zoomSelection.regionsActiveBelowZoom =
                amapCfg.zoomRegionsActiveBelowZoom;
            runtimeOptions.sources.zoomSelection.regionsSupportedZooms =
                amapCfg.zoomRegionsSupported;
            runtimeOptions.sources.zoomSelection.mainSupportedZooms =
                amapCfg.zoomMainSupported;
            runtimeOptions.sources.zoomSelection.poiSupportedZooms =
                amapCfg.zoomPoiSupported;
            runtimeOptions.sources.zoomSelection.dataZoomRemap =
                amapCfg.zoomDataZoomRemap;
            runtimeOptions.sources.styleOverrides =
                amapCfg.styleOverrides;
            gAmapOfficialRuntime = gEngine->installAmapClassicRuntime(
                *gPlatformBridge,
                gAmapType1DecodePool, gAmapPoiDecodePool,
                gAmapTessellationPool, std::move(runtimeOptions));
            if (!gAmapOfficialRuntime) {
                throw std::runtime_error(
                    "AMap official runtime already installed");
            }
            gAmapRegionsLayer =
                gAmapOfficialRuntime->sources().regionsLayer();
            gAmapMainLayer = gAmapOfficialRuntime->sources().mainLayer();
            gAmapPoiLayer = gAmapOfficialRuntime->sources().poiLayer();
            LOGI("AmapE3: atomic official runtime installed");
        }
        // Scene installation consumes the already-sealed runtime contract.
        // In pure-vector mode this is the sole switch that selects the
        // Official vector identity stays sealed; Scene terrain independently
        // owns elevation and receives the official unlit land presentation.
        gSdkFacade->installScene(std::move(sceneConfig));

        // Phase 2c P5:GPU 位移已引擎默认开(Engine.h terrainGpuDisplacementEnabled_
        // = true,pool 在首次 scene update 前急切创建)。运行时 A/B 关闭仍走调试面板
        // 的 setTerrainGpuDisplacementEnabled(false)(GLESView.cpp toggle)。
    } else {
        LOGE("Engine initialization failed");
        clearDemoEngineObjects();
    }
    return gEngineReady;
}

// ── 输入 late-latch:fence 门控的 render-ahead cap(C-V8)────────────────
// 弱机 GPU-bound 时 CPU 领跑 GPU 2-3 帧,latch 到的 pose 要等多帧才上屏 →
// 手指→光子滞后 = 流水线深度 × 帧时,而非 latch 时机。把驱动内那段隐式 GPU
// 等待用显式 fence 挪到 latch **之前**:等完再排输入 → latch 到更新鲜的指位,
// 同时把 render-ahead 压到深度 1。等待总量不变(GPU-bound),不掉吞吐;快机
// fence 立即返回 → 0 成本、自适应,无需按机型开关。
// 运行期 A/B:`adb shell setprop debug.ee.latelatch 0` 关(默认开)。
// (gPrevFrameFence 声明上移至 destroyEGL 之前,因其在 teardown 处先被引用。)
static double gFenceWaitMs = 0.0;          // 上一次门控等待耗时(探针)

static bool lateLatchEnabled() {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.ee.latelatch", prop);
    return prop[0] != '0';  // 未设或非 "0" → 开
}

// 影子自检运行时开关(debug 构建默认开 = 收敛漏报捕网,但同步回读会让交互
// 卡——每 idle 段 20 帧 glReadPixels 排空 GPU 管线,见 kShadowVerifyIdle 注释)。
// 运行期切换:`adb shell setprop debug.ee.shadowverify 0` 关 / `1` 开 /
// 清空回落到构建默认。逐帧读(共享内存,亚微秒,与 latelatch 同模式),值变化
// 才调 setShadowVerifyEnabled。
static bool gLastShadowVerifySetting =
    earth_engine::minimal_globe_demo::kShadowVerifyIdle;
static void applyShadowVerifyRuntimeSwitch() {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.ee.shadowverify", prop);
    bool desired = earth_engine::minimal_globe_demo::kShadowVerifyIdle;
    if (prop[0] == '0') {
        desired = false;
    } else if (prop[0] == '1') {
        desired = true;
    }
    if (desired != gLastShadowVerifySetting) {
        gLastShadowVerifySetting = desired;
        if (gEngine) {
            gEngine->setShadowVerifyEnabled(desired);
            LOGI("ShadowVerify runtime switch -> %s",
                 desired ? "on" : "off");
        }
    }
}

// onFrame 顶部、drainTasks 之前调用:等上一帧 GPU 完成(render-ahead≤1),
// 使随后排空的输入尽量新鲜。带超时,GPU 丢失时不挂死。
static void waitPrevFrameFenceForLatch() {
    gFenceWaitMs = 0.0;
    if (!gPrevFrameFence) return;
    if (!lateLatchEnabled()) {
        // 关闭时不等待,但仍回收 fence,避免句柄泄漏(A/B 切换即时生效)。
        glDeleteSync(gPrevFrameFence);
        gPrevFrameFence = nullptr;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    // GL_SYNC_FLUSH_COMMANDS_BIT:确保 fence 已 flush,否则弱驱动下可能永不 signal。
    // 超时 200ms > 单帧 GPU 上界:正常必在此前 signal;超时则放行不挂死。
    glClientWaitSync(gPrevFrameFence, GL_SYNC_FLUSH_COMMANDS_BIT,
                     200ull * 1000ull * 1000ull);
    gFenceWaitMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    glDeleteSync(gPrevFrameFence);
    gPrevFrameFence = nullptr;
}

static void renderFrame() {
    if (!gEngineReady) return;

    const auto frameStart = std::chrono::steady_clock::now();
    static auto previousFrameStart = frameStart;
    const double callbackIntervalMs =
        std::chrono::duration<double, std::milli>(
            frameStart - previousFrameStart).count();
    previousFrameStart = frameStart;

    // 时间步进（实时）
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastTime).count();
    lastTime = now;

    // 环境系统：时间步进，render 中 update() 计算当前帧天空色
    gEngine->advanceTime(dt);
    // C2/E3:官方 runtime 由 Scene 在 Engine::render 内统一推进；Android
    // 只保留诊断读取，不再计算视域或手工 pump 第二条生命周期路径。
    if (gAmapOfficialRuntime) {
        const auto& sourceBundle = gAmapOfficialRuntime->sources();
        // 共享 type1 验收口径：regions/main 必须命中同一
        // typed cache。全球 z3 冷启 fetch 应约为 64，不再是两个
        // profile 各解码一遍。POI(type2) 由独立 pool 统计。
        static uint64_t amapRawLogCounter = 0;
        if (++amapRawLogCounter % 120 == 1) {
            const auto type1 = sourceBundle.type1CacheStats();
            LOGI("AmapType1Cache fetch=%llu refetch=%llu hit=%llu rawHit=%llu "
                 "resident=%zu/%zuKB raw=%zu/%zuKB",
                 static_cast<unsigned long long>(type1.fetches),
                 static_cast<unsigned long long>(type1.refetches),
                 static_cast<unsigned long long>(type1.hits),
                 static_cast<unsigned long long>(type1.rawHits),
                 type1.residentTiles, type1.residentBytes / 1024,
                 type1.rawTiles, type1.rawBytes / 1024);
            auto logPool = [](const char* name,
                              const std::shared_ptr<ThreadPool>& pool) {
                if (!pool) return;
                const auto s = pool->stats();
                const double avgQueue =
                    s.started ? s.totalQueueWaitMs / s.started : 0.0;
                const double avgWork =
                    s.completed ? s.totalWorkMs / s.completed : 0.0;
                LOGI("MvtPool %s threads=%zu queued=%llu active=%llu "
                     "done=%llu queueAvg=%.2f queueMax=%.2f "
                     "workAvg=%.2f workMax=%.2f",
                     name, pool->threadCount(),
                     static_cast<unsigned long long>(s.queued),
                     static_cast<unsigned long long>(s.active),
                     static_cast<unsigned long long>(s.completed), avgQueue,
                     s.maxQueueWaitMs, avgWork, s.maxWorkMs);
            };
            logPool("type1Decode", gAmapType1DecodePool);
            logPool("poiDecode", gAmapPoiDecodePool);
            logPool("tess", gAmapTessellationPool);
            auto logSource = [](const char* name,
                                const AmapClassicSourceBundle::SourceStats& s) {
                LOGI("AmapSource %s z=%d desired=%lld scanned=%zu render=%zu "
                     "request=%zu pending=%zu tess=%zu ready=%zu active=%zu "
                     "pairs=%zu tree=%.2f commit=%.2f",
                     name, s.selectedZoom,
                     static_cast<long long>(s.desiredTileCount),
                     s.scannedTileCount, s.renderTileCount,
                     s.requestTileCount, s.pendingTileCount,
                     s.tessellatingTileCount, s.readyTileCount,
                     s.activeTileCount, s.activeAncestorPairs, s.treeMs,
                     s.commitMs);
            };
            logSource("regions", sourceBundle.regionsSourceStats());
            logSource("main", sourceBundle.mainSourceStats());
            logSource("poi", sourceBundle.poiSourceStats());
            const auto probe = gAmapOfficialRuntime->maskProbe();
            const uint64_t req = probe.presentHits + probe.asyncPending +
                                 probe.startedFetches + probe.failed;
            LOGI("AmapMask probe req=%llu present=%llu pending=%llu "
                 "fetchStart=%llu failed=%llu lastKey=%s z=%d resident=%zu",
                 static_cast<unsigned long long>(req),
                 static_cast<unsigned long long>(probe.presentHits),
                 static_cast<unsigned long long>(probe.asyncPending),
                 static_cast<unsigned long long>(probe.startedFetches),
                 static_cast<unsigned long long>(probe.failed),
                 probe.lastScheme.c_str(), probe.lastZ, probe.residentPages);
        }
    }
    // 阶段 4:假载体在**引擎 update 之前**推进,这样本帧 tether 读到的就是新
    // 位置 —— 放到 render 之后会让相机永远跟着上一帧的载体,表现为恒定滞后,
    // 而画面上看着只是"跟得有点松"。
    gCarrier.step(1.0 / 60.0);

    // P6 分段:`total − engine − post − swap` 这段宿主前奏此前**没有任何
    // 字段覆盖**,于是"慢帧 187ms 而 engine 只有 2-3ms"只能停在
    // 「时间不在引擎」而无法再往下定位。pre 覆盖整段,mvt 单列最大嫌疑。
    const auto engineStart = std::chrono::steady_clock::now();
    const double preMs = std::chrono::duration<double, std::milli>(
        engineStart - frameStart).count();
    const bool presented =
        gEngine->render(0.0);  // auto-delta（内部 update；必要时 beginFrame→render→endFrame）
    const double engineMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - engineStart).count();

    // C-V8 late-latch:scene draw 提交后打栅栏,下一帧顶部 waitPrevFrameFenceForLatch
    // 等它 → render-ahead≤1。仅真出帧时打;旧栅栏理应已在本帧顶部回收,防御性再清。
    if (presented) {
        if (gPrevFrameFence) glDeleteSync(gPrevFrameFence);
        gPrevFrameFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    const auto postEngineStart = std::chrono::steady_clock::now();
    gHeadingRadians = static_cast<float>(gEngine->cameraHeadingRadians());
    const double postEngineMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - postEngineStart).count();

    double swapMs = 0.0;
    EGLBoolean swapOk = EGL_TRUE;
    if (presented) {
        const auto swapStart = std::chrono::steady_clock::now();
        swapOk = eglSwapBuffers(gDisplay, gSurface);
        swapMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - swapStart).count();
    }

    const double frameTotalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - frameStart).count();
    // 回报本帧 CPU 工作耗时。**刻意不含 swapMs** —— eglSwapBuffers 里绝大部分是
    // 等 vsync 的空转,算进去会让系统以为我们每帧都刚好用满预算,反而不提速。
    gRenderThreadPlacement.reportActualWorkDurationMs(engineMs + postEngineMs);
    const uint64_t frameId = gEngine->presentationTrace().camera.frameId;
    // P5c 标签避让诊断(节流):cand=候选 placed=显示 col=碰撞落选
    // horiz=地平线剔除 proj=视锥外/相机背后。
    // P3 水位:图集满 = 永久丢字(无淘汰),必须能看见逼近过程。
    if (frameId % 600 == 0) {
        const GlyphAtlas* ga = gEngine ? gEngine->renderer()->glyphAtlas()
                                       : nullptr;
        if (ga) {
            LOGI("GlyphAtlas glyphs=%zu shelf=%d/%d (%.0f%%) drops=%d",
                 ga->residentGlyphCount(), ga->shelfUsedHeightPx(),
                 GlyphAtlas::kAtlasSize,
                 100.0 * ga->shelfUsedHeightPx() / GlyphAtlas::kAtlasSize,
                 ga->atlasFullDropCount());
        }
    }
    // 七态标注 dump 按需触发(免重编译诊断口):
    //   adb shell setprop debug.ee.labeldump <值> && adb shell input tap 620 900
    // 值变化才触发一次(app 清不掉系统属性,记上次值去重;重触发换个值)。
    // 纯数字值或 "all" = 全量(去重要求每次换新值,固定 token 两次就用完
    // ——实测踩过,故数字序列 1/2/3… 都算全量);其余值当作标注名过滤
    // 子串(可给 UTF-8 中文名)。
    // 逐帧轮询而非 %N 节流:按需渲染下 tap 只给短帧串,跨不过 N 边界就
    // 永不触发(实测踩过);__system_property_get 是共享内存读,亚微秒。
    // 仍需至少一帧才会读到(idle 全停时先 tap 顶帧)——诊断口按帧驱动是
    // 故意的:dump 读桶表必须在渲染线程。
    {
        static std::string lastLabelDumpProp;
        char prop[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.ee.labeldump", prop);
        if (prop[0] != '\0' && lastLabelDumpProp != prop) {
            lastLabelDumpProp = prop;
            const bool allDigits =
                lastLabelDumpProp.find_first_not_of("0123456789") ==
                std::string::npos;
            const std::string filter =
                (allDigits || lastLabelDumpProp == "all")
                    ? std::string()
                    : lastLabelDumpProp;
            const std::array<const FeatureRenderLayer*, 3> diagnosticLayers{
                gAmapRegionsLayer, gAmapMainLayer, gAmapPoiLayer};
            for (const FeatureRenderLayer* layer : diagnosticLayers) {
                if (!layer) continue;
                // 逐行打(logcat 单条 ~4KB 截断,整段一条会被吞尾)。
                std::istringstream ss(layer->dumpLabelLifecycle(filter));
                std::string line;
                while (std::getline(ss, line)) LOGI("%s", line.c_str());
            }
        }
    }
    char perflogProp[4] = {0};
    __system_property_get("debug.ee.perflog", perflogProp);
    const bool perFrameLog = perflogProp[0] == '1';
    // 采样模式:perflog=4 → 每 4 帧打一行(swap 分布用,~15 行/秒,不触发
    // 设备 logcat 配额);perflog=1 → 只打可疑帧。2026-08-21 GPU swap 专项。
    const int beatN = perflogProp[0] == '4' ? 4
                     : perflogProp[0] == '8' ? 8
                                            : 0;
    // 逐帧模式只打"可疑帧":帧间隔/swap/latch 超阈或慢帧 —— 避免全量刷屏
    // 触发设备 logcat 配额丢日志(2026-08-20 实测 DROPPED 把暂停帧吞掉)。
    const bool suspiciousFrame =
        callbackIntervalMs >= 40.0 || swapMs >= 8.0 ||
        gFenceWaitMs >= 40.0 || frameTotalMs >= 25.0;
    const bool logFrame =
        frameId <= 3 || frameId % 120 == 0 ||
        frameTotalMs >= 25.0 || swapMs >= 8.0 ||
        (perFrameLog && suspiciousFrame) ||
        (beatN > 0 && frameId % beatN == 0);
    if (logFrame) {
        const auto& stageDiag = gEngine->diagnostics();
        LOGI(
            "FrameLoop frame=%llu total=%.3f pre=%.3f mvt=%.3f engine=%.3f "
            "post=%.3f swap=%.3f latch=%.2f callback=%.3f cpu=%d hint=%d presented=%d swapOk=%d "
            "upd=%.2f build=%.2f submit=%.2f terrUpd=%.2f "
            "cam=%.2f env=%.2f base=%.2f srender=%.2f endf=%.2f",
            static_cast<unsigned long long>(frameId),
            frameTotalMs,
            preMs,
            stageDiag.mvtVectorUpdateMs,
            engineMs,
            postEngineMs,
            swapMs,
            // C-V8 late-latch 门控等待:≈单帧 GPU 时长 → render-ahead 曾≥2、方案成立;
            // ≈0 → 深度已是 1、这条无收益(该转预测)。engine 会相应从阻塞变纯 CPU。
            gFenceWaitMs,
            callbackIntervalMs,
            // 渲染线程当前所在核心。这条线程是裸 std::thread(无优先级/无亲和/
            // 无 ADPF 提示),Android 不知道它有显示截止期,实测 ~91% 的帧被放在
            // 小核簇(cpu0-3),同样的活 ~8ms 涨到 ~21ms → 错过 16.67ms 预算掉到
            // 30fps。判"卡"先看这个字段,不要先怀疑引擎做多了活。
            sched_getcpu(),
            gRenderThreadPlacement.status().mode ==
                    RenderThreadPlacement::Mode::PerformanceHint
                ? 1
                : 0,
            presented ? 1 : 0,
            swapOk == EGL_TRUE ? 1 : 0,
            stageDiag.sceneUpdateMs,
            stageDiag.renderCommandBuildMs,
            stageDiag.renderSubmitMs,
            stageDiag.terrainUpdateMs,
            stageDiag.cameraUpdateMs,
            stageDiag.environmentUpdateMs,
            stageDiag.basemapStackUpdateMs,
            stageDiag.sceneRenderMs,
            stageDiag.engineEndFrameMs);
        // 北极星 Phase 0 测量台:每帧(采样)打相机真实位姿,消除"nadir/oblique"
        // 猜测——用它标注每个 measure stop 的实际视角。
        const auto& camTrace = gEngine->presentationTrace().camera;
        const Cartographic camPos =
            Ellipsoid::WGS84().cartesianToCartographic(
                gEngine->camera().position());
        LOGI("CamPose frame=%llu center=%.5f,%.5f camH=%.1f targetH=%.1f "
             "pitchDeg=%.2f headingDeg=%.2f camPos=%.5f,%.5f,%.1f",
             static_cast<unsigned long long>(frameId),
             camTrace.targetLongitudeDegrees,
             camTrace.targetLatitudeDegrees,
             camTrace.cameraHeightMeters,
             camTrace.targetHeightMeters,
             camTrace.pitchRadians * 180.0 / M_PI,
             camTrace.headingRadians * 180.0 / M_PI,
             camPos.longitude() * 180.0 / M_PI,
             camPos.latitude() * 180.0 / M_PI,
             camPos.height());
    }

    // ---- 阶段 3/4/5 机制信号 ----
    // 与 CamPose 分开、且**不受 frameId%120 采样限制**:飞行只有 2~3 秒,
    // 按 120 帧采样会整段漏掉。
    if (gEngine) {
        CameraSystem& cam = gEngine->cameraSystem();
        if (gFlightProbe.armed) {
            ++gFlightProbe.frames;
            gFlightProbe.maxProgress =
                std::max(gFlightProbe.maxProgress, cam.cameraFlightProgress());
            const double agl = gEngine->camera().getHeight() -
                               cam.groundState().terrainHeightMeters;
            if (gFlightProbe.frames % 5 == 0 || !cam.cameraFlightActive()) {
                LOGI("StageFlight n=%d t=%.3f agl=%.0f clamps=%llu "
                     "active=%d selfAnim=%d",
                     gFlightProbe.frames, cam.cameraFlightProgress(), agl,
                     static_cast<unsigned long long>(
                         cam.constraintClampCount() -
                         gFlightProbe.clampsAtStart),
                     cam.cameraFlightActive() ? 1 : 0,
                     cam.isSelfAnimating() ? 1 : 0);
            }
            if (!cam.cameraFlightActive()) {
                const auto geo = Ellipsoid::WGS84().cartesianToCartographic(
                    gEngine->camera().position());
                LOGI("StageFlight DONE frames=%d maxT=%.3f clamps=%llu "
                     "landed=%.5f,%.5f,%.0f ctrl=%s",
                     gFlightProbe.frames, gFlightProbe.maxProgress,
                     static_cast<unsigned long long>(
                         cam.constraintClampCount() -
                         gFlightProbe.clampsAtStart),
                     geo.longitudeDegrees(), geo.latitudeDegrees(),
                     geo.height(), cam.activeControllerName().c_str());
                // 阶段 3 第四条判据的**落地帧就绪快照**,原子单行 —— 逐帧
                // LoadQual 会被 logd 冲掉(demo 每帧 CamPose/LoadQual/LoadGate
                // 洪泛),而这一行一次性、且是 dump 前最新,不会丢。就绪率定义:
                //   R_t = real/(real+fill+ell+unk)  地形真数据占比
                //   R_i = sharp/(sharp+a1+a2+a3++miss) 影像本级占比
                // 稳态基线从落地后 settle 的 LoadQual 心跳读(同位姿同缓存温度)。
                const auto& fq = gEngine->diagnostics();
                LOGI("FlightReady LANDING vis=%d src=%d/%d/%d/%d "
                     "img=%d/%d/%d/%d/%d",
                     fq.visibleTiles,
                     fq.terrainSurfaceRealCommands,
                     fq.terrainSurfaceFillProxyCommands,
                     fq.terrainSurfaceEllipsoidCommands,
                     fq.terrainSurfaceUnknownCommands,
                     fq.imageryExactAttachments,
                     fq.imageryAncestor1Attachments,
                     fq.imageryAncestor2Attachments,
                     fq.imageryAncestor3PlusAttachments,
                     fq.imageryMissingTiles);
                gFlightProbe.armed = false;
            }
        }
        if (gCarrier.active && frameId % 30 == 0) {
            const auto& t = cam.tetheredController();
            // ⚠️ 必须用 glm::length():`glm::dvec3::length()` 返回的是**分量
            // 个数(恒 3)**,不是模长。第一版就写成了成员版,读数恒 3.0 —— 而
            // range=1500,看着像"相机贴在载体上"的引擎 bug,实际引擎完全正确。
            const double distToCarrier = glm::length(
                gEngine->camera().position().raw() - gCarrier.position);
            LOGI("StageTether h=%.4f p=%.4f r=%.4f range=%.1f dist=%.1f "
                 "resolved=%d selfAnim=%d ctrl=%s",
                 t.localHeading(), t.localPitch(), t.localRoll(), t.range(),
                 distToCarrier, t.frameResolved() ? 1 : 0,
                 cam.isSelfAnimating() ? 1 : 0,
                 cam.activeControllerName().c_str());
        }
        if (gOrthographic && frameId % 120 == 0) {
            LOGI("StageOrtho isOrtho=%d widthM=%.0f near=%.1f",
                 gEngine->camera().isOrthographic() ? 1 : 0,
                 gEngine->camera().orthographicWidthMeters(),
                 gEngine->camera().nearPlaneMeters());
        }
    }

    // 加载体验记分卡:把"糊/露底/台阶"这些观感症状翻成可 A/B 的计数,免去
    // 靠录屏和主观描述定位。采样策略与 FrameLoop 不同——**暂态期逐帧打、
    // 稳态期心跳打**:糊块/露底只在加载暂态出现,120 帧心跳会整段错过。
    //   sharp/a1/a2/a3+/miss  = 底图「糊几级」直方图:贴本级 / 退回祖先差
    //                           1、2、3+ 级上采样 / 地形瓦片压根没影像。
    //                           a*+miss>0 即"屏幕上有糊块或空块"。
    //   src=real/fill/ell/unk = 地形几何来源 → fill/ell>0 即"露代理面或裸椭球"
    //   z / texZ              = 可见几何 LOD 跨度 / 实际贴上的影像层跨度
    const auto& q = gEngine->diagnostics();
    const bool loadDirty = (q.imageryParentFallbackAttachments > 0 ||
                            q.imageryMissingTiles > 0 ||
                            q.terrainSurfaceFillProxyCommands > 0 ||
                            q.terrainSurfaceEllipsoidCommands > 0);
    static bool sLoadDirtyPrev = false;
    // 暂态期逐帧 + 刚回到干净的那一帧(记 settle 落点)+ 稳态心跳
    if (loadDirty || sLoadDirtyPrev || frameId % 120 == 0) {
        LOGI("LoadQual frame=%llu vis=%d sharp=%d a1=%d a2=%d a3+=%d miss=%d "
             "src=%d/%d/%d/%d geoZ=%d-%d texZ=%d-%d z=%d-%d dirty=%d",
             static_cast<unsigned long long>(frameId),
             q.visibleTiles,
             q.imageryExactAttachments,
             q.imageryAncestor1Attachments,
             q.imageryAncestor2Attachments,
             q.imageryAncestor3PlusAttachments,
             q.imageryMissingTiles,
             q.terrainSurfaceRealCommands,
             q.terrainSurfaceFillProxyCommands,
             q.terrainSurfaceEllipsoidCommands,
             q.terrainSurfaceUnknownCommands,
             q.imageryMinTargetZoom, q.imageryMaxTargetZoom,
             q.imageryMinTextureZoom, q.imageryMaxTextureZoom,
             q.minVisibleZoom, q.maxVisibleZoom,
             loadDirty ? 1 : 0);
        // LoadQual 回答"屏幕上糊不糊",回答不了"为什么还没好"。这条补上收敛
        // 速率的那一半:**每帧闸门是否打满**。判据(见诊断文档§二)——
        //   fin/rasUp/ms 逐帧顶到上限 → 瓶颈在每帧提交闸门(候选 2 成立);
        //   长期不满而 pend/net 有积压 → 瓶颈在网络或 mapping,方向完全不同。
        //   fin    = 主线程地形 finalize 次数/上限
        //   rasUp  = 影像上传单元数/上限
        //   ms     = 主线程加载耗时/预算(demo 配 4.0ms)
        //   pend   = 地形 请求/上传/终态 待处理
        //   net    = 地形 起/完 · 影像 起/完(累计计数,看斜率)
        //   inflt  = 地形/影像 worker 在途 · 传输层上限
        //   prog   = frameLoadProgressPercentage
        LOGI("LoadGate frame=%llu fin=%d/%d rasUp=%d/%d ms=%.2f/%.2f "
             "pend=%d/%d/%d net=%d/%d·%d/%d inflt=%d/%d·%d/%d prog=%.1f "
             "mode=%c%c",
             static_cast<unsigned long long>(frameId),
             q.budgetMainThreadFinalizesUsed, q.budgetMainThreadFinalizesLimit,
             q.budgetRasterUploadsUsed, q.budgetRasterUploadsLimit,
             q.budgetMainThreadElapsedMs, q.budgetMainThreadTimeLimitMs,
             q.pendingTerrainRequests, q.pendingGltfTerrainUploads,
             q.pendingGltfTerrainTerminalResults,
             q.terrainProviderRequestsStarted,
             q.terrainProviderRequestsCompleted,
             q.rasterProviderRequestsStarted,
             q.rasterProviderRequestsCompleted,
             q.terrainProviderActiveWorkerBlockingRequests,
             q.terrainTransportActiveRequestLimit,
             q.rasterProviderActiveWorkerBlockingRequests,
             q.rasterTransportActiveRequestLimit,
             q.frameLoadProgressPercentage,
             q.budgetInteractionActive ? 'I' : '-',
             q.budgetSmoothingActive ? 'S' : '-');
    }
    sLoadDirtyPrev = loadDirty;

    // 破洞诊断(假设 A:选中却零绘制 → 屏幕上这块本帧没有任何几何,看到的是
    // 天空/大气而不是地面)。LoadQual 只回答"糊不糊/是不是代理面",回答不了
    // "有没有一块地压根没画"——这条补上那个缺口。
    //   sel/ent   = 选择器要渲染的瓦片数 / 实际拿到 render entry 的条目数
    //               (sel 明显大于 ent = 有瓦片连 entry 都没有 → 必然是洞)
    //   miss      = entry 走完 draw 却零命令(= 下面三个桶之和)
    //   nofill    = 既无真几何也无 fill 兜底 ← A 的头号嫌疑
    //   fillnc/ctnc = 有 fill / 有真几何,但 draw builder 没产出命令
    //   nulls     = entry 的 selectedTile / renderTile 指针为空
    //   defer     = 本帧主动跳过同步 prep(也不出现,但属预期节流)
    // ⚠ sel>ent 本身**不是**洞:一个祖先 entry 可覆盖多个选中瓦片(finalizer
    // dedup)。真的没几何的只有 finalizer 的两条丢弃路径(dropcu/dropnb)。
    const int holeCount = q.terrainRenderEntriesMissed +
                          q.terrainRenderEntriesMissingSelected +
                          q.terrainRenderEntriesMissingRender +
                          q.terrainRenderEntryDropClipUv +
                          q.terrainRenderEntryDropNotBuildable;
    static bool sHolePrev = false;
    const bool holeDirty = holeCount > 0;
    // 破洞只在加载暂态出现,120 帧心跳会整段错过:暂态期(loadDirty)逐帧打。
    // 裁剪回退活跃期(clip>0)同样逐帧打——接缝细缝与它的相关性要逐帧对齐。
    if (holeDirty || sHolePrev || loadDirty ||
        q.terrainRenderEntriesAncestorFallback > 0 || frameId % 120 == 0) {
        // dropwhy = 几何就没有 / 没建 mapping / 建了但无可用纹理(含祖先)/
        //           texcoord 越界 / 其它;dropz = 被丢瓦片的 zoom 跨度。
        //           nomap 占多 = 时序问题;notex 占多 = 真缺常驻粗影像。
        // clip = 走「祖先裁剪回退」的 entry 数(切缝无裙墙,是运动期瓦片
        // 边界天色细缝的头号嫌疑,与截图逐帧对齐用)。
        LOGI("HoleQual frame=%llu sel=%d ent=%d clip=%d drop=%d/%d "
             "dropwhy=%d/%d/%d/%d/%d dropz=%d-%d miss=%d nofill=%d "
             "fillnc=%d ctnc=%d nulls=%d/%d defer=%d drawn=%d "
             "fade=%d fade0=%d opmin=%.3f clipdeg=%d "
             "hlFull=%d hlDenseRej=%d hlEvict=%d hlEpochMiss=%d hlGridMiss=%d "
             "remap=%d plainClip=%d spanMis=%d spanKey=%d/%d/%d spanR=%.3f/%.3f "
             "hlRes=%d/%d hlDense=%d/%d "
             "dark=%.4f dirty=%d",
             static_cast<unsigned long long>(frameId),
             q.terrainSelectedForRenderTiles,
             q.terrainRenderEntriesPlanned,
             q.terrainRenderEntriesAncestorFallback,
             q.terrainRenderEntryDropClipUv,
             q.terrainRenderEntryDropNotBuildable,
             q.terrainRenderEntryDropNoGeometry,
             q.terrainRenderEntryDropNoMapping,
             q.terrainRenderEntryDropNoReadyTexture,
             q.terrainRenderEntryDropTexcoordInvalid,
             q.terrainRenderEntryDropOther,
             q.terrainRenderEntryDropMinZoom,
             q.terrainRenderEntryDropMaxZoom,
             q.terrainRenderEntriesMissed,
             q.terrainZeroDrawNoContentNoFill,
             q.terrainZeroDrawFillNoCommands,
             q.terrainZeroDrawContentNoCommands,
             q.terrainRenderEntriesMissingSelected,
             q.terrainRenderEntriesMissingRender,
             q.terrainRenderEntriesDeferred,
             q.terrainRenderEntriesDrawn,
             q.terrainRenderEntriesFaded,
             q.terrainRenderEntriesFullyTransparent,
             static_cast<double>(q.terrainRenderEntryMinOpacity),
             q.terrainRenderEntriesClipDegenerate,
             q.terrainHeightLayerFull,
             q.terrainHeightDenseRejected,
             q.terrainHeightEvicted,
             q.terrainHeightEpochMiss,
             q.terrainHeightGridMiss,
             q.terrainSurfaceClipRemap,
             q.terrainSurfaceClipPlain,
             q.terrainTemplateSpanMismatch,
             q.terrainTemplateMismatchZ,
             q.terrainTemplateMismatchX,
             q.terrainTemplateMismatchY,
             static_cast<double>(q.terrainTemplateMismatchLatRatio),
             static_cast<double>(q.terrainTemplateMismatchLonRatio),
             q.terrainHeightCoarseResident,
             q.terrainHeightCoarseCapacity,
             q.terrainHeightDenseResident,
             q.terrainHeightDenseCapacity,
             gEngine->lastFrameDarkFraction(),
             holeDirty ? 1 : 0);
        // notex 细分:z=被丢瓦片层级 load/ready=该 mapping 两个 RasterOverlayTile
        // 的 LoadState(-1=空) tex=ready 手上有没有纹理
        // anc=祖先链深度/其中建了 mapping 的/其中能拿出可画纹理的。
        //   anc=0/*/*     → 这片是根,没祖先可借
        //   anc=N/0/0     → 祖先在但从没建过 mapping
        //   anc=N/M>0/0   → mapping 在、纹理没了(淘汰或没上传)← 淘汰假说
        if (q.terrainRenderEntryDropNoTexZoom >= 0) {
            LOGI("HoleNoTex frame=%llu z=%d load=%d ready=%d tex=%d "
                 "anc=%d/%d/%d mstate=%d upd=%llu tload=%d tkind=%d",
                 static_cast<unsigned long long>(frameId),
                 q.terrainRenderEntryDropNoTexZoom,
                 q.terrainRenderEntryDropNoTexLoadingState,
                 q.terrainRenderEntryDropNoTexReadyState,
                 q.terrainRenderEntryDropNoTexReadyHasTexture,
                 q.terrainRenderEntryDropNoTexAncestorDepth,
                 q.terrainRenderEntryDropNoTexAncestorsWithMapping,
                 q.terrainRenderEntryDropNoTexAncestorsWithTexture,
                 q.terrainRenderEntryDropNoTexMappingState,
                 q.terrainRenderEntryDropNoTexAuthoritativeUpdates,
                 q.terrainRenderEntryDropNoTexTileLoadState,
                 q.terrainRenderEntryDropNoTexTileContentKind);
        }
    }
    sHolePrev = holeDirty;
}

// ============================================================
// 渲染线程：EGL + Engine 全部归本线程所有，帧节拍来自 AChoreographer。
// UI 线程只做事件整形，经任务队列投递到本线程执行；引擎与 GPU 资源
// 在线程退出前、EGL context 仍有效时销毁。
// ============================================================

class RenderThread {
public:
    void start(ANativeWindow* window) {
        // SurfaceView can deliver surfaceCreated again before a matching
        // surfaceDestroyed. A joinable std::thread cannot be overwritten, and
        // the old EGL / engine lifetime must finish before a new one begins.
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.clear();  // 丢弃上一轮 surface 生命周期遗留的任务
        }
        running_.store(true);
        paused_.store(false);
        thread_ = std::thread([this, window]() { threadMain(window); });
    }

    /// 停止并 join。线程内先销毁引擎（需有效 context）再拆 EGL。
    void stop() {
        running_.store(false);
        wake();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /// 投递任务并**置引擎脏位**。
    ///
    /// 脏位置在这里而不是逐个调用点上,是整个 gating 设计的关键:任务队列是
    /// UI 线程改变引擎状态的**唯一**通道(输入、surface 变更、demo 各按钮、
    /// 图层增删),所以"有任务进来"与"有事发生"等价。逐站点加 requestRender
    /// 有 16 个调用点,漏一个的症状是"这个按钮按了没反应",而且只在 gating
    /// 开启时才复现 —— 对齐 maplibre 把事件型脏位收成单入口 `_update()` 的
    /// 理由:枚举脏源应该是"审一个函数",不是"审整个代码库"。
    void post(std::function<void()> task) {
        postInternal(std::move(task), /*markDirty=*/true);
    }

    /// 投递任务并等待其在渲染线程执行完（诊断读取等需要返回值的场景）。
    /// 超时返回 false。任务捕获必须按值 / shared_ptr——超时后任务仍可能
    /// 被执行，引用捕获会悬垂。
    bool runSync(std::function<void()> task, std::chrono::milliseconds timeout) {
        if (!running_.load()) return false;
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        // **不置脏位**:runSync 是只读查询通道(调试面板轮询诊断字符串)。
        // 把它算成"有事发生",面板每秒一问就等于永不空闲 —— 测量台自己
        // 把被测对象顶住了。
        postInternal([done, task = std::move(task)]() {
            task();
            done->set_value();
        }, /*markDirty=*/false);
        return future.wait_for(timeout) == std::future_status::ready;
    }

    void setPaused(bool paused) {
        paused_.store(paused);
        if (!paused) {
            // AChoreographer 绑定注册线程，恢复帧回调必须投递过去做
            post([this]() { postFrameIfNeeded(); });
        }
    }

private:
    void postInternal(std::function<void()> task, bool markDirty) {
        if (!running_.load()) return;  // 线程未运行时任务直接丢弃
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        if (markDirty && gEngine) {
            gEngine->requestRender("task");
        }
        wake();
    }

    void wake() {
        // 持锁 wake：TLS looper 在渲染线程退出时释放，线程退出前在同一把
        // 锁下置空 looper_，保证 wake 期间目标存活（否则跨线程 UAF）
        std::lock_guard<std::mutex> lock(looperMutex_);
        if (looper_) {
            ALooper_wake(looper_);
        }
    }

    static void frameCallbackThunk(long /*frameTimeNanos*/, void* data) {
        static_cast<RenderThread*>(data)->onFrame();
    }

    // 仅渲染线程调用
    void postFrameIfNeeded() {
        if (!running_.load() || paused_.load() || framePending_) return;
        if (!choreographer_) return;
        AChoreographer_postFrameCallback(choreographer_, &frameCallbackThunk, this);
        framePending_ = true;
    }

    void onFrame() {
        framePending_ = false;
        if (!running_.load() || paused_.load()) return;
        applyShadowVerifyRuntimeSwitch();
        // C-V8 late-latch:先等上一帧 GPU 完成(render-ahead≤1),再排输入,
        // 使 latch 到的指位尽量新鲜。等待被从驱动内 draw 提交处挪到此处。
        // ⚠️ 2026-08-20 输入门控:只有本帧确有输入要 latch 才等;惯性/无输入
        // 帧直接回收 fence 不等待,CPU 与 GPU 重叠保帧率(PHK110 实测无条件
        // 等待 30fps,门控后 60fps)。
        if (gInputPending.exchange(false, std::memory_order_acq_rel)) {
            waitPrevFrameFenceForLatch();
        } else {
            if (gPrevFrameFence) {
                glDeleteSync(gPrevFrameFence);
                gPrevFrameFence = nullptr;
            }
            gFenceWaitMs = 0.0;
        }
        drainTasks();   // 输入先于渲染，保证事件同帧生效
        renderFrame();
        // needsFrame() 不是纯查询：会 exchange 事件脏位、消费 landed pulse
        // 并推进 settle 计数。统一留给 ALooper 返回后的 threadMain 调一次，
        // 否则每个逻辑帧会重复消费状态并重复执行 WorkLedger 审计。
    }

    void drainTasks() {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks.swap(tasks_);
        }
        for (auto& task : tasks) {
            task();
        }
    }

    void threadMain(ANativeWindow* window) {
        // 上一轮线程可能带着未消费的帧回调退出（flag 留 true），回调已随
        // 旧线程死亡，不复位则本轮 postFrameIfNeeded 永远早退 → 永久冻屏
        framePending_ = false;
        {
            std::lock_guard<std::mutex> lock(looperMutex_);
            looper_ = ALooper_prepare(0);
        }
        // 渲染线程放置策略。必须在本线程内调用 —— ADPF 登记的是 gettid()。
        // 默认 Options 就是这里要的(60Hz 目标 16.6ms / urgent-display nice)。
        const auto placement = gRenderThreadPlacement.applyToCurrentThread();
        LOGI("RenderThreadPlacement mode=%s pinnedCores=%d nice=%d",
             RenderThreadPlacement::modeName(placement.mode),
             placement.pinnedCoreCount,
             placement.threadNice);
        if (!initEGL(window)) {
            LOGE("Failed to initialize EGL on render thread");
        } else if (!createEngine()) {
            LOGE("Failed to create Engine on render thread");
        }
        choreographer_ = AChoreographer_getInstance();
        // Phase B 平台级唤醒钩子(§0):WorkLedger Landing 令牌在 worker/网络线程
        // 释放时,踹醒停在 ALooper_pollOnce(-1) 的渲染线程去消费落地产物。没有
        // 它,ledger gating 下 Landing 挂着会真睡且再也醒不过来。wake() 跨线程安全
        // (looperMutex_ 保护);Engine 析构时清除该回调(见 Engine::~Engine)。
        if (gEngine) {
            gEngine->setFrameRequestCallback([this]() { wake(); });
        }
        postFrameIfNeeded();

        while (running_.load()) {
            int events = 0;
            void* data = nullptr;
            // 帧回调在 pollOnce 内部分发；post()/stop() 经 ALooper_wake 唤醒
            ALooper_pollOnce(-1, nullptr, &events, &data);
            drainTasks();
            // gating 开启后线程会停在上面那句 pollOnce 上,帧回调不再自续。
            // 任务(输入事件等)只把脏位置上,真正把循环重新拉起来的是这里 ——
            // 漏了它,输入进得来但画面不动。这也是每轮 Looper 唯一一次
            // needsFrame() 判定，确保事件型状态恰消费一次。
            if ((!gEngine || !gEngine->frameGatingEnabled() ||
                 gEngine->needsFrame())) {
                postFrameIfNeeded();
            }
        }

        drainTasks();
        // 与 applyToCurrentThread 配对:ADPF session 绑的是本线程 tid,线程退出前
        // 关掉。
        gRenderThreadPlacement.release();
        // destroyEGL 内部先清引擎对象（GPU 资源析构需当前 context），再拆 EGL
        destroyEGL();
        choreographer_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(looperMutex_);
            looper_ = nullptr;
        }
    }

    std::thread thread_;
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::mutex looperMutex_;
    ALooper* looper_ = nullptr;  // looperMutex_ 保护；渲染线程退出前置空
    AChoreographer* choreographer_ = nullptr;  // 仅渲染线程访问
    bool framePending_ = false;                // 仅渲染线程访问
};

static RenderThread gRenderThread;

// ---- C2 步骤5:高德矢量瓦片垂直切片 ----
// 独立线程 + 阻塞请求(getBlocking 是影像/地形已验证路径;异步链在本机
// 调度器上未触发,先绕开):版本探测(GET)→ get_tile(POST)→ 签名 URL(GET)
// → 解码/转换(工作线程,纯 CPU)→ gRenderThread.post 灌 FeatureStore。
// UI 线程整形好的输入事件统一从这里投递到渲染线程。
// 屏幕密度（Java surfaceChanged 时设置）。手势阈值以 dp 定义，InputManager
// 用 event.devicePixelRatio 把 dp 换算成物理像素——不填则恒 1，latch 阈值
// 在高密度屏上会偏敏感 density 倍。

static void postInputEvent(const InputEvent& event) {
    InputEvent stamped = event;
    stamped.devicePixelRatio = gDisplayDensity.load();
    gInputPending.store(true, std::memory_order_release);
    gRenderThread.post([stamped]() {
        if (gEngine) {
            gEngine->onInputEvent(stamped);
            gEngine->requestRender("input");
        }
    });
}

// ============================================================
// JNI 桥接
// ============================================================

extern "C" {

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeInit(
    JNIEnv* env, jclass /* clazz */, jobject appContext) {
    if (gAppContext) {
        env->DeleteGlobalRef(gAppContext);
        gAppContext = nullptr;
    }
    if (appContext) {
        gAppContext = env->NewGlobalRef(appContext);
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceCreated(
    JNIEnv* env, jobject /* this */, jobject surface) {
    // Some devices recreate the Surface without first issuing a matching
    // surfaceDestroyed. Finish the prior native lifetime before replacing the
    // ANativeWindow so no render thread or EGL context remains attached to it.
    gRenderThread.stop();
    if (gWindow) {
        ANativeWindow_release(gWindow);
        gWindow = nullptr;
    }
    gWindow = ANativeWindow_fromSurface(env, surface);
    if (!gWindow) {
        LOGE("ANativeWindow_fromSurface failed");
        return;
    }
    // EGL / Engine 全部在渲染线程内创建
    gRenderThread.start(gWindow);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSetDisplayDensity(
    JNIEnv* /* env */, jobject /* this */, jfloat density) {
    gDisplayDensity = density > 0.1f ? density : 1.0f;
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceChanged(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    gWidth = width;
    gHeight = height;
    gRenderThread.post([width, height]() {
        if (gEngine) {
            gEngine->onSurfaceChanged(width, height, gDisplayDensity.load());
        }
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceDestroyed(
    JNIEnv* /* env */, jobject /* this */) {
    cancelInputIfNeeded();
    gRenderThread.stop();  // join；引擎与 EGL 已在线程内销毁
    if (gWindow) {
        ANativeWindow_release(gWindow);
        gWindow = nullptr;
    }
}

// 辅助：通过 JNI 获取 Android 单调时钟（秒）
static double androidUptimeSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) * 1e-9;
}

static void endDebugPinchIfNeeded(float centerX, float centerY) {
    if (!gDebugPinchActive) return;

    InputEvent event;
    event.type = InputEvent::Type::PinchEnd;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
    gDebugPinchActive = false;
}

static void beginDebugPinchIfNeeded(float centerX,
                                    float centerY,
                                    double timestamp) {
    if (gDebugPinchActive) return;

    InputEvent event;
    event.type = InputEvent::Type::PinchStart;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.timestamp = timestamp;
    postInputEvent(event);
    gDebugPinchActive = true;
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeTouchDown(
    JNIEnv* /* env */, jobject /* this */, jfloat x, jfloat y) {
    endDebugPinchIfNeeded(static_cast<float>(gWidth) * 0.5f,
                          static_cast<float>(gHeight) * 0.5f);
    gTouching = true;
    gDragStarted = false;
    gTouchMoved = false;

    // 真按下即投递 PointerDown。此前只在 nativeDrag 的首个 move 里补发，
    // 于是"按下即抬手"的纯点击只到达一个 PointerUp，InputManager 处在 Idle
    // 直接早退 —— Android 上 Click / DoubleClick 从未触发过（单击选中、
    // 双击缩放全是死的）。iOS 侧 touchesBegan 一直是正常发的。
    InputEvent event;
    event.type = InputEvent::Type::PointerDown;
    event.screenX = x;
    event.screenY = y;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
    gDragStarted = true;  // nativeDrag 不必再补发
}

// 双指抬起一指后，剩余手指续接单指拖拽。刻意不投递 PointerDown：这一段是
// 多指手势的尾巴，不应产生 click/double-click；PointerDown 仍由 nativeDrag
// 的首个 move 补发（=改动前的行为）。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResumePointer(
    JNIEnv* /* env */, jobject /* this */) {
    endDebugPinchIfNeeded(static_cast<float>(gWidth) * 0.5f,
                          static_cast<float>(gHeight) * 0.5f);
    gTouching = true;
    gDragStarted = false;
    gTouchMoved = false;
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDrag(
    JNIEnv* /* env */, jobject /* this */,
    jfloat startX, jfloat startY, jfloat endX, jfloat endY,
    jint /*width*/, jint /*height*/) {
    gTouchMoved = true;

    double ts = androidUptimeSeconds();

    if (!gDragStarted) {
        gDragStarted = true;
        InputEvent event;
        event.type = InputEvent::Type::PointerDown;
        event.screenX = startX;
        event.screenY = startY;
        event.pointerType = InputEvent::PointerType::Touch;
        event.timestamp = ts;
        postInputEvent(event);
    }

    InputEvent event;
    event.type = InputEvent::Type::PointerMove;
    event.screenX = endX;
    event.screenY = endY;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = ts;
    postInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeTouchUp(
    JNIEnv* /* env */, jobject /* this */, jfloat x, jfloat y) {
    gTouching = false;

    double ts = androidUptimeSeconds();

    InputEvent upEvent;
    upEvent.type = InputEvent::Type::PointerUp;
    upEvent.screenX = x;
    upEvent.screenY = y;
    upEvent.pointerType = InputEvent::PointerType::Touch;
    upEvent.timestamp = ts;
    postInputEvent(upEvent);

    // 诊断日志（pick 和选择由 InputManager → Scene 回调处理）；
    // pick 读渲染态，投递到渲染线程执行
    if (!gTouchMoved) {
        gRenderThread.post([x, y]() {
            if (!gEngine) return;
            PickResult result = gEngine->pick(x, y);
            if (result.isValid()) {
                const double lngDeg = result.cartographic.longitudeDegrees();
                const double latDeg = result.cartographic.latitudeDegrees();
                LOGI("Tap at (%.0f,%.0f) → lng=%.6f lat=%.6f height=%.2f "
                     "layer=%s feature=%s",
                     x, y, lngDeg, latDeg,
                     result.cartographic.height(),
                     result.layerId.c_str(),
                     result.featureId.c_str());
            } else {
                LOGI("Tap at (%.0f,%.0f) → no hit", x, y);
            }
        });
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchStart(
    JNIEnv* /* env */, jobject /* this */, jfloat centerX, jfloat centerY) {
    endDebugPinchIfNeeded(centerX, centerY);
    gTouching = true;
    gDragStarted = false;
    gTouchMoved = true;

    InputEvent event;
    event.type = InputEvent::Type::PinchStart;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchEnd(
    JNIEnv* /* env */, jobject /* this */, jfloat centerX, jfloat centerY) {
    gTouching = false;

    InputEvent event;
    event.type = InputEvent::Type::PinchEnd;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchRotateTilt(
    JNIEnv* /* env */, jobject /* this */,
    jfloat scale, jfloat rotationRadians,
    jfloat centerX, jfloat centerY, jfloat centerDx, jfloat centerDy,
    jfloat pointer0X, jfloat pointer0Y, jfloat pointer1X, jfloat pointer1Y,
    jint /*width*/, jint /*height*/) {
    InputEvent event;
    event.type = InputEvent::Type::PinchMove;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = scale;
    event.rotationRadians = rotationRadians;
    event.centerDeltaX = centerDx;
    event.centerDeltaY = centerDy;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.hasPointerPair = true;
    event.pointer0X = pointer0X;
    event.pointer0Y = pointer0Y;
    event.pointer1X = pointer1X;
    event.pointer1Y = pointer1Y;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugTilt(
    JNIEnv* /* env */, jobject /* this */,
    jfloat centerDy, jint width, jint height) {
    const float centerX = static_cast<float>(width) * 0.5f;
    const float centerY = static_cast<float>(height) * 0.5f;
    const double timestamp = androidUptimeSeconds();
    beginDebugPinchIfNeeded(centerX, centerY, timestamp);

    InputEvent move;
    move.type = InputEvent::Type::PinchMove;
    move.screenX = centerX;
    move.screenY = centerY;
    move.pinchScale = 1.0f;
    move.centerDeltaY = centerDy;
    move.pointerType = InputEvent::PointerType::Touch;
    move.pointerCount = 2;
    move.timestamp = timestamp + 0.016;
    postInputEvent(move);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugPinchEnd(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    endDebugPinchIfNeeded(
        static_cast<float>(width) * 0.5f,
        static_cast<float>(height) * 0.5f);
}

// 确定性双指路径回放：adb 无法产生多点触控，这里合成固定的两指像素序列
// （含 pointer pair → 走 InputManager latch + 新契约完整链路），供手势回归
// 复测与将来重新插桩时复用。数字键 1-4 触发（见 Java onKeyDown）。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugPinchPath(
    JNIEnv* /* env */, jobject /* this */,
    jint scenario, jint width, jint height) {
    const float cx = static_cast<float>(width) * 0.5f;
    const float cy = static_cast<float>(height) * 0.5f;
    const float halfSpread = 300.0f;
    const double t0 = androidUptimeSeconds();

    auto postMove = [&](int step, float p0x, float p0y, float p1x, float p1y) {
        InputEvent move;
        move.type = InputEvent::Type::PinchMove;
        move.screenX = (p0x + p1x) * 0.5f;
        move.screenY = (p0y + p1y) * 0.5f;
        move.pinchScale = 1.0f;  // 新契约不消费；派生量由 InputManager 计算
        move.pointerType = InputEvent::PointerType::Touch;
        move.pointerCount = 2;
        move.hasPointerPair = true;
        move.pointer0X = p0x;
        move.pointer0Y = p0y;
        move.pointer1X = p1x;
        move.pointer1Y = p1y;
        move.timestamp = t0 + 0.016 * static_cast<double>(step);
        postInputEvent(move);
    };

    {
        InputEvent start;
        start.type = InputEvent::Type::PinchStart;
        start.screenX = cx;
        start.screenY = cy;
        start.pinchScale = 1.0f;
        start.pointerType = InputEvent::PointerType::Touch;
        start.pointerCount = 2;
        start.timestamp = t0;
        postInputEvent(start);
    }

    constexpr int kSteps = 45;
    for (int i = 0; i <= kSteps; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(kSteps);
        switch (scenario) {
            case 0: {  // 纯刚性 pan：质心横移 300px
                const float dx = 300.0f * f;
                postMove(i, cx - halfSpread + dx, cy,
                            cx + halfSpread + dx, cy);
                break;
            }
            case 1: {  // Pitch：双指平行上推 240px
                const float dy = -240.0f * f;
                postMove(i, cx - halfSpread, cy + dy,
                            cx + halfSpread, cy + dy);
                break;
            }
            case 2: {  // 组合：缩放 1.5×+拧 0.5rad+质心斜移
                const float r = halfSpread * (1.0f + 0.5f * f);
                const float a = 0.5f * f;
                const float mx = cx + 150.0f * f;
                const float my = cy - 100.0f * f;
                postMove(i, mx - r * std::cos(a), my - r * std::sin(a),
                            mx + r * std::cos(a), my + r * std::sin(a));
                break;
            }
            default: {  // 3: 慢拧+微缩放（阈值附近，验 latch 后无模式翻转）
                const float a = 0.3f * f;
                const float wobble = 1.0f + 0.02f * ((i % 2 == 0) ? 1.0f : -1.0f);
                const float r = halfSpread * wobble;
                postMove(i, cx - r * std::cos(a), cy - r * std::sin(a),
                            cx + r * std::cos(a), cy + r * std::sin(a));
                break;
            }
        }
    }

    {
        InputEvent end;
        end.type = InputEvent::Type::PinchEnd;
        end.screenX = cx;
        end.screenY = cy;
        end.pinchScale = 1.0f;
        end.pointerType = InputEvent::PointerType::Touch;
        end.pointerCount = 2;
        end.timestamp = t0 + 0.016 * static_cast<double>(kSteps + 1);
        postInputEvent(end);
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugZoom(
    JNIEnv* /* env */, jobject /* this */,
    jfloat scale, jint width, jint height) {
    const float centerX = static_cast<float>(width) * 0.5f;
    const float centerY = static_cast<float>(height) * 0.5f;
    const double ts = androidUptimeSeconds();
    beginDebugPinchIfNeeded(centerX, centerY, ts);

    InputEvent move;
    move.type = InputEvent::Type::PinchMove;
    move.screenX = centerX;
    move.screenY = centerY;
    move.pinchScale = scale;
    move.pointerType = InputEvent::PointerType::Touch;
    move.pointerCount = 2;
    move.timestamp = ts + 0.016;
    postInputEvent(move);

    // 诊断快照读渲染态，投递到渲染线程打印
    gRenderThread.post([scale]() {
    if (!gEngine) return;
    const auto& diag = gEngine->diagnostics();
    const auto& trace = gEngine->presentationTrace();
    const auto& cameraTrace = trace.camera;
    const double cameraRadius = gEngine->camera().position().length();
    const double sphericalAltitude = cameraRadius - 6378137.0;
    const double ellipsoidAltitude =
        Ellipsoid::WGS84().cartesianToCartographic(gEngine->camera().position()).height();
    LOGI("Debug zoom scale=%.2f | tiles vis=%d cached=%d renderSurface=%d "
         "exact=%d parent=%d missing=%d unsupported=%d kicked=%d retained=%d "
         "entry plan=%d/%d draw=%d/%d miss=%d/%d defer=%d/%d fallback=%d prep=%d/%d surface=%d src=%d/%d/%d/%d "
         "z=%d-%d targetZ=%d-%d texZ=%d-%d lod=%.0f eq=%d qRender=%d qWalk=%d qBal=%d "
         "qFrustum=%d qHz=%d qEq2=%d grp=%d/%d/%d "
         "center=%.6f,%.6f targetH=%.2f camH=%.2f pitch=%.6f heading=%.6f vp=%dx%d "
         "ellAlt=%.2f sphAlt=%.2f radius=%.2f FPS=%.1f draw=%d",
         scale,
         diag.visibleTiles,
         diag.cachedTextures,
         diag.renderSurfaceTiles,
         diag.imageryExactAttachments,
         diag.imageryParentFallbackAttachments,
         diag.imageryMissingTiles,
         diag.imageryUnsupportedTiles,
         diag.imageryKickedTiles,
         diag.imageryAncestorRetainedTiles,
         diag.terrainRenderEntriesPlanned,
         diag.terrainRenderEntriesSelectedPlanned,
         diag.terrainRenderEntriesDrawn,
         diag.terrainRenderEntriesSelectedDrawn,
         diag.terrainRenderEntriesMissed,
         diag.terrainRenderEntriesSelectedMissed,
         diag.terrainRenderEntriesDeferred,
         diag.terrainRenderEntriesSelectedDeferred,
         diag.terrainRenderEntriesAncestorFallback,
         diag.terrainRenderEntriesSynchronousPrep,
         diag.terrainRenderEntriesDeferredPrep,
         diag.terrainSurfaceCommandsSubmitted,
         diag.terrainSurfaceRealCommands,
         diag.terrainSurfaceFillProxyCommands,
         diag.terrainSurfaceEllipsoidCommands,
         diag.terrainSurfaceUnknownCommands,
         diag.minVisibleZoom,
         diag.maxVisibleZoom,
         diag.imageryMinTargetZoom,
         diag.imageryMaxTargetZoom,
         diag.imageryMinTextureZoom,
         diag.imageryMaxTextureZoom,
         diag.lodSizePixels,
         diag.quadtreeEqualZoomLayers,
         diag.quadtreeRenderingNodes,
         diag.quadtreeWalkthroughNodes,
         diag.quadtreeNeighborBalancedTiles,
         diag.quadtreeInFrustumNodes,
         diag.quadtreeHorizonTangentPreservedNodes,
         diag.quadtreeEqualZoomSecondPassNodes,
         diag.mercatorTileCount,
         diag.northPolarTileCount,
         diag.southPolarTileCount,
         cameraTrace.targetLongitudeDegrees,
         cameraTrace.targetLatitudeDegrees,
         cameraTrace.targetHeightMeters,
         cameraTrace.cameraHeightMeters,
         cameraTrace.pitchRadians,
         cameraTrace.headingRadians,
         cameraTrace.viewportWidthPixels,
         cameraTrace.viewportHeightPixels,
         ellipsoidAltitude,
         sphericalAltitude,
         cameraRadius,
         diag.fps,
         diag.drawCalls);
    });
}

// ============================================================
// Debug panel JNI
// ============================================================

// 渲染线程上执行：读 gEngine 各状态面拼诊断文本
static std::string buildDiagnosticsText() {
    const auto& diag = gEngine->diagnostics();
    const double cameraRadius = gEngine->camera().position().length();
    const double sphericalAltitude = cameraRadius - 6378137.0;
    const double ellipsoidAltitude =
        Ellipsoid::WGS84().cartesianToCartographic(gEngine->camera().position()).height();
    const double cameraDist = gEngine->camera().position().distanceTo(Vec3::zero());

    char buf[4096];
    snprintf(buf, sizeof(buf),
        "FPS: %.1f  |  Frame: %.1f ms\n"
        "CPU: %.1f ms  |  begin %.1f upd %.1f build %.1f submit %.1f end %.1f\n"
        "Update: cam %.1f env %.1f base %.1f terr %.1f content %.1f\n"
        "Draw calls: %d  |  GPU tex: %d  |  glTF prim: %d\n"
        "Visible tiles: terrain %d content %d/%d  |  Cached: %d\n"
        "Surface meshes: %d (%d terrSurfCmd, %d terrGltfCmd)\n"
        "Terrain surface src: real %d, fill %d, ell %d, unk %d\n"
        "Attachments: %d exact, %d parent, %d missing, %d unsup, %d kicked, %d retained\n"
        "Zoom: %d-%d  |  Img: %d-%d -> tex %d-%d\n"
        "LOD: %.0f px  |  EqZoom: %d\n"
        "QuadTree: %d render, %d walk, %d frustum, %d balanced\n"
        "Occlusion: %d occ, %d wait, %d culled-vis\n"
        "Groups: %d merc, %d N, %d S\n"
        "Camera: ellAlt=%.0fm sphAlt=%.0fm dist=%.0fm\n"
        "LoadQ: %d pre, %d norm, %d urgent  |  Terrain pending: %d req, %d upload, %d terminal\n"
        "Content pending: %d req, %d upload, %d terminal\n"
        "ReqDrop: iss %d%s | key %d dup %d empty %d cls %d upSrc %d upNoC %d disp %d noProv %d stop %d\n"
        "Budget: net %d/%d, terrain-content %d/%d, content %d/%d, raster %d/%d\n"
        "Main budget: fin %d/%d, term %d/%d, rasUp %d/%d, %.1f/%.1f ms, mode %c/%c\n"
        "Provider: terr %d/%d wb %d/%d | cont %d/%d wb %d/%d ext %d/%d | rast %d/%d wb %d/%d\n"
        "Transport limit: terrain %d, content %d, raster %d\n"
        "LoadState: unloading %d, retry %d, unloaded %d, loading %d, loaded %d, done %d, failed %d\n"
        "Content: unknown %d, empty %d, external %d, render %d  |  UnloadQ: %d\n"
        "Raster overlay: missing projections %d\n"
        "Mesh: %d KB  (gen %llu)",
        diag.fps, diag.frameTimeMs,
        diag.engineFrameCpuMs,
        diag.engineBeginFrameMs,
        diag.sceneUpdateMs,
        diag.renderCommandBuildMs,
        diag.renderSubmitMs,
        diag.engineEndFrameMs,
        diag.cameraUpdateMs,
        diag.environmentUpdateMs,
        diag.basemapStackUpdateMs,
        diag.terrainUpdateMs,
        diag.contentTilesetUpdateMs,
        diag.drawCalls, diag.gpuTextureCount, diag.renderGltfPrimitives,
        diag.visibleTiles, diag.contentVisibleTiles, diag.contentTilesets,
        diag.cachedTextures,
        diag.surfaceMeshCount,
        diag.terrainSurfaceTileCommands, diag.terrainGltfPrimitiveCommands,
        diag.terrainSurfaceRealCommands,
        diag.terrainSurfaceFillProxyCommands,
        diag.terrainSurfaceEllipsoidCommands,
        diag.terrainSurfaceUnknownCommands,
        diag.imageryExactAttachments, diag.imageryParentFallbackAttachments,
        diag.imageryMissingTiles, diag.imageryUnsupportedTiles,
        diag.imageryKickedTiles,
        diag.imageryAncestorRetainedTiles,
        diag.minVisibleZoom, diag.maxVisibleZoom,
        diag.imageryMinTargetZoom, diag.imageryMaxTargetZoom,
        diag.imageryMinTextureZoom, diag.imageryMaxTextureZoom,
        diag.lodSizePixels, diag.quadtreeEqualZoomLayers,
        diag.quadtreeRenderingNodes, diag.quadtreeWalkthroughNodes,
        diag.quadtreeInFrustumNodes,
        diag.quadtreeNeighborBalancedTiles,
        diag.quadtreeSelectionOccludedNodes,
        diag.quadtreeSelectionWaitingForOcclusionResultsNodes,
        diag.quadtreeCulledTilesVisited,
        diag.mercatorTileCount, diag.northPolarTileCount,
        diag.southPolarTileCount,
        ellipsoidAltitude, sphericalAltitude, cameraDist,
        diag.loadQueuePreloadRequests,
        diag.loadQueueNormalRequests,
        diag.loadQueueUrgentRequests,
        diag.pendingTerrainRequests,
        diag.pendingGltfTerrainUploads,
        diag.pendingGltfTerrainTerminalResults,
        diag.pendingContentRequests,
        diag.pendingContentUploads,
        diag.pendingContentTerminalResults,
        diag.requestIssued,
        diag.requestBlockedByInflight ? " BLK" : "",
        diag.reqSkipEmptyKey,
        diag.reqSkipAlreadyPending,
        diag.reqSkipEmptyTile,
        diag.reqSkipClassified,
        diag.reqSkipUpsampleSrc,
        diag.reqSkipUpsampleNoContent,
        diag.reqSkipDispatch,
        diag.reqSkipNoProvider,
        diag.reqStopDispatch,
        diag.budgetNetworkRequestsIssued,
        diag.budgetNetworkRequestsLimit,
        diag.budgetTerrainContentNetworkRequestsIssued,
        diag.budgetTerrainContentNetworkRequestsLimit,
        diag.budgetContentNetworkRequestsIssued,
        diag.budgetContentNetworkRequestsLimit,
        diag.budgetRasterNetworkRequestsIssued,
        diag.budgetRasterNetworkRequestsLimit,
        diag.budgetMainThreadFinalizesUsed,
        diag.budgetMainThreadFinalizesLimit,
        diag.budgetTerminalStateTransitionsUsed,
        diag.budgetTerminalStateTransitionsLimit,
        diag.budgetRasterUploadsUsed,
        diag.budgetRasterUploadsLimit,
        diag.budgetMainThreadElapsedMs,
        diag.budgetMainThreadTimeLimitMs,
        diag.budgetInteractionActive ? 'I' : '-',
        diag.budgetSmoothingActive ? 'S' : '-',
        diag.terrainProviderRequestsStarted,
        diag.terrainProviderRequestsCompleted,
        diag.terrainProviderActiveWorkerBlockingRequests,
        diag.terrainProviderPeakWorkerBlockingRequests,
        diag.contentProviderRequestsStarted,
        diag.contentProviderRequestsCompleted,
        diag.contentProviderActiveWorkerBlockingRequests,
        diag.contentProviderPeakWorkerBlockingRequests,
        diag.contentProviderExternalResourceRequestsStarted,
        diag.contentProviderExternalResourceRequestsCompleted,
        diag.rasterProviderRequestsStarted,
        diag.rasterProviderRequestsCompleted,
        diag.rasterProviderActiveWorkerBlockingRequests,
        diag.rasterProviderPeakWorkerBlockingRequests,
        diag.terrainTransportActiveRequestLimit,
        diag.contentTransportActiveRequestLimit,
        diag.rasterTransportActiveRequestLimit,
        diag.terrainLoadUnloadingTiles,
        diag.terrainLoadFailedTemporarilyTiles,
        diag.terrainLoadUnloadedTiles,
        diag.terrainLoadContentLoadingTiles,
        diag.terrainLoadContentLoadedTiles,
        diag.terrainLoadDoneTiles,
        diag.terrainLoadFailedTiles,
        diag.terrainContentUnknownTiles,
        diag.terrainContentEmptyTiles,
        diag.terrainContentExternalTiles,
        diag.terrainContentRenderTiles,
        diag.terrainUnloadQueueTiles,
        diag.missingRasterOverlayProjections,
        diag.surfaceMeshBytes / 1024,
        static_cast<unsigned long long>(diag.terrainGeneration));
    std::string text(buf);
    text += "\n";
    text += minimal_globe_demo::buildRenderEntryDiagnosticsLine(diag);
    text += minimal_globe_demo::buildPresentationTraceSummary(
        gEngine->presentationTrace());
    return text;
}

JNIEXPORT jstring JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetDiagnosticsString(
    JNIEnv* env, jobject /* this */) {
    // 同步投递到渲染线程读取；shared_ptr 捕获防超时后悬垂
    auto text = std::make_shared<std::string>();
    const bool ok = gRenderThread.runSync(
        [text]() {
            *text = gEngine ? buildDiagnosticsText()
                            : std::string("Engine not ready");
        },
        std::chrono::milliseconds(100));
    return env->NewStringUTF(ok ? text->c_str() : "Engine not ready");
}

// 指北针：读取每帧发布的相机方位角(弧度,0=正北,顺时针+)。
JNIEXPORT jfloat JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetHeadingRadians(
    JNIEnv* /* env */, jobject /* this */) {
    return gHeadingRadians.load();
}

// 高德式比例尺：在屏幕中心横向采样 100px，以两条拾取射线和 WGS84 椭球的
// 交点测量地表距离。这样透视、正交和轻微倾斜共用同一投影契约；UI 只需低频
// 查询，不介入相机状态，也不增加瓦片或 GPU 工作。
JNIEXPORT jdouble JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetMetersPerPixel(
    JNIEnv* /* env */, jclass /* clazz */) {
    auto metersPerPixel = std::make_shared<double>(0.0);
    const bool ok = gRenderThread.runSync(
        [metersPerPixel]() {
            if (!gEngine) return;
            const int width = gWidth.load();
            const int height = gHeight.load();
            if (width < 2 || height < 2) return;

            constexpr double kSamplePixels = 100.0;
            const double halfSample =
                std::min(kSamplePixels * 0.5, width * 0.25);
            if (halfSample <= 0.0) return;

            const Camera& camera = gEngine->camera();
            const double centerX = width * 0.5;
            const double centerY = height * 0.5;
            const Ray leftRay = camera.getPickRay(
                centerX - halfSample, centerY, width, height);
            const Ray rightRay = camera.getPickRay(
                centerX + halfSample, centerY, width, height);
            const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
            const std::optional<Vec3> left = ellipsoid.rayIntersection(
                leftRay.origin(), leftRay.direction());
            const std::optional<Vec3> right = ellipsoid.rayIntersection(
                rightRay.origin(), rightRay.direction());
            if (!left || !right) return;

            const Cartographic leftGeo =
                ellipsoid.cartesianToCartographic(*left);
            const Cartographic rightGeo =
                ellipsoid.cartesianToCartographic(*right);
            const GeodesicInverseResult distance =
                ellipsoid.inverse(leftGeo, rightGeo);
            const double sampleWidth = halfSample * 2.0;
            if (distance.converged && std::isfinite(distance.distanceMeters) &&
                distance.distanceMeters > 0.0) {
                *metersPerPixel = distance.distanceMeters / sampleWidth;
            }
        },
        std::chrono::milliseconds(40));
    return ok ? static_cast<jdouble>(*metersPerPixel) : 0.0;
}

// 复位正北朝上（在渲染线程执行，读写相机态）。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResetNorthUp(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (gEngine) gEngine->resetNorthUp();
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResetCamera(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gSdkFacade) return;
        gSdkFacade->resetCamera();
        LOGI("Camera reset to Chongqing demo viewpoint");
    });
}

// 阶段 3:飞到北京。机制信号 = 飞行期逐帧 progress + 碰撞钳位次数增量,
// 落地打终点位姿误差。⚠️钳位次数**必须为 0** —— 那是"拱高让钳位结构性不
// 触发"的判据;非 0 说明是钳位在兜底(画面上两者完全看不出区别)。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugFlyTo(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        CameraSystem& cam = gEngine->cameraSystem();
        Viewpoint dest;
        dest.eyeGeo = Cartographic::fromDegrees(kFlightDestLng, kFlightDestLat,
                                                kFlightDestAlt);
        dest.headingRadians = 0.0;
        dest.pitchRadians = -0.6;
        dest.rollRadians = 0.0;

        // 先算"直接设过去"的落点作参照:飞过去必须落在同一处
        // (setViewpoint 与 flyTo 共用 resolveViewpoint,分岔就会在这里露出来)。
        const Vec3 before = gEngine->camera().position();
        cam.setViewpoint(dest);
        const Vec3 expected = gEngine->camera().position();
        Viewpoint back;
        back.eyeGeo = Ellipsoid::WGS84().cartesianToCartographic(before);
        cam.setViewpoint(back);

        gFlightProbe = FlightProbe{};
        gFlightProbe.clampsAtStart = cam.constraintClampCount();
        const bool started = cam.flyTo(dest);
        gFlightProbe.armed = started;
        LOGI("StageFlight start=%d expectEye=%.1f,%.1f,%.1f dist=%.0fm",
             started ? 1 : 0, expected.x(), expected.y(), expected.z(),
             (expected - before).length());
    });
}

// 阶段 5 的真实用途:可复现的**正俯视**位姿。掠视下的正交是退化用例
// (正交盒半高远大于相机高度 ⇒ 下半部整个在地下 ⇒ 天空色),俯视才是正交要干的活。
//
// 走 setViewpoint 的「部分 viewpoint」语义,顺带在设备上验阶段 2 的**万向节约定**:
// pitch 恰好 −π/2 是奇点(direction 沿天底,绕它转不改视线 ⇒ heading 只能由 up 定,
// 约定 roll=0)。回读 currentViewpoint() 打出来,位姿往返在真机上也必须闭合。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugTether(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        CameraSystem& cam = gEngine->cameraSystem();
        if (!gCarrier.active) {
            gCarrier.active = true;
            gCarrier.useOrientation = false;
        } else if (!gCarrier.useOrientation) {
            gCarrier.useOrientation = true;
        } else {
            gCarrier.active = false;
            cam.selectController(CameraSystem::kFreeGlobeController);
            LOGI("StageTether off (back to free)");
            return;
        }
        gCarrier.step(0.0);          // 先落一次位置,避免首帧 provider 拿到零

        ViewpointFrame frame;
        frame.originProvider = [](glm::dvec3& out) {
            if (!gCarrier.active) return false;
            out = gCarrier.position;
            return true;
        };
        if (gCarrier.useOrientation) {
            frame.orientationProvider = [](glm::dmat3& out) {
                if (!gCarrier.active) return false;
                out = gCarrier.orientation;
                return true;
            };
        }
        cam.tetheredController().setFrame(frame);
        cam.selectController(CameraSystem::kTetheredController);
        cam.tetheredController().setRange(1500.0);
        LOGI("StageTether on orientationProvider=%d",
             gCarrier.useOrientation ? 1 : 0);
    });
}

// 阶段 5:正交/透视切换。切换瞬间把正交宽度设成"透视在当前地面距离处的
// 足迹",两种投影的画面才可比 —— 否则一切过去尺度全变,看不出别的问题。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugToggleOrtho(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    const double w = static_cast<double>(width);
    const double h = static_cast<double>(height);
    gRenderThread.post([w, h]() {
        if (!gEngine) return;
        Camera& camera = gEngine->camera();
        if (gOrthographic) {
            camera.setPerspective(camera.verticalFovRadians(),
                                  camera.nearPlaneMeters(),
                                  camera.farPlaneMeters());
            gOrthographic = false;
            LOGI("StageOrtho off isOrtho=%d", camera.isOrthographic() ? 1 : 0);
            return;
        }
        // 视线与椭球求交拿地面距离;不交(看天)则退回相机椭球高。
        const Ray ray(camera.position(), camera.direction());
        const std::optional<Vec3> hit =
            Ellipsoid::WGS84().rayIntersection(ray.origin(), ray.direction());
        // 同上:glm 成员 length() 是分量个数。这里写错会让正交宽度变成
        // 2·3·tan(fov/2)·aspect ≈ 几米,画面直接糊死。
        const double distance =
            hit ? glm::length(hit->raw() - camera.position().raw())
                : camera.getHeight();
        const double aspect = h > 0.0 ? w / h : 1.0;
        const double widthMeters =
            2.0 * distance * std::tan(camera.verticalFovRadians() * 0.5) *
            aspect;
        // ⚠️ near 显式给定:正交下动态 near 已在 SceneFrameUpdateCoordinator
        // 断掉(那套公式治的是透视的 z_ndc 病态区),这里不给就沿用上一次透视
        // 收紧后的值,可能把相机前方整片切掉。
        camera.setOrthographic(widthMeters, 1.0, camera.farPlaneMeters());
        gOrthographic = true;
        LOGI("StageOrtho on isOrtho=%d widthM=%.0f groundDist=%.0f",
             camera.isOrthographic() ? 1 : 0, widthMeters, distance);
    });
}

// 面板按钮文案的回读口。与 GPU 位移开关同一取向:真值只在引擎里,UI 不存镜像。
// ⚠️ 读 camera.isOrthographic() 而不是 gOrthographic —— surface 重建后相机是新
// 造的(回透视),而 gOrthographic 这个 demo 侧变量会留在 true,两者会分叉。
JNIEXPORT jboolean JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetOrtho(
    JNIEnv* /* env */, jobject /* this */) {
    auto on = std::make_shared<bool>(false);
    gRenderThread.runSync(
        [on]() {
            if (gEngine) *on = gEngine->camera().isOrthographic();
        },
        std::chrono::milliseconds(100));
    return *on ? JNI_TRUE : JNI_FALSE;
}

// 系留三态:0=Free,1=跟车(仅 originProvider),2=座舱(加 orientationProvider)。
// ⚠️ 先看当前驱动者是不是 Tethered:引擎重建后选择器回到 Free,而 gCarrier
// 这个 demo 侧结构还留着 active=true —— 只读 gCarrier 会报出不存在的系留态。
JNIEXPORT jint JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetTetherState(
    JNIEnv* /* env */, jobject /* this */) {
    auto state = std::make_shared<int>(0);
    gRenderThread.runSync(
        [state]() {
            if (!gEngine) return;
            if (gEngine->cameraSystem().activeControllerName() !=
                CameraSystem::kTetheredController) {
                return;  // 不是系留在驱动 ⇒ 0,无论 gCarrier 记着什么
            }
            *state = gCarrier.useOrientation ? 2 : 1;
        },
        std::chrono::milliseconds(100));
    return static_cast<jint>(*state);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeGrazingView(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        // 固定可复现的斜视地平线位姿(性能测量用,复现 horizon-jank 重视野):
        // 相机在重庆上空低空(6km),视线仅比本地水平面下俯 4° → 近景高 LOD、
        // 远景延伸到地平线,把最大数量的地形/底图瓦片纳入考量。
        const auto& ellipsoid = Ellipsoid::WGS84();
        const double centerLng = 106.508, centerLat = 29.617;
        const double camAlt = 6000.0;
        const double pitchDeg = 4.0;
        auto camEcef = ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(centerLng, centerLat, camAlt));
        Vec3 up = ellipsoid.geodeticSurfaceNormal(camEcef);
        Vec3 north = (Vec3::unitZ() - up * up.dot(Vec3::unitZ())).normalized();
        const double p = pitchDeg * 3.14159265358979323846 / 180.0;
        Vec3 dir = (north * std::cos(p) - up * std::sin(p)).normalized();
        gEngine->camera().setView(camEcef, dir, up);
        LOGI("Grazing horizon view set (Chongqing %.0fm alt, pitch %.0f deg down)",
             camAlt, pitchDeg);
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeTerrainGrazingView(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        // 低 AGL 贴地掠视(动态 near 换源的复现位姿):相机在缙云山东南麓
        // 低空(~700m 椭球高,当地谷底 ~250m → AGL ~450m),视线朝西北山脊
        // 下俯 3°。旧 near 公式(椭球高×0.5≈350m 起步,但注意旧公式下限
        // 150m、按 nadir 不扣地形,在高原/山地会拉到千米级)会把前景坡体
        // 切出"看到山内部"的截面;新公式按近场最近几何收紧。若起手位姿低
        // 于地形+50m,帧末哨兵会自动抬升——位姿仍可复现。
        const auto& ellipsoid = Ellipsoid::WGS84();
        const double lng = 106.44, lat = 29.70;
        const double camAlt = 700.0;
        const double pitchDeg = 3.0;
        const double headingDeg = 315.0;  // 朝西北(缙云山脊方向)
        auto camEcef = ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(lng, lat, camAlt));
        Vec3 up = ellipsoid.geodeticSurfaceNormal(camEcef);
        Vec3 north = (Vec3::unitZ() - up * up.dot(Vec3::unitZ())).normalized();
        Vec3 east = north.cross(up).normalized();  // ENU: north × up = east
        const double h = headingDeg * 3.14159265358979323846 / 180.0;
        Vec3 horiz = (north * std::cos(h) + east * std::sin(h)).normalized();
        const double p = pitchDeg * 3.14159265358979323846 / 180.0;
        Vec3 dir = (horiz * std::cos(p) - up * std::sin(p)).normalized();
        gEngine->camera().setView(camEcef, dir, up);
        LOGI("Terrain grazing view set (%.2f,%.2f alt=%.0fm heading=%.0f pitch=-%.0f)",
             lng, lat, camAlt, headingDeg, pitchDeg);
    });
}

// 北极星 Phase 2c 地形 GPU 位移 A/B 运行时开关(设备侧前后对比用)。
// on=启用共享位移模板路径(Stage A 贴椭球);off=回现 per-tile baked VBO。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSetGpuTerrain(
    JNIEnv* /* env */, jobject /* this */, jboolean enabled) {
    const bool on = (enabled == JNI_TRUE);
    gRenderThread.post([on]() {
        if (!gEngine) return;
        gEngine->setTerrainGpuDisplacementEnabled(on);
        LOGI("Terrain GPU displacement %s", on ? "ENABLED" : "disabled");
    });
}

// ⚠️ 开关的**真值在引擎里**,不在 Java 字段里。surface 重建 = 引擎全重建,
// 这个标志会回到默认 true;Activity 旋转重建则会把 Java 侧字段清回默认。
// 两边各存一份必然静默分叉 ⇒ UI 每次显示前回读这里,不自己记。
JNIEXPORT jboolean JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetGpuTerrain(
    JNIEnv* /* env */, jobject /* this */) {
    // 引擎未就绪时报默认值(Engine.h terrainGpuDisplacementEnabled_ = true),
    // 与"重建后引擎实际处于什么档"一致。
    auto on = std::make_shared<bool>(true);
    gRenderThread.runSync(
        [on]() {
            if (gEngine) *on = gEngine->terrainGpuDisplacementEnabled();
        },
        std::chrono::milliseconds(100));
    return *on ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePause(
    JNIEnv* /* env */, jobject /* this */) {
    cancelInputIfNeeded();
    gRenderThread.setPaused(true);  // 暂停帧回调；任务队列仍在服务
    gRenderThread.post([]() {
        if (gPlatformBridge) {
            gPlatformBridge->onEnterBackground();
        }
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResume(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (gPlatformBridge) {
            gPlatformBridge->onEnterForeground();
        }
    });
    gRenderThread.setPaused(false);
}

} // extern "C"
