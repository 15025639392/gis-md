#import "MetalView.h"

#include "earth_engine/Engine.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/platform/ios/RenderDeviceMetal.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/layers/BasemapLayer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/environment/TimeController.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "MacPlatformBridge.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>
#include <chrono>
#include <memory>

using namespace earth_engine;

namespace {

constexpr const char* kGaodeSatelliteTemplate =
    "https://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}";
constexpr bool kEnableDebugOverlayForDemo = false;
constexpr bool kShowNormalMapForDemo = false;
constexpr bool kUseGaodeSatelliteForDemo = true;

} // anonymous namespace

@interface MetalView ()
@property (nonatomic, retain) CADisplayLink* displayLink;
@end

@implementation MetalView {
    std::unique_ptr<RenderDeviceMetal> _renderDevice;
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<MacPlatformBridge> _platformBridge;
    BOOL _engineReady;
    BOOL _initialized;
    int _frameCount;
    CGFloat _lastMagnification;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        [self setupGestures];
    }
    return self;
}

- (CALayer *)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.colorspace = nil;
    return layer;
}

/// Must be called after view is in a window.
- (void)startEngineWithScale:(CGFloat)scale {
    if (_initialized) return;
    _initialized = YES;

    // Defer Metal setup to next run loop iteration to ensure
    // the window is fully on-screen
    dispatch_async(dispatch_get_main_queue(), ^{
        CAMetalLayer* metalLayer = (CAMetalLayer*)self.layer;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        metalLayer.device = device;
        metalLayer.framebufferOnly = YES;
        metalLayer.maximumDrawableCount = 3;
        self.layer.contentsScale = scale;

        [self createEngine];
        [self setupDisplayLink];
    });
}

- (void)createEngine {
    CAMetalLayer* metalLayer = (CAMetalLayer*)self.layer;
    _renderDevice = std::make_unique<RenderDeviceMetal>((__bridge void*)metalLayer);
    _engine = std::make_unique<Engine>(_renderDevice.get());

    _engine->onSurfaceCreated();

    CGFloat scale = self.layer.contentsScale;
    _engine->onSurfaceChanged(
        static_cast<int>(self.bounds.size.width * scale),
        static_cast<int>(self.bounds.size.height * scale),
        static_cast<float>(scale));

    _engineReady = _engine->isReady();
    if (_engineReady) {
        NSLog(@"Engine initialized successfully");

        std::unique_ptr<ImageryProvider> provider;
        std::unique_ptr<TileScheme> scheme;

        if (kUseGaodeSatelliteForDemo) {
            _platformBridge = std::make_unique<MacPlatformBridge>();
            auto xyz = std::make_unique<XYZImageryProvider>(
                kGaodeSatelliteTemplate,
                "Gaode/Amap satellite imagery");
            xyz->setZoomRange(0, 18);
            xyz->setOpenGlobusGroupedY(true);
            xyz->setOpenGlobusPolarGroupsEnabled(false);
            xyz->setPlatformBridge(_platformBridge.get());
            provider = std::move(xyz);
            scheme = TileScheme::createOpenGlobusEarth();
        } else {
            provider = std::make_unique<DebugImageryProvider>();
            scheme = TileScheme::createXYZWebMercator();
        }

        auto layer = std::make_unique<BasemapLayer>(
            std::move(provider), std::move(scheme), _renderDevice.get());
        layer->setNormalMapDebugEnabled(kShowNormalMapForDemo);
        _engine->addLayer(std::move(layer));

        _engine->setDebugOverlayEnabled(kEnableDebugOverlayForDemo);

        // Match Android demo: default view over Chongqing, China
        {
            const auto& ellipsoid = Ellipsoid::WGS84();
            const double centerLng = 106.508, centerLat = 29.617;
            auto targetEcef = ellipsoid.cartographicToCartesian(
                Cartographic::fromDegrees(centerLng, centerLat, 0.0));
            auto camEcef = ellipsoid.cartographicToCartesian(
                Cartographic::fromDegrees(centerLng, centerLat, 30000.0));
            Vec3 up = ellipsoid.geodeticSurfaceNormal(targetEcef);
            _engine->camera().lookAt(camEcef, targetEcef, up);
        }

        double nowJd = currentJulianDate();
        _engine->setTime(nowJd);

        // Render one frame immediately to verify the pipeline
        _engine->render(0.016);
        NSLog(@"First frame rendered");
    } else {
        NSLog(@"Engine initialization FAILED");
    }
}

- (void)setupDisplayLink {
    // macOS 26.2 displays: use NSTimer for rendering loop.
    // NSView.displayLinkWithTarget:selector: may return nil for views
    // in certain window configurations. NSTimer is reliable.
    [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
                                    target:self
                                  selector:@selector(renderFrame)
                                  userInfo:nil
                                   repeats:YES];
}

- (void)setupGestures {
    NSPanGestureRecognizer* pan = [[NSPanGestureRecognizer alloc]
        initWithTarget:self action:@selector(handlePan:)];
    pan.buttonMask = 1;
    [self addGestureRecognizer:pan];

    NSMagnificationGestureRecognizer* pinch = [[NSMagnificationGestureRecognizer alloc]
        initWithTarget:self action:@selector(handleMagnification:)];
    [self addGestureRecognizer:pinch];

    NSPanGestureRecognizer* rightPan = [[NSPanGestureRecognizer alloc]
        initWithTarget:self action:@selector(handlePan:)];
    rightPan.buttonMask = 2;
    [self addGestureRecognizer:rightPan];
}

- (void)handlePan:(NSPanGestureRecognizer*)gesture {
    if (!_engineReady) return;

    NSPoint location = [gesture locationInView:self];
    CGFloat scale = self.layer.contentsScale;
    double timestamp = [NSProcessInfo processInfo].systemUptime;

    switch (gesture.state) {
        case NSGestureRecognizerStateBegan: {
            InputEvent event;
            event.type = InputEvent::Type::PointerDown;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pointerType = InputEvent::PointerType::Mouse;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        case NSGestureRecognizerStateChanged: {
            InputEvent event;
            event.type = InputEvent::Type::PointerMove;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pointerType = InputEvent::PointerType::Mouse;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        case NSGestureRecognizerStateEnded:
        case NSGestureRecognizerStateCancelled: {
            InputEvent event;
            event.type = gesture.state == NSGestureRecognizerStateCancelled
                ? InputEvent::Type::Cancel
                : InputEvent::Type::PointerUp;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pointerType = InputEvent::PointerType::Mouse;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        default:
            break;
    }
}

- (void)handleMagnification:(NSMagnificationGestureRecognizer*)gesture {
    if (!_engineReady) return;

    NSPoint location = [gesture locationInView:self];
    CGFloat scale = self.layer.contentsScale;
    double timestamp = [NSProcessInfo processInfo].systemUptime;

    switch (gesture.state) {
        case NSGestureRecognizerStateBegan: {
            _lastMagnification = 0.0;
            InputEvent event;
            event.type = InputEvent::Type::PinchStart;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pinchScale = 1.0f;
            event.pointerType = InputEvent::PointerType::Touch;
            event.pointerCount = 2;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        case NSGestureRecognizerStateChanged: {
            float delta = static_cast<float>(gesture.magnification);
            float perFrameScale = 1.0f + delta;
            _lastMagnification = gesture.magnification;

            InputEvent event;
            event.type = InputEvent::Type::PinchMove;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pinchScale = perFrameScale;
            event.pointerType = InputEvent::PointerType::Touch;
            event.pointerCount = 2;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        case NSGestureRecognizerStateEnded:
        case NSGestureRecognizerStateCancelled: {
            _lastMagnification = 0.0;
            InputEvent event;
            event.type = gesture.state == NSGestureRecognizerStateCancelled
                ? InputEvent::Type::Cancel
                : InputEvent::Type::PinchEnd;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pinchScale = 1.0f;
            event.pointerType = InputEvent::PointerType::Touch;
            event.pointerCount = 2;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        default:
            break;
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
    const auto& diag = _engine->diagnostics();
    if (_frameCount <= 3 || _frameCount % 60 == 0) {
        const auto& cam = _engine->camera();
        double camH = cam.position().length();
        fprintf(stderr,
            "[MetalView] Frame %d | draw=%d vis=%d cache=%d surf=%d "
            "lod=%.0f que=%d exact=%d missing=%d unsup=%d "
            "camAlt=%.0f\n",
            _frameCount,
            diag.drawCalls,
            diag.visibleTiles,
            diag.cachedTextures,
            diag.renderSurfaceTiles,
            diag.lodSizePixels,
            diag.queuedRequests,
            diag.imageryExactAttachments,
            diag.imageryMissingTiles,
            diag.imageryUnsupportedTiles,
            camH - 6378137.0);
    }
}

- (void)resizeWithOldSuperviewSize:(NSSize)oldSize {
    [super resizeWithOldSuperviewSize:oldSize];
    if (_engine) {
        CGFloat scale = self.layer.contentsScale;
        _engine->onSurfaceChanged(
            static_cast<int>(self.bounds.size.width * scale),
            static_cast<int>(self.bounds.size.height * scale),
            static_cast<float>(scale));
    }
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (_engine && self.window) {
        CGFloat scale = self.window.backingScaleFactor;
        self.layer.contentsScale = scale;
        _engine->onSurfaceChanged(
            static_cast<int>(self.bounds.size.width * scale),
            static_cast<int>(self.bounds.size.height * scale),
            static_cast<float>(scale));
    }
}

- (void)dealloc {
    if (_engine) {
        InputEvent event;
        event.type = InputEvent::Type::Cancel;
        event.timestamp = [NSProcessInfo processInfo].systemUptime;
        _engine->onInputEvent(event);
    }
    [_displayLink invalidate];
    [_displayLink release];
    _engine.reset();
    _renderDevice.reset();
    _platformBridge.reset();
    [super dealloc];
}

@end
