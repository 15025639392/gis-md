#import "IosPlatformBridge.h"

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <Security/Security.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

// PImpl definition — must be complete before the method bodies below.
struct IosPlatformBridgeImpl {
    NSURLSession* session = nil;
};

namespace earth_engine {

// ============================================================
// HttpRequest 实现（RAII 取消 NSURLSessionDataTask）
// ============================================================

class IosHttpRequest final : public HttpRequest {
public:
    explicit IosHttpRequest(NSURLSessionDataTask* task) : task_(task) {}
    ~IosHttpRequest() override { cancel(); }
    void cancel() override {
        if (task_) {
            [task_ cancel];
            task_ = nil;
        }
    }
private:
    NSURLSessionDataTask* task_;
};

// ============================================================
// IosPlatformBridge
// ============================================================

IosPlatformBridge::IosPlatformBridge() {
    impl_ = new IosPlatformBridgeImpl();
    impl_->session = [NSURLSession sessionWithConfiguration:
        [NSURLSessionConfiguration defaultSessionConfiguration]];
}

IosPlatformBridge::~IosPlatformBridge() {
    if (impl_) {
        [impl_->session invalidateAndCancel];
        delete impl_;
    }
}

void IosPlatformBridge::onMemoryPressure() {}
void IosPlatformBridge::onEnterBackground() {}
void IosPlatformBridge::onEnterForeground() {}

std::unique_ptr<HttpRequest> IosPlatformBridge::get(
    const std::string& url,
    std::function<void(int, std::vector<uint8_t>)> callback,
    HttpRequestOptions options) {
    // 调用线程是引擎裸 std::thread（无外层池），不包池则 autoreleased
    // 临时对象随线程生命周期无限累积
    @autoreleasepool {

    NSString* nsUrl = [NSString stringWithUTF8String:url.c_str()];
    NSURL* requestUrl = [NSURL URLWithString:nsUrl];

    if (!requestUrl) {
        if (callback) {
            dispatch_async(dispatch_get_main_queue(), ^{
                callback(-1, {});
            });
        }
        return nullptr;
    }

    NSMutableURLRequest* request =
        [NSMutableURLRequest requestWithURL:requestUrl];
    for (const auto& header : options.headers) {
        [request setValue:[NSString stringWithUTF8String:header.second.c_str()]
       forHTTPHeaderField:[NSString stringWithUTF8String:header.first.c_str()]];
    }

    auto cb = std::make_shared<std::function<void(int, std::vector<uint8_t>)>>(
        std::move(callback));

    NSURLSessionDataTask* task =
        [impl_->session dataTaskWithRequest:request
            completionHandler:^(NSData* data,
                                NSURLResponse* response,
                                NSError* error) {
            if (!*cb) return;
            int status = 200;
            if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                NSHTTPURLResponse* httpResp = (NSHTTPURLResponse*)response;
                status = (int)httpResp.statusCode;
            }
            std::vector<uint8_t> body;
            if (data && !error) {
                body.assign(
                    (const uint8_t*)data.bytes,
                    (const uint8_t*)data.bytes + data.length);
            }
            (*cb)(status, std::move(body));
        }];

    [task resume];
    return std::make_unique<IosHttpRequest>(task);

    } // @autoreleasepool
}

std::string IosPlatformBridge::cacheDirectory() const {
    @autoreleasepool {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(
            NSCachesDirectory, NSUserDomainMask, YES);
        if (paths.count > 0) {
            return [paths[0] UTF8String];
        }
        return "/tmp";
    }
}

std::string IosPlatformBridge::documentsDirectory() const {
    @autoreleasepool {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(
            NSDocumentDirectory, NSUserDomainMask, YES);
        if (paths.count > 0) {
            return [paths[0] UTF8String];
        }
        return "/tmp";
    }
}

std::unique_ptr<DecodedImage> IosPlatformBridge::decodeImage(
    const uint8_t* data, size_t len) {
    // 线程池逐瓦片调用，无池则每次解码的 autoreleased 对象永不释放
    @autoreleasepool {

    NSData* nsData = [NSData dataWithBytes:data length:len];
    CGImageSourceRef src = CGImageSourceCreateWithData(
        (__bridge CFDataRef)nsData, nil);
    if (!src) return nullptr;

    CGImageRef cgImage = CGImageSourceCreateImageAtIndex(src, 0, nil);
    CFRelease(src);
    if (!cgImage) return nullptr;

    size_t width = CGImageGetWidth(cgImage);
    size_t height = CGImageGetHeight(cgImage);
    if (width == 0 || height == 0) {
        CGImageRelease(cgImage);
        return nullptr;
    }

    auto image = std::make_unique<DecodedImage>();
    image->width = (int)width;
    image->height = (int)height;
    image->channels = 4;
    image->pixels.resize(width * height * 4);

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        image->pixels.data(), width, height, 8, width * 4,
        colorSpace, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(colorSpace);
    if (!ctx) {
        CGImageRelease(cgImage);
        return nullptr;
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), cgImage);
    CGContextRelease(ctx);
    CGImageRelease(cgImage);

    return image;

    } // @autoreleasepool
}

void IosPlatformBridge::log(LogLevel, const std::string& tag,
                            const std::string& message) {
    NSLog(@"[%s] %s", tag.c_str(), message.c_str());
}

DeviceInfo IosPlatformBridge::deviceInfo() const {
    @autoreleasepool {
        DeviceInfo info;
        info.platform = "iOS";
        info.model = UIDevice.currentDevice.model.UTF8String ?: "iOS";
        info.osVersion = UIDevice.currentDevice.systemVersion.UTF8String ?: "";
        info.screenDensity = static_cast<float>(UIScreen.mainScreen.scale);
        info.screenWidthPx = static_cast<int>(
            UIScreen.mainScreen.bounds.size.width * UIScreen.mainScreen.scale);
        info.screenHeightPx = static_cast<int>(
            UIScreen.mainScreen.bounds.size.height * UIScreen.mainScreen.scale);
        info.cpuCores = static_cast<int>([NSProcessInfo processInfo].processorCount);
        info.totalMemoryBytes =
            static_cast<int64_t>([NSProcessInfo processInfo].physicalMemory);
        return info;
    }
}

std::string IosPlatformBridge::getToken(const std::string& providerId) const {
    @autoreleasepool {
        NSString* service = [NSString stringWithFormat:
            @"com.earthengine.provider.%@",
            [NSString stringWithUTF8String:providerId.c_str()]];

        NSDictionary* query = @{
            (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService : service,
            (__bridge id)kSecReturnData : @YES,
            (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne
        };

        CFTypeRef result = nil;
        OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query,
                                              &result);
        if (status == errSecSuccess && result) {
            NSData* data = CFBridgingRelease(result);
            return std::string((const char*)data.bytes, data.length);
        }
        return "";
    }
}

} // namespace earth_engine
