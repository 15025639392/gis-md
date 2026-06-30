#import "AppDelegate.h"
#import "MetalView.h"

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    NSRect frame = NSMakeRect(0, 0, 1280, 720);
    NSUInteger style = NSWindowStyleMaskTitled
                     | NSWindowStyleMaskClosable
                     | NSWindowStyleMaskMiniaturizable
                     | NSWindowStyleMaskResizable;

    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Earth Engine — Minimal Globe";
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];

    MetalView* view = [[MetalView alloc] initWithFrame:frame];
    self.window.contentView = view;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

@end
