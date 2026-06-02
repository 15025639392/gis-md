#include "AndroidPlatformBridge.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
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

    // 同步执行 HTTP（在 XYZImageryProvider 的后台线程中调用）
    auto body = androidHttpGet(url);
    int code = body.empty() ? -1 : 200;
    callback(code, std::move(body));

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
    // 使用 Android BitmapFactory 解码（硬件加速）
    JNIEnv* env = getJniEnv();
    if (!env) { detachJni(); return nullptr; }

    // 将 C++ 数据转为 Java byte[]
    jbyteArray byteArray = env->NewByteArray(static_cast<jsize>(len));
    env->SetByteArrayRegion(byteArray, 0, static_cast<jsize>(len),
                            reinterpret_cast<const jbyte*>(data));

    // BitmapFactory.decodeByteArray(data, offset, length)
    jclass bmpFactoryClass = env->FindClass("android/graphics/BitmapFactory");
    jmethodID decodeMethod = env->GetStaticMethodID(
        bmpFactoryClass, "decodeByteArray",
        "([BII)Landroid/graphics/Bitmap;");
    jobject bitmap = env->CallStaticObjectMethod(
        bmpFactoryClass, decodeMethod, byteArray, 0, static_cast<jint>(len));
    env->DeleteLocalRef(byteArray);
    env->DeleteLocalRef(bmpFactoryClass);

    if (!bitmap || clearPendingJniException(env, "BitmapFactory.decode")) {
        detachJni();
        return nullptr;
    }

    // 获取宽高
    jclass bmpClass = env->FindClass("android/graphics/Bitmap");
    jmethodID getWidth = env->GetMethodID(bmpClass, "getWidth", "()I");
    jmethodID getHeight = env->GetMethodID(bmpClass, "getHeight", "()I");
    int w = env->CallIntMethod(bitmap, getWidth);
    int h = env->CallIntMethod(bitmap, getHeight);

    if (w <= 0 || h <= 0) {
        env->DeleteLocalRef(bitmap);
        env->DeleteLocalRef(bmpClass);
        detachJni();
        return nullptr;
    }

    // bitmap.getPixels(pixels, offset, stride, x, y, width, height)
    jintArray pixelArray = env->NewIntArray(w * h);
    jmethodID getPixels = env->GetMethodID(
        bmpClass, "getPixels", "([IIIIIII)V");
    env->CallVoidMethod(bitmap, getPixels, pixelArray, 0, w, 0, 0, w, h);

    jint* jpixels = env->GetIntArrayElements(pixelArray, nullptr);

    auto img = std::make_unique<DecodedImage>();
    img->width = w;
    img->height = h;
    img->channels = 4;
    img->pixels.resize(static_cast<size_t>(w * h * 4));

    // Android Bitmap 格式是 ARGB，转为 RGBA
    for (int i = 0; i < w * h; ++i) {
        uint32_t argb = static_cast<uint32_t>(jpixels[i]);
        uint8_t a = (argb >> 24) & 0xFF;
        uint8_t r = (argb >> 16) & 0xFF;
        uint8_t g = (argb >> 8) & 0xFF;
        uint8_t b = argb & 0xFF;
        img->pixels[i * 4 + 0] = r;
        img->pixels[i * 4 + 1] = g;
        img->pixels[i * 4 + 2] = b;
        img->pixels[i * 4 + 3] = a;
    }

    env->ReleaseIntArrayElements(pixelArray, jpixels, JNI_ABORT);
    env->DeleteLocalRef(pixelArray);
    env->DeleteLocalRef(bitmap);
    env->DeleteLocalRef(bmpClass);
    detachJni();

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
