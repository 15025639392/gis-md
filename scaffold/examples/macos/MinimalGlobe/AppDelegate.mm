#import "AppDelegate.h"
#import "MetalView.h"

@interface AppDelegate ()
@property (nonatomic, retain) NSWindow* window;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    NSRect frame = NSMakeRect(0, 0, 1024, 768);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled |
                                                       NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskMiniaturizable |
                                                       NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [self.window setTitle:@"earth-md — 3D Globe"];
    [self.window center];

    MetalView* metalView = [[MetalView alloc] initWithFrame:self.window.contentView.bounds];
    metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.window.contentView = metalView;

    [self.window makeKeyAndOrderFront:nil];

    // Engine setup after view is in window
    [metalView startEngineWithScale:self.window.backingScaleFactor];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

@end
