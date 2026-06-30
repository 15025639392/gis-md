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
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        self.layer = [CAMetalLayer layer];
        _metalLayer = (CAMetalLayer*)self.layer;
        _metalLayer.device = MTLCreateSystemDefaultDevice();
        _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        _metalLayer.framebufferOnly = NO;

        // Engine + RenderDevice
        _renderDevice = std::make_unique<earth_engine::RenderDeviceMetal>(
            (__bridge void*)_metalLayer);
        _platformBridge = std::make_unique<earth_engine::MacPlatformBridge>();
        _engine = std::make_unique<earth_engine::Engine>(_renderDevice.get());

        _engine->onSurfaceCreated();
        _engine->onSurfaceChanged(
            (int)frameRect.size.width,
            (int)frameRect.size.height, 1.0f);

        // SDK scene config — Beijing viewpoint
        _sdkFacade = std::make_unique<earth_engine::EarthEngineSdkFacade>(
            *_engine, *_renderDevice, *_platformBridge);

        earth_engine::EarthSceneConfig config;
        config.initialCamera = {116.3913, 39.9039, 15000000.0};
        config.tileset = {4.0, 2.0};
        _sdkFacade->installScene(config);

        _engineReady = _engine->isReady();
        _frameCount = 0;
    }
    return self;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        [self startRenderLoop];
    }
}

static CVReturn displayLinkCallback(
    CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
    CVOptionFlags, CVOptionFlags*, void* context) {
    MetalView* self = (__bridge MetalView*)context;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self renderFrame];
    });
    return kCVReturnSuccess;
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

    // Periodic log
    if (_frameCount % 60 == 0 && _engine) {
        auto pos = _engine->camera().position();
        auto carto = earth_engine::Ellipsoid::WGS84()
            .cartesianToCartographic(pos);
        NSLog(@"[MinimalGlobe] frame=%d alt=%.0fm lng=%.2f lat=%.2f",
              _frameCount, carto.height(),
              carto.longitudeDegrees(), carto.latitudeDegrees());
    }
}

- (void)dealloc {
    [self stopRenderLoop];
    _sdkFacade.reset();
    _engine.reset();
    _platformBridge.reset();
    _renderDevice.reset();
}

@end
