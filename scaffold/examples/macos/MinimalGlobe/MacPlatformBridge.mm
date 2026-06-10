#import "MacPlatformBridge.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

namespace earth_engine {

namespace {

class MacHttpRequest : public HttpRequest {
public:
    explicit MacHttpRequest(NSURLSessionDataTask* task) : task_(task) {}
    ~MacHttpRequest() override { cancel(); }
    void cancel() override { [task_ cancel]; }
private:
    NSURLSessionDataTask* task_;
};

} // anonymous namespace

std::unique_ptr<HttpRequest> MacPlatformBridge::get(
    const std::string& url,
    std::function<void(int statusCode, std::vector<uint8_t> body)> callback)
{
    NSURL* nsUrl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!nsUrl) {
        callback(-1, {});
        return nullptr;
    }

    NSURLSession* session = [NSURLSession sharedSession];
    NSLog(@"[MacBridge] Requesting: %s", url.c_str());
    NSURLSessionDataTask* task = [session dataTaskWithURL:nsUrl
                                        completionHandler:^(NSData* data,
                                                            NSURLResponse* response,
                                                            NSError* error) {
        if (error) {
            NSLog(@"[MacBridge] NSURL error: %@", error.localizedDescription);
            callback(-1, {});
            return;
        }
        NSHTTPURLResponse* httpResponse = (NSHTTPURLResponse*)response;
        int statusCode = (int)httpResponse.statusCode;
        NSLog(@"[MacBridge] Response: %d, data length: %lu", statusCode, (unsigned long)data.length);
        if (statusCode != 200) {
            callback(statusCode, {});
            return;
        }
        const uint8_t* bytes = (const uint8_t*)data.bytes;
        callback(statusCode,
                 std::vector<uint8_t>(bytes, bytes + data.length));
    }];
    [task resume];

    return std::make_unique<MacHttpRequest>(task);
}

std::string MacPlatformBridge::cacheDirectory() const {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(
        NSCachesDirectory, NSUserDomainMask, YES);
    NSString* cache = [paths firstObject];
    return cache.UTF8String ?: "/tmp";
}

std::string MacPlatformBridge::documentsDirectory() const {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    NSString* docs = [paths firstObject];
    return docs.UTF8String ?: ".";
}

std::unique_ptr<DecodedImage> MacPlatformBridge::decodeImage(
    const uint8_t* data, size_t len)
{
    @autoreleasepool {
        NSData* nsData = [NSData dataWithBytes:data length:len];
        NSImage* image = [[NSImage alloc] initWithData:nsData];
        if (!image || !image.isValid) return nullptr;

        // NSImage 没有直接的 CGImage 属性，需要从 representations 获取
        CGImageRef cgImage = [image CGImageForProposedRect:nil
                                                   context:nil
                                                     hints:nil];
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

void MacPlatformBridge::log(LogLevel /*level*/,
                            const std::string& tag,
                            const std::string& message)
{
    NSLog(@"[%s] %s", tag.c_str(), message.c_str());
}

DeviceInfo MacPlatformBridge::deviceInfo() const {
    DeviceInfo info;
    info.platform = "macOS";
    info.model = "Mac";

    NSOperatingSystemVersion ver = [NSProcessInfo processInfo].operatingSystemVersion;
    info.osVersion = [NSString stringWithFormat:@"%ld.%ld.%ld",
                      (long)ver.majorVersion,
                      (long)ver.minorVersion,
                      (long)ver.patchVersion].UTF8String;

    NSScreen* mainScreen = [NSScreen mainScreen];
    if (mainScreen) {
        CGFloat scale = mainScreen.backingScaleFactor;
        NSRect frame = mainScreen.frame;
        info.screenDensity = static_cast<float>(scale);
        info.screenWidthPx = static_cast<int>(frame.size.width * scale);
        info.screenHeightPx = static_cast<int>(frame.size.height * scale);
    } else {
        info.screenDensity = 2.0f;
        info.screenWidthPx = 1440;
        info.screenHeightPx = 900;
    }

    info.cpuCores = static_cast<int>([NSProcessInfo processInfo].processorCount);
    info.totalMemoryBytes = static_cast<int64_t>(
        [NSProcessInfo processInfo].physicalMemory);
    return info;
}

} // namespace earth_engine
