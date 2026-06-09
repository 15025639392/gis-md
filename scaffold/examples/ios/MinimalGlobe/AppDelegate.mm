#import "AppDelegate.h"
#import "MetalView.h"

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];

    UIViewController *vc = [[UIViewController alloc] init];
    MetalView *metalView = [[MetalView alloc] initWithFrame:self.window.bounds];
    metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    vc.view = metalView;

    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];

    return YES;
}

@end
