#include "AndroidPlatformBridge.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <algorithm>
#include <cstddef>
#include <android/bitmap.h>
#include <android/log.h>
#include <jni.h>

#define LOG_TAG "AndroidBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace earth_engine {

// ============================================================
// JNI HTTP 实现
// ============================================================

// 全局 JavaVM + 缓存的 JNI 引用
static JavaVM* gJvm = nullptr;
static jclass gJniHttpHelperClass = nullptr;
static jmethodID gHttpGetMethod = nullptr;

void AndroidPlatformBridge_InitJvm(void* vm) {
    gJvm = static_cast<JavaVM*>(vm);
    // 在主线程缓存 JniHttpHelper class 引用
    if (gJvm) {
        JNIEnv* env = nullptr;
        if (gJvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
            jclass cls = env->FindClass("com/earthengine/minimalglobe/JniHttpHelper");
            if (cls) {
                gJniHttpHelperClass = static_cast<jclass>(env->NewGlobalRef(cls));
                gHttpGetMethod = env->GetStaticMethodID(
                    gJniHttpHelperClass, "httpGet", "(Ljava/lang/String;)[B");
            }
        }
    }
}

// --- JNI HTTP 辅助：在 C++ 线程中 attach/detach ---

static JNIEnv* getJniEnv() {
    if (!gJvm) return nullptr;
    JNIEnv* env = nullptr;
    jint res = gJvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        gJvm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

static void detachJni() {
    if (gJvm) {
        gJvm->DetachCurrentThread();
    }
}

static bool clearPendingJniException(JNIEnv* env, const char* context) {
    if (!env || !env->ExceptionCheck()) return false;
    LOGE("JNI exception during %s", context);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

/// 使用 JNI 调用 Java JniHttpHelper.httpGet() 执行 HTTP GET
static std::vector<uint8_t> androidHttpGet(const std::string& url) {
    JNIEnv* env = getJniEnv();
    if (!env) {
        LOGE("JNIEnv unavailable for HTTP request");
        detachJni();
        return {};
    }
    LOGI("androidHttpGet: env=%p gJniHttpHelperClass=%p gHttpGetMethod=%p url=%s",
         (void*)env, (void*)gJniHttpHelperClass, (void*)gHttpGetMethod, url.c_str());

    if (!gJniHttpHelperClass || !gHttpGetMethod) {
        LOGE("JniHttpHelper not initialized (call InitJvm first)");
        detachJni();
        return {};
    }

    jstring urlStr = env->NewStringUTF(url.c_str());
    jbyteArray resultArray = static_cast<jbyteArray>(
        env->CallStaticObjectMethod(gJniHttpHelperClass, gHttpGetMethod, urlStr));
    env->DeleteLocalRef(urlStr);

    std::vector<uint8_t> result;
    if (resultArray) {
        jsize len = env->GetArrayLength(resultArray);
        jbyte* data = env->GetByteArrayElements(resultArray, nullptr);
        result.assign(reinterpret_cast<uint8_t*>(data),
                      reinterpret_cast<uint8_t*>(data) + len);
        env->ReleaseByteArrayElements(resultArray, data, JNI_ABORT);
        env->DeleteLocalRef(resultArray);
    }

    detachJni();
    return result;
}

// ============================================================
// AndroidHttpRequest
// ============================================================

class AndroidHttpRequest : public HttpRequest {
public:
    AndroidHttpRequest() = default;
    ~AndroidHttpRequest() override { cancel(); }
    void cancel() override { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }
private:
    std::atomic<bool> cancelled_{false};
};

// ============================================================
// AndroidPlatformBridge
// ============================================================

struct AndroidPlatformBridge::Impl {
    void* jvm;
};

AndroidPlatformBridge::AndroidPlatformBridge(void* jvm)
    : impl_(std::make_unique<Impl>()) {
    impl_->jvm = jvm;
    if (!gJvm) {
        gJvm = static_cast<JavaVM*>(jvm);
    }
}

AndroidPlatformBridge::~AndroidPlatformBridge() = default;

void AndroidPlatformBridge::onMemoryPressure() {}
void AndroidPlatformBridge::onEnterBackground() {}
void AndroidPlatformBridge::onEnterForeground() {}

std::unique_ptr<HttpRequest> AndroidPlatformBridge::get(
    const std::string& url,
    std::function<void(int, std::vector<uint8_t>)> callback) {

    // Always run Java networking off the Android main thread. Some callers
    // synchronously wait for this callback during startup metadata probes.
    std::thread([url, callback = std::move(callback)]() mutable {
        auto body = androidHttpGet(url);
        int code = body.empty() ? -1 : 200;
        callback(code, std::move(body));
    }).detach();

    return std::make_unique<AndroidHttpRequest>();
}

std::string AndroidPlatformBridge::cacheDirectory() const {
    return "/data/data/com.earthengine.minimalglobe/cache";
}

std::string AndroidPlatformBridge::documentsDirectory() const {
    return "/data/data/com.earthengine.minimalglobe/files";
}

std::unique_ptr<DecodedImage> AndroidPlatformBridge::decodeImage(
    const uint8_t* data, size_t len) {
    // 使用 Android BitmapFactory 解码为 ARGB_8888，再显式拷贝为 RGBA8。
    JNIEnv* env = getJniEnv();
    if (!env) { detachJni(); return nullptr; }

    // 将 C++ 数据转为 Java byte[]
    jbyteArray byteArray = env->NewByteArray(static_cast<jsize>(len));
    env->SetByteArrayRegion(byteArray, 0, static_cast<jsize>(len),
                            reinterpret_cast<const jbyte*>(data));

    jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
    jfieldID argb8888Field = env->GetStaticFieldID(
        configClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    jobject argb8888 = env->GetStaticObjectField(configClass, argb8888Field);

    jclass optionsClass = env->FindClass("android/graphics/BitmapFactory$Options");
    jmethodID optionsCtor = env->GetMethodID(optionsClass, "<init>", "()V");
    jobject options = env->NewObject(optionsClass, optionsCtor);
    jfieldID inPreferredConfig = env->GetFieldID(
        optionsClass, "inPreferredConfig", "Landroid/graphics/Bitmap$Config;");
    jfieldID inMutable = env->GetFieldID(optionsClass, "inMutable", "Z");
    env->SetObjectField(options, inPreferredConfig, argb8888);
    env->SetBooleanField(options, inMutable, JNI_TRUE);

    if (clearPendingJniException(env, "BitmapFactory.Options setup")) {
        env->DeleteLocalRef(byteArray);
        if (options) env->DeleteLocalRef(options);
        if (optionsClass) env->DeleteLocalRef(optionsClass);
        if (argb8888) env->DeleteLocalRef(argb8888);
        if (configClass) env->DeleteLocalRef(configClass);
        detachJni();
        return nullptr;
    }

    // BitmapFactory.decodeByteArray(data, offset, length, options)
    jclass bmpFactoryClass = env->FindClass("android/graphics/BitmapFactory");
    jmethodID decodeMethod = env->GetStaticMethodID(
        bmpFactoryClass, "decodeByteArray",
        "([BIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;");
    jobject bitmap = env->CallStaticObjectMethod(
        bmpFactoryClass, decodeMethod, byteArray, 0, static_cast<jint>(len), options);
    env->DeleteLocalRef(byteArray);
    env->DeleteLocalRef(bmpFactoryClass);
    env->DeleteLocalRef(options);
    env->DeleteLocalRef(optionsClass);
    env->DeleteLocalRef(argb8888);
    env->DeleteLocalRef(configClass);

    if (!bitmap || clearPendingJniException(env, "BitmapFactory.decode")) {
        LOGE("BitmapFactory failed to decode %zu bytes", len);
        detachJni();
        return nullptr;
    }

    AndroidBitmapInfo info{};
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.width == 0 || info.height == 0) {
        LOGE("AndroidBitmap_getInfo failed");
        env->DeleteLocalRef(bitmap);
        detachJni();
        return nullptr;
    }

    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        LOGE("Unsupported bitmap format %u for decoded image", info.format);
        env->DeleteLocalRef(bitmap);
        detachJni();
        return nullptr;
    }

    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS ||
        !pixels) {
        LOGE("AndroidBitmap_lockPixels failed");
        env->DeleteLocalRef(bitmap);
        detachJni();
        return nullptr;
    }

    auto img = std::make_unique<DecodedImage>();
    img->width = static_cast<int>(info.width);
    img->height = static_cast<int>(info.height);
    img->channels = 4;
    img->pixels.resize(static_cast<size_t>(info.width * info.height * 4));

    const auto* src = static_cast<const uint8_t*>(pixels);
    for (uint32_t y = 0; y < info.height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * info.stride;
        auto dst = img->pixels.begin() +
            static_cast<ptrdiff_t>(static_cast<size_t>(y) * info.width * 4);
        std::copy(row, row + static_cast<size_t>(info.width) * 4, dst);
    }

    AndroidBitmap_unlockPixels(env, bitmap);
    env->DeleteLocalRef(bitmap);
    detachJni();

    LOGI("Decoded image %dx%d from %zu bytes", img->width, img->height, len);
    return img;
}

void AndroidPlatformBridge::log(LogLevel /*level*/, const std::string& tag,
                                 const std::string& message) {
    __android_log_print(ANDROID_LOG_INFO, tag.c_str(), "%s", message.c_str());
}

DeviceInfo AndroidPlatformBridge::deviceInfo() const {
    DeviceInfo info;
    info.platform = "Android";
    info.cpuCores = 4;
    return info;
}

std::string AndroidPlatformBridge::getToken(const std::string&) const {
    return "";
}

} // namespace earth_engine
