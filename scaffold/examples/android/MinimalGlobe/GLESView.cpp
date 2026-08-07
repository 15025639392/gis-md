#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include <sched.h>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "earth_engine/Engine.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/data/FeatureClusterIndex.h"
#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/data/FeatureSnapQuery.h"
#include "earth_engine/core/async/AsyncSystem.h"
#include "earth_engine/data/MvtVectorSource.h"
#include "earth_engine/data/StyleFilter.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/VectorImageryProvider.h"
#include "earth_engine/renderer/VectorPageDrawer.h"
#include "earth_engine/platform/bridge/CurlMultiRequestScheduler.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/platform/android/RenderDeviceGLES.h"
#include "earth_engine/platform/android/AndroidPlatformBridge.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/sdk/EarthEngineSdkFacade.h"
#include "earth_engine/threading/RenderThreadPlacement.h"

#include "MinimalGlobeDiagnostics.h"
#include "MinimalGlobeDemoConfig.h"
#include "MinimalGlobeDemoLayers.h"

#define LOG_TAG "MinimalGlobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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

// 每帧发布相机方位角(弧度),UI 指北针无锁读取。
static std::atomic<float> gHeadingRadians{0.0f};

// Engine + RenderDevice
static std::unique_ptr<RenderDeviceGLES> gRenderDevice;
static std::unique_ptr<Engine> gEngine;
static std::unique_ptr<AndroidPlatformBridge> gPlatformBridge;
static std::unique_ptr<EarthEngineSdkFacade> gSdkFacade;
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

// ---- 矢量 P2 demo 编辑流(应用层最小实现) ----
// 引擎只出 pick/snap/预览接口;会话状态/undo 栈全在这里(demo 即参考实现)。
// gEditMode 由 UI 线程写、两线程读;其余编辑态仅渲染线程访问。
static std::atomic<bool> gEditMode{false};
static FeatureRenderLayer* gDemoFeatureLayer = nullptr;  // Engine 持有所有权
struct EditDragState {
    bool active = false;
    FeatureId featureId = kInvalidFeatureId;
    int ringIndex = -1;
    int vertexIndex = -1;
    double vertexHeight = 0.0;
    std::vector<std::vector<Cartographic>> rings;  // 工作副本
};
static EditDragState gEditDrag;
static std::vector<Feature> gEditUndoStack;  // 抓取时的编辑前快照
// P5a 编辑手柄(应用层):抓取时把被编辑环的顶点灌成 Point 要素,专用
// 手柄层渲染;拖拽实时更新被拖顶点,松手/取消清空。引擎只出点渲染能力。
static FeatureRenderLayer* gEditHandleLayer = nullptr;  // Engine 持有所有权

// ---- P6c 聚合演示(应用层)。引擎只出层级聚合索引与查询,聚合点怎么画、
// 何时刷新全在这里:源数据存在本地 FeatureStore(不进渲染),按相机 zoom
// 查询索引 → 结果写进一个普通 FeatureRenderLayer 当"显示层"。 ----
static FeatureStore gClusterSourceStore;
static FeatureClusterIndex gClusterIndex;
static FeatureRenderLayer* gClusterLayer = nullptr;  // Engine 持有所有权
static int gClusterShownLevel = -9999;               // 上次刷新用的 zoom 档
static std::vector<FeatureId> gEditHandleIds;

// ---- P4 MVT 只读底图(应用层接线)。引擎侧 MvtVectorSource 只出
// 选择/解码/灌注,网络与样式在这里:fetch 走 CurlScheduler,渲染挂
// 一个普通 FeatureRenderLayer(store 即 source 的灌注目标)。 ----
static FeatureRenderLayer* gMvtBasemapLayer = nullptr;  // Engine 持有所有权
static std::unique_ptr<MvtVectorSource> gMvtSource;     // 渲染线程访问
// E1:MVT 解码 + 镶嵌的 worker 池。独立于引擎的瓦片加载池 —— 底图镶嵌是
// 突发型重负载(换 zoom 时整视口一起来),混进地形/影像池会挤掉它们的
// 加载额度。2 线程:再多也只是把内存峰值抬高,commit 侧本就有帧预算。
static std::unique_ptr<ThreadPool> gMvtWorkerPool;
static std::unique_ptr<VectorPageDrawer> gMvtPageDrawer;  // C-2c
// HttpRequest 是取消句柄(析构即取消),须持有到完成;完成 id 攒起来
// 由下一次发请求时(渲染线程)剪除,避免在 curl 回调线程里析构句柄。
struct MvtFetchInflight {
    std::mutex mutex;
    uint64_t nextId = 0;
    std::unordered_map<uint64_t, std::unique_ptr<HttpRequest>> requests;
    std::vector<uint64_t> completed;
};
static MvtFetchInflight gMvtFetch;

// MVT 瓦片拉取(E1 几何通路与 E4 影像通路共用)。⚠️ HttpRequest 取消句柄
// 必须持有至完成,且**不能在 curl 回调线程析构** —— 完成 id 攒批,下次发
// 请求时在调用线程剪除。
static void mvtFetchTile(const TileKey& key,
                         std::function<void(int, std::vector<uint8_t>)> cb) {
    std::string url = minimal_globe_demo::kMvtBasemapUrlTemplate;
    auto replace = [&url](const char* token, int value) {
        size_t pos = url.find(token);
        if (pos != std::string::npos) {
            url.replace(pos, 3, std::to_string(value));
        }
    };
    replace("{z}", key.z);
    replace("{x}", key.x);
    replace("{y}", key.y);
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(gMvtFetch.mutex);
        for (uint64_t done : gMvtFetch.completed) {
            gMvtFetch.requests.erase(done);
        }
        gMvtFetch.completed.clear();
        id = gMvtFetch.nextId++;
    }
    auto handle = CurlMultiRequestScheduler::shared().get(
        url,
        [cb = std::move(cb), id](int statusCode, std::vector<uint8_t> body) {
            cb(statusCode, std::move(body));
            std::lock_guard<std::mutex> lock(gMvtFetch.mutex);
            gMvtFetch.completed.push_back(id);
        },
        HttpRequestOptions(HttpRequestPriority::Low));
    std::lock_guard<std::mutex> lock(gMvtFetch.mutex);
    gMvtFetch.requests[id] = std::move(handle);
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
    // MVT 源先停:它持有 basemap 层 store 的引用(层归 Engine 所有),
    // 且在飞的 HttpRequest 句柄析构即取消。
    gMvtSource.reset();
    gMvtPageDrawer.reset();  // 先于 pool/device 释放(持有 GPU buffer)
    gMvtWorkerPool.reset();
    {
        std::lock_guard<std::mutex> lock(gMvtFetch.mutex);
        gMvtFetch.requests.clear();
        gMvtFetch.completed.clear();
    }
    gMvtBasemapLayer = nullptr;   // Engine 持有,随 gEngine 一起销毁
    gDemoFeatureLayer = nullptr;  // Engine 持有,随 gEngine 一起销毁
    gEditHandleLayer = nullptr;
    gClusterLayer = nullptr;
    gClusterShownLevel = -9999;  // 下次装载重新刷一遍聚合显示层
    gEditHandleIds.clear();
    gEditDrag = EditDragState{};
    gEditUndoStack.clear();
    gSdkFacade.reset();
    gEngine.reset();
    gRenderDevice.reset();
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
    if (!eglChooseConfig(gDisplay, msaaAttribs, &config, 1, &numConfigs) ||
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

static void destroyEGL() {
    clearDemoEngineObjects();

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
    gEngine->onSurfaceChanged(gWidth, gHeight, 1.0f);

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
        // E4:矢量底图走影像通道。**必须在 installScene 之前**注册 ——
        // overlay 是 Tileset 构造时一次性交进去的。
        if (minimal_globe_demo::kEnableMvtBasemap &&
            minimal_globe_demo::kMvtBasemapAsOverlay) {
            gMvtWorkerPool = std::make_unique<ThreadPool>(2);
            VectorImageryProvider::Options vopt;
            vopt.id = "mvt-basemap";
            vopt.schemeId = "XYZ-WebMercator";
            vopt.minZoom = minimal_globe_demo::kMvtBasemapMinZoom;
            vopt.maxZoom = minimal_globe_demo::kMvtBasemapMaxZoom;
            vopt.tileSize = 512;
            vopt.style = minimal_globe_demo::makeMvtRasterStyle();
            auto provider = std::make_unique<VectorImageryProvider>(
                std::move(vopt),
                [](const TileKey& key,
                   VectorImageryProvider::FetchCallback cb) {
                    mvtFetchTile(key, std::move(cb));
                },
                gMvtWorkerPool.get());
            RasterOverlay::Options oopts;
            oopts.maximumScreenSpaceError = 2.0;
            oopts.minimumZoom = minimal_globe_demo::kMvtBasemapMinZoom;
            oopts.maximumZoom = minimal_globe_demo::kMvtBasemapMaxZoom;
            // 矢量底图缺瓦时不该阻塞「整帧可呈现」判定 —— 卫星影像才是底。
            oopts.blocksCompleteRenderable = false;
            // 矢量底图是**叠加层**不是底图影像:role 决定它不驱动 refine
            // (细化由卫星底图与地形定)。C-1 之后合成次序不再看 role ——
            // 页存储与 mappedRaster 都按 overlay 列表序合成同一批源。
            oopts.role = RasterOverlayRole::AnnotationOverlay;
            // C-2c:矢量由 VectorPageDrawer 在页上**直接 GPU 叠画**(页原生
            // 分辨率),故不参与页存储的 CPU 合成 —— 否则页里同一份内容出现
            // 两次:糊版垫在清晰版下面,清晰线周围挂一圈糊边。
            // 这个 overlay 仍保留:它是页存储 miss cell 的降级路径。
            oopts.compositeIntoPageStore = false;
            gSdkFacade->addCustomImageryOverlay(
                std::move(provider), TileScheme::createXYZWebMercator(),
                oopts);
            LOGI("VectorE4 MVT basemap as raster overlay (draped fallback)");
        }

        gSdkFacade->installScene(
            minimal_globe_demo::makeDefaultDemoSceneConfig());

        // C-2c:矢量在页原生分辨率上直接画进页存储 array 层(干掉 C-1b 那条
        // 「z14 源放大 8 倍贴 z17 页」的糊)。必须在 installScene 之后 ——
        // 页存储随场景建立。
        if (minimal_globe_demo::kMvtBasemapAsOverlay && gEngine) {
            VectorPageDrawer::Options dopts;
            dopts.style = minimal_globe_demo::makeMvtRasterStyle();
            dopts.maxSourceZoom = minimal_globe_demo::kMvtBasemapMaxZoom;
            dopts.pageSizeTexels = 256;  // 与 TerrainPageStore::Config 一致
            gMvtPageDrawer = std::make_unique<VectorPageDrawer>(
                gRenderDevice.get(), gEngine->renderer(),
                gMvtWorkerPool.get(), std::move(dopts),
                [](const TileKey& key,
                   std::function<void(int, std::vector<uint8_t>)> cb) {
                    mvtFetchTile(key, std::move(cb));
                });
            gEngine->setTerrainPageDecorator(gMvtPageDrawer.get());
            LOGI("VectorC2 page drawer installed (GPU draping)");
        }

        // ---- P4 MVT 只读底图:先于编辑演示层挂(先挂先画,垫底)。----
        if (minimal_globe_demo::kEnableMvtBasemap &&
            !minimal_globe_demo::kMvtBasemapAsOverlay) {
            // E1:底图走**瓦片桶**(worker 全链镶嵌),不再灌 store,故
            // 细桶那个 workaround 已无意义 —— 它当初是为了让「整城要素塞进
            // 空间分桶 store」时增量激活不退化成整桶全量重镶。桶尺寸留默认,
            // 该层的 store 现在只承载 demo 自己的编辑要素。
            auto basemapLayer = std::make_unique<FeatureRenderLayer>(
                "mvt-basemap", gRenderDevice.get(), Ellipsoid::WGS84());
            FeatureRenderStyle bs;
            // 贴地:stencil 分类 + 区域高度范围(零地形采样)。此前这里是
            // Absolute 抬 500m,因为贴地体要逐顶点采地形高度、而 worker 拿不到
            // 采样器(旧 store 路径能采,代价是单帧 235s)。改由 ctx 带一对
            // 标量后该约束消失,见 FeatureRenderLayer::TessellationContext。
            bs.altitudeMode = FeatureAltitudeMode::ClampToGround;
            bs.heightOffset = 0.0;
            // 按源图层分流的最小样式(tippecanoe 输出层名,数据侧对齐):
            // water 蓝面、building 灰面、缺省面淡绿;线统一浅白,宽随 zoom。
            bs.fillColorExpr = StyleExpression::match(
                "mvt_layer",
                {{"water",
                  StyleExpression::literal({0.25f, 0.50f, 0.85f, 0.55f})},
                 {"building",
                  StyleExpression::literal({0.60f, 0.60f, 0.62f, 0.55f})}},
                StyleExpression::literal({0.45f, 0.65f, 0.45f, 0.30f}));
            bs.lineColor = {0.95f, 0.95f, 0.90f, 0.85f};
            bs.lineWidthExpr = StyleExpression::interpolateLinear(
                StyleExpression::zoom(),
                {{8.0, StyleExpression::literal(1.0)},
                 {15.0, StyleExpression::literal(5.0)}});
            basemapLayer->setStyle(bs);
            // 贴地挤出体的纵向跨度来源(worker 侧读)。见配置项注释:取窄了
            // 体穿不透地形,该片区路网整片消失。
            basemapLayer->setWorkerTerrainHeightRange(
                minimal_globe_demo::kBasemapTerrainMinHeight,
                minimal_globe_demo::kBasemapTerrainMaxHeight);
            gMvtBasemapLayer = basemapLayer.get();

            gMvtWorkerPool = std::make_unique<ThreadPool>(2);
            MvtVectorSource::Options mvtOpts;
            // E2:道路分级过滤从数据侧(tippecanoe -j)搬回样式侧。改分级
            // 策略不再需要重切整套瓦片,同一份数据也能给不同样式复用 ——
            // 数据只管密度,样式管取舍(对齐 maplibre)。
            {
                using C = StyleFilter::Compare;
                // 分级表:粗档只留干线,细档逐步放开。瓦片 z 固定 → 每块
                // 瓦片按自己的 z 求值一次,**相机缩放不触发任何重镶**。
                SourceLayerRule roads;
                roads.layer = "roads";
                roads.filter = StyleFilter::any({
                    StyleFilter::all({
                        StyleFilter::zoomCompare(C::Less, 9),
                        StyleFilter::in("highway", {"motorway", "trunk",
                                                    "primary"})}),
                    StyleFilter::all({
                        StyleFilter::zoomCompare(C::GreaterEqual, 9),
                        StyleFilter::zoomCompare(C::Less, 10),
                        StyleFilter::in("highway", {"motorway", "trunk",
                                                    "primary", "secondary"})}),
                    StyleFilter::all({
                        StyleFilter::zoomCompare(C::GreaterEqual, 10),
                        StyleFilter::zoomCompare(C::Less, 12),
                        StyleFilter::in("highway", {"motorway", "trunk",
                                                    "primary", "secondary",
                                                    "tertiary"})}),
                    StyleFilter::zoomCompare(C::GreaterEqual, 12),
                });
                SourceLayerRule building;
                building.layer = "building";
                building.minZoom = 13;  // 建筑只在近景可辨,粗档整层跳过
                SourceLayerRule water;
                water.layer = "water";
                mvtOpts.layerRules = {roads, building, water};
            }
            mvtOpts.tree.minZoom = minimal_globe_demo::kMvtBasemapMinZoom;
            mvtOpts.tree.maxZoom = minimal_globe_demo::kMvtBasemapMaxZoom;
            auto fetchFn = [](const TileKey& key,
                              MvtVectorSource::FetchCallback cb) {
                mvtFetchTile(key, std::move(cb));
            };
            // E1 接线:镶嵌钩子在 worker 上跑,持一份样式快照(图集置空,
            // 见 FeatureRenderLayer::workerTessellationContext 的线程契约);
            // commit/drop 在渲染线程由 update() 调。裸指针安全:两者生命
            // 周期都由 gMvtSource.reset() 先于图层销毁保证。
            FeatureRenderLayer* layerPtr = basemapLayer.get();
            MvtVectorSource::Sinks sinks;
            sinks.tessellate = [layerPtr](std::vector<Feature>&& features) {
                return FeatureRenderLayer::tessellateTileMesh(
                    layerPtr->workerTessellationContext(), features);
            };
            sinks.commit = [layerPtr](const TileKey& key,
                                      FeatureTileMesh&& mesh) {
                layerPtr->commitTileMesh(key, std::move(mesh));
            };
            sinks.drop = [layerPtr](const TileKey& key) {
                layerPtr->dropTileMesh(key);
            };
            gMvtSource = std::make_unique<MvtVectorSource>(
                mvtOpts, std::move(sinks), std::move(fetchFn),
                gMvtWorkerPool.get());
            gEngine->addFeatureRenderLayer(std::move(basemapLayer));
            LOGI("VectorP4 MVT basemap installed: %s (z%d-%d)",
                 minimal_globe_demo::kMvtBasemapUrlTemplate,
                 minimal_globe_demo::kMvtBasemapMinZoom,
                 minimal_globe_demo::kMvtBasemapMaxZoom);
        }

        // 矢量数据系统 P1 真机验证:demo 相机(重庆)附近挂一面一线。
        // heightOffset 抬离地表(该区地形 ~200-800m)防 depthTest 埋没;
        // 贴地钳制属 P3。
        if (minimal_globe_demo::kEnableVectorDemoLayers) {
            constexpr double kDeg = M_PI / 180.0;
            auto vectorLayer = std::make_unique<FeatureRenderLayer>(
                "demo-vector-p1", gRenderDevice.get(), Ellipsoid::WGS84());
            FeatureRenderStyle style;
            style.fillColor = {0.20f, 0.55f, 0.95f, 0.35f};
            style.lineColor = {1.00f, 0.72f, 0.05f, 0.95f};
            style.lineWidthPx = 6.0f;
            // P6d dash 真机验证:60m 一节、划段 60%(贴地世界米制,拉远
            // 变密拉近变疏是透视语义;设 0 恢复实线)。
            style.lineDashPeriodMeters = 60.0f;
            style.lineDashOnFraction = 0.6f;
            // 贴地:fill 走 stencil 像素贴合(P6a),线/outline 同走 stencil
            // 墙带体(P6d 终态,免疫陡变地形断线与抬升视差)。下面两个参数
            // 只服务后端不支持 stencil 时的方案 A 回落(细分 + 抬升过渡档,
            // 断线根因与实测档位见 commit 588e5afde)。
            style.altitudeMode = FeatureAltitudeMode::ClampToGround;
            style.heightOffset = 2.5;
            style.clampDensifyMeters = 8.0;
            // P6b 数据驱动样式:fill 色按 zone 属性(stencil 按色分组)、
            // 点色按 kind 三色、线宽随 zoom 插值(拉远变细凑近变粗)。
            style.fillColorExpr = StyleExpression::match(
                "zone",
                {{"core",
                  StyleExpression::literal({0.20f, 0.55f, 0.95f, 0.35f})}},
                StyleExpression::literal({0.90f, 0.30f, 0.20f, 0.35f}));
            style.pointColorExpr = StyleExpression::match(
                "kind",
                {{"tower",
                  StyleExpression::literal({1.00f, 0.35f, 0.25f, 0.95f})},
                 {"gate",
                  StyleExpression::literal({1.00f, 0.85f, 0.20f, 0.95f})}},
                // 兜底分支给中性白:这一支走 P6c 位图图标(beacon),顶点色
                // 对位图是 tint 乘子,染色会盖掉图本身的三段色,验不了 uv。
                StyleExpression::literal({1.00f, 1.00f, 1.00f, 1.00f}));
            style.lineWidthExpr = StyleExpression::interpolateLinear(
                StyleExpression::zoom(),
                {{10.0, StyleExpression::literal(2.0)},
                 {15.0, StyleExpression::literal(10.0)}});
            // P6c 数据驱动选图:tower → 内置水滴 pin(底尖压在锚点上),
            // gate → 内置五角星,缺省 → 位图图标 beacon(下面注入;图集
            // 代次变化会自动触发重镶,注入晚于建桶也能补上)。
            // 一屏同时覆盖「解析 SDF 形状」与「位图图集」两条通道。
            style.pointImageExpr = StyleExpression::match(
                "kind",
                {{"tower", StyleExpression::literalString("pin")},
                 {"gate", StyleExpression::literalString("star")}},
                StyleExpression::literalString("beacon"));
            style.pointSizePx = 26.0f;
            // marker 语义:图形整个立在锚点上方(而非以锚点为中心)。斜视
            // 下居中锚定会让下半个符号被前方地面按深度遮掉——billboard 用
            // 的是锚点深度,身下的地更近。
            style.pointAnchor = SymbolAnchor::Bottom;
            // 底部锚定后符号整体上移,标注基线要让开符号高度,否则字压图。
            style.labelOffsetPx = style.pointSizePx + 6.0f;
            vectorLayer->setStyle(style);

            // 尺寸压到 RESET 预设视角(106.508,29.617,1.5km,-45°)一屏内:
            // 面 ~1.1km 见方带边界,线折两折穿过视野中心。
            // 尺寸 ~550m,钉在 RESET 视角(1500m/-45°)中带:800m 面高下
            // 角点全部可见可拾取。
            Feature poly;
            poly.type = GeometryType::Polygon;
            poly.rings = {{
                Cartographic(106.5055 * kDeg, 29.6200 * kDeg),
                Cartographic(106.5105 * kDeg, 29.6200 * kDeg),
                Cartographic(106.5105 * kDeg, 29.6250 * kDeg),
                Cartographic(106.5055 * kDeg, 29.6250 * kDeg),
                Cartographic(106.5055 * kDeg, 29.6200 * kDeg)}};
            poly.properties["name"] = "示范区 A";
            poly.properties["zone"] = "core";
            vectorLayer->store().addFeature(std::move(poly));

            // P6b 验证:第二个面 zone 缺省 → fill 表达式兜底红,与示范区 A
            // 的 core 蓝形成 stencil 双色组。
            Feature annex;
            annex.type = GeometryType::Polygon;
            annex.rings = {{
                Cartographic(106.5115 * kDeg, 29.6200 * kDeg),
                Cartographic(106.5145 * kDeg, 29.6200 * kDeg),
                Cartographic(106.5145 * kDeg, 29.6230 * kDeg),
                Cartographic(106.5115 * kDeg, 29.6230 * kDeg),
                Cartographic(106.5115 * kDeg, 29.6200 * kDeg)}};
            annex.properties["name"] = "附属区 B";
            vectorLayer->store().addFeature(std::move(annex));

            Feature route;
            route.type = GeometryType::LineString;
            route.rings = {{
                Cartographic(106.5020 * kDeg, 29.6180 * kDeg),
                Cartographic(106.5060 * kDeg, 29.6220 * kDeg),
                Cartographic(106.5100 * kDeg, 29.6190 * kDeg),
                Cartographic(106.5140 * kDeg, 29.6230 * kDeg)}};
            route.properties["name"] = "巡线 Route-1";
            vectorLayer->store().addFeature(std::move(route));

            // P5c 避让验证簇:~60m 间距 5 个标注点,RESET 视角下标签屏幕
            // 盒相互重叠 → 避让隐藏一部分(fade),凑近才逐个显出。
            for (int i = 0; i < 5; ++i) {
                Feature obs;
                obs.type = GeometryType::Point;
                obs.rings = {{Cartographic(
                    (106.5040 + 0.0006 * (i % 3)) * kDeg,
                    (29.6260 + 0.0005 * (i / 3)) * kDeg)}};
                obs.properties["name"] =
                    std::string("观测点-") + std::to_string(i + 1);
                // P6b:kind 轮转 tower/gate/(缺省) → 点色红/黄/兜底绿。
                if (i % 3 == 0) obs.properties["kind"] = "tower";
                else if (i % 3 == 1) obs.properties["kind"] = "gate";
                vectorLayer->store().addFeature(std::move(obs));
            }

            // P6c 图标:注入一张程序生成的位图图标(应用层供 RGBA 像素,
            // 引擎不做图片解码)。竖向三段色(上橙/中白/下青)是故意的——
            // 屏幕上若上下颠倒即说明图集 uv 的 v 方向接反了。
            {
                constexpr int kIconW = 24;
                constexpr int kIconH = 32;
                std::vector<uint8_t> icon(
                    static_cast<size_t>(kIconW) * kIconH * 4, 0);
                for (int y = 0; y < kIconH; ++y) {
                    for (int x = 0; x < kIconW; ++x) {
                        uint8_t* px =
                            &icon[(static_cast<size_t>(y) * kIconW + x) * 4];
                        const bool border = x < 2 || y < 2 ||
                                            x >= kIconW - 2 || y >= kIconH - 2;
                        if (border) {
                            px[0] = px[1] = px[2] = 20;
                            px[3] = 255;
                        } else if (y < kIconH / 3) {
                            px[0] = 250; px[1] = 140; px[2] = 30; px[3] = 255;
                        } else if (y < kIconH * 2 / 3) {
                            px[0] = px[1] = px[2] = 245;
                            px[3] = 255;
                        } else {
                            px[0] = 20; px[1] = 190; px[2] = 200; px[3] = 255;
                        }
                    }
                }
                if (gEngine->addIconImage("beacon", kIconW, kIconH, icon)) {
                    LOGI("VectorP6c icon injected: beacon %dx%d",
                         kIconW, kIconH);
                } else {
                    LOGI("VectorP6c icon injection FAILED");
                }
            }
            // P5b 标注字体(应用层读文件供字节,引擎不碰文件系统)。候选序:
            // Oplus-Serif=本机中文 TrueType;NotoSansCJK.ttc 是 CFF 会被
            // stbtt 拒(留在表里做健壮性验证);Roboto 兜底拉丁。
            const char* fontCandidates[] = {
                "/system/fonts/Oplus-Serif.ttf",
                "/system/fonts/DroidSansFallback.ttf",
                "/system/fonts/NotoSansCJK-Regular.ttc",
                "/system/fonts/Roboto-Regular.ttf",
            };
            for (const char* path : fontCandidates) {
                std::ifstream in(path, std::ios::binary);
                if (!in) continue;
                std::vector<uint8_t> bytes(
                    (std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
                if (bytes.empty()) continue;
                if (gEngine->setLabelFontData(std::move(bytes))) {
                    LOGI("VectorP5b label font: %s", path);
                    break;
                }
                LOGI("VectorP5b font rejected (CFF/parse): %s", path);
            }

            gDemoFeatureLayer = vectorLayer.get();
            gEngine->addFeatureRenderLayer(std::move(vectorLayer));

            // P5a 编辑手柄层(应用层):白色 SDF 圆点,贴地略高于要素防遮。
            auto handleLayer = std::make_unique<FeatureRenderLayer>(
                "edit-handles", gRenderDevice.get(), Ellipsoid::WGS84());
            FeatureRenderStyle handleStyle;
            handleStyle.pointColor = {1.0f, 1.0f, 1.0f, 0.95f};
            handleStyle.pointSizePx = 20.0f;
            handleStyle.altitudeMode = FeatureAltitudeMode::ClampToGround;
            handleStyle.heightOffset = 14.0;
            handleLayer->setStyle(handleStyle);
            gEditHandleLayer = handleLayer.get();
            gEngine->addFeatureRenderLayer(std::move(handleLayer));

            // ---- P6c 聚合演示(应用层)----
            // 源数据:重庆周边 ~25km 内 300 个点,分三团(团内密、团间疏),
            // 拉远看是三个大簇、凑近逐级散开。源 store 不进引擎渲染。
            {
                gClusterSourceStore.clear();
                const double clusterCenters[3][2] = {{106.50, 29.60},
                                                     {106.62, 29.66},
                                                     {106.44, 29.72}};
                uint32_t seed = 12345u;
                auto nextRand = [&seed]() {
                    // 固定种子的 LCG:每次启动布点一致,便于 A/B 比对。
                    seed = seed * 1664525u + 1013904223u;
                    return static_cast<double>(seed >> 8) /
                           static_cast<double>(1u << 24);
                };
                for (int i = 0; i < 300; ++i) {
                    const auto& c = clusterCenters[i % 3];
                    Feature p;
                    p.type = GeometryType::Point;
                    p.rings = {{Cartographic(
                        (c[0] + (nextRand() - 0.5) * 0.06) * kDeg,
                        (c[1] + (nextRand() - 0.5) * 0.04) * kDeg)}};
                    gClusterSourceStore.addFeature(std::move(p));
                }
                FeatureClusterOptions clusterOpts;
                clusterOpts.minZoom = 0;
                clusterOpts.maxZoom = 16;
                clusterOpts.radiusPx = 70.0;
                gClusterIndex.build(gClusterSourceStore, clusterOpts);

                // 显示层:簇与单点共用一层,靠 cluster 属性数据驱动区分
                // (簇 = 青圆 + 计数标签;单点 = 白圆无标签。尺寸是 zoom
                // 驱动的 uniform,不能逐要素分大小,故只用颜色区分)。
                auto clusterLayer = std::make_unique<FeatureRenderLayer>(
                    "demo-clusters", gRenderDevice.get(), Ellipsoid::WGS84());
                FeatureRenderStyle cs;
                cs.altitudeMode = FeatureAltitudeMode::ClampToGround;
                cs.heightOffset = 8.0;
                cs.labelProperty = "name";  // 簇写 count,单点留空不出标签
                cs.labelSizePx = 22.0f;
                cs.pointSizePx = 34.0f;
                cs.pointAnchor = SymbolAnchor::Bottom;  // 同上:整圆立在锚点上
                cs.labelOffsetPx = 0.5f * cs.pointSizePx;  // 计数压在圆心
                cs.pointColorExpr = StyleExpression::match(
                    "cluster",
                    {{"1", StyleExpression::literal(
                               {0.10f, 0.75f, 0.85f, 0.85f})}},
                    StyleExpression::literal({1.0f, 1.0f, 1.0f, 0.9f}));
                clusterLayer->setStyle(cs);
                gClusterLayer = clusterLayer.get();
                gEngine->addFeatureRenderLayer(std::move(clusterLayer));
                LOGI("VectorP6c cluster demo: %zu source points, %zu levels",
                     gClusterSourceStore.size(), gClusterIndex.levelCount());
            }
            LOGI("VectorP1 demo layer installed: 1 polygon + 1 line");
        }
        // Phase 2c P5:GPU 位移已引擎默认开(Engine.h terrainGpuDisplacementEnabled_
        // = true,pool 在首次 scene update 前急切创建)。运行时 A/B 关闭仍走调试面板
        // 的 setTerrainGpuDisplacementEnabled(false)(GLESView.cpp toggle)。
    } else {
        LOGE("Engine initialization failed");
        clearDemoEngineObjects();
    }
    return gEngineReady;
}

// ---- 矢量 P2 demo 编辑流(以下函数仅渲染线程调用) ----

static void clearEditHandles() {
    if (!gEditHandleLayer) return;
    for (FeatureId id : gEditHandleIds) {
        gEditHandleLayer->store().removeFeature(id);
    }
    gEditHandleIds.clear();
}

// 抓取时:被编辑环的每个顶点一个手柄(polygon 闭合末点不重复)。
static void populateEditHandles() {
    if (!gEditHandleLayer || !gEditDrag.active) return;
    clearEditHandles();
    const auto& ring =
        gEditDrag.rings[static_cast<size_t>(gEditDrag.ringIndex)];
    const Feature* feature =
        gDemoFeatureLayer->store().getFeature(gEditDrag.featureId);
    const bool closedDup =
        feature && feature->type == GeometryType::Polygon &&
        ring.size() >= 2 &&
        ring.front().longitude() == ring.back().longitude() &&
        ring.front().latitude() == ring.back().latitude();
    const size_t count = ring.size() - (closedDup ? 1 : 0);
    gEditHandleIds.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Feature handle;
        handle.type = GeometryType::Point;
        handle.rings = {{ring[i]}};
        gEditHandleIds.push_back(
            gEditHandleLayer->store().addFeature(std::move(handle)));
    }
}

// 拖拽中:只更新被拖顶点的手柄(闭合末点映射回首手柄)。
static void updateDraggedHandle(const Cartographic& target) {
    if (!gEditHandleLayer || gEditHandleIds.empty()) return;
    const size_t idx =
        static_cast<size_t>(gEditDrag.vertexIndex) % gEditHandleIds.size();
    const Feature* handle =
        gEditHandleLayer->store().getFeature(gEditHandleIds[idx]);
    if (!handle) return;
    Feature moved = *handle;
    moved.rings = {{target}};
    moved.bounds = Rectangle();
    gEditHandleLayer->store().updateFeature(moved);
}

// 手势起点:pick 顶点 → 抓取(undo 快照 + beginEditPreview)。
static void editTouchDown(float x, float y) {
    if (!gEngine || !gDemoFeatureLayer || gEditDrag.active) return;
    FrameState pickFrame;
    pickFrame.camera = &gEngine->camera();
    pickFrame.viewportWidthPixels = gWidth.load();
    pickFrame.viewportHeightPixels = gHeight.load();
    const FeaturePickResult hit =
        gDemoFeatureLayer->pick(pickFrame, x, y, 48.0f);
    if (hit.part != FeaturePickResult::Part::Vertex) {
        LOGI("EditFlow: no vertex at (%.0f,%.0f) part=%d", x, y,
             static_cast<int>(hit.part));
        return;
    }
    const Feature* feature =
        gDemoFeatureLayer->store().getFeature(hit.featureId);
    if (!feature) return;
    if (!gDemoFeatureLayer->beginEditPreview(hit.featureId)) return;
    // P5c 编辑联动:选中(抓取)要素标签提权,避让时优先显示。
    gDemoFeatureLayer->setLabelPriorityFeature(hit.featureId);
    gEditUndoStack.push_back(*feature);
    gEditDrag.active = true;
    gEditDrag.featureId = hit.featureId;
    gEditDrag.ringIndex = hit.ringIndex;
    gEditDrag.vertexIndex = hit.vertexIndex;
    gEditDrag.vertexHeight = hit.position.height();
    gEditDrag.rings = feature->rings;
    populateEditHandles();
    LOGI("EditFlow: grab feature=%llu ring=%d vertex=%d distPx=%.1f handles=%zu",
         static_cast<unsigned long long>(hit.featureId),
         hit.ringIndex, hit.vertexIndex, hit.distancePx,
         gEditHandleIds.size());
}

// 拖拽:指尖地面坐标 → snap 候选吸附 → 更新预览。
static void editTouchMove(float x, float y) {
    if (!gEngine || !gDemoFeatureLayer || !gEditDrag.active) return;
    const PickResult ground = gEngine->pick(x, y);
    if (!ground.isValid()) return;
    Cartographic target(ground.cartographic.longitude(),
                        ground.cartographic.latitude(),
                        gEditDrag.vertexHeight);
    // snap 容差 = 24px 换算地面米(相机距离 × 每像素弧度),排除自身。
    const double dist =
        (ground.worldPosition - gEngine->camera().position()).length();
    const double tolMeters = std::max(
        5.0, dist * gEngine->camera().verticalFovRadians() /
                 std::max(1, gHeight.load()) * 24.0);
    const auto snap = FeatureSnapQuery::nearest(
        gDemoFeatureLayer->store(), Ellipsoid::WGS84(), target, tolMeters,
        gEditDrag.featureId);
    if (snap) {
        target = Cartographic(snap->position.longitude(),
                              snap->position.latitude(),
                              gEditDrag.vertexHeight);
        LOGI("EditFlow: snap to feature=%llu %s idx=%d dist=%.1fm",
             static_cast<unsigned long long>(snap->featureId),
             snap->part == SnapCandidate::Part::Vertex ? "vertex" : "edge",
             snap->vertexIndex, snap->distanceMeters);
    }
    auto& ring = gEditDrag.rings[gEditDrag.ringIndex];
    ring[static_cast<size_t>(gEditDrag.vertexIndex)] = target;
    // polygon 闭合环:拖首/末点时同步另一端保持闭合。
    const Feature* feature =
        gDemoFeatureLayer->store().getFeature(gEditDrag.featureId);
    if (feature && feature->type == GeometryType::Polygon &&
        ring.size() >= 2) {
        if (gEditDrag.vertexIndex == 0) {
            ring.back() = target;
        } else if (static_cast<size_t>(gEditDrag.vertexIndex) ==
                   ring.size() - 1) {
            ring.front() = target;
        }
    }
    gDemoFeatureLayer->updateEditPreview(gEditDrag.rings);
    updateDraggedHandle(target);
}

// 松手:commit 落库(undo 快照已在抓取时入栈)+ 结束预览。
static void editTouchUp() {
    if (!gDemoFeatureLayer || !gEditDrag.active) return;
    const Feature* feature =
        gDemoFeatureLayer->store().getFeature(gEditDrag.featureId);
    if (feature) {
        Feature edited = *feature;
        edited.rings = gEditDrag.rings;
        edited.bounds = Rectangle();  // store 从 rings 重算
        gDemoFeatureLayer->store().updateFeature(edited);
        LOGI("EditFlow: commit feature=%llu version=%llu undoDepth=%zu",
             static_cast<unsigned long long>(gEditDrag.featureId),
             static_cast<unsigned long long>(
                 gDemoFeatureLayer->store()
                     .getFeature(gEditDrag.featureId)->version),
             gEditUndoStack.size());
    }
    gDemoFeatureLayer->endEditPreview();
    gEditDrag = EditDragState{};
    clearEditHandles();
}

static int gFrameCount = 0;
/// P6c 聚合演示的每帧刷新(应用层职责:引擎只出索引,画什么由这里定)。
/// 相机 zoom 档变化才重建显示层——聚合是层级预聚,同一档内结果不变,
/// 平移不需要重建(300 点直接全量查,不做视口裁剪)。渲染线程调用。
static void refreshClusterDisplay() {
    if (!gClusterLayer || gClusterIndex.empty()) return;
    const double camHeight =
        Ellipsoid::WGS84()
            .cartesianToCartographic(gEngine->camera().position())
            .height();
    // 与引擎 zoom 驱动样式同一换算(web 墨卡托惯例)。
    const double zoom = std::min(
        24.0, std::max(0.0, std::log2(4.0e7 / std::max(1.0, camHeight))));
    const int level = static_cast<int>(std::lround(zoom));
    if (level == gClusterShownLevel) return;
    gClusterShownLevel = level;

    const Rectangle world(-M_PI, -M_PI / 2.0, M_PI, M_PI / 2.0);
    const auto clusters = gClusterIndex.query(world, zoom);
    gClusterLayer->store().clear();
    for (const auto& c : clusters) {
        Feature f;
        f.type = GeometryType::Point;
        f.rings = {{Cartographic(c.longitude, c.latitude)}};
        if (c.isCluster()) {
            f.properties["cluster"] = "1";
            f.properties["name"] = std::to_string(c.count);
        }
        gClusterLayer->store().addFeature(std::move(f));
    }
    LOGI("VectorP6c clusters: zoom=%.2f level=%d entries=%zu", zoom, level,
         clusters.size());
}

static void renderFrame() {
    if (!gEngineReady) return;

    const auto frameStart = std::chrono::steady_clock::now();
    static auto previousFrameStart = frameStart;
    const double callbackIntervalMs =
        std::chrono::duration<double, std::milli>(
            frameStart - previousFrameStart).count();
    previousFrameStart = frameStart;

    const auto sdkStart = std::chrono::steady_clock::now();
    if (gSdkFacade) {
        gSdkFacade->update();
    }
    const double sdkMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - sdkStart).count();

    // 时间步进（实时）
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastTime).count();
    lastTime = now;

    // 环境系统：时间步进，render 中 update() 计算当前帧天空色
    gEngine->advanceTime(dt);
    if (minimal_globe_demo::kEnableVectorDemoLayers) {
        refreshClusterDisplay();
    }
    if (gMvtSource) {
        // P4 MVT 底图驱动(渲染线程契约):地平线视口 + 相机高定 zoom。
        const Ellipsoid& wgs84 = Ellipsoid::WGS84();
        const Cartographic camCarto =
            wgs84.cartesianToCartographic(gEngine->camera().position());
        const Vec3& radii = wgs84.radii();
        const double minRadius =
            std::min(radii.x(), std::min(radii.y(), radii.z()));
        gMvtSource->update(
            MvtVectorSource::horizonViewRectangle(camCarto, minRadius),
            std::max(1.0, camCarto.height()));
        static uint64_t mvtLogCounter = 0;
        if (++mvtLogCounter % 120 == 1) {
            LOGI("VectorE1 mvt: active=%zu meshes=%zu loaded=%zu pending=%zu "
                 "failed=%zu",
                 gMvtSource->activeTileCount(),
                 gMvtBasemapLayer ? gMvtBasemapLayer->tileMeshCount() : 0,
                 gMvtSource->tree().loadedCount(),
                 gMvtSource->tree().pendingCount(),
                 gMvtSource->tree().failedCount());
        }
    }
    const auto engineStart = std::chrono::steady_clock::now();
    const bool presented =
        gEngine->render(0.0);  // auto-delta（内部 update；必要时 beginFrame→render→endFrame）
    const double engineMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - engineStart).count();

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
        if (swapOk == EGL_TRUE) {
            ++gFrameCount;
        }
    }

    const double frameTotalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - frameStart).count();
    // 回报本帧 CPU 工作耗时。**刻意不含 swapMs** —— eglSwapBuffers 里绝大部分是
    // 等 vsync 的空转,算进去会让系统以为我们每帧都刚好用满预算,反而不提速。
    gRenderThreadPlacement.reportActualWorkDurationMs(engineMs + postEngineMs);
    const uint64_t frameId = gEngine->presentationTrace().camera.frameId;
    // P5c 标签避让诊断(节流):cand=候选 placed=显示 col=碰撞落选
    // horiz=地平线剔除 proj=视锥外/相机背后。
    if (gDemoFeatureLayer && frameId % 120 == 0) {
        const auto& ls = gDemoFeatureLayer->labelPlacementStats();
        if (ls.candidates > 0) {
            LOGI("LabelPlace frame=%llu cand=%d placed=%d col=%d horiz=%d "
                 "proj=%d",
                 static_cast<unsigned long long>(frameId), ls.candidates,
                 ls.placed, ls.collided, ls.culledHorizon,
                 ls.culledProjection);
        }
    }
    if (frameId <= 3 || frameId % 120 == 0 ||
        frameTotalMs >= 25.0 || swapMs >= 8.0) {
        LOGI(
            "FrameLoop frame=%llu total=%.3f sdk=%.3f engine=%.3f "
            "post=%.3f swap=%.3f callback=%.3f cpu=%d hint=%d presented=%d swapOk=%d",
            static_cast<unsigned long long>(frameId),
            frameTotalMs,
            sdkMs,
            engineMs,
            postEngineMs,
            swapMs,
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
            swapOk == EGL_TRUE ? 1 : 0);
        // 北极星 Phase 0 测量台:每帧(采样)打相机真实位姿,消除"nadir/oblique"
        // 猜测——用它标注每个 measure stop 的实际视角。
        const auto& camTrace = gEngine->presentationTrace().camera;
        LOGI("CamPose frame=%llu center=%.5f,%.5f camH=%.1f targetH=%.1f "
             "pitchDeg=%.2f headingDeg=%.2f",
             static_cast<unsigned long long>(frameId),
             camTrace.targetLongitudeDegrees,
             camTrace.targetLatitudeDegrees,
             camTrace.cameraHeightMeters,
             camTrace.targetHeightMeters,
             camTrace.pitchRadians * 180.0 / M_PI,
             camTrace.headingRadians * 180.0 / M_PI);
    }

    // 加载体验记分卡:把"糊/露底/台阶"这些观感症状翻成可 A/B 的计数,免去
    // 靠录屏和主观描述定位。采样策略与 FrameLoop 不同——**暂态期逐帧打、
    // 稳态期心跳打**:糊块/露底只在加载暂态出现,120 帧心跳会整段错过。
    //   sharp/a1/a2/a3+/miss  = 底图「糊几级」直方图:贴本级 / 退回祖先差
    //                           1、2、3+ 级上采样 / 地形瓦片压根没影像。
    //                           a*+miss>0 即"屏幕上有糊块或空块"。
    //   src=real/fill/ell/unk = 地形几何来源 → fill/ell>0 即"露代理面或裸椭球"
    //   z / texZ              = 可见几何 LOD 跨度 / 实际贴上的影像层跨度
    //   fade                  = cross-fade 正在过渡的瓦片数
    const auto& q = gEngine->diagnostics();
    const bool loadDirty = (q.imageryParentFallbackAttachments > 0 ||
                            q.imageryMissingTiles > 0 ||
                            q.terrainSurfaceFillProxyCommands > 0 ||
                            q.terrainSurfaceEllipsoidCommands > 0);
    static bool sLoadDirtyPrev = false;
    // 暂态期逐帧 + 刚回到干净的那一帧(记 settle 落点)+ 稳态心跳
    if (loadDirty || sLoadDirtyPrev || frameId % 120 == 0) {
        LOGI("LoadQual frame=%llu vis=%d sharp=%d a1=%d a2=%d a3+=%d miss=%d "
             "src=%d/%d/%d/%d geoZ=%d-%d texZ=%d-%d z=%d-%d fade=%d dirty=%d",
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
             q.quadtreeFadingNodes,
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
             "fillnc=%d ctnc=%d nulls=%d/%d defer=%d drawn=%d dirty=%d",
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

    void post(std::function<void()> task) {
        if (!running_.load()) return;  // 线程未运行时任务直接丢弃
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        wake();
    }

    /// 投递任务并等待其在渲染线程执行完（诊断读取等需要返回值的场景）。
    /// 超时返回 false。任务捕获必须按值 / shared_ptr——超时后任务仍可能
    /// 被执行，引用捕获会悬垂。
    bool runSync(std::function<void()> task, std::chrono::milliseconds timeout) {
        if (!running_.load()) return false;
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        post([done, task = std::move(task)]() {
            task();
            done->set_value();
        });
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
        drainTasks();   // 输入先于渲染，保证事件同帧生效
        renderFrame();
        postFrameIfNeeded();
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
        postFrameIfNeeded();

        while (running_.load()) {
            int events = 0;
            void* data = nullptr;
            // 帧回调在 pollOnce 内部分发；post()/stop() 经 ALooper_wake 唤醒
            ALooper_pollOnce(-1, nullptr, &events, &data);
            drainTasks();
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

// UI 线程整形好的输入事件统一从这里投递到渲染线程。
// 屏幕密度（Java surfaceChanged 时设置）。手势阈值以 dp 定义，InputManager
// 用 event.devicePixelRatio 把 dp 换算成物理像素——不填则恒 1，latch 阈值
// 在高密度屏上会偏敏感 density 倍。
static float gDisplayDensity = 1.0f;

static void postInputEvent(const InputEvent& event) {
    InputEvent stamped = event;
    stamped.devicePixelRatio = gDisplayDensity;
    gRenderThread.post([stamped]() {
        if (gEngine) {
            gEngine->onInputEvent(stamped);
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
            gEngine->onSurfaceChanged(width, height, 1.0f);
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

    // 编辑模式的触摸不喂相机手势流（editTouchDown 由 nativeDrag 首个 move 发）。
    if (gEditMode.load(std::memory_order_relaxed)) {
        return;
    }

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

    // 编辑模式:触摸走顶点拖拽编辑流(渲染线程),不喂相机。
    if (gEditMode.load(std::memory_order_relaxed)) {
        const bool first = !gDragStarted;
        gDragStarted = true;
        gRenderThread.post([first, startX, startY, endX, endY]() {
            if (first) editTouchDown(startX, startY);
            editTouchMove(endX, endY);
        });
        return;
    }

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

    if (gEditMode.load(std::memory_order_relaxed)) {
        gRenderThread.post([]() { editTouchUp(); });
        return;
    }

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
         "entry plan=%d/%d/%d draw=%d/%d/%d miss=%d/%d/%d defer=%d/%d/%d fallback=%d prep=%d/%d surface=%d src=%d/%d/%d/%d "
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
         diag.terrainRenderEntriesFadingPlanned,
         diag.terrainRenderEntriesDrawn,
         diag.terrainRenderEntriesSelectedDrawn,
         diag.terrainRenderEntriesFadingDrawn,
         diag.terrainRenderEntriesMissed,
         diag.terrainRenderEntriesSelectedMissed,
         diag.terrainRenderEntriesFadingMissed,
         diag.terrainRenderEntriesDeferred,
         diag.terrainRenderEntriesSelectedDeferred,
         diag.terrainRenderEntriesFadingDeferred,
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
        "QuadTree: %d render, %d walk, %d frustum, %d fade, %d balanced\n"
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
        "Mesh: %d KB  |  Terrain tiles: %d (gen %llu)",
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
        diag.quadtreeInFrustumNodes, diag.quadtreeFadingNodes,
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
        diag.surfaceMeshBytes / 1024, diag.terrainCachedTiles,
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

// 复位正北朝上（在渲染线程执行，读写相机态）。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResetNorthUp(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (gEngine) gEngine->resetNorthUp();
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeAddDemoVectorLayer(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine || !gRenderDevice) return;
        minimal_globe_demo::addDemoVectorLayer(*gEngine, *gRenderDevice);
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

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSetEditMode(
    JNIEnv* /* env */, jobject /* this */, jboolean enabled) {
    const bool on = (enabled == JNI_TRUE);
    gEditMode.store(on, std::memory_order_relaxed);
    gRenderThread.post([on]() {
        // 关闭时若拖拽中:cancel(不落库,弹掉抓取时压入的 undo 快照)。
        if (!on && gEditDrag.active && gDemoFeatureLayer) {
            gDemoFeatureLayer->endEditPreview();
            gEditDrag = EditDragState{};
            clearEditHandles();
            if (!gEditUndoStack.empty()) gEditUndoStack.pop_back();
        }
        // 退出编辑模式 = 取消选中,标签提权一并清除。
        if (!on && gDemoFeatureLayer) {
            gDemoFeatureLayer->setLabelPriorityFeature(kInvalidFeatureId);
        }
        LOGI("EditFlow: edit mode %s", on ? "ON" : "OFF");
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeUndoEdit(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gDemoFeatureLayer || gEditDrag.active) return;
        if (gEditUndoStack.empty()) {
            LOGI("EditFlow: undo stack empty");
            return;
        }
        Feature snapshot = gEditUndoStack.back();
        gEditUndoStack.pop_back();
        snapshot.bounds = Rectangle();  // store 从 rings 重算
        gDemoFeatureLayer->store().updateFeature(snapshot);
        LOGI("EditFlow: undo feature=%llu → version=%llu undoDepth=%zu",
             static_cast<unsigned long long>(snapshot.id),
             static_cast<unsigned long long>(
                 gDemoFeatureLayer->store().getFeature(snapshot.id)->version),
             gEditUndoStack.size());
    });
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
