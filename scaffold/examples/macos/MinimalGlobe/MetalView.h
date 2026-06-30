#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

namespace earth_engine {
class Engine;
class RenderDeviceMetal;
class MacPlatformBridge;
class EarthEngineSdkFacade;
}

@interface MetalView : NSView
- (void)startRenderLoop;
- (void)stopRenderLoop;
@property (readonly) CAMetalLayer* metalLayer;
@end
