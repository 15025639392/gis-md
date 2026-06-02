#import <MetalKit/MetalKit.h>
#import <QuartzCore/CADisplayLink.h>
#import <objc/runtime.h>

#include "earth_engine/Engine.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/platform/ios/RenderDeviceMetal.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/layers/BasemapLayer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/environment/TimeController.h"
#include "earth_engine/scene/FrameState.h"
#include <chrono>
#include <memory>

using namespace earth_engine;

/// 最小 Metal 渲染视图 — 使用 earth_engine::Engine + RenderDeviceMetal。
@interface MetalView : MTKView
@end

@implementation MetalView {
    CADisplayLink *_displayLink;
    std::unique_ptr<RenderDeviceMetal> _renderDevice;
    std::unique_ptr<Engine> _engine;
    BOOL _engineReady;

    // Touch state
    BOOL _touching;
    CGFloat _lastPinchScale;
}

- (instancetype)initWithFrame:(CGRect)frame {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:frame device:device];
    if (self) {
        self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        self.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        self.clearColor = MTLClearColorMake(0.0, 0.0, 0.1, 1.0);
        self.enableSetNeedsDisplay = NO;

        // 创建引擎
        [self createEngine];

        // 渲染循环
        _displayLink = [CADisplayLink displayLinkWithTarget:self
                                                   selector:@selector(renderFrame)];
        [_displayLink addToRunLoop:NSRunLoop.mainRunLoop
                           forMode:NSRunLoopCommonModes];

        // 手势
        [self setupGestures];
    }
    return self;
}

- (void)createEngine {
    // 将 CAMetalLayer 传递给 RenderDeviceMetal
    // 注意：self.layer 在 MTKView 中已是 CAMetalLayer
    CAMetalLayer *metalLayer = (CAMetalLayer *)self.layer;
    _renderDevice = std::make_unique<RenderDeviceMetal>((__bridge void *)metalLayer);
    _engine = std::make_unique<Engine>(_renderDevice.get());

    _engine->onSurfaceCreated();
    CGFloat scale = self.contentScaleFactor;
    _engine->onSurfaceChanged(
        static_cast<int>(self.bounds.size.width * scale),
        static_cast<int>(self.bounds.size.height * scale),
        static_cast<float>(scale));

    _engineReady = _engine->isReady();
    if (_engineReady) {
        NSLog(@"Engine initialized successfully");

        // 添加调试底图图层
        auto provider = std::make_unique<DebugImageryProvider>();
        auto scheme = TileScheme::createXYZWebMercator();
        auto layer = std::make_unique<BasemapLayer>(
            std::move(provider), std::move(scheme), _renderDevice.get());
        _engine->addLayer(std::move(layer));
        NSLog(@"Debug basemap layer added");

        // 开启调试叠加层
        _engine->setDebugOverlayEnabled(true);
        NSLog(@"Debug overlay enabled");

        // 设置模拟时间为当前系统时间
        double nowJd = currentJulianDate();
        _engine->setTime(nowJd);
        NSLog(@"Simulation time set to JD %.3f", nowJd);
    } else {
        NSLog(@"Engine initialization failed");
    }
}

- (void)setupGestures {
    UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc]
        initWithTarget:self action:@selector(handlePan:)];
    [self addGestureRecognizer:pan];

    UIPinchGestureRecognizer *pinch = [[UIPinchGestureRecognizer alloc]
        initWithTarget:self action:@selector(handlePinch:)];
    [self addGestureRecognizer:pinch];
}

- (void)handlePan:(UIPanGestureRecognizer *)gesture {
    if (!_engineReady) return;

    CGPoint location = [gesture locationInView:self];
    CGFloat scale = self.contentScaleFactor;
    // CACurrentMediaTime 返回单调递增秒数（与 InputEvent.timestamp 约定一致）
    double timestamp = CACurrentMediaTime();

    switch (gesture.state) {
        case UIGestureRecognizerStateBegan: {
            _touching = YES;
            earth_engine::InputEvent event;
            event.type = earth_engine::InputEvent::Type::PointerDown;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        case UIGestureRecognizerStateChanged: {
            earth_engine::InputEvent event;
            event.type = earth_engine::InputEvent::Type::PointerMove;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        case UIGestureRecognizerStateEnded:
        case UIGestureRecognizerStateCancelled: {
            _touching = NO;
            earth_engine::InputEvent event;
            event.type = earth_engine::InputEvent::Type::PointerUp;
            event.screenX = static_cast<float>(location.x * scale);
            event.screenY = static_cast<float>(location.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.timestamp = timestamp;
            _engine->onInputEvent(event);
            break;
        }
        default:
            break;
    }
}

- (void)handlePinch:(UIPinchGestureRecognizer *)gesture {
    if (!_engineReady) return;

    switch (gesture.state) {
        case UIGestureRecognizerStateBegan: {
            _lastPinchScale = 1.0;
            earth_engine::InputEvent event;
            event.type = earth_engine::InputEvent::Type::PinchStart;
            event.pinchScale = 1.0f;
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.timestamp = CACurrentMediaTime();
            _engine->onInputEvent(event);
            break;
        }
        case UIGestureRecognizerStateChanged: {
            earth_engine::InputEvent event;
            event.type = earth_engine::InputEvent::Type::PinchMove;
            event.pinchScale = static_cast<float>(gesture.scale);
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.timestamp = CACurrentMediaTime();
            _engine->onInputEvent(event);
            break;
        }
        case UIGestureRecognizerStateEnded:
        case UIGestureRecognizerStateCancelled: {
            _lastPinchScale = 1.0;
            earth_engine::InputEvent event;
            event.type = earth_engine::InputEvent::Type::PinchEnd;
            event.pinchScale = 1.0f;
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.timestamp = CACurrentMediaTime();
            _engine->onInputEvent(event);
            break;
        }
        default:
            break;
    }
}

- (void)renderFrame {
    if (!_engineReady) return;

    // 时间步进（使用 CADisplayLink 的 targetTimestamp 或自计时）
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastTime).count();
    lastTime = now;

    _engine->advanceTime(dt);
    _engine->render(0.0);  // auto-delta

    // 动态 clear color（1 帧滞后，视觉无感）
    float cr, cg, cb, ca;
    _engine->getClearColor(cr, cg, cb, ca);
    self.clearColor = MTLClearColorMake(cr, cg, cb, ca);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    if (_engine) {
        CGFloat scale = self.contentScaleFactor;
        _engine->onSurfaceChanged(
            static_cast<int>(self.bounds.size.width * scale),
            static_cast<int>(self.bounds.size.height * scale),
            static_cast<float>(scale));
    }
}

- (void)dealloc {
    _engine.reset();
    _renderDevice.reset();
    [_displayLink invalidate];
}

@end
