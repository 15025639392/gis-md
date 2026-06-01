#import <MetalKit/MetalKit.h>
#import <QuartzCore/CADisplayLink.h>

/// 最小 Metal 渲染视图。
/// 创建 CAMetalLayer + CADisplayLink 驱动渲染循环。
/// 后续阶段将集成 earth_engine::Engine 和 RenderDeviceMetal。
@interface MetalView : MTKView
@end

@implementation MetalView {
    CADisplayLink *_displayLink;
}

- (instancetype)initWithFrame:(CGRect)frame {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:frame device:device];
    if (self) {
        self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        self.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        self.clearColor = MTLClearColorMake(0.0, 0.0, 0.1, 1.0);
        self.enableSetNeedsDisplay = NO;

        // 渲染循环
        _displayLink = [CADisplayLink displayLinkWithTarget:self
                                                   selector:@selector(renderFrame)];
        [_displayLink addToRunLoop:NSRunLoop.mainRunLoop
                           forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (void)renderFrame {
    // TODO: 后续阶段集成 earth_engine::Engine::render()
    // 当前仅清屏
    id<MTLCommandBuffer> cmdBuf = /* ... */;
    // [cmdBuf presentDrawable:self.currentDrawable];
    // [cmdBuf commit];
}

- (void)dealloc {
    [_displayLink invalidate];
}

@end
