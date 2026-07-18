#import "MetalView.h"

#include <chrono>

#include "earth_engine/Engine.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/platform/macos/MacPlatformBridge.h"
#include "earth_engine/platform/ios/RenderDeviceMetal.h"
#include "earth_engine/sdk/EarthEngineSdkFacade.h"
#include "earth_engine/sdk/EarthSceneConfig.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"

@implementation MetalView {
    CVDisplayLinkRef _displayLink;
    std::unique_ptr<earth_engine::RenderDeviceMetal> _renderDevice;
    std::unique_ptr<earth_engine::MacPlatformBridge> _platformBridge;
    std::unique_ptr<earth_engine::Engine> _engine;
    std::unique_ptr<earth_engine::EarthEngineSdkFacade> _sdkFacade;
    bool _engineReady;
    int _frameCount;
    // 合帧标记：主队列已有未执行的 render block 时不再追加，防止慢帧下
    // display link 持续投递导致主队列积压、输入延迟无限增长（P2-17）。
    std::atomic<bool> _framePending;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        _metalLayer = [CAMetalLayer layer];
        _metalLayer.device = MTLCreateSystemDefaultDevice();
        _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        _metalLayer.framebufferOnly = YES;
        _metalLayer.drawableSize = frameRect.size;
        // 允许 nextDrawable 超时返回 nil(引擎侧 nil 即跳帧):禁止超时会在
        // GPU 落后时无限期阻塞主线程(P1-4)。
        _metalLayer.allowsNextDrawableTimeout = YES;
        _metalLayer.displaySyncEnabled = YES;
        self.layer = _metalLayer;

        // Engine + RenderDevice (created early, surface inited in viewDidMoveToWindow)
        _renderDevice = std::make_unique<earth_engine::RenderDeviceMetal>(
            (__bridge void*)_metalLayer);
        _platformBridge = std::make_unique<earth_engine::MacPlatformBridge>();
        _engine = std::make_unique<earth_engine::Engine>(_renderDevice.get());
        _frameCount = 0;
    }
    return self;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window && !_engineReady) {
        NSRect frame = self.frame;
        _metalLayer.drawableSize = frame.size;

        _engine->onSurfaceCreated();
        _engine->onSurfaceChanged(
            (int)frame.size.width,
            (int)frame.size.height, 1.0f);

        _sdkFacade = std::make_unique<earth_engine::EarthEngineSdkFacade>(
            *_engine, *_renderDevice, *_platformBridge);

        earth_engine::EarthSceneConfig config;
        config.initialCamera = {106.508, 29.617, 30000.0};
        config.terrain = {
            earth_engine::TerrainSourceKind::QuantizedMesh,
            "http://127.0.0.1:8099/{z}/{x}/{y}.terrain",
            "http://127.0.0.1:8099/layer.json",
            "QuantizedMesh Terrain",
            0, 12, 65, false
        };
        config.tileset = {4.0, 2.0};

        // Gaode satellite base imagery (same as Android demo)
        earth_engine::RasterOverlaySourceConfig satellite;
        satellite.imageryKind = earth_engine::ImagerySourceKind::Xyz;
        satellite.urlTemplate =
            "http://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}";
        satellite.attribution = "Gaode/Amap satellite";
        satellite.minimumZoom = 0;
        satellite.maximumZoom = 18;
        satellite.overlayMinimumZoom = 0;
        satellite.overlayMaximumZoom = 0;
        satellite.maximumSimultaneousTileLoads = 20;
        satellite.maximumScreenSpaceError = 2.0;
        satellite.opacity = 1.0f;
        satellite.role = earth_engine::RasterOverlayRole::BaseImagery;
        satellite.priority = earth_engine::RasterOverlayPriority::High;
        satellite.fallbackPolicy =
            earth_engine::RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
        satellite.blocksCompleteRenderable = true;
        config.rasterOverlays.push_back(satellite);

        _sdkFacade->installScene(config);
        _engineReady = _engine->isReady();
        NSLog(@"[MinimalGlobe] Scene installed. Engine ready=%d drawSize=%.0fx%.0f",
              _engineReady, frame.size.width, frame.size.height);
    }
    if (self.window) {
        [self startRenderLoop];
    }
}

static CVReturn displayLinkCallback(
    CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
    CVOptionFlags, CVOptionFlags*, void* context) {
    MetalView* self = (__bridge MetalView*)context;
    if (![self tryMarkFramePending]) {
        return kCVReturnSuccess;  // 上一帧的 block 还没执行，合帧丢弃
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self clearFramePending];
        [self renderFrame];
    });
    return kCVReturnSuccess;
}

- (BOOL)tryMarkFramePending {
    bool expected = false;
    return _framePending.compare_exchange_strong(expected, true);
}

- (void)clearFramePending {
    _framePending.store(false);
}

- (void)startRenderLoop {
    CVDisplayLinkCreateWithActiveCGDisplays(&_displayLink);
    CVDisplayLinkSetOutputCallback(_displayLink, displayLinkCallback,
                                   (__bridge void*)self);
    CVDisplayLinkStart(_displayLink);
}

- (void)stopRenderLoop {
    if (_displayLink) {
        CVDisplayLinkStop(_displayLink);
        CVDisplayLinkRelease(_displayLink);
        _displayLink = nil;
    }
}

- (void)renderFrame {
    if (!_engineReady) return;

    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastTime).count();
    lastTime = now;

    _engine->advanceTime(dt);
    _engine->render(0.0);
    ++_frameCount;

    // First frame diagnostic
    if (_frameCount == 1) {
        const auto& diag = _engine->diagnostics();
        NSLog(@"[MinimalGlobe] FIRST FRAME: draw=%d tiles=%d fps=%.1f "
              "surface=%d surfCmd=%d terrSurf=%d terrGltf=%d",
              diag.drawCalls, diag.visibleTiles, diag.fps,
              diag.surfaceMeshCount,
              diag.terrainSurfaceCommandsSubmitted,
              diag.terrainSurfaceTileCommands,
              diag.terrainGltfPrimitiveCommands);
    }

    // Periodic log
    if (_frameCount % 60 == 0 && _engine) {
        const auto& diag = _engine->diagnostics();
        auto pos = _engine->camera().position();
        auto carto = earth_engine::Ellipsoid::WGS84()
            .cartesianToCartographic(pos);
        NSLog(@"[MinimalGlobe] frame=%d FPS=%.1f draw=%d tiles=%d alt=%.0fm lng=%.2f lat=%.2f",
              _frameCount, diag.fps, diag.drawCalls, diag.visibleTiles,
              carto.height(),
              carto.longitudeDegrees(), carto.latitudeDegrees());
    }
}

- (void)dealloc {
    [self stopRenderLoop];
    _engineReady = false;
    _sdkFacade.reset();
    _engine.reset();
    _platformBridge.reset();
    _renderDevice.reset();
    [super dealloc];
}

@end
