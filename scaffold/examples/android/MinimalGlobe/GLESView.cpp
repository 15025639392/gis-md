#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <chrono>
#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "earth_engine/Engine.h"
#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/platform/android/RenderDeviceGLES.h"
#include "earth_engine/platform/android/AndroidPlatformBridge.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/providers/HeightmapTerrainProvider.h"
#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/VectorLayer.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/data/GeoJsonParser.h"
#include "earth_engine/style/OverlayStyle.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/environment/TimeController.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/Scene.h"

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
static std::vector<std::unique_ptr<RasterOverlay>> gRasterOverlays;
static std::vector<std::unique_ptr<ActivatedRasterOverlay>> gActivatedRasterOverlays;
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

static std::string tileKeyLabel(const TileKey& key) {
    std::ostringstream out;
    out << key.schemeId << "/" << key.z << "/" << key.x << "/" << key.y;
    return out.str();
}

static const char* renderCommandKindLabel(RenderCommandKind kind) {
    switch (kind) {
        case RenderCommandKind::SkyBackground: return "sky";
        case RenderCommandKind::AtmosphereBackground: return "atmo";
        case RenderCommandKind::GlobeSurface: return "globe";
        case RenderCommandKind::SurfaceTile: return "surface";
        case RenderCommandKind::GltfPrimitive: return "gltf";
        case RenderCommandKind::GltfPrimitiveInstanced: return "gltf-i";
        case RenderCommandKind::VectorOverlay: return "vector";
        case RenderCommandKind::Unknown:
        default: return "unknown";
    }
}

static std::string buildPresentationTraceSummary(const PresentationTrace& trace) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);

    out << "\n\nPresentation trace\n";
    out << "Camera: center="
        << trace.camera.targetLongitudeDegrees << ","
        << trace.camera.targetLatitudeDegrees
        << " h=" << static_cast<int>(trace.camera.targetHeightMeters)
        << " camH=" << static_cast<int>(trace.camera.cameraHeightMeters)
        << " pitch=" << trace.camera.pitchRadians
        << " heading=" << trace.camera.headingRadians
        << " vp=" << trace.camera.viewportWidthPixels
        << "x" << trace.camera.viewportHeightPixels << "\n";
    out << "Selector views: " << trace.selectorViews.size();
    if (!trace.selectorViews.empty()) {
        out << " firstVpH=" << trace.selectorViews.front().viewportHeightPixels;
    }
    out << "\n";

    size_t surfaceCount = 0;
    out << "Commands:";
    for (const PresentationCommandTrace& command : trace.commands) {
        if (command.kind != RenderCommandKind::SurfaceTile &&
            command.kind != RenderCommandKind::GltfPrimitive &&
            command.kind != RenderCommandKind::GltfPrimitiveInstanced) {
            continue;
        }
        if (surfaceCount >= 4) {
            out << " ...";
            break;
        }
        out << " " << renderCommandKindLabel(command.kind)
            << "(gz=" << command.surfaceGeometryZoom
            << ",tz=" << command.surfaceTextureZoom
            << ",idx=" << command.indexCount
            << "/" << command.surfaceMeshIndexCount
            << ",base=real"
            << ",rs=" << command.surfaceBaseRasterState;
        if (command.surfaceBaseIsRectangleTile) {
            out << ",rect";
        }
        if (command.surfaceSkirtIndexCount > 0) {
            out << ",skirt-" << command.surfaceSkirtIndexCount;
        }
        if (command.surfaceClipEnabled > 0.5f) {
            out << ",clip";
        }
        out << ")";
        ++surfaceCount;
    }
    if (surfaceCount == 0) {
        out << " none";
    }
    out << "\n";

    const PresentationTilesetTrace* terrainTrace =
        trace.tilesets.empty() ? nullptr : &trace.tilesets.front();
    if (terrainTrace) {
        out << "Tileset: visible=" << terrainTrace->visibleTiles.size()
            << " renderEntries=" << terrainTrace->renderEntries.size()
            << " zoom=" << terrainTrace->minVisibleZoom
            << "-" << terrainTrace->maxVisibleZoom
            << " lod=" << static_cast<int>(terrainTrace->lodSizePixels)
            << "\n";
        const size_t visibleCount =
            std::min<size_t>(terrainTrace->visibleTiles.size(), 4);
        out << "Visible:";
        for (size_t i = 0; i < visibleCount; ++i) {
            out << " " << tileKeyLabel(terrainTrace->visibleTiles[i]);
        }
        if (terrainTrace->visibleTiles.size() > visibleCount) {
            out << " ...";
        }
        out << "\n";

        const size_t entryCount =
            std::min<size_t>(terrainTrace->renderEntries.size(), 4);
        out << "Render:";
        for (size_t i = 0; i < entryCount; ++i) {
            const auto& entry = terrainTrace->renderEntries[i];
            out << " " << tileKeyLabel(entry.selectedKey)
                << "->" << tileKeyLabel(entry.renderKey);
            if (entry.usesAncestorFallback) {
                out << "[fallback]";
            }
            if (entry.surfaceClipEnabled) {
                out << "[clip]";
            }
        }
        if (terrainTrace->renderEntries.size() > entryCount) {
            out << " ...";
        }
        out << "\n";
    }

    return out.str();
}

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

static constexpr const char* kFabdemTerrainTemplate =
    "http://192.168.1.4:8001/{z}/{x}/{y}.png";
static constexpr const char* kQuantizedMeshTerrainTemplate =
    "http://192.168.100.141:8090/{z}/{x}/{y}.terrain";
static constexpr const char* kQuantizedMeshTerrainLayerJson =
    "http://192.168.100.141:8090/layer.json";
static constexpr const char* kGaodeSatelliteTemplate =
    "https://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}";
static constexpr const char* kGaodeRoadNetTemplate =
    "https://webst0{s}.is.autonavi.com/appmaptile?style=8&x={x}&y={y}&z={z}";
static constexpr const char* kRobotExpressiveGlbUrl =
    "https://maptalks.org/maptalks.three/demo/data/RobotExpressive.glb";
static constexpr bool kEnableTerrainForDemo = true;
static constexpr bool kUseGaodeSatelliteForDemo = true;
static constexpr bool kEnableGaodeRoadNetOverlayForDemo = true;
static constexpr bool kEnableRobotExpressiveGltfDemo = false;
/// Use QuantizedMesh terrain (cesium-native format) instead of RGB heightmap.
static constexpr bool kUseQuantizedMeshTerrain = true;

static void clearDemoEngineObjects() {
    gEngine.reset();
    gActivatedRasterOverlays.clear();
    gRasterOverlays.clear();
    gRenderDevice.reset();
    gPlatformBridge.reset();
    gEngineReady = false;
}

static void setDemoCamera() {
    if (!gEngine) return;

    const auto& ellipsoid = Ellipsoid::WGS84();
    const double centerLng = 106.508;
    const double centerLat = 29.617;
    auto targetEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(centerLng, centerLat, 0.0));
    auto camEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(centerLng, centerLat, 30000.0));
    Vec3 up = ellipsoid.geodeticSurfaceNormal(targetEcef);
    gEngine->camera().lookAt(camEcef, targetEcef, up);
}

static TilesetOptions makeDemoTilesetOptions() {
    TilesetOptions options;
    options.mainThreadLoadingTimeLimit = 4.0;
    options.tileCacheUnloadTimeLimit = 2.0;
    return options;
}

static RasterOverlay::Options makeDemoRasterOverlayOptions(
    float opacity,
    RasterOverlayRole role,
    RasterOverlayPriority priority,
    RasterOverlayFallbackPolicy fallbackPolicy,
    bool blocksCompleteRenderable) {
    RasterOverlay::Options options{};
    options.maximumSimultaneousTileLoads = 20;
    options.maximumScreenSpaceError = 2.0;
    options.minimumZoom = 0;
    options.maximumZoom = 0;
    options.visible = true;
    options.opacity = opacity;
    options.role = role;
    options.priority = priority;
    options.fallbackPolicy = fallbackPolicy;
    options.blocksCompleteRenderable = blocksCompleteRenderable;
    return options;
}

static std::unique_ptr<TerrainProvider> createDemoTerrainProvider() {
    if (!kEnableTerrainForDemo) {
        return {};
    }

    if (kUseQuantizedMeshTerrain) {
        auto qm = std::make_unique<QuantizedMeshTerrainProvider>(
            kQuantizedMeshTerrainTemplate, "QuantizedMesh Terrain");
        qm->setZoomRange(0, 12);
        qm->setTileSize(65);
        qm->setFlipYForUrl(false);
        qm->setPlatformBridge(gPlatformBridge.get());
        if (!qm->configureFromLayerJsonUrl(kQuantizedMeshTerrainLayerJson)) {
            LOGE("QuantizedMesh layer.json load failed: %s",
                 kQuantizedMeshTerrainLayerJson);
        }
        return qm;
    }

    auto hm = std::make_unique<HeightmapTerrainProvider>(
        kFabdemTerrainTemplate, "Mapbox Terrain-RGB");
    hm->setZoomRange(0, 14);
    hm->setEncoding(HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    hm->setTileSize(514);
    hm->setPlatformBridge(gPlatformBridge.get());
    return hm;
}

static void addActivatedRasterOverlay(
    std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    std::unique_ptr<ImageryProvider> provider,
    std::unique_ptr<TileScheme> scheme,
    RasterOverlay::Options options) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::move(provider),
        std::move(scheme),
        options);
    auto active = std::make_unique<ActivatedRasterOverlay>(*overlay);

    rasterOverlays.push_back(active.get());
    gRasterOverlays.push_back(std::move(overlay));
    gActivatedRasterOverlays.push_back(std::move(active));
}

static std::vector<ActivatedRasterOverlay*> createDemoRasterOverlays() {
    gActivatedRasterOverlays.clear();
    gRasterOverlays.clear();

    std::vector<ActivatedRasterOverlay*> rasterOverlays;
    if (kUseGaodeSatelliteForDemo) {
        auto xyz = std::make_unique<XYZImageryProvider>(
            kGaodeSatelliteTemplate, "Gaode/Amap satellite");
        xyz->setZoomRange(0, 18);
        xyz->setPlatformBridge(gPlatformBridge.get());
        addActivatedRasterOverlay(
            rasterOverlays,
            std::move(xyz),
            TileScheme::createXYZWebMercator(),
            makeDemoRasterOverlayOptions(
                1.0f,
                RasterOverlayRole::BaseImagery,
                RasterOverlayPriority::High,
                RasterOverlayFallbackPolicy::AncestorOrPlaceholder,
                true));
        LOGI("Gaode satellite basemap enabled");

        if (kEnableGaodeRoadNetOverlayForDemo) {
            auto road = std::make_unique<XYZImageryProvider>(
                kGaodeRoadNetTemplate, "Gaode/Amap road network");
            road->setZoomRange(0, 18);
            road->setPlatformBridge(gPlatformBridge.get());
            addActivatedRasterOverlay(
                rasterOverlays,
                std::move(road),
                TileScheme::createXYZWebMercator(),
                makeDemoRasterOverlayOptions(
                    0.92f,
                    RasterOverlayRole::AnnotationOverlay,
                    RasterOverlayPriority::Low,
                    RasterOverlayFallbackPolicy::SkipUntilReady,
                    false));
            LOGI("Gaode road network overlay enabled");
        }
    } else {
        auto dbg = std::make_unique<DebugImageryProvider>();
        addActivatedRasterOverlay(
            rasterOverlays,
            std::move(dbg),
            TileScheme::createXYZWebMercator(),
            makeDemoRasterOverlayOptions(
                1.0f,
                RasterOverlayRole::BaseImagery,
                RasterOverlayPriority::High,
                RasterOverlayFallbackPolicy::AncestorOrPlaceholder,
                true));
    }
    return rasterOverlays;
}

static void addRobotExpressiveTileset(const TilesetOptions& tilesetOptions) {
    if (!kEnableRobotExpressiveGltfDemo || !gEngine) {
        return;
    }

    const TileKey robotKey{"Geographic-TMS", 0, 1, 0};
    auto gltfProvider = std::make_unique<SingleGltfContentProvider>(
        robotKey,
        std::string(kRobotExpressiveGlbUrl),
        "RobotExpressive GLB");
    gltfProvider->setPlatformBridge(gPlatformBridge.get());
    gltfProvider->setEastNorthUpPlacementDegrees(
        106.508,
        29.617,
        650.0,
        420.0);

    auto robotTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        gRenderDevice.get(),
        tilesetOptions,
        std::move(gltfProvider));
    gEngine->addTileset(std::move(robotTileset));
    LOGI("RobotExpressive glTF tileset added: %s",
         kRobotExpressiveGlbUrl);
}

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

        setDemoCamera();

        // 创建 Android JNI HTTP 桥接
        gPlatformBridge = std::make_unique<AndroidPlatformBridge>(gJvm);

        // cesium-native aligned: create unified Tileset
        {
            auto terrainProvider = createDemoTerrainProvider();
            std::vector<ActivatedRasterOverlay*> rasterOverlays =
                createDemoRasterOverlays();
            const TilesetOptions tilesetOptions = makeDemoTilesetOptions();
            auto tileset = std::make_unique<Tileset>(
                std::move(terrainProvider),
                TileScheme::createGeographicTMS(),
                std::move(rasterOverlays),
                gRenderDevice.get(),
                tilesetOptions);
            gEngine->setTileset(std::move(tileset));
            LOGI("Unified Tileset created (cesium-native architecture)");

            addRobotExpressiveTileset(tilesetOptions);
        }

        // Keep the feature path available, but avoid covering the basemap while
        // validating XYZ Web Mercator tile alignment.
        // addDemoVectorLayer();

        // 设置模拟时间为固定的白天时间（2026-06-10 14:00 UTC+8 = 06:00 UTC）
        // 对应 JD 2461188.75，确保看到完整大气散射效果
        gEngine->setTime(2461188.75);
        LOGI("Simulation time set to fixed daytime JD 2461188.75 (2026-06-10 14:00 UTC+8)");
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
    event.pointerCount = 2;
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
    const double cameraRadius = gEngine->camera().position().length();
    const double sphericalAltitude = cameraRadius - 6378137.0;
    const double ellipsoidAltitude =
        Ellipsoid::WGS84().cartesianToCartographic(gEngine->camera().position()).height();
    LOGI("Debug zoom scale=%.2f | tiles vis=%d cached=%d renderSurface=%d "
         "exact=%d parent=%d missing=%d unsupported=%d kicked=%d retained=%d "
         "z=%d-%d targetZ=%d-%d texZ=%d-%d lod=%.0f eq=%d qRender=%d qWalk=%d qBal=%d "
         "qFrustum=%d qHz=%d qEq2=%d grp=%d/%d/%d ellAlt=%.2f sphAlt=%.2f radius=%.2f FPS=%.1f draw=%d",
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
Java_com_earthengine_minimalglobe_GLESView_nativeGetDiagnosticsString(
    JNIEnv* env, jobject /* this */) {
    if (!gEngine) return env->NewStringUTF("Engine not ready");

    const auto& diag = gEngine->diagnostics();
    const double cameraRadius = gEngine->camera().position().length();
    const double sphericalAltitude = cameraRadius - 6378137.0;
    const double ellipsoidAltitude =
        Ellipsoid::WGS84().cartesianToCartographic(gEngine->camera().position()).height();
    const double cameraDist = gEngine->camera().position().distanceTo(Vec3::zero());

    char buf[2200];
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
    text += buildPresentationTraceSummary(gEngine->presentationTrace());
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeAddDemoVectorLayer(
    JNIEnv* /* env */, jobject /* this */) {
    if (!gEngine) return;
    addDemoVectorLayer();
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeResetCamera(
    JNIEnv* /* env */, jobject /* this */) {
    if (!gEngine) return;
    setDemoCamera();
    LOGI("Camera reset to Chongqing demo viewpoint");
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativePause(
    JNIEnv* /* env */, jobject /* this */) {
    cancelInputIfNeeded();
    if (gPlatformBridge) {
        gPlatformBridge->onEnterBackground();
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_minimalglobe_GLESView_nativeResume(
    JNIEnv* /* env */, jobject /* this */) {
    if (gPlatformBridge) {
        gPlatformBridge->onEnterForeground();
    }
}

} // extern "C"
