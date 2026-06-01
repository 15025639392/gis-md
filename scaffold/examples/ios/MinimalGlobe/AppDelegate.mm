#import <UIKit/UIKit.h>

/// iOS 最小地球引擎示例 — AppDelegate。
/// 仅创建 window 和 MetalView controller。
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];

    UIViewController *vc = [[UIViewController alloc] init];
    // MetalView 将在 viewDidLoad 中创建并添加到 vc.view
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];

    return YES;
}

@end
