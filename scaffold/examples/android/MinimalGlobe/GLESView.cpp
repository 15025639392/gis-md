#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <chrono>
#include <memory>
#include <string>

#include "earth_engine/Engine.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/platform/android/RenderDeviceGLES.h"
#include "earth_engine/platform/android/AndroidPlatformBridge.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/sdk/EarthEngineSdkFacade.h"

#include "MinimalGlobeDiagnostics.h"
#include "MinimalGlobeDemoConfig.h"
#include "MinimalGlobeDemoLayers.h"

#define LOG_TAG "MinimalGlobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace earth_engine;

// ============================================================
// EGL / GL ES 3.0 上下文
// ============================================================

static EGLDisplay gDisplay = EGL_NO_DISPLAY;
static EGLSurface gSurface = EGL_NO_SURFACE;
static EGLContext gContext = EGL_NO_CONTEXT;
static ANativeWindow* gWindow = nullptr;
static int gWidth = 0, gHeight = 0;

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

// Touch state
static bool gTouching = false;
static bool gDragStarted = false;
static bool gTouchMoved = false;

// Debug panel state
static bool gDebugPinchActive = false;

static double androidUptimeSeconds();

static void cancelInputIfNeeded() {
    if (!gEngine) return;
    InputEvent event;
    event.type = InputEvent::Type::Cancel;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = androidUptimeSeconds();
    gEngine->onInputEvent(event);
    gTouching = false;
    gDragStarted = false;
    gTouchMoved = false;
    gDebugPinchActive = false;
}

static void clearDemoEngineObjects() {
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

    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(gDisplay, attribs, &config, 1, &numConfigs)) return false;
    if (numConfigs < 1) return false;

    gSurface = eglCreateWindowSurface(gDisplay, config, window, nullptr);
    if (gSurface == EGL_NO_SURFACE) return false;

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    gContext = eglCreateContext(gDisplay, config, EGL_NO_CONTEXT, ctxAttribs);
    if (gContext == EGL_NO_CONTEXT) return false;

    if (!eglMakeCurrent(gDisplay, gSurface, gSurface, gContext)) return false;

    eglQuerySurface(gDisplay, gSurface, EGL_WIDTH, &gWidth);
    eglQuerySurface(gDisplay, gSurface, EGL_HEIGHT, &gHeight);

    LOGI("EGL initialized: %dx%d, GL: %s, GLSL: %s",
         gWidth, gHeight,
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
        gPlatformBridge = std::make_unique<AndroidPlatformBridge>(gJvm);
        gSdkFacade =
            std::make_unique<EarthEngineSdkFacade>(
                *gEngine,
                *gRenderDevice,
                *gPlatformBridge);
        gSdkFacade->installScene(
            minimal_globe_demo::makeDefaultDemoSceneConfig());
    } else {
        LOGE("Engine initialization failed");
        clearDemoEngineObjects();
    }
    return gEngineReady;
}

static int gFrameCount = 0;
static void renderFrame() {
    if (!gEngineReady) return;

    // 时间步进（实时）
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastTime).count();
    lastTime = now;

    // 环境系统：时间步进，render 中 update() 计算当前帧天空色
    gEngine->advanceTime(dt);
    gEngine->render(0.0);  // auto-delta（内部 beginFrame→update 计算 clearColor→render→endFrame）

    // 读取本帧计算的 clear color，设置为下一帧的 glClear 颜色（1 帧滞后，视觉无感）
    float cr, cg, cb, ca;
    gEngine->getClearColor(cr, cg, cb, ca);
    glClearColor(cr, cg, cb, ca);

    eglSwapBuffers(gDisplay, gSurface);
    ++gFrameCount;
}

// ============================================================
// JNI 桥接
// ============================================================

extern "C" {

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceCreated(
    JNIEnv* env, jobject /* this */, jobject surface) {

    gWindow = ANativeWindow_fromSurface(env, surface);
    if (!initEGL(gWindow)) {
        LOGE("Failed to initialize EGL");
        return;
    }
    if (!createEngine()) {
        LOGE("Failed to create Engine");
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceChanged(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    gWidth = width;
    gHeight = height;
    if (gEngine) {
        gEngine->onSurfaceChanged(width, height, 1.0f);
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeRenderFrame(
    JNIEnv* /* env */, jobject /* this */) {
    renderFrame();
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceDestroyed(
    JNIEnv* /* env */, jobject /* this */) {
    cancelInputIfNeeded();
    destroyEGL();
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
    if (!gDebugPinchActive || !gEngine) return;

    InputEvent event;
    event.type = InputEvent::Type::PinchEnd;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.timestamp = androidUptimeSeconds();
    gEngine->onInputEvent(event);
    gDebugPinchActive = false;
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
    if (!gEngine) return;
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
        gEngine->onInputEvent(event);
    }

    InputEvent event;
    event.type = InputEvent::Type::PointerMove;
    event.screenX = endX;
    event.screenY = endY;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = ts;
    gEngine->onInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeTouchUp(
    JNIEnv* /* env */, jobject /* this */, jfloat x, jfloat y) {
    gTouching = false;
    if (!gEngine) return;

    double ts = androidUptimeSeconds();

    InputEvent upEvent;
    upEvent.type = InputEvent::Type::PointerUp;
    upEvent.screenX = x;
    upEvent.screenY = y;
    upEvent.pointerType = InputEvent::PointerType::Touch;
    upEvent.timestamp = ts;
    gEngine->onInputEvent(upEvent);

    // 诊断日志（pick 和选择由 InputManager → Scene 回调处理）
    if (!gTouchMoved) {
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
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchStart(
    JNIEnv* /* env */, jobject /* this */, jfloat centerX, jfloat centerY) {
    endDebugPinchIfNeeded(centerX, centerY);
    gTouching = true;
    gDragStarted = false;
    gTouchMoved = true;
    if (!gEngine) return;

    InputEvent event;
    event.type = InputEvent::Type::PinchStart;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.timestamp = androidUptimeSeconds();
    gEngine->onInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchEnd(
    JNIEnv* /* env */, jobject /* this */, jfloat centerX, jfloat centerY) {
    gTouching = false;
    if (!gEngine) return;

    InputEvent event;
    event.type = InputEvent::Type::PinchEnd;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = androidUptimeSeconds();
    gEngine->onInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchRotateTilt(
    JNIEnv* /* env */, jobject /* this */,
    jfloat scale, jfloat rotationRadians,
    jfloat centerX, jfloat centerY, jfloat centerDx, jfloat centerDy,
    jfloat pointer0X, jfloat pointer0Y, jfloat pointer1X, jfloat pointer1Y,
    jint /*width*/, jint /*height*/) {
    if (!gEngine) return;
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
    gEngine->onInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugZoom(
    JNIEnv* /* env */, jobject /* this */,
    jfloat scale, jint width, jint height) {
    if (!gEngine) return;

    const float centerX = static_cast<float>(width) * 0.5f;
    const float centerY = static_cast<float>(height) * 0.5f;
    const double ts = androidUptimeSeconds();

    if (!gDebugPinchActive) {
        InputEvent start;
        start.type = InputEvent::Type::PinchStart;
        start.screenX = centerX;
        start.screenY = centerY;
        start.pinchScale = 1.0f;
        start.pointerType = InputEvent::PointerType::Touch;
        start.pointerCount = 2;
        start.timestamp = ts;
        gEngine->onInputEvent(start);
        gDebugPinchActive = true;
    }

    InputEvent move;
    move.type = InputEvent::Type::PinchMove;
    move.screenX = centerX;
    move.screenY = centerY;
    move.pinchScale = scale;
    move.pointerType = InputEvent::PointerType::Touch;
    move.pointerCount = 2;
    move.timestamp = ts + 0.016;
    gEngine->onInputEvent(move);

    const auto& diag = gEngine->diagnostics();
    const auto& trace = gEngine->presentationTrace();
    const auto& cameraTrace = trace.camera;
    const double cameraRadius = gEngine->camera().position().length();
    const double sphericalAltitude = cameraRadius - 6378137.0;
    const double ellipsoidAltitude =
        Ellipsoid::WGS84().cartesianToCartographic(gEngine->camera().position()).height();
    LOGI("Debug zoom scale=%.2f | tiles vis=%d cached=%d renderSurface=%d "
         "exact=%d parent=%d missing=%d unsupported=%d kicked=%d retained=%d "
         "entry plan=%d/%d/%d draw=%d/%d/%d miss=%d/%d/%d defer=%d/%d/%d fallback=%d prep=%d/%d surface/globe/masked=%d/%d/%d "
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
         diag.globeFallbackCommands,
         diag.globeFallbackMaskedTerrainEntries,
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
}

// ============================================================
// Debug panel JNI
// ============================================================

JNIEXPORT jstring JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetDiagnosticsString(
    JNIEnv* env, jobject /* this */) {
    if (!gEngine) return env->NewStringUTF("Engine not ready");

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
        "Surface meshes: %d (%d ellip, %d terr, %d ready, %d parent, %d trans)\n"
        "Attachments: %d exact, %d parent, %d missing, %d unsup, %d kicked, %d retained\n"
        "Zoom: %d-%d  |  Img: %d-%d -> tex %d-%d\n"
        "LOD: %.0f px  |  EqZoom: %d\n"
        "QuadTree: %d render, %d walk, %d frustum, %d fade, %d balanced\n"
        "Occlusion: %d occ, %d wait, %d culled-vis\n"
        "Groups: %d merc, %d N, %d S\n"
        "Camera: ellAlt=%.0fm sphAlt=%.0fm dist=%.0fm\n"
        "LoadQ: %d pre, %d norm, %d urgent  |  Terrain pending: %d req, %d upload, %d terminal\n"
        "Content pending: %d req, %d upload, %d terminal\n"
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
        diag.surfaceMeshCount, diag.ellipsoidSurfaceMeshes,
        diag.terrainSurfaceMeshes, diag.terrainReadySurfaceMeshes,
        diag.terrainParentFallbackMeshes, diag.terrainTransitionSurfaceMeshes,
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
        diag.pendingTerrainUploads,
        diag.pendingTerrainTerminalResults,
        diag.pendingContentRequests,
        diag.pendingContentUploads,
        diag.pendingContentTerminalResults,
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
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeAddDemoVectorLayer(
    JNIEnv* /* env */, jobject /* this */) {
    if (!gEngine || !gRenderDevice) return;
    minimal_globe_demo::addDemoVectorLayer(*gEngine, *gRenderDevice);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResetCamera(
    JNIEnv* /* env */, jobject /* this */) {
    if (!gSdkFacade) return;
    gSdkFacade->resetCamera();
    LOGI("Camera reset to Chongqing demo viewpoint");
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePause(
    JNIEnv* /* env */, jobject /* this */) {
    cancelInputIfNeeded();
    if (gPlatformBridge) {
        gPlatformBridge->onEnterBackground();
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResume(
    JNIEnv* /* env */, jobject /* this */) {
    if (gPlatformBridge) {
        gPlatformBridge->onEnterForeground();
    }
}

} // extern "C"
