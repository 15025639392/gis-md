#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <chrono>
#include <algorithm>

#include "earth_engine/Engine.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/platform/android/RenderDeviceGLES.h"
#include "earth_engine/platform/android/AndroidPlatformBridge.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/providers/HeightmapTerrainProvider.h"
#include "earth_engine/layers/BasemapLayer.h"
#include "earth_engine/layers/TerrainLayer.h"
#include "earth_engine/layers/VectorLayer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/data/GeoJsonParser.h"
#include "earth_engine/style/OverlayStyle.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/environment/TimeController.h"
#include "earth_engine/scene/FrameState.h"

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
static bool gDebugPinchActive = false;

static constexpr const char* kFabdemTerrainTemplate =
    "http://192.168.1.4:8090/{z}/{x}/{y}.png";
static constexpr const char* kGaodeSatelliteTemplate =
    "https://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}";
static constexpr bool kEnableTerrainForDemo = false;
static constexpr bool kEnableDebugOverlayForDemo = false;
static constexpr bool kShowNormalMapForDemo = false;
static constexpr bool kUseGaodeSatelliteForDemo = true;

static void addDemoVectorLayer() {
    static constexpr const char* kDemoGeoJson = R"json(
{
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "id": "beijing-marker",
      "properties": { "name": "Beijing" },
      "geometry": { "type": "Point", "coordinates": [116.3913, 39.9075, 0] }
    },
    {
      "type": "Feature",
      "id": "demo-route",
      "properties": { "name": "Demo route" },
      "geometry": {
        "type": "LineString",
        "coordinates": [[116.30, 39.86], [116.39, 39.91], [116.48, 39.95]]
      }
    }
  ]
}
)json";

    auto features = GeoJsonParser::parse(kDemoGeoJson);
    if (features.empty()) {
        LOGE("Demo GeoJSON parse failed");
        return;
    }

    auto style = makeDefaultPointStyle();
    style.layer.geometry = PointStyle{
        PointStyle::Shape::Circle,
        12.0f,
        Color{0.95f, 0.20f, 0.12f, 1.0f},
        Color::white(),
        2.0f,
        true,
        true
    };

    auto vectorLayer = std::make_unique<VectorLayer>(
        "android-demo-vector",
        std::move(features),
        style,
        gRenderDevice.get());
    gEngine->addVectorLayer(std::move(vectorLayer));
    LOGI("Demo vector layer added");
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
    gEngine.reset();
    gRenderDevice.reset();
    gPlatformBridge.reset();
    gEngineReady = false;

    eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gContext != EGL_NO_CONTEXT) eglDestroyContext(gDisplay, gContext);
    if (gSurface != EGL_NO_SURFACE) eglDestroySurface(gDisplay, gSurface);
    if (gDisplay != EGL_NO_DISPLAY) eglTerminate(gDisplay);
    gContext = EGL_NO_CONTEXT;
    gSurface = EGL_NO_SURFACE;
    gDisplay = EGL_NO_DISPLAY;
}

static bool createEngine() {
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

        // 创建 Android JNI HTTP 桥接
        gPlatformBridge = std::make_unique<AndroidPlatformBridge>(gJvm);

        std::unique_ptr<ImageryProvider> provider;
        if (kUseGaodeSatelliteForDemo) {
            auto xyz = std::make_unique<XYZImageryProvider>(
                kGaodeSatelliteTemplate,
                "Gaode/Amap satellite imagery");
            // Gaode satellite returns reliable native imagery through z18 in
            // this demo region; higher z requests can produce solid placeholder
            // tiles, so keep provider capability separate from camera LOD.
            xyz->setZoomRange(0, 18);
            xyz->setOpenGlobusGroupedY(true);
            xyz->setOpenGlobusPolarGroupsEnabled(false);
            xyz->setPlatformBridge(gPlatformBridge.get());
            provider = std::move(xyz);
            LOGI("Gaode satellite provider enabled: %s", kGaodeSatelliteTemplate);
            LOGI("Gaode/Amap tiles are GCJ-02 aligned; this demo is visual only, not WGS84 control-point acceptance");
        } else {
            provider = std::make_unique<DebugImageryProvider>();
            LOGI("Debug standard XYZ WebMercator provider enabled");
        }

        auto scheme = kUseGaodeSatelliteForDemo
            ? TileScheme::createOpenGlobusEarth()
            : TileScheme::createXYZWebMercator();
        auto layer = std::make_unique<BasemapLayer>(
            std::move(provider), std::move(scheme), gRenderDevice.get());
        layer->setNormalMapDebugEnabled(kShowNormalMapForDemo);
        gEngine->addLayer(std::move(layer));
        LOGI("Basemap layer added; normal map debug %s",
             kShowNormalMapForDemo ? "enabled" : "disabled");

        if (kEnableTerrainForDemo) {
            // FABDEM raster DEM served by dems/scripts/serve_tiles.py.
            // XYZ WebMercator / Mapbox Terrain-RGB, WGS84 ellipsoid heights in meters.
            auto terrainProvider = std::make_unique<HeightmapTerrainProvider>(
                kFabdemTerrainTemplate,
                "FABDEM raster DEM");
            terrainProvider->setZoomRange(0, 12);
            terrainProvider->setEncoding(HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
            terrainProvider->setPlatformBridge(gPlatformBridge.get());
            auto terrainLayer = std::make_unique<TerrainLayer>(
                std::move(terrainProvider),
                TileScheme::createXYZWebMercator());
            terrainLayer->setEnabled(true);
            gEngine->setTerrainLayer(std::move(terrainLayer));
            gEngine->setTerrainEnabled(true);
            LOGI("FABDEM terrain layer added: %s", kFabdemTerrainTemplate);
        } else {
            LOGI("FABDEM terrain disabled for clean SurfaceTile validation");
        }

        // Keep the feature path available, but avoid covering the basemap while
        // validating XYZ Web Mercator tile alignment.
        // addDemoVectorLayer();

        gEngine->setDebugOverlayEnabled(kEnableDebugOverlayForDemo);
        LOGI("Debug overlay %s",
             kEnableDebugOverlayForDemo ? "enabled" : "disabled");

        // 设置模拟时间为当前系统时间
        double nowJd = currentJulianDate();
        gEngine->setTime(nowJd);
        LOGI("Simulation time set to JD %.3f (Unix %.0f)",
             nowJd, julianToUnix(nowJd));
    } else {
        LOGE("Engine initialization failed");
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

    Vec3 sunDir = gEngine->sunDirection();

    eglSwapBuffers(gDisplay, gSurface);

    gFrameCount++;
    if (gFrameCount <= 1 || gFrameCount % 300 == 0) {
        const auto& diag = gEngine->diagnostics();
        LOGI("Frame %d | tiles vis=%d cached=%d renderSurface=%d mesh=%d "
             "attach=%d exact=%d parent=%d normalMap=%d stale=%d missingGen=%d | "
             "missing=%d unsupported=%d "
             "lod=%.0f eq=%d qRender=%d qWalk=%d qFrustum=%d qFade=%d "
             "grp=%d/%d/%d gen=%llu | "
             "sun=(%.2f,%.2f,%.2f) | FPS=%.1f draw=%d",
             gFrameCount, diag.visibleTiles, diag.cachedTextures,
             diag.renderSurfaceTiles, diag.surfaceMeshCount,
             diag.imageryAttachments, diag.imageryExactAttachments,
             diag.imageryParentFallbackAttachments,
             diag.normalMapTextures,
             diag.staleSurfaceCommands, diag.missingGenerationSurfaceCommands,
             diag.imageryMissingTiles,
             diag.imageryUnsupportedTiles,
             diag.lodSizePixels,
             diag.quadtreeEqualZoomLayers,
             diag.quadtreeRenderingNodes,
             diag.quadtreeWalkthroughNodes,
             diag.quadtreeInFrustumNodes,
             diag.quadtreeFadingNodes,
             diag.mercatorTileCount,
             diag.northPolarTileCount,
             diag.southPolarTileCount,
             static_cast<unsigned long long>(diag.maxSurfaceGeneration),
             sunDir.x(), sunDir.y(), sunDir.z(),
             diag.fps, diag.drawCalls);
    }
}

// ============================================================
// JNI 桥接
// ============================================================

extern "C" {

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeSurfaceCreated(
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
Java_com_earthengine_minimalglobe_GLESView_nativeSurfaceChanged(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    gWidth = width;
    gHeight = height;
    if (gEngine) {
        gEngine->onSurfaceChanged(width, height, 1.0f);
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeRenderFrame(
    JNIEnv* /* env */, jobject /* this */) {
    renderFrame();
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeSurfaceDestroyed(
    JNIEnv* /* env */, jobject /* this */) {
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
    event.timestamp = androidUptimeSeconds();
    gEngine->onInputEvent(event);
    gDebugPinchActive = false;
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeTouchDown(
    JNIEnv* /* env */, jobject /* this */) {
    endDebugPinchIfNeeded(static_cast<float>(gWidth) * 0.5f,
                          static_cast<float>(gHeight) * 0.5f);
    gTouching = true;
    gDragStarted = false;
    gTouchMoved = false;
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeDrag(
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
Java_com_earthengine_minimalglobe_GLESView_nativeTouchUp(
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
Java_com_earthengine_minimalglobe_GLESView_nativePinchStart(
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
    event.timestamp = androidUptimeSeconds();
    gEngine->onInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativePinchEnd(
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
Java_com_earthengine_minimalglobe_GLESView_nativePinchRotateTilt(
    JNIEnv* /* env */, jobject /* this */,
    jfloat scale, jfloat rotationRadians,
    jfloat centerX, jfloat centerY, jfloat centerDx, jfloat centerDy,
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
    event.timestamp = androidUptimeSeconds();
    gEngine->onInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeDebugZoom(
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
    move.timestamp = ts + 0.016;
    gEngine->onInputEvent(move);

    const auto& diag = gEngine->diagnostics();
    const double cameraRadius = gEngine->camera().position().length();
    const double cameraAltitude = cameraRadius - 6378137.0;
    LOGI("Debug zoom scale=%.2f | tiles vis=%d cached=%d renderSurface=%d "
         "exact=%d parent=%d missing=%d unsupported=%d lod=%.0f eq=%d qRender=%d qWalk=%d "
         "qFrustum=%d grp=%d/%d/%d alt=%.2f radius=%.2f FPS=%.1f draw=%d",
         scale,
         diag.visibleTiles,
         diag.cachedTextures,
         diag.renderSurfaceTiles,
         diag.imageryExactAttachments,
         diag.imageryParentFallbackAttachments,
         diag.imageryMissingTiles,
         diag.imageryUnsupportedTiles,
         diag.lodSizePixels,
         diag.quadtreeEqualZoomLayers,
         diag.quadtreeRenderingNodes,
         diag.quadtreeWalkthroughNodes,
         diag.quadtreeInFrustumNodes,
         diag.mercatorTileCount,
         diag.northPolarTileCount,
         diag.southPolarTileCount,
         cameraAltitude,
         cameraRadius,
         diag.fps,
         diag.drawCalls);
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativePause(
    JNIEnv* /* env */, jobject /* this */) {
    // TODO: 后续阶段通知 PlatformBridge::onEnterBackground()
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeResume(
    JNIEnv* /* env */, jobject /* this */) {
    // TODO: 后续阶段通知 PlatformBridge::onEnterForeground()
}

} // extern "C"
