#import "MetalView.h"
#import <QuartzCore/CADisplayLink.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include "earth_engine/Engine.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/platform/ios/RenderDeviceMetal.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
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

namespace {

constexpr const char* kGaodeSatelliteTemplate =
    "https://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}";
constexpr bool kUseGaodeSatelliteForDemo = true;

class IosDemoHttpRequest : public HttpRequest {
public:
    void cancel() override {}
};

class IosDemoPlatformBridge : public PlatformBridge {
public:
    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int statusCode, std::vector<uint8_t> body)> callback) override {
        std::thread([url, callback = std::move(callback)]() {
            @autoreleasepool {
                NSURL* nsUrl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
                if (!nsUrl) {
                    callback(-1, {});
                    return;
                }

                NSURLRequest* request = [NSURLRequest requestWithURL:nsUrl
                                                         cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                     timeoutInterval:15.0];
                NSHTTPURLResponse* response = nil;
                NSError* error = nil;
                NSData* data = [NSURLConnection sendSynchronousRequest:request
                                                      returningResponse:&response
                                                                  error:&error];
                if (error || !data || response.statusCode != 200) {
                    callback(error ? -1 : static_cast<int>(response.statusCode), {});
                    return;
                }

                const uint8_t* bytes = static_cast<const uint8_t*>(data.bytes);
                callback(static_cast<int>(response.statusCode),
                         std::vector<uint8_t>(bytes, bytes + data.length));
            }
        }).detach();
        return std::make_unique<IosDemoHttpRequest>();
    }

    std::string cacheDirectory() const override { return NSTemporaryDirectory().UTF8String ?: "/tmp"; }
    std::string documentsDirectory() const override { return NSHomeDirectory().UTF8String ?: "."; }

    std::unique_ptr<DecodedImage> decodeImage(const uint8_t* data, size_t len) override {
        @autoreleasepool {
            NSData* nsData = [NSData dataWithBytes:data length:len];
            UIImage* image = [UIImage imageWithData:nsData];
            CGImageRef cgImage = image.CGImage;
            if (!cgImage) return nullptr;

            const size_t width = CGImageGetWidth(cgImage);
            const size_t height = CGImageGetHeight(cgImage);
            if (width == 0 || height == 0) return nullptr;

            auto decoded = std::make_unique<DecodedImage>();
            decoded->width = static_cast<int>(width);
            decoded->height = static_cast<int>(height);
            decoded->channels = 4;
            decoded->pixels.resize(width * height * 4);

            CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
            CGContextRef context = CGBitmapContextCreate(
                decoded->pixels.data(),
                width,
                height,
                8,
                width * 4,
                colorSpace,
                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(colorSpace);
            if (!context) return nullptr;

            CGContextDrawImage(context, CGRectMake(0, 0, width, height), cgImage);
            CGContextRelease(context);
            return decoded;
        }
    }

    void log(LogLevel /*level*/, const std::string& tag, const std::string& message) override {
        NSLog(@"[%s] %s", tag.c_str(), message.c_str());
    }

    DeviceInfo deviceInfo() const override {
        DeviceInfo info;
        info.platform = "iOS";
        info.model = UIDevice.currentDevice.model.UTF8String ?: "iOS";
        info.osVersion = UIDevice.currentDevice.systemVersion.UTF8String ?: "";
        info.screenDensity = static_cast<float>(UIScreen.mainScreen.scale);
        info.screenWidthPx = static_cast<int>(UIScreen.mainScreen.bounds.size.width * UIScreen.mainScreen.scale);
        info.screenHeightPx = static_cast<int>(UIScreen.mainScreen.bounds.size.height * UIScreen.mainScreen.scale);
        info.cpuCores = static_cast<int>([NSProcessInfo processInfo].processorCount);
        info.totalMemoryBytes = static_cast<int64_t>([NSProcessInfo processInfo].physicalMemory);
        return info;
    }

    std::string getToken(const std::string& /*providerId*/) const override { return ""; }
};

class FlatTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "flat-terrain"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 14; }
    int tileSize() const override { return 17; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(const TileKey& key,
                     CancellationToken,
                     HeightmapCallback callback) override {
        auto heightmap = std::make_unique<DecodedHeightmap>();
        heightmap->tileSize = tileSize();
        heightmap->heights.assign(static_cast<size_t>(tileSize() * tileSize()), 0.0f);
        callback(key, TerrainTileLoadResult::success(std::move(heightmap)));
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t) override {
        return nullptr;
    }
};

} // namespace

@implementation MetalView {
    CADisplayLink *_displayLink;
    std::unique_ptr<RenderDeviceMetal> _renderDevice;
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<IosDemoPlatformBridge> _platformBridge;
    std::vector<std::unique_ptr<RasterOverlay>> _rasterOverlays;
    std::vector<std::unique_ptr<ActivatedRasterOverlay>> _activatedRasterOverlays;
    BOOL _engineReady;
    int _frameCount;

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
        NSLog(@"Engine initialized successfully, camera pos: %.1f,%.1f,%.1f",
              _engine->camera().position().x(),
              _engine->camera().position().y(),
              _engine->camera().position().z());

        std::unique_ptr<ImageryProvider> provider;
        std::unique_ptr<TileScheme> scheme;
        if (kUseGaodeSatelliteForDemo) {
            _platformBridge = std::make_unique<IosDemoPlatformBridge>();
            auto xyz = std::make_unique<XYZImageryProvider>(
                kGaodeSatelliteTemplate,
                "Gaode/Amap satellite imagery");
            xyz->setZoomRange(0, 18);
            xyz->setOpenGlobusGroupedY(true);
            xyz->setOpenGlobusPolarGroupsEnabled(false);
            xyz->setPlatformBridge(_platformBridge.get());
            provider = std::move(xyz);
            scheme = TileScheme::createOpenGlobusEarth();
            NSLog(@"Gaode satellite provider enabled: %s", kGaodeSatelliteTemplate);
            NSLog(@"Gaode/Amap tiles are GCJ-02 aligned; this demo is visual only, not WGS84 control-point acceptance");
        } else {
            provider = std::make_unique<DebugImageryProvider>();
            scheme = TileScheme::createXYZWebMercator();
            NSLog(@"Debug standard XYZ WebMercator provider enabled");
        }

        _rasterOverlays.clear();
        _activatedRasterOverlays.clear();

        auto overlay = std::make_unique<RasterOverlay>(
            std::move(provider), std::move(scheme), RasterOverlay::Options{});
        auto activeOverlay = std::make_unique<ActivatedRasterOverlay>(*overlay);
        activeOverlay->setTileProvider(std::make_unique<RasterOverlayTileProvider>(
            overlay->getProvider(),
            overlay->getTileScheme(),
            _renderDevice.get()));
        activeOverlay->getTileProvider()->setOwner(overlay.get());

        std::vector<ActivatedRasterOverlay*> rasterOverlays{activeOverlay.get()};
        _rasterOverlays.push_back(std::move(overlay));
        _activatedRasterOverlays.push_back(std::move(activeOverlay));

        TilesetOptions tilesetOptions;
        auto tileset = std::make_unique<Tileset>(
            std::make_unique<FlatTerrainProvider>(),
            TileScheme::createXYZWebMercator(),
            std::move(rasterOverlays),
            _renderDevice.get(),
            tilesetOptions);
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
            event.type = gesture.state == UIGestureRecognizerStateCancelled
                ? earth_engine::InputEvent::Type::Cancel
                : earth_engine::InputEvent::Type::PointerUp;
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

    // UIPinchGestureRecognizer 的 locationInView 返回双指中心
    CGPoint center = [gesture locationInView:self];
    CGFloat scale = self.contentScaleFactor;
    // UIPinchGestureRecognizer.scale 是累积值（相对手势起点），
    // CameraController::onPinchGesture 期望逐帧 scale（与 OpenGlobus
    // zoomCur.length / zoomPrev.length 一致）。
    float perFrameScale = 1.0f;
    float cumulativeScale = static_cast<float>(gesture.scale);

    switch (gesture.state) {
        case UIGestureRecognizerStateBegan: {
            _lastPinchScale = 1.0f;
            earth_engine::InputEvent event;
            event.type = earth_engine::InputEvent::Type::PinchStart;
            event.screenX = static_cast<float>(center.x * scale);
            event.screenY = static_cast<float>(center.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pinchScale = 1.0f;
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.pointerCount = 2;
            event.timestamp = CACurrentMediaTime();
            _engine->onInputEvent(event);
            break;
        }
        case UIGestureRecognizerStateChanged: {
            // 逐帧 scale = 当前累积 / 上一帧累积
            perFrameScale = (_lastPinchScale > 0.001f)
                ? cumulativeScale / _lastPinchScale
                : 1.0f;
            _lastPinchScale = cumulativeScale;

            earth_engine::InputEvent event;
            event.type = earth_engine::InputEvent::Type::PinchMove;
            event.screenX = static_cast<float>(center.x * scale);
            event.screenY = static_cast<float>(center.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pinchScale = perFrameScale;
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.pointerCount = 2;
            event.timestamp = CACurrentMediaTime();
            _engine->onInputEvent(event);
            break;
        }
        case UIGestureRecognizerStateEnded:
        case UIGestureRecognizerStateCancelled: {
            _lastPinchScale = 1.0f;
            earth_engine::InputEvent event;
            event.type = gesture.state == UIGestureRecognizerStateCancelled
                ? earth_engine::InputEvent::Type::Cancel
                : earth_engine::InputEvent::Type::PinchEnd;
            event.screenX = static_cast<float>(center.x * scale);
            event.screenY = static_cast<float>(center.y * scale);
            event.devicePixelRatio = static_cast<float>(scale);
            event.pinchScale = 1.0f;
            event.pointerType = earth_engine::InputEvent::PointerType::Touch;
            event.pointerCount = 2;
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
