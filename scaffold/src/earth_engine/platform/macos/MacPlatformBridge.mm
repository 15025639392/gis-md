#import "MacPlatformBridge.h"

#include "../../core/math/MathUtils.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <fstream>
#include <sstream>

// PImpl definition — must be before implementation to complete the type
struct MacPlatformBridgeImpl {
    NSURLSession* session = nil;
};

namespace earth_engine {

// ============================================================
// HttpRequest 实现
// ============================================================

class MacHttpRequest final : public HttpRequest {
public:
    explicit MacHttpRequest(NSURLSessionDataTask* task) : task_(task) {}
    ~MacHttpRequest() override { cancel(); }
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
// MacPlatformBridge
// ============================================================

MacPlatformBridge::MacPlatformBridge() {
    impl_ = new MacPlatformBridgeImpl();
    impl_->session = [NSURLSession sessionWithConfiguration:
        [NSURLSessionConfiguration defaultSessionConfiguration]];
}

MacPlatformBridge::~MacPlatformBridge() {
    if (impl_) {
        [impl_->session invalidateAndCancel];
        delete impl_;
    }
}

void MacPlatformBridge::onEnterBackground() {}
void MacPlatformBridge::onEnterForeground() {}

std::unique_ptr<HttpRequest> MacPlatformBridge::get(
    const std::string& url,
    std::function<void(int, std::vector<uint8_t>)> callback,
    HttpRequestOptions options) {

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

    auto cb = std::make_shared<std::function<void(int, std::vector<uint8_t>)>>(
        std::move(callback));

    NSURLSessionDataTask* task =
        [impl_->session dataTaskWithURL:requestUrl
            completionHandler:^(NSData* data,
                                NSURLResponse* response,
                                NSError* error) {
            if (!*cb) return;
            int status = 200;
            if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                NSHTTPURLResponse* httpResp = (NSHTTPURLResponse*)response;
                status = (int)httpResp.statusCode;
                // I-P1:响应头输出(HttpCache 计算过期/ETag 重验)。
                if (options.responseHeaders) {
                    NSDictionary* allHeaders = [httpResp allHeaderFields];
                    options.responseHeaders->reserve(allHeaders.count);
                    for (NSString* key in allHeaders) {
                        NSString* value = [allHeaders objectForKey:key];
                        options.responseHeaders->emplace_back(
                            std::string([key UTF8String]),
                            std::string([value UTF8String]));
                    }
                }
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
    return std::make_unique<MacHttpRequest>(task);
}

std::string MacPlatformBridge::cacheDirectory() const {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(
        NSCachesDirectory, NSUserDomainMask, YES);
    if (paths.count > 0) {
        return [paths[0] UTF8String];
    }
    return "/tmp";
}

std::string MacPlatformBridge::documentsDirectory() const {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    if (paths.count > 0) {
        return [paths[0] UTF8String];
    }
    return "/tmp";
}

std::unique_ptr<DecodedImage> MacPlatformBridge::decodeImage(
    const uint8_t* data, size_t len) {

    NSData* nsData = [NSData dataWithBytes:data length:len];
    CGImageSourceRef src = CGImageSourceCreateWithData(
        (__bridge CFDataRef)nsData, nil);
    if (!src) return nullptr;

    CGImageRef cgImage = CGImageSourceCreateImageAtIndex(src, 0, nil);
    CFRelease(src);
    if (!cgImage) return nullptr;

    size_t width = CGImageGetWidth(cgImage);
    size_t height = CGImageGetHeight(cgImage);
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
    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), cgImage);
    CGContextRelease(ctx);
    CGImageRelease(cgImage);

    return image;
}

void MacPlatformBridge::log(LogLevel, const std::string& tag,
                            const std::string& message) {
    NSLog(@"[%s] %s", tag.c_str(), message.c_str());
}

DeviceInfo MacPlatformBridge::deviceInfo() const {
    DeviceInfo info;
    info.platform = "macOS";
    return info;
}

std::string MacPlatformBridge::getToken(const std::string& providerId) const {
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

} // namespace earth_engine
