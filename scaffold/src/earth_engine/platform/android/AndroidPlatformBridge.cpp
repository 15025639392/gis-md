#include "AndroidPlatformBridge.h"

#include "../bridge/CurlPlatformBridge.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <android/bitmap.h>
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#define LOG_TAG "AndroidBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace earth_engine {

// ============================================================
// JNI 平台辅助
// ============================================================

static JavaVM* gJvm = nullptr;

void AndroidPlatformBridge_InitJvm(void* vm) {
    gJvm = static_cast<JavaVM*>(vm);
}

// --- JNI 线程 attach/detach 辅助 ---

// 线程生命周期 attach(P1-8):解码跑在长寿命工作线程上,每次调用
// attach/detach 是全税,且旧实现的无条件 Detach 会把本已附着的 Java
// 线程(如 UI 线程)从 JVM 上摘下来。改为 attach 一次 + pthread key
// 析构时 detach(Android 官方推荐模式);已附着线程原样复用、绝不摘。
static pthread_key_t gJniThreadKey;
static pthread_once_t gJniThreadKeyOnce = PTHREAD_ONCE_INIT;

static void detachJniAtThreadExit(void* /*env*/) {
    if (gJvm) {
        gJvm->DetachCurrentThread();
    }
}

static void createJniThreadKey() {
    pthread_key_create(&gJniThreadKey, detachJniAtThreadExit);
}

static JNIEnv* getThreadJniEnv() {
    if (!gJvm) return nullptr;
    JNIEnv* env = nullptr;
    jint res = gJvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (res == JNI_OK) {
        return env;  // 本已附着(宿主 Java 线程或已初始化的工作线程)
    }
    if (res != JNI_EDETACHED ||
        gJvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }
    pthread_once(&gJniThreadKeyOnce, createJniThreadKey);
    pthread_setspecific(gJniThreadKey, env);  // 非空值触发线程退出析构
    return env;
}

static bool clearPendingJniException(JNIEnv* env, const char* context) {
    if (!env || !env->ExceptionCheck()) return false;
    LOGE("JNI exception during %s", context);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

/// RAII JNI env：仅当本次是自己 attach 的线程才在析构时 detach。
/// 在已附着的 Java 线程（如 UI 线程）上使用时绝不 detach，
/// 避免把宿主线程从 JVM 上摘下来。
struct JniScope {
    JNIEnv* env = nullptr;
    JniScope() {
        if (!gJvm) return;
        jint res = gJvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (res == JNI_EDETACHED) {
            if (gJvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached_ = true;
            } else {
                env = nullptr;
            }
        }
    }
    ~JniScope() {
        if (attached_ && gJvm) {
            gJvm->DetachCurrentThread();
        }
    }
    JniScope(const JniScope&) = delete;
    JniScope& operator=(const JniScope&) = delete;

private:
    bool attached_ = false;
};

static std::string jstringToStd(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return result;
}

/// 调 Context.getCacheDir()/getFilesDir() 并取绝对路径。失败返回空串。
static std::string contextDirPath(JNIEnv* env, jobject context,
                                  const char* methodName) {
    jclass contextClass = env->GetObjectClass(context);
    jmethodID dirMethod = env->GetMethodID(
        contextClass, methodName, "()Ljava/io/File;");
    if (clearPendingJniException(env, methodName)) {
        env->DeleteLocalRef(contextClass);
        return {};
    }
    jobject file = env->CallObjectMethod(context, dirMethod);
    env->DeleteLocalRef(contextClass);
    if (clearPendingJniException(env, methodName) || !file) {
        if (file) env->DeleteLocalRef(file);
        return {};
    }
    jclass fileClass = env->GetObjectClass(file);
    jmethodID absPathMethod = env->GetMethodID(
        fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    auto path = static_cast<jstring>(env->CallObjectMethod(file, absPathMethod));
    env->DeleteLocalRef(fileClass);
    env->DeleteLocalRef(file);
    if (clearPendingJniException(env, "getAbsolutePath")) {
        if (path) env->DeleteLocalRef(path);
        return {};
    }
    std::string result = jstringToStd(env, path);
    if (path) env->DeleteLocalRef(path);
    return result;
}

/// 读 Build 类静态 String 字段（MODEL / VERSION.RELEASE）。失败返回空串。
static std::string buildStaticString(JNIEnv* env, const char* className,
                                     const char* fieldName) {
    jclass cls = env->FindClass(className);
    if (clearPendingJniException(env, className) || !cls) return {};
    jfieldID field = env->GetStaticFieldID(cls, fieldName, "Ljava/lang/String;");
    if (clearPendingJniException(env, fieldName)) {
        env->DeleteLocalRef(cls);
        return {};
    }
    auto value = static_cast<jstring>(env->GetStaticObjectField(cls, field));
    env->DeleteLocalRef(cls);
    std::string result = jstringToStd(env, value);
    if (value) env->DeleteLocalRef(value);
    return result;
}

// ============================================================
// AndroidPlatformBridge
// ============================================================

struct AndroidPlatformBridge::Impl {
    explicit Impl(void* jvmPtr) : jvm(jvmPtr) {}
    void* jvm = nullptr;
    CurlPlatformBridge networkBridge;

    // 构造时缓存的平台信息。无 Context 时保留默认值。
    std::string cacheDir = "/data/data/com.earthengine.minimalglobe/cache";
    std::string documentsDir = "/data/data/com.earthengine.minimalglobe/files";
    DeviceInfo deviceInfo;

    void initNativeDeviceInfo() {
        deviceInfo.platform = "Android";
        const int cores = static_cast<int>(std::thread::hardware_concurrency());
        deviceInfo.cpuCores = cores > 0 ? cores : 1;
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long pageSize = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && pageSize > 0) {
            deviceInfo.totalMemoryBytes =
                static_cast<int64_t>(pages) * static_cast<int64_t>(pageSize);
        }
    }

    void queryAppContext(jobject appContext) {
        JniScope scope;
        JNIEnv* env = scope.env;
        if (!env) {
            LOGE("queryAppContext: no JNIEnv, keeping defaults");
            return;
        }

        const std::string cache = contextDirPath(env, appContext, "getCacheDir");
        if (!cache.empty()) cacheDir = cache;
        const std::string files = contextDirPath(env, appContext, "getFilesDir");
        if (!files.empty()) documentsDir = files;

        deviceInfo.model = buildStaticString(env, "android/os/Build", "MODEL");
        deviceInfo.osVersion =
            buildStaticString(env, "android/os/Build$VERSION", "RELEASE");

        // Context.getResources().getDisplayMetrics() → density / widthPixels / heightPixels
        jclass contextClass = env->GetObjectClass(appContext);
        jmethodID getResources = env->GetMethodID(
            contextClass, "getResources", "()Landroid/content/res/Resources;");
        env->DeleteLocalRef(contextClass);
        if (!clearPendingJniException(env, "getResources lookup")) {
            jobject resources = env->CallObjectMethod(appContext, getResources);
            if (!clearPendingJniException(env, "getResources") && resources) {
                jclass resourcesClass = env->GetObjectClass(resources);
                jmethodID getDisplayMetrics = env->GetMethodID(
                    resourcesClass, "getDisplayMetrics",
                    "()Landroid/util/DisplayMetrics;");
                env->DeleteLocalRef(resourcesClass);
                if (!clearPendingJniException(env, "getDisplayMetrics lookup")) {
                    jobject metrics =
                        env->CallObjectMethod(resources, getDisplayMetrics);
                    if (!clearPendingJniException(env, "getDisplayMetrics") &&
                        metrics) {
                        jclass metricsClass = env->GetObjectClass(metrics);
                        jfieldID densityField =
                            env->GetFieldID(metricsClass, "density", "F");
                        jfieldID widthField =
                            env->GetFieldID(metricsClass, "widthPixels", "I");
                        jfieldID heightField =
                            env->GetFieldID(metricsClass, "heightPixels", "I");
                        if (!clearPendingJniException(env,
                                                      "DisplayMetrics fields")) {
                            deviceInfo.screenDensity =
                                env->GetFloatField(metrics, densityField);
                            deviceInfo.screenWidthPx =
                                env->GetIntField(metrics, widthField);
                            deviceInfo.screenHeightPx =
                                env->GetIntField(metrics, heightField);
                        }
                        env->DeleteLocalRef(metricsClass);
                        env->DeleteLocalRef(metrics);
                    }
                }
                env->DeleteLocalRef(resources);
            }
        }

        LOGI("Platform context: cacheDir=%s model=%s os=%s density=%.1f "
             "screen=%dx%d cores=%d mem=%lldMB",
             cacheDir.c_str(),
             deviceInfo.model.c_str(),
             deviceInfo.osVersion.c_str(),
             deviceInfo.screenDensity,
             deviceInfo.screenWidthPx,
             deviceInfo.screenHeightPx,
             deviceInfo.cpuCores,
             static_cast<long long>(deviceInfo.totalMemoryBytes / (1024 * 1024)));
    }
};

AndroidPlatformBridge::AndroidPlatformBridge(void* jvm, void* appContext)
    : impl_(std::make_unique<Impl>(jvm)) {
    if (!gJvm) {
        gJvm = static_cast<JavaVM*>(jvm);
    }
    impl_->initNativeDeviceInfo();
    if (appContext) {
        impl_->queryAppContext(static_cast<jobject>(appContext));
    }
}

AndroidPlatformBridge::~AndroidPlatformBridge() = default;

void AndroidPlatformBridge::onMemoryPressure() {}
void AndroidPlatformBridge::onEnterBackground() {
    impl_->networkBridge.onEnterBackground();
}
void AndroidPlatformBridge::onEnterForeground() {}

std::unique_ptr<HttpRequest> AndroidPlatformBridge::get(
    const std::string& url,
    std::function<void(int, std::vector<uint8_t>)> callback,
    HttpRequestOptions options) {

    return impl_->networkBridge.get(
        url,
        std::move(callback),
        options);
}

std::unique_ptr<HttpRequest> AndroidPlatformBridge::post(
    const std::string& url,
    std::vector<uint8_t> body,
    const std::string& contentType,
    std::function<void(int, std::vector<uint8_t>)> callback,
    HttpRequestOptions options) {
    return impl_->networkBridge.post(
        url,
        std::move(body),
        contentType,
        std::move(callback),
        options);
}

int AndroidPlatformBridge::maximumActiveRequests() const {
    return impl_->networkBridge.maximumActiveRequests();
}

std::string AndroidPlatformBridge::cacheDirectory() const {
    return impl_->cacheDir;
}

std::string AndroidPlatformBridge::documentsDirectory() const {
    return impl_->documentsDir;
}

namespace {

/// BitmapFactory JNI 句柄一次性解析缓存(P1-8):jmethodID/jfieldID 进程内
/// 恒有效;jclass 与 Config 枚举常量升级为 global ref。首个调用线程完成
/// 初始化(magic static 线程安全),之后每次解码零 FindClass/GetMethodID。
struct BitmapJniHandles {
    jclass optionsClass = nullptr;        // global ref
    jmethodID optionsCtor = nullptr;
    jfieldID inPreferredConfig = nullptr;
    jfieldID inMutable = nullptr;
    jclass bitmapFactoryClass = nullptr;  // global ref
    jmethodID decodeByteArray = nullptr;
    jobject argb8888 = nullptr;           // global ref
    bool valid = false;
};

const BitmapJniHandles& bitmapJniHandles(JNIEnv* env) {
    static const BitmapJniHandles handles = [env] {
        BitmapJniHandles h;
        jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
        jclass optionsClass =
            env->FindClass("android/graphics/BitmapFactory$Options");
        jclass factoryClass = env->FindClass("android/graphics/BitmapFactory");
        if (!configClass || !optionsClass || !factoryClass ||
            clearPendingJniException(env, "Bitmap JNI class lookup")) {
            return h;
        }
        jfieldID argb8888Field = env->GetStaticFieldID(
            configClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
        jobject argb8888 =
            argb8888Field
                ? env->GetStaticObjectField(configClass, argb8888Field)
                : nullptr;
        h.optionsCtor = env->GetMethodID(optionsClass, "<init>", "()V");
        h.inPreferredConfig = env->GetFieldID(
            optionsClass,
            "inPreferredConfig",
            "Landroid/graphics/Bitmap$Config;");
        h.inMutable = env->GetFieldID(optionsClass, "inMutable", "Z");
        h.decodeByteArray = env->GetStaticMethodID(
            factoryClass,
            "decodeByteArray",
            "([BIILandroid/graphics/BitmapFactory$Options;)"
            "Landroid/graphics/Bitmap;");
        if (!argb8888 || !h.optionsCtor || !h.inPreferredConfig ||
            !h.inMutable || !h.decodeByteArray ||
            clearPendingJniException(env, "Bitmap JNI member lookup")) {
            env->DeleteLocalRef(configClass);
            env->DeleteLocalRef(optionsClass);
            env->DeleteLocalRef(factoryClass);
            if (argb8888) env->DeleteLocalRef(argb8888);
            return h;
        }
        h.optionsClass =
            static_cast<jclass>(env->NewGlobalRef(optionsClass));
        h.bitmapFactoryClass =
            static_cast<jclass>(env->NewGlobalRef(factoryClass));
        h.argb8888 = env->NewGlobalRef(argb8888);
        env->DeleteLocalRef(configClass);
        env->DeleteLocalRef(optionsClass);
        env->DeleteLocalRef(factoryClass);
        env->DeleteLocalRef(argb8888);
        h.valid = h.optionsClass && h.bitmapFactoryClass && h.argb8888;
        return h;
    }();
    return handles;
}

} // namespace

std::unique_ptr<DecodedImage> AndroidPlatformBridge::decodeImage(
    const uint8_t* data, size_t len) {
    // 使用 Android BitmapFactory 解码为 ARGB_8888，再显式拷贝为 RGBA8。
    // env 为线程生命周期 attach(见 getThreadJniEnv),本函数不再 detach。
    JNIEnv* env = getThreadJniEnv();
    if (!env) return nullptr;
    const BitmapJniHandles& jni = bitmapJniHandles(env);
    if (!jni.valid) {
        LOGE("Bitmap JNI handles unavailable");
        return nullptr;
    }

    // 将 C++ 数据转为 Java byte[]
    jbyteArray byteArray = env->NewByteArray(static_cast<jsize>(len));
    if (!byteArray) {
        clearPendingJniException(env, "NewByteArray");
        return nullptr;
    }
    env->SetByteArrayRegion(byteArray, 0, static_cast<jsize>(len),
                            reinterpret_cast<const jbyte*>(data));

    jobject options = env->NewObject(jni.optionsClass, jni.optionsCtor);
    if (!options || clearPendingJniException(env, "BitmapFactory.Options setup")) {
        env->DeleteLocalRef(byteArray);
        if (options) env->DeleteLocalRef(options);
        return nullptr;
    }
    env->SetObjectField(options, jni.inPreferredConfig, jni.argb8888);
    env->SetBooleanField(options, jni.inMutable, JNI_TRUE);

    // BitmapFactory.decodeByteArray(data, offset, length, options)
    jobject bitmap = env->CallStaticObjectMethod(
        jni.bitmapFactoryClass,
        jni.decodeByteArray,
        byteArray,
        0,
        static_cast<jint>(len),
        options);
    env->DeleteLocalRef(byteArray);
    env->DeleteLocalRef(options);

    if (!bitmap || clearPendingJniException(env, "BitmapFactory.decode")) {
        LOGE("BitmapFactory failed to decode %zu bytes", len);
        if (bitmap) env->DeleteLocalRef(bitmap);
        return nullptr;
    }

    AndroidBitmapInfo info{};
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.width == 0 || info.height == 0) {
        LOGE("AndroidBitmap_getInfo failed");
        env->DeleteLocalRef(bitmap);
        return nullptr;
    }

    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        LOGE("Unsupported bitmap format %u for decoded image", info.format);
        env->DeleteLocalRef(bitmap);
        return nullptr;
    }

    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS ||
        !pixels) {
        LOGE("AndroidBitmap_lockPixels failed");
        env->DeleteLocalRef(bitmap);
        return nullptr;
    }

    auto img = std::make_unique<DecodedImage>();
    img->width = static_cast<int>(info.width);
    img->height = static_cast<int>(info.height);
    img->channels = 4;
    img->pixels.resize(static_cast<size_t>(info.width * info.height * 4));

    const auto* src = static_cast<const uint8_t*>(pixels);
    const size_t rowBytes = static_cast<size_t>(info.width) * 4;
    if (info.stride == rowBytes) {
        std::copy(src, src + rowBytes * info.height, img->pixels.begin());
    } else {
        for (uint32_t y = 0; y < info.height; ++y) {
            const uint8_t* row = src + static_cast<size_t>(y) * info.stride;
            auto dst = img->pixels.begin() +
                static_cast<ptrdiff_t>(static_cast<size_t>(y) * rowBytes);
            std::copy(row, row + rowBytes, dst);
        }
    }

    AndroidBitmap_unlockPixels(env, bitmap);
    env->DeleteLocalRef(bitmap);

    LOGI("Decoded image %dx%d from %zu bytes", img->width, img->height, len);
    return img;
}

void AndroidPlatformBridge::log(LogLevel level, const std::string& tag,
                                 const std::string& message) {
    int priority = ANDROID_LOG_INFO;
    switch (level) {
        case LogLevel::Debug:   priority = ANDROID_LOG_DEBUG; break;
        case LogLevel::Info:    priority = ANDROID_LOG_INFO;  break;
        case LogLevel::Warning: priority = ANDROID_LOG_WARN;  break;
        case LogLevel::Error:   priority = ANDROID_LOG_ERROR; break;
    }
    __android_log_print(priority, tag.c_str(), "%s", message.c_str());
}

DeviceInfo AndroidPlatformBridge::deviceInfo() const {
    return impl_->deviceInfo;
}

std::string AndroidPlatformBridge::getToken(const std::string&) const {
    return "";
}

} // namespace earth_engine
