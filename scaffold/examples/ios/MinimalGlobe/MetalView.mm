#import "MetalView.h"
#import <QuartzCore/CADisplayLink.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include "earth_engine/Engine.h"
#include "earth_engine/content/EllipsoidTerrainContentProvider.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/platform/ios/RenderDeviceMetal.h"
#include "earth_engine/platform/ios/IosPlatformBridge.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/environment/TimeController.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace earth_engine;

// PlatformBridge 现由引擎提供：earth_engine::IosPlatformBridge
// （NSURLSession 网络 / ImageIO 解码 / Keychain 鉴权 / UIKit 设备信息）。
// 原先此处的 IosDemoPlatformBridge 内联桩已移除。

@implementation MetalView {
    CADisplayLink *_displayLink;
    std::unique_ptr<RenderDeviceMetal> _renderDevice;
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<IosPlatformBridge> _platformBridge;
    std::vector<std::unique_ptr<RasterOverlay>> _rasterOverlays;
    std::vector<std::unique_ptr<ActivatedRasterOverlay>> _activatedRasterOverlays;
    BOOL _engineReady;
    int _frameCount;

    // Touch state（裸 touches 接管：单指 Pointer*，双指 Pinch*+pointer pair。
    // 旧 UIPinchGestureRecognizer 只给累积 scale，rotation/质心位移全丢——
    // twist/pitch 在 iOS 上曾是死代码；换裸 touches 后与 Android 同一条
    // InputManager latch + 新契约路径。）
    NSMutableArray<UITouch *> *_activeTouches;
    BOOL _pinchActive;
    BOOL _suppressSingleUntilAllUp;
}

- (instancetype)initWithFrame:(CGRect)frame {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:frame device:device];
    if (self) {
        self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        self.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        self.clearColor = MTLClearColorMake(0.0, 0.0, 0.1, 1.0);
        self.enableSetNeedsDisplay = NO;
        self.multipleTouchEnabled = YES;  // MTKView 默认关，双指事件必需
        _activeTouches = [NSMutableArray array];

        // 创建引擎
        [self createEngine];

        // 渲染循环
        _displayLink = [CADisplayLink displayLinkWithTarget:self
                                                   selector:@selector(renderFrame)];
        [_displayLink addToRunLoop:NSRunLoop.mainRunLoop
                           forMode:NSRunLoopCommonModes];

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
        NSLog(@"Engine initialized successfully, camera pos: %.1f,%.1f,%.1f",
              _engine->camera().position().x(),
              _engine->camera().position().y(),
              _engine->camera().position().z());

        std::unique_ptr<ImageryProvider> provider =
            std::make_unique<DebugImageryProvider>();
        std::unique_ptr<TileScheme> scheme =
            TileScheme::createXYZWebMercator();
        NSLog(@"Debug standard XYZ WebMercator provider enabled");

        _rasterOverlays.clear();
        _activatedRasterOverlays.clear();

        auto overlay = std::make_unique<RasterOverlay>(
            std::move(provider), std::move(scheme), RasterOverlay::Options{});
        auto activeOverlay = std::make_unique<ActivatedRasterOverlay>(*overlay);

        std::vector<ActivatedRasterOverlay*> rasterOverlays{activeOverlay.get()};
        _rasterOverlays.push_back(std::move(overlay));
        _activatedRasterOverlays.push_back(std::move(activeOverlay));

        TilesetOptions tilesetOptions;
        auto tileset = std::make_unique<Tileset>(
            TileScheme::createXYZWebMercator(),
            std::move(rasterOverlays),
            _renderDevice.get(),
            tilesetOptions,
            std::make_unique<EllipsoidTerrainContentProvider>(
                "XYZ-WebMercator"));
        _engine->setTileset(std::move(tileset));
        NSLog(@"Unified Tileset raster overlay added");

        // 设置模拟时间为当前系统时间
        double nowJd = currentJulianDate();
        _engine->setTime(nowJd);
        NSLog(@"Simulation time set to JD %.3f", nowJd);
    } else {
        NSLog(@"Engine initialization failed");
    }
}

// ---- 裸 touches 手势输入 ----
// 单指：PointerDown/Move/Up（drag/click 由 InputManager 识别）。
// 双指：PinchStart + 带 pointer pair 的 PinchMove（派生量与 Manipulate/Pitch
// latch 由 InputManager 统一计算）+ PinchEnd。
// 双指结束后剩余单指抑制到全部抬起（避免尾巴误产生 click/drag）。

- (earth_engine::InputEvent)baseEventAt:(CGPoint)location {
    CGFloat scale = self.contentScaleFactor;
    earth_engine::InputEvent event;
    event.screenX = static_cast<float>(location.x * scale);
    event.screenY = static_cast<float>(location.y * scale);
    event.devicePixelRatio = static_cast<float>(scale);
    event.pointerType = earth_engine::InputEvent::PointerType::Touch;
    // CACurrentMediaTime 返回单调递增秒数（与 InputEvent.timestamp 约定一致）
    event.timestamp = CACurrentMediaTime();
    return event;
}

- (void)sendPinchMoveFromActiveTouches {
    CGFloat scale = self.contentScaleFactor;
    CGPoint p0 = [_activeTouches[0] locationInView:self];
    CGPoint p1 = [_activeTouches[1] locationInView:self];
    CGPoint center = CGPointMake((p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5);

    earth_engine::InputEvent event = [self baseEventAt:center];
    event.type = earth_engine::InputEvent::Type::PinchMove;
    event.pointerCount = 2;
    event.hasPointerPair = true;
    event.pointer0X = static_cast<float>(p0.x * scale);
    event.pointer0Y = static_cast<float>(p0.y * scale);
    event.pointer1X = static_cast<float>(p1.x * scale);
    event.pointer1Y = static_cast<float>(p1.y * scale);
    _engine->onInputEvent(event);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    if (!_engineReady) return;
    for (UITouch *touch in touches) {
        if (![_activeTouches containsObject:touch]) {
            [_activeTouches addObject:touch];
        }
    }
    if (_activeTouches.count == 1 && !_suppressSingleUntilAllUp) {
        earth_engine::InputEvent down = [self
            baseEventAt:[_activeTouches[0] locationInView:self]];
        down.type = earth_engine::InputEvent::Type::PointerDown;
        _engine->onInputEvent(down);
    } else if (_activeTouches.count >= 2 && !_pinchActive) {
        _pinchActive = YES;
        CGPoint p0 = [_activeTouches[0] locationInView:self];
        CGPoint p1 = [_activeTouches[1] locationInView:self];
        earth_engine::InputEvent start = [self baseEventAt:
            CGPointMake((p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5)];
        start.type = earth_engine::InputEvent::Type::PinchStart;
        start.pointerCount = 2;
        _engine->onInputEvent(start);
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    if (!_engineReady) return;
    if (_pinchActive && _activeTouches.count >= 2) {
        [self sendPinchMoveFromActiveTouches];
    } else if (_activeTouches.count == 1 && !_suppressSingleUntilAllUp) {
        earth_engine::InputEvent move = [self
            baseEventAt:[_activeTouches[0] locationInView:self]];
        move.type = earth_engine::InputEvent::Type::PointerMove;
        _engine->onInputEvent(move);
    }
}

- (void)finishTouches:(NSSet<UITouch *> *)touches cancelled:(BOOL)cancelled {
    if (!_engineReady) return;
    CGPoint last = _activeTouches.count > 0
        ? [_activeTouches[0] locationInView:self]
        : CGPointZero;
    for (UITouch *touch in touches) {
        [_activeTouches removeObject:touch];
    }
    if (_pinchActive && _activeTouches.count < 2) {
        _pinchActive = NO;
        _suppressSingleUntilAllUp = YES;
        earth_engine::InputEvent end = [self baseEventAt:last];
        end.type = earth_engine::InputEvent::Type::PinchEnd;
        end.pointerCount = 2;
        _engine->onInputEvent(end);
    }
    if (_activeTouches.count == 0) {
        if (!_suppressSingleUntilAllUp) {
            UITouch *lifted = touches.anyObject;
            earth_engine::InputEvent up = [self
                baseEventAt:[lifted locationInView:self]];
            up.type = cancelled
                ? earth_engine::InputEvent::Type::Cancel
                : earth_engine::InputEvent::Type::PointerUp;
            _engine->onInputEvent(up);
        }
        _suppressSingleUntilAllUp = NO;
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];
    [self finishTouches:touches cancelled:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesCancelled:touches withEvent:event];
    [self finishTouches:touches cancelled:YES];
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

    ++_frameCount;
    if (_frameCount <= 3 || _frameCount % 300 == 0) {
        const auto& diag = _engine->diagnostics();
        Vec3 sunDir = _engine->sunDirection();
        NSLog(@"Frame %d | tiles vis=%d cached=%d renderSurface=%d mesh=%d "
              "attach=%d exact=%d parent=%d missing=%d unsupported=%d "
              "lod=%.0f eq=%d qRender=%d qWalk=%d qFrustum=%d "
              "grp=%d/%d/%d | sun=(%.2f,%.2f,%.2f) | FPS=%.1f draw=%d",
              _frameCount,
              diag.visibleTiles,
              diag.cachedTextures,
              diag.renderSurfaceTiles,
              diag.surfaceMeshCount,
              diag.imageryAttachments,
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
              sunDir.x(), sunDir.y(), sunDir.z(),
              diag.fps,
              diag.drawCalls);
    }
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
    if (_engine) {
        earth_engine::InputEvent event;
        event.type = earth_engine::InputEvent::Type::Cancel;
        event.timestamp = CACurrentMediaTime();
        _engine->onInputEvent(event);
    }
    _engine.reset();
    _renderDevice.reset();
    _platformBridge.reset();
    [_displayLink invalidate];
}

@end
