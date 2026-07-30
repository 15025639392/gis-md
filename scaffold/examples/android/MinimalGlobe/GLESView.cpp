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
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "earth_engine/Engine.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/data/FeatureSnapQuery.h"
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

// [GESTDIAG] 每帧由渲染线程发布当前手势锚点的屏幕投影(物理像素)，UI 线程
// 无锁读取用于绘制锚点标记。定位"双指瞬间偏移"用，定位后整体移除。
static std::atomic<bool> gAnchorActive{false};
static std::atomic<float> gAnchorScreenX{0.0f};
static std::atomic<float> gAnchorScreenY{0.0f};

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
static std::vector<FeatureId> gEditHandleIds;

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
    gDemoFeatureLayer = nullptr;  // Engine 持有,随 gEngine 一起销毁
    gEditHandleLayer = nullptr;
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
    const EGLint msaaAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_SAMPLE_BUFFERS, 1, EGL_SAMPLES, 4,
        EGL_NONE
    };
    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
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
    EGLint chosenSamples = 0;
    eglGetConfigAttrib(gDisplay, config, EGL_SAMPLES, &chosenSamples);
    __android_log_print(ANDROID_LOG_INFO, "GLESView",
                        "EGL config MSAA samples=%d", chosenSamples);

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
        gSdkFacade->installScene(
            minimal_globe_demo::makeDefaultDemoSceneConfig());

        // 矢量数据系统 P1 真机验证:demo 相机(重庆)附近挂一面一线。
        // heightOffset 抬离地表(该区地形 ~200-800m)防 depthTest 埋没;
        // 贴地钳制属 P3。
        {
            constexpr double kDeg = M_PI / 180.0;
            auto vectorLayer = std::make_unique<FeatureRenderLayer>(
                "demo-vector-p1", gRenderDevice.get(), Ellipsoid::WGS84());
            FeatureRenderStyle style;
            style.fillColor = {0.20f, 0.55f, 0.95f, 0.35f};
            style.lineColor = {1.00f, 0.72f, 0.05f, 0.95f};
            style.lineWidthPx = 6.0f;
            // P3 贴地:逐顶点采样地形高钳制(渲染网格一致采样)。残余误差
            // = fill 三角形横跨网格折痕(山脊)的线性切角项,随细分间距二次
            // 收缩;offset 10m 覆盖之(像素级贴合属 stencil 方案 B 终态)。
            style.altitudeMode = FeatureAltitudeMode::ClampToGround;
            style.heightOffset = 10.0;
            style.clampDensifyMeters = 40.0;
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
            vectorLayer->store().addFeature(std::move(poly));

            Feature route;
            route.type = GeometryType::LineString;
            route.rings = {{
                Cartographic(106.5020 * kDeg, 29.6180 * kDeg),
                Cartographic(106.5060 * kDeg, 29.6220 * kDeg),
                Cartographic(106.5100 * kDeg, 29.6190 * kDeg),
                Cartographic(106.5140 * kDeg, 29.6230 * kDeg)}};
            vectorLayer->store().addFeature(std::move(route));

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
    const auto engineStart = std::chrono::steady_clock::now();
    const bool presented =
        gEngine->render(0.0);  // auto-delta（内部 update；必要时 beginFrame→render→endFrame）
    const double engineMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - engineStart).count();

    // [GESTDIAG] 发布当前手势锚点屏幕投影（用当前帧相机，故标记随相机每帧跟随）。
    const auto postEngineStart = std::chrono::steady_clock::now();
    {
        Vec3 anchorWorld;
        bool published = false;
        if (gEngine->debugAnchorWorld(anchorWorld)) {
            const int w = gWidth.load();
            const int h = gHeight.load();
            const glm::dmat4 vp = gEngine->camera()
                .viewProjectionMatrix(static_cast<double>(w),
                                      static_cast<double>(h)).raw();
            glm::dvec4 clip = vp * glm::dvec4(anchorWorld.raw(), 1.0);
            if (clip.w > 1e-9) {  // w>0 → 锚点在相机前方
                clip /= clip.w;
                gAnchorScreenX = static_cast<float>((clip.x + 1.0) * 0.5 * w);
                gAnchorScreenY = static_cast<float>((1.0 - clip.y) * 0.5 * h);
                gAnchorActive = true;
                published = true;
            }
        }
        if (!published) gAnchorActive = false;
    }

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
    if (holeDirty || sHolePrev || loadDirty || frameId % 120 == 0) {
        // dropwhy = 几何就没有 / 没建 mapping / 建了但无可用纹理(含祖先)/
        //           texcoord 越界 / 其它;dropz = 被丢瓦片的 zoom 跨度。
        //           nomap 占多 = 时序问题;notex 占多 = 真缺常驻粗影像。
        LOGI("HoleQual frame=%llu sel=%d ent=%d drop=%d/%d "
             "dropwhy=%d/%d/%d/%d/%d dropz=%d-%d miss=%d nofill=%d "
             "fillnc=%d ctnc=%d nulls=%d/%d defer=%d drawn=%d dirty=%d",
             static_cast<unsigned long long>(frameId),
             q.terrainSelectedForRenderTiles,
             q.terrainRenderEntriesPlanned,
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
static void postInputEvent(const InputEvent& event) {
    gRenderThread.post([event]() {
        if (gEngine) {
            gEngine->onInputEvent(event);
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

// [GESTDIAG] 无锁读取渲染线程发布的锚点屏幕投影。out[0]=x, out[1]=y(物理像素)。
// 返回 true 表示当前有活动手势锚点。
JNIEXPORT jboolean JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetAnchorScreen(
    JNIEnv* env, jobject /* this */, jfloatArray out) {
    if (out != nullptr && env->GetArrayLength(out) >= 2) {
        jfloat vals[2] = { gAnchorScreenX.load(), gAnchorScreenY.load() };
        env->SetFloatArrayRegion(out, 0, 2, vals);
    }
    return gAnchorActive.load() ? JNI_TRUE : JNI_FALSE;
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
