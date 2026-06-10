#import <AppKit/AppKit.h>

@interface MetalView : NSView

- (instancetype)initWithFrame:(NSRect)frame;
- (void)startEngineWithScale:(CGFloat)scale;

@end
