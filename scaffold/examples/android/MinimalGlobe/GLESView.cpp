#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include <sched.h>
#include <sys/system_properties.h>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <optional>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <unordered_map>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/renderer/GlyphAtlas.h"
#include "earth_engine/Engine.h"
#include "earth_engine/camera/CameraSystem.h"
#include "earth_engine/camera/CameraPose.h"
#include "earth_engine/camera/Viewpoint.h"
#include "earth_engine/camera/controllers/TetheredController.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/data/FeatureClusterIndex.h"
#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/data/FeatureSnapQuery.h"
#include "earth_engine/core/async/AsyncSystem.h"
#include "earth_engine/data/MvtFeatureConverter.h"
#include "earth_engine/data/MvtVectorSource.h"
#include "earth_engine/data/StyleFilter.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/platform/bridge/CurlMultiRequestScheduler.h"
#include "earth_engine/data/MvtTileFetchCache.h"
#include "earth_engine/data/AmapTileManifest.h"
#include "earth_engine/data/AmapVectorTile.h"
#include "earth_engine/data/AmapGeometry.h"
#include "earth_engine/data/AmapVectorSource.h"
#include "earth_engine/providers/RoadFieldSource.h"
#include <nlohmann/json.hpp>
#include "earth_engine/style/StyleDocument.h"
#include "earth_engine/providers/VectorDrapeImageryProvider.h"
#include "earth_engine/providers/AmapDrapeImageryProvider.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/platform/android/RenderDeviceGLES.h"
#include "earth_engine/platform/android/AndroidPlatformBridge.h"
#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/sdk/EarthEngineSdkFacade.h"
#include "earth_engine/threading/RenderThreadPlacement.h"

#include "MinimalGlobeDiagnostics.h"
#include "MinimalGlobeDemoConfig.h"
#include "MinimalGlobeDemoLayers.h"

#define LOG_TAG "MinimalGlobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/// Android 测量台布尔属性。未设置时保持编译期默认；显式 0/1 覆盖，其他值
/// 视为未设置。所有开关只在进程启动建场时读取，A/B 前 force-stop 重启即可。
static bool startupBoolProperty(const char* name, bool fallback) {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get(name, prop);
    if (prop[0] == '0' && prop[1] == '\0') return false;
    if (prop[0] == '1' && prop[1] == '\0') return true;
    return fallback;
}

static size_t startupSizeProperty(const char* name, size_t fallback) {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get(name, prop);
    if (!prop[0]) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(prop, &end, 10);
    if (end == prop || *end != '\0' || parsed == 0) return fallback;
    return static_cast<size_t>(parsed);
}

// ============================================================
// 阶段 3/4/5 真机验证钩子(数字键 7/8/9)
// ============================================================
//
// 这三个阶段(飞行/系留/正交)在 demo 里**没有任何产品入口**,不接出来就只能
// 靠 host 判据。每个钩子都配一条机制信号日志 —— 画面"看着像对的"分不清
// "真跑了"和"根本没走到那条路"。

// 阶段 3:飞行目标(重庆 → 北京)。
constexpr double kFlightDestLng = 116.397;
constexpr double kFlightDestLat = 39.908;
constexpr double kFlightDestAlt = 3000.0;
struct FlightProbe {
    bool armed = false;
    uint64_t clampsAtStart = 0;
    double maxProgress = 0.0;
    int frames = 0;
};
FlightProbe gFlightProbe;

// 阶段 4:假载体 —— 绕重庆做匀速圆周运动并自转,驱动 tether 的两个 provider。
struct FakeCarrier {
    bool active = false;
    double angleRad = 0.0;          // 圆周相位
    double radiusDeg = 0.02;        // ~2km 半径
    double centerLng = 106.508;
    double centerLat = 29.617;
    double altMeters = 1200.0;
    bool useOrientation = false;    // true = 接 orientationProvider(座舱/roll 跟随)
    glm::dvec3 position{0.0};
    glm::dmat3 orientation{1.0};

    void step(double dt) {
        if (!active) return;
        angleRad += dt * 0.5;       // ~12s 一圈
        const double lng = centerLng + radiusDeg * std::cos(angleRad);
        const double lat = centerLat + radiusDeg * std::sin(angleRad);
        position = earth_engine::Ellipsoid::WGS84()
                       .cartographicToCartesian(
                           earth_engine::Cartographic::fromDegrees(lng, lat, altMeters))
                       .raw();
        // 机体系:绕本地垂直轴按航向转,并随相位横滚(验 roll 跟随)。
        const glm::dmat3 enu = earth_engine::CameraPose::enuFrameAt(position);
        const double heading = angleRad + 1.5707963;   // 切向
        const double roll = 0.5 * std::sin(angleRad * 2.0);
        const glm::dquat q = glm::angleAxis(-heading, enu[2]) *
                             glm::angleAxis(roll, enu[1]);
        orientation = glm::dmat3(q * enu[0], q * enu[1], q * enu[2]);
    }
};
FakeCarrier gCarrier;

// 阶段 5:正交开关。宽度取切换瞬间"透视在地面处的足迹",两者画面才可比。
bool gOrthographic = false;


using namespace earth_engine;

// 渲染线程放置策略(ADPF → uclamp_min → 性能核亲和,三级降级)已下沉成 SDK helper,
// 成因、实测数据与各级细节见 earth_engine/threading/RenderThreadPlacement.h。宿主自建
// 的渲染线程都要走一遍,否则 EAS 会把这条匿名的裸 std::thread 扔进小核簇。
namespace {
RenderThreadPlacement gRenderThreadPlacement;
}  // namespace

// ============================================================
// EGL / GL ES 3.0 上下文
// ============================================================

static EGLDisplay gDisplay = EGL_NO_DISPLAY;
static EGLSurface gSurface = EGL_NO_SURFACE;
static EGLContext gContext = EGL_NO_CONTEXT;
static ANativeWindow* gWindow = nullptr;
// 宽高被 UI 线程（触摸事件整形）与渲染线程（EGL/引擎）两侧读写，用原子避免撕裂
static std::atomic<int> gWidth{0}, gHeight{0};

// 每帧发布相机方位角(弧度),UI 指北针无锁读取。
static std::atomic<float> gHeadingRadians{0.0f};
// P6 分段:本帧 gMvtSource->update 耗时(渲染线程唯一写者,读同线程)
static double gFrameMvtMs = 0.0;

// Engine + RenderDevice
static std::unique_ptr<RenderDeviceGLES> gRenderDevice;
static std::unique_ptr<Engine> gEngine;
static std::unique_ptr<AndroidPlatformBridge> gPlatformBridge;
static std::unique_ptr<EarthEngineSdkFacade> gSdkFacade;
static bool gEngineReady = false;

// JNI_OnLoad — 存储 JavaVM 引用
static JavaVM* gJvm = nullptr;
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    gJvm = vm;
    AndroidPlatformBridge_InitJvm(vm);
    return JNI_VERSION_1_6;
}

// Application Context 的 global ref（GLESView 构造时经 nativeInit 传入），
// 供 AndroidPlatformBridge 查询目录 / 设备信息。
static jobject gAppContext = nullptr;

// Touch state
static bool gTouching = false;
static bool gDragStarted = false;
static bool gTouchMoved = false;

// Debug panel state
static bool gDebugPinchActive = false;

// ---- 矢量 P2 demo 编辑流(应用层最小实现) ----
// 引擎只出 pick/snap/预览接口;会话状态/undo 栈全在这里(demo 即参考实现)。
// gEditMode 由 UI 线程写、两线程读;其余编辑态仅渲染线程访问。
static std::atomic<bool> gEditMode{false};
static FeatureRenderLayer* gDemoFeatureLayer = nullptr;  // Engine 持有所有权
struct EditDragState {
    bool active = false;
    FeatureId featureId = kInvalidFeatureId;
    int ringIndex = -1;
    int vertexIndex = -1;
    double vertexHeight = 0.0;
    std::vector<std::vector<Cartographic>> rings;  // 工作副本
};
static EditDragState gEditDrag;
static std::vector<Feature> gEditUndoStack;  // 抓取时的编辑前快照
// P5a 编辑手柄(应用层):抓取时把被编辑环的顶点灌成 Point 要素,专用
// 手柄层渲染;拖拽实时更新被拖顶点,松手/取消清空。引擎只出点渲染能力。
static FeatureRenderLayer* gEditHandleLayer = nullptr;  // Engine 持有所有权

// ---- P6c 聚合演示(应用层)。引擎只出层级聚合索引与查询,聚合点怎么画、
// 何时刷新全在这里:源数据存在本地 FeatureStore(不进渲染),按相机 zoom
// 查询索引 → 结果写进一个普通 FeatureRenderLayer 当"显示层"。 ----
static FeatureStore gClusterSourceStore;
static FeatureClusterIndex gClusterIndex;
static FeatureRenderLayer* gClusterLayer = nullptr;  // Engine 持有所有权
static int gClusterShownLevel = -9999;               // 上次刷新用的 zoom 档
static std::vector<FeatureId> gEditHandleIds;

// ---- P4 MVT 只读底图(应用层接线)。引擎侧 MvtVectorSource 只出
// 选择/解码/灌注,网络与样式在这里:fetch 走 CurlScheduler,渲染挂
// 一个普通 FeatureRenderLayer(store 即 source 的灌注目标)。 ----
static FeatureRenderLayer* gMvtBasemapLayer = nullptr;  // Engine 持有所有权
static std::unique_ptr<MvtVectorSource> gMvtSource;     // 渲染线程访问

// ---- C2/E3:高德矢量。type2 面:无地形时走 VectorFill(V30 地球网格);
// drape overlay 仍注册,无页则不出。路网/建筑/POI 仍主源 FeatureRenderLayer。 ----
static std::shared_ptr<AmapDrapeImageryProvider::RegionCache> gAmapRegionCache;
static std::unique_ptr<AmapRegionsVectorSource> gAmapRegionsSource;
static std::unique_ptr<AmapWaterVectorSource> gAmapWater12Source;
static std::unique_ptr<AmapMainVectorSource> gAmapMainSource;
static std::unique_ptr<AmapPoiVectorSource> gAmapPoiSource;
static FeatureRenderLayer* gAmapRegionsLayer = nullptr;  // Engine 持有
static FeatureRenderLayer* gAmapWater12Layer = nullptr;  // Engine 持有
static FeatureRenderLayer* gAmapMainLayer = nullptr;  // Engine 持有
static FeatureRenderLayer* gAmapPoiLayer = nullptr;  // Engine 持有
// E1:MVT/高德使用两条独立后台通道。decode 负责压缩字节→载荷，tess 负责
// Feature→网格及 drape/road-field 合成。此前二者共用严格 FIFO 池，全球
// z3 首批 regions/main 镶嵌会把后来的 POI decode 挡在队尾；网络 8 秒已
// 完成，画面却要 20-38 秒才收敛。拆池消除队头阻塞，线程总数仍按设备
// 内存/核心有界，不减少瓦片或可见细节。
static std::shared_ptr<ThreadPool> gMvtDecodePool;
static std::shared_ptr<ThreadPool> gAmapPoiDecodePool;
static std::shared_ptr<ThreadPool> gMvtTessellationPool;
static minimal_globe_demo::MvtWorkerBudget gMvtWorkerBudget;
// 刀2:面 drape 与路网场共享的 MVT 瓦 fetch+decode 缓存(同一批 z14 祖先
// 瓦零重复拉取)。shared_ptr:两消费者 + 迟到回调自持。
static std::shared_ptr<MvtTileFetchCache> gMvtTileCache;
// V26 一期换肤钩子:留生产者指针供 nativeDebugRestyle 换样式。
//   drape:raw(unique_ptr move 进 overlay,归其持有;teardown 置空,与
//   gDemoFeatureLayer 同一套裸指针纪律);场:shared(与 request lambda 共持)。
static VectorDrapeImageryProvider* gDrapeProviderRaw = nullptr;
static std::shared_ptr<RoadFieldSource> gRoadFieldSource;
static bool gNightStyle = false;
// HttpRequest 是取消句柄(析构即取消),须持有到完成;完成 id 攒起来
// 由下一次发请求时(渲染线程)剪除,避免在 curl 回调线程里析构句柄。
struct MvtFetchInflight {
    std::mutex mutex;
    uint64_t nextId = 0;
    std::unordered_map<uint64_t, std::unique_ptr<HttpRequest>> requests;
    std::vector<uint64_t> completed;
};
static MvtFetchInflight gMvtFetch;

static void ensureMvtWorkerPools() {
    if (gMvtDecodePool && gAmapPoiDecodePool && gMvtTessellationPool) return;

    minimal_globe_demo::MvtWorkerBudget budget{
        minimal_globe_demo::kMvtDecodeThreadsFallback,
        1,
        minimal_globe_demo::kMvtTessellationThreadsFallback};
    DeviceInfo device;
    if (gPlatformBridge) {
        device = gPlatformBridge->deviceInfo();
        budget = minimal_globe_demo::chooseMvtWorkerBudget(
            device.cpuCores, device.totalMemoryBytes);
    }
    budget.decodeThreads = std::clamp<size_t>(
        startupSizeProperty("debug.ee.mvtdecode", budget.decodeThreads), 1, 4);
    budget.poiDecodeThreads = std::clamp<size_t>(
        startupSizeProperty("debug.ee.amapdecode", budget.poiDecodeThreads),
        1, 4);
    budget.tessellationThreads = std::clamp<size_t>(
        startupSizeProperty("debug.ee.mvttess", budget.tessellationThreads),
        1, 8);
    gMvtWorkerBudget = budget;
    gMvtDecodePool = std::make_shared<ThreadPool>(budget.decodeThreads);
    gAmapPoiDecodePool = std::make_shared<ThreadPool>(budget.poiDecodeThreads);
    gMvtTessellationPool =
        std::make_shared<ThreadPool>(budget.tessellationThreads);
    LOGI("MvtWorkers split type1Decode=%zu poiDecode=%zu tess=%zu cores=%d "
         "memory=%lldMB model=%s",
         budget.decodeThreads, budget.poiDecodeThreads,
         budget.tessellationThreads, device.cpuCores,
         static_cast<long long>(device.totalMemoryBytes / (1024 * 1024)),
         device.model.empty() ? "unknown" : device.model.c_str());
}

// MVT 瓦片拉取(E1 几何通路与 E4 影像通路共用)。⚠️ HttpRequest 取消句柄
// 必须持有至完成,且**不能在 curl 回调线程析构** —— 完成 id 攒批,下次发
// 请求时在调用线程剪除。
// 样式/源配置文档候选目录,按序尝试。⚠️ external 那条只有 app 自建目录才
// 可读:adb shell mkdir 建出来 owner=shell,app 读 Permission denied(真机
// 踩过,scoped storage 语义)——debug 变体用 internal + run-as cp 注入最稳:
//   adb push style-*.json sources.json /data/local/tmp/ &&
//   adb shell run-as com.earthengine.minimalglobe sh -c \
//     'mkdir -p files && cp /data/local/tmp/*.json files/'
static constexpr const char* kStyleDocDirs[] = {
    "/data/data/com.earthengine.minimalglobe/files",
    "/sdcard/Android/data/com.earthengine.minimalglobe/files",
};

// V26 尾项:数据源 URL 启动期外置。sources.json 只在 createEngine 装配时
// 读一次(运行期热切源刻意不做,见 MinimalGlobeDemoConfig.h)。MVT URL 的
// 消费点是下方 fetch 闭包(不经 SceneConfig),故落一个装配期一次写、之后
// 只读的全局;terrain/imagery 经 makeDefaultDemoSceneConfig(&ov) 走工厂。
static std::string gMvtBasemapUrl =
    minimal_globe_demo::kMvtBasemapUrlTemplate;

static minimal_globe_demo::DemoSourceOverrides loadDemoSourceOverrides() {
    minimal_globe_demo::DemoSourceOverrides ov;
    for (const char* dir : kStyleDocDirs) {
        const std::string path = std::string(dir) + "/sources.json";
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;  // 文件缺席不是错误(内置兜底)
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        std::string err;
        if (minimal_globe_demo::parseDemoSourceOverrides(text, ov, err)) {
            LOGI("DemoSources applied from %s: mvt=%s imagery=%s terrain=%s",
                 path.c_str(),
                 ov.mvtUrlTemplate.empty() ? "(builtin)"
                                           : ov.mvtUrlTemplate.c_str(),
                 ov.imageryUrlTemplate.empty() ? "(builtin)"
                                               : ov.imageryUrlTemplate.c_str(),
                 ov.terrainUrlTemplate.empty()
                     ? "(builtin)"
                     : ov.terrainUrlTemplate.c_str());
            return ov;
        }
        // fail-loud:坏文档整份拒收回落内置,LOGE 后不再试下一路径
        // (半坏配置静默换目录比报错更难排查)。
        LOGE("DemoSources rejected %s: %s(回落内置 URL)", path.c_str(),
             err.c_str());
        return minimal_globe_demo::DemoSourceOverrides{};
    }
    return ov;
}

static void mvtFetchTile(const TileKey& key,
                         std::function<void(int, std::vector<uint8_t>)> cb) {
    std::string url = gMvtBasemapUrl;
    auto replace = [&url](const char* token, int value) {
        size_t pos = url.find(token);
        if (pos != std::string::npos) {
            url.replace(pos, 3, std::to_string(value));
        }
    };
    replace("{z}", key.z);
    replace("{x}", key.x);
    replace("{y}", key.y);
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(gMvtFetch.mutex);
        for (uint64_t done : gMvtFetch.completed) {
            gMvtFetch.requests.erase(done);
        }
        gMvtFetch.completed.clear();
        id = gMvtFetch.nextId++;
    }
    auto handle = CurlMultiRequestScheduler::shared().get(
        url,
        [cb = std::move(cb), id](int statusCode, std::vector<uint8_t> body) {
            cb(statusCode, std::move(body));
            std::lock_guard<std::mutex> lock(gMvtFetch.mutex);
            gMvtFetch.completed.push_back(id);
        },
        HttpRequestOptions(HttpRequestPriority::Low));
    std::lock_guard<std::mutex> lock(gMvtFetch.mutex);
    gMvtFetch.requests[id] = std::move(handle);
}

// ---- C2/E3:高德瓦片异步 fetch 链(引擎级通路用)。
// 与 demo 切片的阻塞链同一数据路径(web/init → web_map/get_tile →
// 签名 URL),但全程异步回调,匹配 MvtTileFetchCacheT::FetchFn 契约。
// 版本探测结果跨瓦片共享(版本号是全局数据 stamp,逐瓦探测是纯浪费)。
// ---------------------------------------------------------------------
struct AmapFetchState {
    std::mutex mutex;
    std::string version;       // 已探测的版本 stamp(空 = 未探测)
    bool versionResolved = false;
    bool versionProbing = false;  // 探测在途(防并发首探)
    /// 探测在途时排队的瓦片请求(版本就绪后统一放行)。
    std::vector<std::function<void(bool)>> versionWaiters;
    uint64_t nextId = 0;
    std::unordered_map<uint64_t, std::unique_ptr<HttpRequest>> requests;
    std::vector<uint64_t> completed;
};
static AmapFetchState gAmapFetch;

/// 渲染线程专用:剪除已完成的 HttpRequest 句柄(析构即取消,故不能在
/// curl 回调线程 erase)。每帧驱动 amap 源前调用。
static void amapCleanupCompleted() {
    std::lock_guard<std::mutex> lock(gAmapFetch.mutex);
    for (uint64_t done : gAmapFetch.completed) {
        gAmapFetch.requests.erase(done);
    }
    gAmapFetch.completed.clear();
}

/// 完成一次 amap 瓦片 fetch:版本(缓存)→ manifest POST → 签名 URL GET。
/// 回调 (statusCode, body) 在任意线程;**任何失败路径都必须回调**,否则
/// MvtTileFetchCacheT 的 in-flight 永不解除、树 pending 永不消化 ——
/// 失败回调 status != 200 或空 body,由 cache 记失败账本。
static void amapFetchTile(const TileKey& key, int requestType,
                          std::function<void(int, std::vector<uint8_t>)> cb) {
    const std::string webKey = minimal_globe_demo::kAmapWebKey;
    const std::string referer = minimal_globe_demo::kAmapReferer;

    // 持有 in-flight HttpRequest 句柄到完成(析构即取消)。
    // ⚠️ 完成 id 的剪除**只在渲染线程**做(见 amapCleanupCompleted),
    // 绝不能在 curl 回调线程 erase —— 析构 HttpRequest 会 cancel 句柄,
    // cancel 可能等待 curl 内部锁,而回调正持着该锁 → 自锁死循环。
    // (与 gMvtFetch 同纪律;gMvtFetch 的剪除在渲染线程的发请求路径上。)
    auto allocId = []() -> uint64_t {
        std::lock_guard<std::mutex> lock(gAmapFetch.mutex);
        return gAmapFetch.nextId++;
    };
    auto hold = [](uint64_t id, std::unique_ptr<HttpRequest> handle) {
        std::lock_guard<std::mutex> lock(gAmapFetch.mutex);
        gAmapFetch.requests[id] = std::move(handle);
    };
    auto release = [](uint64_t id) {
        std::lock_guard<std::mutex> lock(gAmapFetch.mutex);
        gAmapFetch.completed.push_back(id);
    };

    // 阶段 3:GET 签名瓦片 URL → 字节。
    auto fetchSignedUrl = [cb, referer, allocId, hold, release](
                              const std::string& url) {
        const uint64_t id = allocId();
        auto handle = CurlMultiRequestScheduler::shared().get(
            url,
            [cb, release, id](int statusCode, std::vector<uint8_t> body) {
                cb(statusCode, std::move(body));
                release(id);
            },
            HttpRequestOptions(HttpRequestPriority::Low,
                               {{"Referer", referer}}));
        hold(id, std::move(handle));
    };

    // 阶段 2:POST get_tile manifest → 解析签名 URL → 阶段 3。
    auto fetchManifest = [key, requestType, referer, cb, fetchSignedUrl,
                          allocId, hold, release](const std::string& version) {
        AmapManifestConfig cfg;
        cfg.key = minimal_globe_demo::kAmapWebKey;
        cfg.referer = referer;
        cfg.version = version;
        const std::string url = buildGetTileUrl(cfg);
        const std::string bodyStr = buildGetTileBody(
            {{key.x, key.y, key.z, requestType}}, cfg, version);
        const uint64_t id = allocId();
        auto handle = CurlMultiRequestScheduler::shared().post(
            url, std::vector<uint8_t>(bodyStr.begin(), bodyStr.end()),
            "application/x-www-form-urlencoded",
            [cb, fetchSignedUrl, key, requestType, release, id](
                int statusCode, std::vector<uint8_t> body) {
                if (statusCode != 200 || body.empty()) {
                    cb(0, {});
                    release(id);
                    return;
                }
                std::vector<AmapTileUrl> urls;
                std::string err;
                if (!parseTileUrls(
                        std::string(body.begin(), body.end()), urls, &err) ||
                    urls.empty()) {
                    cb(0, {});
                    release(id);
                    return;
                }
                AmapTileUrl selected;
                const AmapTileRequest request{key.x, key.y, key.z,
                                              requestType};
                if (!selectAmapTileUrl(urls, request, selected, &err)) {
                    LOGE("AmapDemo: manifest URL mismatch: %s", err.c_str());
                    cb(0, {});
                    release(id);
                    return;
                }
                fetchSignedUrl(selected.url);
                release(id);
            },
            HttpRequestOptions(HttpRequestPriority::Low,
                               {{"Referer", referer}}));
        hold(id, std::move(handle));
    };

    // 阶段 1:版本探测(跨瓦片共享;已解析直接跳阶段 2)。
    // 并发首探由 versionProbing 标志拦下(首个请求发起 init,其余排队)。
    bool probeNeeded = false;
    bool resolved = false;
    std::string resolvedVersion;
    {
        std::lock_guard<std::mutex> lock(gAmapFetch.mutex);
        if (gAmapFetch.versionResolved && !gAmapFetch.version.empty()) {
            resolved = true;
            resolvedVersion = gAmapFetch.version;
        } else {
            gAmapFetch.versionResolved = false;
            gAmapFetch.versionWaiters.push_back(
                [cb, fetchManifest](bool ok) {
                    if (!ok) {
                        cb(0, {});
                        return;
                    }
                    std::string version;
                    {
                        std::lock_guard<std::mutex> lock(gAmapFetch.mutex);
                        version = gAmapFetch.version;
                    }
                    fetchManifest(version);
                });
            if (!gAmapFetch.versionProbing) {
                gAmapFetch.versionProbing = true;
                probeNeeded = true;
            }
        }
    }
    if (resolved && !resolvedVersion.empty()) {
        fetchManifest(resolvedVersion);  // 锁外调用,避免自死锁
        return;
    }
    if (!probeNeeded) return;  // 探测已在途,本 key 已排队等放行

    const std::string initUrl = "https://jsapi.amap.com/web/init?key=" + webKey;
    const uint64_t id = allocId();
    auto handle = CurlMultiRequestScheduler::shared().get(
        initUrl,
        [release, id](int statusCode, std::vector<uint8_t> body) {
            std::string version;
            if (statusCode == 200) {
                try {
                    const auto doc =
                        nlohmann::json::parse(body.begin(), body.end());
                    const auto inner =
                        nlohmann::json::parse(doc.value("tile", "{}"));
                    version = inner.value("v", "");
                } catch (const std::exception&) {
                    version.clear();
                }
            }
            std::vector<std::function<void(bool)>> waiters;
            {
                std::lock_guard<std::mutex> lock(gAmapFetch.mutex);
                gAmapFetch.version = version;
                // 探测失败不是版本“解析为空”的终态。保持 unresolved，
                // 后续请求可重新探测，避免一次瞬时网络错误毒死整个进程。
                gAmapFetch.versionResolved = !version.empty();
                gAmapFetch.versionProbing = false;
                waiters.swap(gAmapFetch.versionWaiters);
            }
            const bool ok = !version.empty();
            for (auto& w : waiters) w(ok);
            release(id);
        },
        HttpRequestOptions(HttpRequestPriority::Low,
                           {{"Referer", referer}}));
    hold(id, std::move(handle));
}

// ---- C2 步骤5:高德矢量瓦片垂直切片 ----
// 定义在文件尾部(gRenderThread 之后),见 amapLoadDemoTile。
static void amapLoadDemoTile(FeatureRenderLayer* layer, int x, int y, int z,
                             bool regionsOnly);


static double androidUptimeSeconds();
static void postInputEvent(const InputEvent& event);

// UI 线程调用：投递 Cancel 事件到渲染线程，并复位 UI 侧触摸状态。
static void cancelInputIfNeeded() {
    InputEvent event;
    event.type = InputEvent::Type::Cancel;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
    gTouching = false;
    gDragStarted = false;
    gTouchMoved = false;
    gDebugPinchActive = false;
}

static void clearDemoEngineObjects() {
    // MVT 源先停:它持有 basemap 层 store 的引用(层归 Engine 所有),
    // 且在飞的 HttpRequest 句柄析构即取消。
    gMvtSource.reset();
    gAmapRegionCache.reset();
    gAmapRegionsSource.reset();
    gAmapWater12Source.reset();
    gAmapMainSource.reset();
    gAmapPoiSource.reset();
    {
        std::lock_guard<std::mutex> lock(gMvtFetch.mutex);
        gMvtFetch.requests.clear();
        gMvtFetch.completed.clear();
    }
    {
        std::lock_guard<std::mutex> lock(gAmapFetch.mutex);
        gAmapFetch.requests.clear();
        gAmapFetch.completed.clear();
        gAmapFetch.versionWaiters.clear();
        gAmapFetch.version.clear();
        gAmapFetch.versionResolved = false;
        gAmapFetch.versionProbing = false;
    }
    gMvtBasemapLayer = nullptr;   // Engine 持有,随 gEngine 一起销毁
    gAmapMainLayer = nullptr;     // Engine 持有,随 gEngine 一起销毁
    gAmapPoiLayer = nullptr;      // Engine 持有,随 gEngine 一起销毁
    gDemoFeatureLayer = nullptr;  // Engine 持有,随 gEngine 一起销毁
    gEditHandleLayer = nullptr;
    gClusterLayer = nullptr;
    gClusterShownLevel = -9999;  // 下次装载重新刷一遍聚合显示层
    gEditHandleIds.clear();
    gEditDrag = EditDragState{};
    gEditUndoStack.clear();
    if (gEngine) {
        gEngine->setStyleTargets(nullptr, nullptr, nullptr);  // V26 三期
    }
    gDrapeProviderRaw = nullptr;  // overlay 随 facade 亡,裸指针先置空
    gRoadFieldSource.reset();
    gNightStyle = false;
    gSdkFacade.reset();
    gEngine.reset();
    gRenderDevice.reset();
    gMvtDecodePool.reset();
    gAmapPoiDecodePool.reset();
    gMvtTessellationPool.reset();
    gMvtWorkerBudget = {};
    gPlatformBridge.reset();
    gEngineReady = false;
}

static bool initEGL(ANativeWindow* window) {
    gDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gDisplay == EGL_NO_DISPLAY) return false;

    EGLint major, minor;
    if (!eglInitialize(gDisplay, &major, &minor)) return false;

    // 优先请求 4x MSAA(默认帧缓冲多重采样,eglSwapBuffers 自动 resolve,无需
    // 改 shader/离屏帧缓冲);驱动不支持则回退无 MSAA。消除地形/海岸线/建筑轮廓
    // 边缘爬行。
    // stencil 8 位:P6 矢量 stencil 分类贴地(阴影体计数)需要。
    const EGLint msaaAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 1, EGL_SAMPLES, 4,
        EGL_NONE
    };
    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs = 0;
    // MSAA A/B(2026-08-21 GPU swap 专项):4x MSAA 在 1240×2772 × 2 pass 下
    // 是 GPU 帧时大头候选(swap 10-14ms)。运行时 `adb shell setprop
    // debug.ee.msaa 0` 关,重进 app 生效。
    char msaaProp[4] = {0};
    __system_property_get("debug.ee.msaa", msaaProp);
    const bool wantMsaa = msaaProp[0] != '0';
    const EGLint* chosenAttribs = wantMsaa ? msaaAttribs : attribs;
    if (!eglChooseConfig(gDisplay, chosenAttribs, &config, 1, &numConfigs) ||
        numConfigs < 1) {
        if (!eglChooseConfig(gDisplay, attribs, &config, 1, &numConfigs)) {
            return false;
        }
        if (numConfigs < 1) return false;
    }
    EGLint chosenSamples = 0, chosenStencil = 0, chosenDepth = 0;
    eglGetConfigAttrib(gDisplay, config, EGL_SAMPLES, &chosenSamples);
    eglGetConfigAttrib(gDisplay, config, EGL_STENCIL_SIZE, &chosenStencil);
    eglGetConfigAttrib(gDisplay, config, EGL_DEPTH_SIZE, &chosenDepth);
    __android_log_print(ANDROID_LOG_INFO, "GLESView",
                        "EGL config MSAA samples=%d depth=%d stencil=%d",
                        chosenSamples, chosenDepth, chosenStencil);

    gSurface = eglCreateWindowSurface(gDisplay, config, window, nullptr);
    if (gSurface == EGL_NO_SURFACE) return false;

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    gContext = eglCreateContext(gDisplay, config, EGL_NO_CONTEXT, ctxAttribs);
    if (gContext == EGL_NO_CONTEXT) return false;

    if (!eglMakeCurrent(gDisplay, gSurface, gSurface, gContext)) return false;

    EGLint surfaceWidth = 0, surfaceHeight = 0;
    eglQuerySurface(gDisplay, gSurface, EGL_WIDTH, &surfaceWidth);
    eglQuerySurface(gDisplay, gSurface, EGL_HEIGHT, &surfaceHeight);
    gWidth = surfaceWidth;
    gHeight = surfaceHeight;

    LOGI("EGL initialized: %dx%d, GL: %s, GLSL: %s",
         surfaceWidth, surfaceHeight,
         glGetString(GL_VERSION),
         glGetString(GL_SHADING_LANGUAGE_VERSION));

    return true;
}

// C-V8 late-latch:上一帧 GPU 完成栅栏(仅渲染线程访问)。声明于此因 destroyEGL
// 在 teardown 处先引用它;创建/等待逻辑见 renderFrame 前的 late-latch 段。
static GLsync gPrevFrameFence = nullptr;
/// 有待处理的输入事件(UI 线程置位,渲染线程 onFrame 顶部消费)。C-V8 的
/// fence 等待只为 latch 新鲜输入;惯性/无输入帧没有 latch 收益,跳过等待
/// 让 CPU 与 GPU 重叠,避免帧率塌陷(2026-08-20 PHK110:GPU≈16ms 贴预算,
/// 无条件等待+CPU 4.4ms=20ms>16.7ms → 30fps)。
static std::atomic<bool> gInputPending{false};

static void destroyEGL() {
    clearDemoEngineObjects();

    // C-V8 late-latch fence 随 context 失效,趁 context 仍 current 回收。
    if (gPrevFrameFence) {
        glDeleteSync(gPrevFrameFence);
        gPrevFrameFence = nullptr;
    }

    eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gContext != EGL_NO_CONTEXT) eglDestroyContext(gDisplay, gContext);
    if (gSurface != EGL_NO_SURFACE) eglDestroySurface(gDisplay, gSurface);
    if (gDisplay != EGL_NO_DISPLAY) eglTerminate(gDisplay);
    gContext = EGL_NO_CONTEXT;
    gSurface = EGL_NO_SURFACE;
    gDisplay = EGL_NO_DISPLAY;
}

static bool createEngine() {
    clearDemoEngineObjects();
    gRenderDevice = std::make_unique<RenderDeviceGLES>();
    gEngine = std::make_unique<Engine>(gRenderDevice.get());

    gEngine->onSurfaceCreated();
    gEngine->onSurfaceChanged(gWidth, gHeight, 1.0f);

    gEngineReady = gEngine->isReady();
    if (gEngineReady) {
        const bool amapVectorEnabled =
            minimal_globe_demo::kEnableAmapVectorDemo &&
            startupBoolProperty("debug.ee.amapvector", true);
        LOGI("Engine initialized successfully, camera pos: %.1f,%.1f,%.1f",
             gEngine->camera().position().x(),
             gEngine->camera().position().y(),
             gEngine->camera().position().z());

        // 创建 Android 平台桥接；网络由 native curl scheduler 调度。
        gPlatformBridge = std::make_unique<AndroidPlatformBridge>(gJvm, gAppContext);
        gSdkFacade =
            std::make_unique<EarthEngineSdkFacade>(
                *gEngine,
                *gRenderDevice,
                *gPlatformBridge);
        // V26 尾项:sources.json 启动期读一次,MVT URL 写进 fetch 全局,
        // terrain/imagery 传工厂。必须先于 drape/场安装(fetch 闭包已建)。
        const minimal_globe_demo::DemoSourceOverrides sourceOverrides =
            loadDemoSourceOverrides();
        if (!sourceOverrides.mvtUrlTemplate.empty()) {
            gMvtBasemapUrl = sourceOverrides.mvtUrlTemplate;
        }

        // ---- 刀1 矢量**面** drape:MVT 面栅格化冒充影像进页存储合成。----
        // 必须在 installScene **之前**注册:pendingCustomOverlays_ 在
        // installScene 里消费,排在配置 overlay(卫星影像)之后 = 叠其上。
        if (minimal_globe_demo::kEnableMvtDrapeBasemap) {
            ensureMvtWorkerPools();
            if (!gMvtTileCache) {
                gMvtTileCache = std::make_shared<MvtTileFetchCache>(
                    [](const TileKey& key,
                       MvtTileFetchCache::FetchCallback cb) {
                        mvtFetchTile(key, std::move(cb));
                    },
                    minimal_globe_demo::kMvtTileCacheDecoded,
                    minimal_globe_demo::kMvtTileCacheRaw, gMvtDecodePool);
            }
            VectorDrapeImageryProvider::Options dopts;
            dopts.id = "mvt-drape";
            dopts.minZoom = minimal_globe_demo::kMvtBasemapMinZoom;
            dopts.dataMaxZoom = minimal_globe_demo::kMvtBasemapMaxZoom;
            // 与卫星影像同深:页 determination 按屏幕清晰度要多深,面就
            // 现画多深(overzoom),这是"动态栅格化"的机关。
            dopts.advertisedMaxZoom =
                minimal_globe_demo::kMeasureImageryMaxZoom;
            dopts.tileSize = 256;  // 页原生边长,再大页存储也会缩回 256
            dopts.style = minimal_globe_demo::makeMvtDrapeStyle();
            // 页网格按首源(高德卫星,GCJ-02)建 → 矢量矩形先平移回 WGS84
            // 再选 OSM 瓦,与渲染侧 worldOffset 修正互补(逐像素同位)。
            dopts.gcj02SourceGrid =
                minimal_globe_demo::kUseGaodeSatelliteForDemo;
            auto drapeProvider =
                std::make_unique<VectorDrapeImageryProvider>(
                    std::move(dopts), gMvtTileCache,
                    gMvtTessellationPool);  // Assembly 内持 weak 防拆除竞态
            RasterOverlay::Options oopts;
            oopts.role = RasterOverlayRole::AnnotationOverlay;
            oopts.priority = RasterOverlayPriority::Normal;
            // 矢量面缺席不该阻塞地形瓦片判定 complete(server 不在时整场
            // 景仍按纯影像走)。
            oopts.blocksCompleteRenderable = false;
            // ⚠️ mappedRaster 兜底路径按 overlay 自己的 georeference 喂
            // key:本 provider 产出的是"GCJ 空间矩形"语义的图(见
            // gcj02SourceGrid),故声明 Gcj02 让兜底版也走同一套修正,
            // 否则页 miss 瞬间面会闪 ~500m 错位。
            oopts.georeference =
                minimal_globe_demo::kUseGaodeSatelliteForDemo
                    ? RasterOverlayGeoreference::Gcj02WebMercator
                    : RasterOverlayGeoreference::Standard;
            gDrapeProviderRaw = drapeProvider.get();  // V26 换肤钩子
            gSdkFacade->addCustomImageryOverlay(
                std::move(drapeProvider),
                TileScheme::createXYZWebMercator(), oopts);
            LOGI("VectorDrape MVT face basemap overlay installed: %s "
                 "(data z%d-%d, advertised z%d, gcj=%d)",
                 gMvtBasemapUrl.c_str(),
                 minimal_globe_demo::kMvtBasemapMinZoom,
                 minimal_globe_demo::kMvtBasemapMaxZoom,
                 minimal_globe_demo::kMeasureImageryMaxZoom,
                 minimal_globe_demo::kUseGaodeSatelliteForDemo ? 1 : 0);
        }

        // ---- 刀2 路网线 SDF 场:页存储"第二平面"生产者注入。----
        // setRoadFieldSource 须在首帧渲染前调(页存储 lazy init 快照 Config)。
        if (minimal_globe_demo::kEnableMvtRoadField &&
            !minimal_globe_demo::kEnableEPlanRoadRibbon) {
            ensureMvtWorkerPools();
            if (!gMvtTileCache) {
                gMvtTileCache = std::make_shared<MvtTileFetchCache>(
                    [](const TileKey& key,
                       MvtTileFetchCache::FetchCallback cb) {
                        mvtFetchTile(key, std::move(cb));
                    },
                    minimal_globe_demo::kMvtTileCacheDecoded,
                    minimal_globe_demo::kMvtTileCacheRaw, gMvtDecodePool);
            }
            RoadFieldSource::Options fopts;
            fopts.dataMaxZoom = minimal_globe_demo::kMvtBasemapMaxZoom;
            fopts.fieldSize = 256;  // = 页边长(共享间接查找的前提)
            fopts.style = minimal_globe_demo::makeMvtRoadFieldStyle();
            fopts.gcj02SourceGrid =
                minimal_globe_demo::kUseGaodeSatelliteForDemo;
            auto roadField = std::make_shared<RoadFieldSource>(
                std::move(fopts), gMvtTileCache, gMvtTessellationPool);
            gRoadFieldSource = roadField;  // V26 换肤钩子
            gEngine->setRoadFieldSource(
                [roadField](const TileKey& pageKey, CancellationToken token,
                            std::function<void(std::vector<uint8_t>)> cb) {
                    roadField->requestField(pageKey, token, std::move(cb));
                },
                minimal_globe_demo::kMvtRoadFieldColor,
                minimal_globe_demo::kMvtRoadFieldWidthRampPx,
                minimal_globe_demo::kMvtRoadFieldMaxZoom);
            LOGI("VectorRoadField SDF source installed (data z%d, gcj=%d)",
                 minimal_globe_demo::kMvtBasemapMaxZoom,
                 minimal_globe_demo::kUseGaodeSatelliteForDemo ? 1 : 0);
        }

        // ---- 高德 type2 面 V1 drape:必须在 installScene 之前注册 overlay。
        if (amapVectorEnabled) {
            ensureMvtWorkerPools();
            if (!gAmapRegionCache) {
                gAmapRegionCache =
                    std::make_shared<AmapDrapeImageryProvider::RegionCache>(
                        [](const TileKey& k,
                           AmapDrapeImageryProvider::RegionCache::
                               FetchCallback cb) {
                            amapFetchTile(k, 1, std::move(cb));
                        },
                        minimal_globe_demo::kMvtTileCacheDecoded,
                        minimal_globe_demo::kMvtTileCacheRaw,
                        gMvtDecodePool);
            }
            AmapDrapeImageryProvider::Options aopts;
            aopts.id = "amap-drape";
            aopts.minZoom = 0;
            aopts.dataMinZoom = 10;
            aopts.dataMaxZoom = 10;
            aopts.advertisedMaxZoom =
                minimal_globe_demo::kMeasureImageryMaxZoom;
            aopts.tileSize = 256;
            aopts.style = minimal_globe_demo::makeAmapDrapeStyle();
            aopts.gcj02SourceGrid =
                minimal_globe_demo::kUseGaodeSatelliteForDemo;
            auto amapDrape = std::make_unique<AmapDrapeImageryProvider>(
                std::move(aopts), gAmapRegionCache, gMvtTessellationPool);
            RasterOverlay::Options aoopts;
            aoopts.role = RasterOverlayRole::AnnotationOverlay;
            aoopts.priority = RasterOverlayPriority::Normal;
            aoopts.blocksCompleteRenderable = false;
            aoopts.georeference =
                minimal_globe_demo::kUseGaodeSatelliteForDemo
                    ? RasterOverlayGeoreference::Gcj02WebMercator
                    : RasterOverlayGeoreference::Standard;
            gSdkFacade->addCustomImageryOverlay(
                std::move(amapDrape), TileScheme::createXYZWebMercator(),
                aoopts);
            LOGI("AmapDrape type2 overlay installed (data z10, advertised z%d, "
                 "gcj=%d)",
                 minimal_globe_demo::kMeasureImageryMaxZoom,
                 minimal_globe_demo::kUseGaodeSatelliteForDemo ? 1 : 0);
        }

        EarthSceneConfig sceneConfig =
            minimal_globe_demo::makeDefaultDemoSceneConfig(&sourceOverrides);
        sceneConfig.aerialFog = startupBoolProperty(
            "debug.ee.aerialfog", sceneConfig.aerialFog);
        sceneConfig.gpuPassTiming = startupBoolProperty(
            "debug.ee.gputiming", sceneConfig.gpuPassTiming);
        LOGI("RuntimeAB amapVector=%d aerialFog=%d gpuTiming=%d",
             amapVectorEnabled ? 1 : 0,
             sceneConfig.aerialFog ? 1 : 0,
             sceneConfig.gpuPassTiming ? 1 : 0);
        gSdkFacade->installScene(sceneConfig);

        // ---- P4 MVT 只读底图:先于编辑演示层挂(先挂先画,垫底)。----
        if (minimal_globe_demo::kEnableMvtBasemap) {
            // E1:底图走**瓦片桶**(worker 全链镶嵌),不再灌 store,故
            // 细桶那个 workaround 已无意义 —— 它当初是为了让「整城要素塞进
            // 空间分桶 store」时增量激活不退化成整桶全量重镶。桶尺寸留默认,
            // 该层的 store 现在只承载 demo 自己的编辑要素。
            auto basemapLayer = std::make_unique<FeatureRenderLayer>(
                "mvt-basemap", gRenderDevice.get(), Ellipsoid::WGS84());
            FeatureRenderStyle bs;
            // 贴地:stencil 分类 + 区域高度范围(零地形采样)。此前这里是
            // Absolute 抬 500m,因为贴地体要逐顶点采地形高度、而 worker 拿不到
            // 采样器(旧 store 路径能采,代价是单帧 235s)。改由 ctx 带一对
            // 标量后该约束消失,见 FeatureRenderLayer::TessellationContext。
            bs.altitudeMode = FeatureAltitudeMode::ClampToGround;
            bs.heightOffset = 0.0;
            // 按源图层分流的最小样式(tippecanoe 输出层名,数据侧对齐):
            // water 蓝面、building 灰面、缺省面淡绿;线统一浅白,宽随 zoom。
            bs.fillColorExpr = StyleExpression::match(
                "mvt_layer",
                {{"water",
                  StyleExpression::literal({0.25f, 0.50f, 0.85f, 0.55f})},
                 {"building",
                  StyleExpression::literal({0.60f, 0.60f, 0.62f, 0.55f})}},
                StyleExpression::literal({0.45f, 0.65f, 0.45f, 0.30f}));
            bs.lineColor = {0.95f, 0.95f, 0.90f, 0.85f};
            bs.lineWidthExpr = StyleExpression::interpolateLinear(
                StyleExpression::zoom(),
                {{8.0, StyleExpression::literal(1.0)},
                 {15.0, StyleExpression::literal(5.0)}});
            if (minimal_globe_demo::kEnableEPlanRoadRibbon) {
                // E 方案 P1 路网样式:按 highway 类逐要素配色(镶嵌期
                // 求值烘进顶点,零每帧成本);宽度 zoom 插值;dash 后续
                // 可加(长度 SoFar 已携带)。细分密度服务 P2 贴地曲率。
                bs.terrainClampRibbon = true;
                bs.clampDensifyMeters = 50.0;
                bs.lineColorExpr = StyleExpression::match(
                    "highway",
                    {{"motorway",
                      StyleExpression::literal(
                          {0.98f, 0.95f, 0.88f, 0.90f})},
                     {"trunk",
                      StyleExpression::literal(
                          {0.97f, 0.92f, 0.80f, 0.88f})},
                     {"primary",
                      StyleExpression::literal(
                          {0.96f, 0.90f, 0.78f, 0.86f})},
                     {"secondary",
                      StyleExpression::literal(
                          {0.93f, 0.87f, 0.74f, 0.82f})},
                     {"tertiary",
                      StyleExpression::literal(
                          {0.90f, 0.84f, 0.70f, 0.78f})},
                     {"residential",
                      StyleExpression::literal(
                          {0.88f, 0.82f, 0.68f, 0.72f})}},
                    StyleExpression::literal(
                        {0.86f, 0.80f, 0.66f, 0.65f}));
                bs.lineWidthExpr = StyleExpression::interpolateLinear(
                    StyleExpression::zoom(),
                    {{8.0, StyleExpression::literal(1.0)},
                     {15.0, StyleExpression::literal(4.0)}});
            }
            // 符号刀A:POI 点。暖红在亮白路网/绿地上都有对比;底部锚定
            // 规避「居中锚定被身前地形吃掉下半个」(P6c 明记的深度语义)。
            bs.pointColor = {0.92f, 0.26f, 0.21f, 0.95f};
            bs.pointAnchor = SymbolAnchor::Bottom;
            // 符号刀E:kind 驱动的分类观感(P6b match 表达式,镶嵌期逐要素
            // 求值烘进顶点,零每帧成本)。图形/颜色语义:地名=金星、
            // 车站=蓝方、机场=蓝菱、景点=绿三角、医院=白十字位无 → 星形
            // 家族按内置形状就近取;未列 kind 落默认暖红圆。
            bs.pointImageExpr = StyleExpression::match(
                "kind",
                {{"place:city", StyleExpression::literalString("star")},
                 {"place:town", StyleExpression::literalString("star")},
                 {"place:district", StyleExpression::literalString("star")},
                 {"place:suburb", StyleExpression::literalString("star")},
                 {"railway:station", StyleExpression::literalString("square")},
                 {"aeroway:aerodrome",
                  StyleExpression::literalString("diamond")},
                 {"tourism:attraction",
                  StyleExpression::literalString("triangle")},
                 {"tourism:museum",
                  StyleExpression::literalString("triangle")},
                 {"leisure:park", StyleExpression::literalString("triangle")},
                 {"amenity:hospital", StyleExpression::literalString("pin")}},
                StyleExpression::literalString("circle"));
            bs.pointColorExpr = StyleExpression::match(
                "kind",
                {{"place:city",
                  StyleExpression::literal({1.00f, 0.84f, 0.25f, 0.95f})},
                 {"place:town",
                  StyleExpression::literal({1.00f, 0.84f, 0.25f, 0.95f})},
                 {"place:district",
                  StyleExpression::literal({1.00f, 0.84f, 0.25f, 0.90f})},
                 {"place:suburb",
                  StyleExpression::literal({1.00f, 0.84f, 0.25f, 0.90f})},
                 {"railway:station",
                  StyleExpression::literal({0.25f, 0.52f, 0.96f, 0.95f})},
                 {"aeroway:aerodrome",
                  StyleExpression::literal({0.25f, 0.52f, 0.96f, 0.95f})},
                 {"tourism:attraction",
                  StyleExpression::literal({0.30f, 0.75f, 0.40f, 0.95f})},
                 {"tourism:museum",
                  StyleExpression::literal({0.30f, 0.75f, 0.40f, 0.95f})},
                 {"leisure:park",
                  StyleExpression::literal({0.30f, 0.75f, 0.40f, 0.95f})},
                 {"amenity:hospital",
                  StyleExpression::literal({0.95f, 0.95f, 0.95f, 0.95f})}},
                StyleExpression::literal({0.92f, 0.26f, 0.21f, 0.95f}));
            basemapLayer->setStyle(bs);
            // 贴地体的高度范围由 SceneRenderPipeline 每帧从可见地形瓦片汇总,
            // demo 侧不再设 —— 两处真相会在相机飞离本区时打架。
            gMvtBasemapLayer = basemapLayer.get();

            ensureMvtWorkerPools();
            MvtVectorSource::Options mvtOpts;
            // E2:道路分级过滤从数据侧(tippecanoe -j)搬回样式侧。改分级
            // 策略不再需要重切整套瓦片,同一份数据也能给不同样式复用 ——
            // 数据只管密度,样式管取舍(对齐 maplibre)。
            {
                using C = StyleFilter::Compare;
                // 分级表:粗档只留干线,细档逐步放开。瓦片 z 固定 → 每块
                // 瓦片按自己的 z 求值一次,**相机缩放不触发任何重镶**。
                SourceLayerRule roads;
                roads.layer = "roads";
                roads.filter = StyleFilter::any({
                    StyleFilter::all({
                        StyleFilter::zoomCompare(C::Less, 9),
                        StyleFilter::in("highway", {"motorway", "trunk",
                                                    "primary"})}),
                    StyleFilter::all({
                        StyleFilter::zoomCompare(C::GreaterEqual, 9),
                        StyleFilter::zoomCompare(C::Less, 10),
                        StyleFilter::in("highway", {"motorway", "trunk",
                                                    "primary", "secondary"})}),
                    StyleFilter::all({
                        StyleFilter::zoomCompare(C::GreaterEqual, 10),
                        StyleFilter::zoomCompare(C::Less, 12),
                        StyleFilter::in("highway", {"motorway", "trunk",
                                                    "primary", "secondary",
                                                    "tertiary"})}),
                    StyleFilter::zoomCompare(C::GreaterEqual, 12),
                });
                // 刀1:water/building **面层不再进 stencil 链**(整层排除,
                // 连 Feature 都不产生)—— 面 fill 实测 ~75ms GPU 是发热
                // 真凶,已改走 kEnableMvtDrapeBasemap 的栅格 drape(页存储
                // 合成)。本链只承载线,待刀2(SDF 场)落地后整链退役。

                // POI 符号刀A:点要素按 rank 分级放行(rank 语义见数据管线
                // extract_chongqing_geojson.py 的 POI_RANKS:1=城市级地名,
                // 6=一般设施)。同 roads:分级在样式侧,瓦片 z 固定 →
                // 相机缩放不触发重镶。
                SourceLayerRule poi;
                poi.layer = "poi";
                poi.filter = StyleFilter::any({
                    StyleFilter::all({
                        StyleFilter::zoomCompare(C::Less, 10),
                        StyleFilter::compare("rank", C::LessEqual, 2.0)}),
                    StyleFilter::all({
                        StyleFilter::zoomCompare(C::GreaterEqual, 10),
                        StyleFilter::zoomCompare(C::Less, 12),
                        StyleFilter::compare("rank", C::LessEqual, 4.0)}),
                    StyleFilter::zoomCompare(C::GreaterEqual, 12),
                });
                // 符号刀A:本链现役只承载 poi(线走 SDF 场、面走 drape)。
                // ⚠️ 整层排除靠 includeLayers 白名单,**不是** layerRules——
                // rules 里未列出的层是「全收」不是「跳过」(rule==nullptr 即
                // 不过滤)。曾因此把 roads/water/building 全量灌进 stencil
                // 几何链:新老两种路网同屏叠加 + 单帧 GPU ~54ms。
                // A/B 对拍旧几何路径时:includeLayers 加回 "roads" 并把上方
                // roads 分级规则塞回 layerRules(缺分级会全量画)。
                (void)roads;
                mvtOpts.includeLayers = {"poi"};
                mvtOpts.layerRules = {poi};
                if (minimal_globe_demo::kEnableEPlanRoadRibbon) {
                    // E 方案 P1:路网接回瓦片桶几何通道(ribbon 模式)。
                    // 与 D2 场互斥(同瓦双画):RoadFieldSource 安装已在
                    // 上方跳过。
                    mvtOpts.includeLayers = {"poi", "roads"};
                    mvtOpts.layerRules = {poi, roads};
                }
            }
            mvtOpts.tree.minZoom = minimal_globe_demo::kMvtBasemapMinZoom;
            mvtOpts.tree.maxZoom = minimal_globe_demo::kMvtBasemapMaxZoom;
            if (minimal_globe_demo::kEnableEPlanRoadRibbon) {
                mvtOpts.tree.refinement =
                    VectorTileTree::RefinementPolicy::GeometryReplace;
            }
            // 获取层单一化(刀A.5):与 drape/场共享同一 MvtTileFetchCache
            // —— 同一块数据瓦三消费方网络恰一次、解码恰一次、内存恰一份。
            if (!gMvtTileCache) {
                gMvtTileCache = std::make_shared<MvtTileFetchCache>(
                    [](const TileKey& key,
                       MvtTileFetchCache::FetchCallback cb) {
                        mvtFetchTile(key, std::move(cb));
                    },
                    minimal_globe_demo::kMvtTileCacheDecoded,
                    minimal_globe_demo::kMvtTileCacheRaw, gMvtDecodePool);
            }
            // E1 接线:镶嵌钩子在 worker 上跑,持一份样式快照(图集置空,
            // 见 FeatureRenderLayer::workerTessellationContext 的线程契约);
            // commit/drop 在渲染线程由 update() 调。裸指针安全:两者生命
            // 周期都由 gMvtSource.reset() 先于图层销毁保证。
            FeatureRenderLayer* layerPtr = basemapLayer.get();
            MvtVectorSource::Sinks sinks;
            sinks.tessellate = [layerPtr](const TileKey& key,
                                          std::vector<Feature>&& features) {
                // 贴地体高度范围按**本瓦片矩形**取局部值(拿不到相交地形瓦片
                // 时退回全屏范围)。宽视野下这是矢量 fill 的主导因子:体高
                // 直接换算成屏幕覆盖。
                // kMeasureDisablePerTileRange = A/B 对照组(退回全局范围)。
                return FeatureRenderLayer::tessellateTileMesh(
                    minimal_globe_demo::kMeasureDisablePerTileRange
                        ? layerPtr->workerTessellationContext()
                        : layerPtr->workerTessellationContextForArea(
                              mvtTileRectangle(key)),
                    features);
            };
            sinks.commit = [layerPtr](const TileKey& key,
                                      FeatureTileMesh& mesh) {
                return layerPtr->commitTileMesh(key, mesh);
            };
            sinks.drop = [layerPtr](const TileKey& key) {
                layerPtr->dropTileMesh(key);
            };
            gMvtSource = std::make_unique<MvtVectorSource>(
                mvtOpts, std::move(sinks), gMvtTileCache,
                gMvtTessellationPool);
            gEngine->addFeatureRenderLayer(std::move(basemapLayer));
            // V26 三期:样式文档分发目标注册(drape/场在前面已建;任一为
            // 空 = 该路不分发)。teardown 与三个 g* 指针同点清。
            gEngine->setStyleTargets(gDrapeProviderRaw, gRoadFieldSource,
                                     gMvtBasemapLayer);
            LOGI("VectorP4 MVT basemap installed: %s (z%d-%d)",
                 gMvtBasemapUrl.c_str(),
                 minimal_globe_demo::kMvtBasemapMinZoom,
                 minimal_globe_demo::kMvtBasemapMaxZoom);
        }

        // ---- 高德矢量:type2 面 VectorFill(z10)垫底,再上路网/建筑/POI。----
        if (amapVectorEnabled) {
            FeatureRenderStyle as;
            // amap.com 复刻:平面渲染(无地形耦合)。Absolute + 抬升,
            // 不贴地采样、不细分(用瓦片原始密度)、无地形代次重钳——
            // 消除缩放时的重钳风暴与近景 stencil 片元成本。
            as.altitudeMode = FeatureAltitudeMode::Absolute;
            as.heightOffset = 2.5;  // 抬升防 z-fight
            as.lineWidthPx = 3.0f;
            // 道路逐 code 配色(@xinzhi/amap-style spec colors.roads 首档色):
            // 高速深蓝 → 省道县道浅蓝 → 巷弄更浅,体现路网层级。
            as.lineColorExpr = StyleExpression::match(
                "amap_class",
                {{"20001",
                  StyleExpression::literal({0.431f, 0.592f, 0.733f, 0.95f})},
                 {"20002",
                  StyleExpression::literal({0.675f, 0.737f, 0.792f, 0.95f})},
                 {"20003",
                  StyleExpression::literal({0.725f, 0.804f, 0.851f, 0.95f})},
                 {"20004",
                  StyleExpression::literal({0.749f, 0.812f, 0.867f, 0.95f})},
                 {"20007",
                  StyleExpression::literal({0.808f, 0.859f, 0.902f, 0.95f})},
                 {"20008",
                  StyleExpression::literal({0.824f, 0.875f, 0.925f, 0.95f})},
                 {"20009",
                  StyleExpression::literal({0.816f, 0.875f, 0.910f, 0.95f})},
                 {"20011",
                  StyleExpression::literal({0.486f, 0.706f, 0.902f, 0.95f})},
                 {"20012",
                  StyleExpression::literal({0.859f, 0.890f, 0.945f, 0.95f})},
                 {"20013",
                  StyleExpression::literal({0.859f, 0.890f, 0.945f, 0.95f})},
                 {"20018",
                  StyleExpression::literal({0.859f, 0.882f, 0.918f, 0.95f})},
                 {"20023",
                  StyleExpression::literal({0.855f, 0.855f, 0.855f, 0.95f})},
                 {"20030",
                  StyleExpression::literal({0.796f, 0.812f, 0.827f, 0.95f})}},
                StyleExpression::literal({0.800f, 0.840f, 0.880f, 0.90f}));
            // 路宽随 zoom 变化(spec widthStops 简化):高速 20001 在
            // z10→3px、z12→4、z14→6,小级别更细。
            as.lineWidthExpr = StyleExpression::match(
                "amap_class",
                {{"20001",
                  StyleExpression::interpolateLinear(
                      StyleExpression::zoom(),
                      {{10.0, StyleExpression::literal(3.0)},
                       {12.0, StyleExpression::literal(4.0)},
                       {14.0, StyleExpression::literal(6.0)},
                       {17.0, StyleExpression::literal(8.0)}})},
                 {"20004",
                  StyleExpression::interpolateLinear(
                      StyleExpression::zoom(),
                      {{10.0, StyleExpression::literal(2.0)},
                       {13.0, StyleExpression::literal(3.0)},
                       {16.0, StyleExpression::literal(5.0)}})},
                 {"20009",
                  StyleExpression::interpolateLinear(
                      StyleExpression::zoom(),
                      {{15.0, StyleExpression::literal(1.0)},
                       {17.0, StyleExpression::literal(3.0)}})}},
                StyleExpression::interpolateLinear(
                    StyleExpression::zoom(),
                    {{10.0, StyleExpression::literal(2.0)},
                     {14.0, StyleExpression::literal(3.0)},
                     {17.0, StyleExpression::literal(4.0)}}));
            // fill 按 classCode 分流,配色对齐 @xinzhi/amap-style palette:
            //   30001 → kind(61 绿地 #ace798 / 63 水系 #80dfff / 15 海洋);
            //   30002 → regionBlocks 逐 subKey 用地类型色(colors.regionBlocks,
            //            缺省 subKey=1 → 兜底 $block #eeeeee);
            //   90001 → 建筑 roof 基色；未在官方 surface 表里的面透明。
            const auto kBlock = StyleExpression::literal(
                {0.933f, 0.933f, 0.933f, 1.00f});  // #eeeeee
            const auto kGreen = StyleExpression::literal(
                {0.675f, 0.906f, 0.596f, 1.00f});  // #ace798,fill-opacity=1(golden)
            const auto kWater = StyleExpression::literal(
                {0.502f, 0.875f, 1.00f, 1.00f});   // #80dfff,fill-opacity=1(golden)
            const auto kBuilding = StyleExpression::literal(
                {0.847f, 0.890f, 0.925f, 1.00f});  // #d8e3ec,官方默认 roof
            const auto kTransparent = StyleExpression::literal(
                {0.0f, 0.0f, 0.0f, 0.0f});
            auto color = [](const char* hex, float a = 1.0f) {
                auto hv = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return 0;
                };
                const int r = hv(hex[0]) * 16 + hv(hex[1]);
                const int g = hv(hex[2]) * 16 + hv(hex[3]);
                const int b = hv(hex[4]) * 16 + hv(hex[5]);
                return StyleExpression::literal(
                    {r / 255.0f, g / 255.0f, b / 255.0f, a});
            };
            const auto kRegionBlocks = StyleExpression::match(
                "amap_subkey",
                {{"3", color("d7edfc")},  {"4", color("e1eef7")},
                 {"5", color("b8eea4")},  {"6", color("daeafb")},
                 {"7", color("e9eaf0")},  {"8", color("e9e9f6")},
                 {"9", color("ade999")},  {"10", color("f8cacd")},
                 {"11", color("e0e7fb")}, {"12", color("e1eef7")},
                 {"19", color("92ecbe")}, {"20", color("92ecbe")},
                 {"21", color("92ecbe")}, {"22", color("f0f7fa")},
                 {"23", color("e6daf4")}, {"24", color("f4dcc1")},
                 {"25", color("d1dcf5")}, {"26", color("dae6ae")},
                 {"27", color("e5e2af")}, {"28", color("c6e4dc")},
                 {"29", color("f6d4d4")}, {"30", color("ebcded")},
                 {"31", color("d7edfc")}, {"32", color("e1eef7")},
                 {"33", color("b8eea4")}, {"34", color("daeafb")},
                 {"35", color("e9eaf0")}, {"36", color("e9e9f6")},
                 {"37", color("ade999")}, {"38", color("e0e7fb")},
                 {"41", color("faf8f5")}, {"42", color("e5f1f8")},
                 {"43", color("e5f1f8")}, {"44", color("5fe3dc")},
                 {"45", color("ffbab9")}, {"46", color("82dff9")},
                 {"47", color("cfc7dc")}, {"49", color("ffedb0")},
                 {"50", color("80c2ff")}, {"53", color("e4ecf6")},
                 {"54", color("ffd76c", 0.298f)}},
                kBlock);
            // 高德 surface/road 样式的固定压盖顺序。ordinal 在 worker
            // 镶嵌时分组、在 Scene 命令层跨瓦片排序，不依赖 PBF feature
            // 顺序、图层注册顺序或 unordered_map 遍历顺序。
            as.paintOrder = 80;
            as.paintOrderExpr = StyleExpression::match(
                "amap_class",
                {{"30002", StyleExpression::literal(30.0)},
                 {"90001", StyleExpression::literal(60.0)},
                 // 道路按 minor→major；当前单线样式只有 fill pass，
                 // casing 独立 pass 后续可直接占用 70 段而无需改架构。
                 {"20030", StyleExpression::literal(70.0)},
                 {"20023", StyleExpression::literal(71.0)},
                 {"20018", StyleExpression::literal(72.0)},
                 {"20013", StyleExpression::literal(73.0)},
                 {"20012", StyleExpression::literal(74.0)},
                 {"20011", StyleExpression::literal(75.0)},
                 {"20009", StyleExpression::literal(76.0)},
                 {"20008", StyleExpression::literal(77.0)},
                 {"20007", StyleExpression::literal(78.0)},
                 {"20004", StyleExpression::literal(79.0)},
                 {"20003", StyleExpression::literal(80.0)},
                 {"20002", StyleExpression::literal(81.0)},
                 {"20001", StyleExpression::literal(82.0)},
                 {"90003", StyleExpression::literal(90.0)},
                 // type4 content.#3 的 30003/kind64 不在官方 surface
                 // 列表中；透明且垫底，不能以未知陆地色压住水面。
                 {"30003", StyleExpression::literal(0.0)}},
                // 未单列的边界等线类仍在 surface/building 之后。
                StyleExpression::literal(70.0));
            // Nebula classCode is not globally unique: 20009 can be a road
            // or a type-3 building. Geometry type is the authoritative first
            // discriminator; class remains the subtype/order key for lines.
            as.paintOrderExpr = StyleExpression::match(
                "amap_type", {{"3", StyleExpression::literal(60.0)}},
                as.paintOrderExpr);
            as.fillColorExpr = StyleExpression::match(
                "amap_class",
                {{"30001",
                  // ⚠️ z14 主源 type2 水/绿地**不渲染**(透明):实测 z14 档案
                  // 在该区域的河道是错位碎片(与 amap.com/z12 真河位差
                  // ~300-900m),叠加 z12 水层会产生平行双带 = 「瓦片横条
                  // 状错位」。水/绿地统一由常显 z12 水层(amap-water12)
                  // 提供;本层只保留 30002 地块与路网/建筑/POI。
                  kTransparent},
                 {"30002", kRegionBlocks},
                 {"90001", kBuilding},
                 {"30003", kTransparent}},
                kTransparent);
            as.fillColorExpr = StyleExpression::match(
                "amap_type", {{"3", kBuilding}}, as.fillColorExpr);
            if (minimal_globe_demo::kHideAmapBuildingsForCompare) {
                // [1:1 对照临时] 建筑透明 + 关挤出:深色挤出体是 fill 对照
                // 的最大噪声源(纯黑建筑问题另行修)。90001 匹配不到时走
                // 原 fillColorExpr。
                as.buildingExtrusion = false;
                as.fillColorExpr = StyleExpression::match(
                    "amap_type",
                    {{"3",
                      StyleExpression::literal(
                          {0.0f, 0.0f, 0.0f, 0.0f})}},
                    as.fillColorExpr);
            }
            ensureMvtWorkerPools();
            // 粗源 z10 type2 → VectorFill(V30 地球网格)。无地形时 drape
            // 不出画,这条才是水/绿地的上屏路。先挂垫底。
            FeatureRenderStyle rs;
            rs.paintOrder = 10;
            rs.paintOrderExpr = StyleExpression::match(
                "amap_class",
                {{"30001",
                  StyleExpression::match(
                      "amap_kind",
                      {{"61", StyleExpression::literal(10.0)},
                       {"63", StyleExpression::literal(20.0)},
                       {"15", StyleExpression::literal(20.0)}},
                      StyleExpression::literal(10.0))},
                 {"30002", StyleExpression::literal(30.0)}},
                StyleExpression::literal(10.0));
            rs.altitudeMode = FeatureAltitudeMode::Absolute;
            rs.heightOffset = 2.5;
            // 粗源必须有自己的 surface 配色。主源 as 会把错位的
            // 30001 设透明，不能复用，否则 z10 的连续水/绿底实际不出画。
            rs.fillColorExpr = StyleExpression::match(
                "amap_class",
                {{"30001",
                  StyleExpression::match(
                      "amap_kind",
                      {{"61", kGreen}, {"63", kWater}, {"15", kWater}},
                      kTransparent)},
                 {"30002", kRegionBlocks}},
                kTransparent);
            rs.lineColor = {0.0f, 0.0f, 0.0f, 0.0f};
            rs.lineWidthPx = 0.0f;
            rs.buildingExtrusion = false;
            rs.stencilFillEnabled = false;
            // [A/B] V30 细分与 CDT even-odd flood-fill 不兼容(细分后
            // 填成碎点/网格,真机实证),保持关闭。
            rs.globeFillMaxEdgeMeters = 0.0;
            // z10 粗源只在远景显示:zoom ≤ 11.5(camHeight ≳ 13.8km)时
            // 出粗面;近景(zoom > 11.5)让位给主源 z12-14 细面,避免
            // 粗像素块盖在细面上 = 双源叠加「破破烂烂」。
            rs.maxZoom = 11.5;
            auto regionsLayer = std::make_unique<FeatureRenderLayer>(
                "amap-regions", gRenderDevice.get(), Ellipsoid::WGS84());
            regionsLayer->setStyle(rs);
            gAmapRegionsLayer = regionsLayer.get();
            gEngine->addFeatureRenderLayer(std::move(regionsLayer));
            if (!gAmapRegionCache) {
                gAmapRegionCache =
                    std::make_shared<AmapDrapeImageryProvider::RegionCache>(
                        [](const TileKey& k,
                           AmapDrapeImageryProvider::RegionCache::
                               FetchCallback cb) {
                            amapFetchTile(k, 1, std::move(cb));
                        },
                        minimal_globe_demo::kMvtTileCacheDecoded,
                        minimal_globe_demo::kMvtTileCacheRaw,
                        gMvtDecodePool);
            }
            AmapRegionsVectorSource::Options rOpts;
            rOpts.debugName = "amap-regions";
            // 高德并不是只有 z10 区域档。canonical view zoom 经
            // amapDataZoom 映射到服务端的 3/6/8/10/12/14 离散档位。
            // 粗区域层只消费到 z10：高空用 z3/6/8
            // 覆盖全球可见范围，不能把全国视野截成重庆中心最近 256 张
            // z10 瓦片。
            rOpts.tree.minZoom = 3;
            rOpts.tree.maxZoom = 10;
            rOpts.tree.supportedZooms = {3, 6, 8, 10};
            rOpts.tree.dataZoomForCanonicalZoom = [](int z) {
                return amapDataZoom(z);
            };
            rOpts.tree.scheme = TileScheme::createAmapGeographic();
            rOpts.tree.maxTilesPerView = 256;
            rOpts.tree.refinement =
                VectorTileTree::RefinementPolicy::GeometryReplace;
            rOpts.maxTessellationsInFlight = 8;
            AmapRegionsVectorSource::Sinks rSinks;
            FeatureRenderLayer* rLayer = gAmapRegionsLayer;
            rSinks.tessellate =
                [rLayer](const TileKey& key,
                         std::vector<Feature>&& features) {
                    return FeatureRenderLayer::tessellateTileMesh(
                        rLayer->workerTessellationContextForArea(
                            amapTileRectangle(key)),
                        features);
                };
            rSinks.commit = [rLayer](const TileKey& key,
                                     FeatureTileMesh& mesh) {
                return rLayer->commitTileMesh(key, mesh);
            };
            rSinks.drop = [rLayer](const TileKey& key) {
                rLayer->dropTileMesh(key);
            };
            gAmapRegionsSource = std::make_unique<AmapRegionsVectorSource>(
                rOpts, std::move(rSinks), gAmapRegionCache,
                gMvtTessellationPool);
            LOGI("AmapE3: regions VectorFill installed (z3/6/8/10 type2, globeFill 400m)");

            // ---- 常显 z12 粗水层:复刻 amap.com 的「粗档水底 + 细档面」叠层。----
            // 引擎 tile-bucket 做 LOD 替换(z14 子瓦加载后 z12 父瓦被替换),
            // 而 z14 水体块在瓦缝有源数据空档(实测 280m)→ 只剩 z14 时河
            // 流断。amap.com 同视野同时拉 z8/z10/z12/z14 全档,粗档水是
            // 连续大掩膜(实测 z12 视野区 4.97% 连续河带),垫在细块下盖住
            // 接缝。这里加一个树恒 z12、样式只出 30001(水/绿地)的常显层,
            // 注册在 z14 主层之前 = 画在其下。
            auto water12Layer = std::make_unique<FeatureRenderLayer>(
                "amap-water12", gRenderDevice.get(), Ellipsoid::WGS84());
            FeatureRenderStyle w12s;
            w12s.paintOrder = 20;
            w12s.paintOrderExpr = StyleExpression::match(
                "amap_kind",
                {{"61", StyleExpression::literal(20.0)},
                 {"63", StyleExpression::literal(50.0)},
                 {"15", StyleExpression::literal(50.0)}},
                StyleExpression::literal(20.0));
            w12s.altitudeMode = FeatureAltitudeMode::Absolute;
            w12s.heightOffset = 2.5;
            w12s.fillColorExpr = StyleExpression::match(
                "amap_class",
                {{"30001",
                  StyleExpression::match(
                      "amap_kind",
                      {{"61", kGreen}, {"63", kWater}, {"15", kWater}},
                      kTransparent)}},
                kTransparent);
            w12s.lineColor = {0.0f, 0.0f, 0.0f, 0.0f};
            w12s.lineWidthPx = 0.0f;
            w12s.buildingExtrusion = false;
            w12s.stencilFillEnabled = false;
            w12s.globeFillMaxEdgeMeters = 0.0;
            // z12 是近景水系接缝底板，不是全球底图。远景由 regions
            // 的 z3/6/8/10 档连续覆盖；否则固定 z12 + 256 瓦上限会再次
            // 在相机中心形成一块孤立的“重庆水面岛”。
            w12s.minZoom = 11.5;
            water12Layer->setStyle(w12s);
            gAmapWater12Layer = water12Layer.get();
            gEngine->addFeatureRenderLayer(std::move(water12Layer));
            AmapWaterVectorSource::Options w12Opts;
            w12Opts.debugName = "amap-water12";
            w12Opts.tree.minZoom = 12;
            w12Opts.tree.maxZoom = 12;
            w12Opts.tree.scheme = TileScheme::createAmapGeographic();
            w12Opts.tree.maxTilesPerView = 256;
            w12Opts.tree.refinement =
                VectorTileTree::RefinementPolicy::GeometryReplace;
            w12Opts.maxTessellationsInFlight = 8;
            AmapWaterVectorSource::Sinks w12Sinks;
            FeatureRenderLayer* w12Layer = gAmapWater12Layer;
            w12Sinks.tessellate =
                [w12Layer](const TileKey& key,
                           std::vector<Feature>&& features) {
                    return FeatureRenderLayer::tessellateTileMesh(
                        w12Layer->workerTessellationContextForArea(
                            amapTileRectangle(key)),
                        features);
                };
            w12Sinks.commit = [w12Layer](const TileKey& key,
                                         FeatureTileMesh& mesh) {
                return w12Layer->commitTileMesh(key, mesh);
            };
            w12Sinks.drop = [w12Layer](const TileKey& key) {
                w12Layer->dropTileMesh(key);
            };
            gAmapWater12Source = std::make_unique<AmapWaterVectorSource>(
                w12Opts, std::move(w12Sinks), gAmapRegionCache,
                gMvtTessellationPool);
            LOGI("AmapE3: water12 base installed (z12 type2, water/green only)");

            // 主源:路网/建筑/轨道与 30002 地块 z12-14 网格。
            // regions/main/water12 共享 gAmapRegionCache 的完整 type1
            // 解码载荷，不再对同一 gzip/protobuf 做两次完整解码。
            auto mainLayer = std::make_unique<FeatureRenderLayer>(
                "amap-vector", gRenderDevice.get(), Ellipsoid::WGS84());
            mainLayer->setStyle(as);
            gAmapMainLayer = mainLayer.get();  // Engine 将持有所有权
            gEngine->addFeatureRenderLayer(std::move(mainLayer));
            AmapMainVectorSource::Options mOpts;
            mOpts.debugName = "amap-main";
            mOpts.tree.minZoom = 3;
            mOpts.tree.maxZoom = 14;
            mOpts.tree.scheme = TileScheme::createAmapGeographic();
            // 高德数据是离散档位，不是只有重庆验证过的 z12/z14。
            // amapDataZoom 负责 canonical → 服务端档位：全球视野 z3，
            // 逐级进入 z6/8/10/12/14。这样扩大视野时降数据 LOD，而
            // 不是提高 maxTilesPerView 硬拉全球细瓦。
            mOpts.tree.supportedZooms = {3, 6, 8, 10, 12, 14};
            mOpts.tree.dataZoomForCanonicalZoom = [](int z) {
                return amapDataZoom(z);
            };
            // 近景 z14 视口约 84 瓦(1.5km 高)，默认 64 会继续降到
            // z12；抬高闸让 z14 完整进入。远景不靠这个上限硬撑，前面的
            // 离散档位会先降到 z10/8/6/3。
            mOpts.tree.maxTilesPerView = 256;
            mOpts.tree.refinement =
                VectorTileTree::RefinementPolicy::GeometryReplace;
            mOpts.maxTessellationsInFlight = 8;
            AmapMainVectorSource::Sinks mSinks;
            FeatureRenderLayer* mLayer = gAmapMainLayer;
            mSinks.tessellate =
                [mLayer](const TileKey& key,
                         std::vector<Feature>&& features) {
                    return FeatureRenderLayer::tessellateTileMesh(
                        mLayer->workerTessellationContextForArea(
                            amapTileRectangle(key)),
                        features);
                };
            mSinks.commit = [mLayer](const TileKey& key,
                                     FeatureTileMesh& mesh) {
                return mLayer->commitTileMesh(key, mesh);
            };
            mSinks.drop = [mLayer](const TileKey& key) {
                mLayer->dropTileMesh(key);
            };
            gAmapMainSource = std::make_unique<AmapMainVectorSource>(
                mOpts, std::move(mSinks), gAmapRegionCache,
                gMvtTessellationPool);
            LOGI("AmapE3: main source installed (z3/6/8/10/12/14, amap 4326 grid)");

            // POI 源:type 0 通用 POI 点标签(z14)。点符号 + 名称文字。
            FeatureRenderStyle ps;
            ps.paintOrder = 100;
            ps.altitudeMode = FeatureAltitudeMode::Absolute;
            ps.heightOffset = 2.5;
            ps.pointSizePx = 5.0f;
            ps.pointColor = {0.95f, 0.55f, 0.25f, 0.95f};
            ps.pointImage = "circle";
            ps.labelProperty = "name";
            ps.labelSizePx = 16.0f;
            ps.labelOffsetPx = 10.0f;
            ps.labelColor = {0.15f, 0.20f, 0.30f, 0.95f};
            ps.labelHaloColor = {1.0f, 1.0f, 1.0f, 0.85f};
            ps.labelHaloPx = 1.5f;
            auto poiLayer = std::make_unique<FeatureRenderLayer>(
                "amap-poi", gRenderDevice.get(), Ellipsoid::WGS84());
            poiLayer->setStyle(ps);
            gAmapPoiLayer = poiLayer.get();
            gEngine->addFeatureRenderLayer(std::move(poiLayer));
            auto poiCache =
                std::make_shared<MvtTileFetchCacheT<
                    AmapDecodedTile, AmapPoiDecodedTileDecodeTraits>>(
                    [](const TileKey& k,
                       MvtTileFetchCacheT<AmapDecodedTile,
                                          AmapPoiDecodedTileDecodeTraits>::
                           FetchCallback cb) {
                        amapFetchTile(k, 2, std::move(cb));
                    },
                    minimal_globe_demo::kMvtTileCacheDecoded,
                    minimal_globe_demo::kMvtTileCacheRaw,
                    gAmapPoiDecodePool);
            AmapPoiVectorSource::Options pOpts;
            pOpts.debugName = "amap-poi";
            pOpts.tree.minZoom = 3;
            pOpts.tree.maxZoom = 14;
            pOpts.tree.scheme = TileScheme::createAmapGeographic();
            pOpts.tree.supportedZooms = {3, 6, 8, 10, 12, 14};
            pOpts.tree.dataZoomForCanonicalZoom = [](int z) {
                return amapDataZoom(z);
            };
            pOpts.tree.maxTilesPerView = 256;
            pOpts.maxTessellationsInFlight = 8;
            AmapPoiVectorSource::Sinks pSinks;
            FeatureRenderLayer* pLayer = gAmapPoiLayer;
            pSinks.tessellate =
                [pLayer](const TileKey& key,
                         std::vector<Feature>&& features) {
                    return FeatureRenderLayer::tessellateTileMesh(
                        pLayer->workerTessellationContextForArea(
                            amapTileRectangle(key)),
                        features);
                };
            pSinks.commit = [pLayer](const TileKey& key,
                                     FeatureTileMesh& mesh) {
                return pLayer->commitTileMesh(key, mesh);
            };
            pSinks.drop = [pLayer](const TileKey& key) {
                pLayer->dropTileMesh(key);
            };
            gAmapPoiSource = std::make_unique<AmapPoiVectorSource>(
                pOpts, std::move(pSinks), poiCache,
                gMvtTessellationPool);
            LOGI("AmapE3: POI source installed (z3/6/8/10/12/14, type-0 labels)");
        }

        // P5b 标注字体(应用层读文件供字节,引擎不碰文件系统)。**不在任何
        // 图层开关内**:MVT 底图 POI 标签(符号刀B)与 demo 编辑层都消费
        // 同一 GlyphAtlas,字体注入是共享前置。候选序:Oplus-Serif=本机中文
        // TrueType;NotoSansCJK.ttc 是 CFF 会被 stbtt 拒(留表验证健壮性);
        // Roboto 兜底拉丁。
        {
            const char* fontCandidates[] = {
                "/system/fonts/Oplus-Serif.ttf",
                "/system/fonts/DroidSansFallback.ttf",
                "/system/fonts/NotoSansCJK-Regular.ttc",
                "/system/fonts/Roboto-Regular.ttf",
            };
            for (const char* path : fontCandidates) {
                std::ifstream in(path, std::ios::binary);
                if (!in) continue;
                std::vector<uint8_t> bytes(
                    (std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
                if (bytes.empty()) continue;
                if (gEngine->setLabelFontData(std::move(bytes))) {
                    LOGI("VectorP5b label font: %s", path);
                    break;
                }
                LOGI("VectorP5b font rejected (CFF/parse): %s", path);
            }
        }

        // 矢量数据系统 P1 真机验证:demo 相机(重庆)附近挂一面一线。
        // heightOffset 抬离地表(该区地形 ~200-800m)防 depthTest 埋没;
        // 贴地钳制属 P3。
        if (minimal_globe_demo::kEnableVectorDemoLayers) {
            constexpr double kDeg = M_PI / 180.0;
            auto vectorLayer = std::make_unique<FeatureRenderLayer>(
                "demo-vector-p1", gRenderDevice.get(), Ellipsoid::WGS84());
            FeatureRenderStyle style;
            style.fillColor = {0.20f, 0.55f, 0.95f, 0.35f};
            style.lineColor = {1.00f, 0.72f, 0.05f, 0.95f};
            style.lineWidthPx = 6.0f;
            // P6d dash 真机验证:60m 一节、划段 60%(贴地世界米制,拉远
            // 变密拉近变疏是透视语义;设 0 恢复实线)。
            style.lineDashPeriodMeters = 60.0f;
            style.lineDashOnFraction = 0.6f;
            // 贴地:fill 走 stencil 像素贴合(P6a),线/outline 同走 stencil
            // 墙带体(P6d 终态,免疫陡变地形断线与抬升视差)。下面两个参数
            // 只服务后端不支持 stencil 时的方案 A 回落(细分 + 抬升过渡档,
            // 断线根因与实测档位见 commit 588e5afde)。
            style.altitudeMode = FeatureAltitudeMode::ClampToGround;
            style.heightOffset = 2.5;
            style.clampDensifyMeters = 8.0;
            // P6b 数据驱动样式:fill 色按 zone 属性(stencil 按色分组)、
            // 点色按 kind 三色、线宽随 zoom 插值(拉远变细凑近变粗)。
            style.fillColorExpr = StyleExpression::match(
                "zone",
                {{"core",
                  StyleExpression::literal({0.20f, 0.55f, 0.95f, 0.35f})}},
                StyleExpression::literal({0.90f, 0.30f, 0.20f, 0.35f}));
            style.pointColorExpr = StyleExpression::match(
                "kind",
                {{"tower",
                  StyleExpression::literal({1.00f, 0.35f, 0.25f, 0.95f})},
                 {"gate",
                  StyleExpression::literal({1.00f, 0.85f, 0.20f, 0.95f})}},
                // 兜底分支给中性白:这一支走 P6c 位图图标(beacon),顶点色
                // 对位图是 tint 乘子,染色会盖掉图本身的三段色,验不了 uv。
                StyleExpression::literal({1.00f, 1.00f, 1.00f, 1.00f}));
            style.lineWidthExpr = StyleExpression::interpolateLinear(
                StyleExpression::zoom(),
                {{10.0, StyleExpression::literal(2.0)},
                 {15.0, StyleExpression::literal(10.0)}});
            // P6c 数据驱动选图:tower → 内置水滴 pin(底尖压在锚点上),
            // gate → 内置五角星,缺省 → 位图图标 beacon(下面注入;图集
            // 代次变化会自动触发重镶,注入晚于建桶也能补上)。
            // 一屏同时覆盖「解析 SDF 形状」与「位图图集」两条通道。
            style.pointImageExpr = StyleExpression::match(
                "kind",
                {{"tower", StyleExpression::literalString("pin")},
                 {"gate", StyleExpression::literalString("star")}},
                StyleExpression::literalString("beacon"));
            style.pointSizePx = 26.0f;
            // marker 语义:图形整个立在锚点上方(而非以锚点为中心)。斜视
            // 下居中锚定会让下半个符号被前方地面按深度遮掉——billboard 用
            // 的是锚点深度,身下的地更近。
            style.pointAnchor = SymbolAnchor::Bottom;
            // 底部锚定后符号整体上移,标注基线要让开符号高度,否则字压图。
            style.labelOffsetPx = style.pointSizePx + 6.0f;
            vectorLayer->setStyle(style);

            // 尺寸压到 RESET 预设视角(106.508,29.617,1.5km,-45°)一屏内:
            // 面 ~1.1km 见方带边界,线折两折穿过视野中心。
            // 尺寸 ~550m,钉在 RESET 视角(1500m/-45°)中带:800m 面高下
            // 角点全部可见可拾取。
            Feature poly;
            poly.type = GeometryType::Polygon;
            poly.rings = {{
                Cartographic(106.5055 * kDeg, 29.6200 * kDeg),
                Cartographic(106.5105 * kDeg, 29.6200 * kDeg),
                Cartographic(106.5105 * kDeg, 29.6250 * kDeg),
                Cartographic(106.5055 * kDeg, 29.6250 * kDeg),
                Cartographic(106.5055 * kDeg, 29.6200 * kDeg)}};
            poly.properties["name"] = "示范区 A";
            poly.properties["zone"] = "core";
            vectorLayer->store().addFeature(std::move(poly));

            // P6b 验证:第二个面 zone 缺省 → fill 表达式兜底红,与示范区 A
            // 的 core 蓝形成 stencil 双色组。
            Feature annex;
            annex.type = GeometryType::Polygon;
            annex.rings = {{
                Cartographic(106.5115 * kDeg, 29.6200 * kDeg),
                Cartographic(106.5145 * kDeg, 29.6200 * kDeg),
                Cartographic(106.5145 * kDeg, 29.6230 * kDeg),
                Cartographic(106.5115 * kDeg, 29.6230 * kDeg),
                Cartographic(106.5115 * kDeg, 29.6200 * kDeg)}};
            annex.properties["name"] = "附属区 B";
            vectorLayer->store().addFeature(std::move(annex));

            Feature route;
            route.type = GeometryType::LineString;
            route.rings = {{
                Cartographic(106.5020 * kDeg, 29.6180 * kDeg),
                Cartographic(106.5060 * kDeg, 29.6220 * kDeg),
                Cartographic(106.5100 * kDeg, 29.6190 * kDeg),
                Cartographic(106.5140 * kDeg, 29.6230 * kDeg)}};
            route.properties["name"] = "巡线 Route-1";
            vectorLayer->store().addFeature(std::move(route));

            // P5c 避让验证簇:~60m 间距 5 个标注点,RESET 视角下标签屏幕
            // 盒相互重叠 → 避让隐藏一部分(fade),凑近才逐个显出。
            for (int i = 0; i < 5; ++i) {
                Feature obs;
                obs.type = GeometryType::Point;
                obs.rings = {{Cartographic(
                    (106.5040 + 0.0006 * (i % 3)) * kDeg,
                    (29.6260 + 0.0005 * (i / 3)) * kDeg)}};
                obs.properties["name"] =
                    std::string("观测点-") + std::to_string(i + 1);
                // P6b:kind 轮转 tower/gate/(缺省) → 点色红/黄/兜底绿。
                if (i % 3 == 0) obs.properties["kind"] = "tower";
                else if (i % 3 == 1) obs.properties["kind"] = "gate";
                vectorLayer->store().addFeature(std::move(obs));
            }

            // P6c 图标:注入一张程序生成的位图图标(应用层供 RGBA 像素,
            // 引擎不做图片解码)。竖向三段色(上橙/中白/下青)是故意的——
            // 屏幕上若上下颠倒即说明图集 uv 的 v 方向接反了。
            {
                constexpr int kIconW = 24;
                constexpr int kIconH = 32;
                std::vector<uint8_t> icon(
                    static_cast<size_t>(kIconW) * kIconH * 4, 0);
                for (int y = 0; y < kIconH; ++y) {
                    for (int x = 0; x < kIconW; ++x) {
                        uint8_t* px =
                            &icon[(static_cast<size_t>(y) * kIconW + x) * 4];
                        const bool border = x < 2 || y < 2 ||
                                            x >= kIconW - 2 || y >= kIconH - 2;
                        if (border) {
                            px[0] = px[1] = px[2] = 20;
                            px[3] = 255;
                        } else if (y < kIconH / 3) {
                            px[0] = 250; px[1] = 140; px[2] = 30; px[3] = 255;
                        } else if (y < kIconH * 2 / 3) {
                            px[0] = px[1] = px[2] = 245;
                            px[3] = 255;
                        } else {
                            px[0] = 20; px[1] = 190; px[2] = 200; px[3] = 255;
                        }
                    }
                }
                if (gEngine->addIconImage("beacon", kIconW, kIconH, icon)) {
                    LOGI("VectorP6c icon injected: beacon %dx%d",
                         kIconW, kIconH);
                } else {
                    LOGI("VectorP6c icon injection FAILED");
                }
            }
            gDemoFeatureLayer = vectorLayer.get();
            gEngine->addFeatureRenderLayer(std::move(vectorLayer));

            // P5a 编辑手柄层(应用层):白色 SDF 圆点,贴地略高于要素防遮。
            auto handleLayer = std::make_unique<FeatureRenderLayer>(
                "edit-handles", gRenderDevice.get(), Ellipsoid::WGS84());
            FeatureRenderStyle handleStyle;
            handleStyle.pointColor = {1.0f, 1.0f, 1.0f, 0.95f};
            handleStyle.pointSizePx = 20.0f;
            handleStyle.altitudeMode = FeatureAltitudeMode::ClampToGround;
            handleStyle.heightOffset = 14.0;
            handleLayer->setStyle(handleStyle);
            gEditHandleLayer = handleLayer.get();
            gEngine->addFeatureRenderLayer(std::move(handleLayer));

            // ---- P6c 聚合演示(应用层)----
            // 源数据:重庆周边 ~25km 内 300 个点,分三团(团内密、团间疏),
            // 拉远看是三个大簇、凑近逐级散开。源 store 不进引擎渲染。
            {
                gClusterSourceStore.clear();
                const double clusterCenters[3][2] = {{106.50, 29.60},
                                                     {106.62, 29.66},
                                                     {106.44, 29.72}};
                uint32_t seed = 12345u;
                auto nextRand = [&seed]() {
                    // 固定种子的 LCG:每次启动布点一致,便于 A/B 比对。
                    seed = seed * 1664525u + 1013904223u;
                    return static_cast<double>(seed >> 8) /
                           static_cast<double>(1u << 24);
                };
                for (int i = 0; i < 300; ++i) {
                    const auto& c = clusterCenters[i % 3];
                    Feature p;
                    p.type = GeometryType::Point;
                    p.rings = {{Cartographic(
                        (c[0] + (nextRand() - 0.5) * 0.06) * kDeg,
                        (c[1] + (nextRand() - 0.5) * 0.04) * kDeg)}};
                    gClusterSourceStore.addFeature(std::move(p));
                }
                FeatureClusterOptions clusterOpts;
                clusterOpts.minZoom = 0;
                clusterOpts.maxZoom = 16;
                clusterOpts.radiusPx = 70.0;
                gClusterIndex.build(gClusterSourceStore, clusterOpts);

                // 显示层:簇与单点共用一层,靠 cluster 属性数据驱动区分
                // (簇 = 青圆 + 计数标签;单点 = 白圆无标签。尺寸是 zoom
                // 驱动的 uniform,不能逐要素分大小,故只用颜色区分)。
                auto clusterLayer = std::make_unique<FeatureRenderLayer>(
                    "demo-clusters", gRenderDevice.get(), Ellipsoid::WGS84());
                FeatureRenderStyle cs;
                cs.altitudeMode = FeatureAltitudeMode::ClampToGround;
                cs.heightOffset = 8.0;
                cs.labelProperty = "name";  // 簇写 count,单点留空不出标签
                cs.labelSizePx = 22.0f;
                cs.pointSizePx = 34.0f;
                cs.pointAnchor = SymbolAnchor::Bottom;  // 同上:整圆立在锚点上
                cs.labelOffsetPx = 0.5f * cs.pointSizePx;  // 计数压在圆心
                cs.pointColorExpr = StyleExpression::match(
                    "cluster",
                    {{"1", StyleExpression::literal(
                               {0.10f, 0.75f, 0.85f, 0.85f})}},
                    StyleExpression::literal({1.0f, 1.0f, 1.0f, 0.9f}));
                clusterLayer->setStyle(cs);
                gClusterLayer = clusterLayer.get();
                gEngine->addFeatureRenderLayer(std::move(clusterLayer));
                LOGI("VectorP6c cluster demo: %zu source points, %zu levels",
                     gClusterSourceStore.size(), gClusterIndex.levelCount());
            }
            LOGI("VectorP1 demo layer installed: 1 polygon + 1 line");
        }

        // ---- 海拔着色轨迹 demo(2026-08-23,独立开关默认开)----
        // 数据 = 现有 FeatureStore(LineString 顶点带椭球高);
        // 渐变 = 复用既有 VectorLine48 顶点布局:逐顶点椭球高 → 线性渐变
        // RGBA8 烘进 a_color,lengthSoFar 原样携带(dash 语义不变),不新增
        // shader/顶点属性。Absolute 模式走方案 A ribbon —— stencil 贴地线
        // 是整线分组色,逐顶点色需体积 mesh 扩展(后置)。
        // 路线故意起伏(420→1720→1200m):若颜色跟着海拔而非里程走,一眼
        // 可见;在 RESET 预设视角(1.5km/-45°)中段穿过视野。
        if (minimal_globe_demo::kEnableElevationTrajectoryDemo) {
            constexpr double kDeg = M_PI / 180.0;
            auto trajectoryLayer = std::make_unique<FeatureRenderLayer>(
                "demo-elevation-trajectory", gRenderDevice.get(),
                Ellipsoid::WGS84());
            FeatureRenderStyle ts;
            ts.altitudeMode = FeatureAltitudeMode::Absolute;
            ts.lineWidthPx = 9.0f;
            ts.lineColorGradientByHeight = true;
            ts.lineColorGradientHeightMinMeters = 400.0f;
            ts.lineColorGradientHeightMaxMeters = 1700.0f;
            ts.lineColorGradientLow = {0.10f, 0.55f, 0.25f, 0.95f};
            ts.lineColorGradientHigh = {0.90f, 0.15f, 0.15f, 0.95f};
            trajectoryLayer->setStyle(ts);

            Feature trail;
            trail.type = GeometryType::LineString;
            trail.properties["name"] = "海拔着色轨迹";
            trail.rings = {{}};
            const struct { double lon, lat, h; } kTrail[] = {
                {106.5200, 29.5900, 420}, {106.5160, 29.5960, 580},
                {106.5110, 29.6010, 760}, {106.5070, 29.6040, 620},
                {106.5035, 29.6085, 1050}, {106.5000, 29.6130, 1420},
                {106.4970, 29.6180, 1280}, {106.4930, 29.6220, 1580},
                {106.4890, 29.6260, 1720}, {106.4850, 29.6300, 1450},
                {106.4810, 29.6345, 1650}, {106.4770, 29.6390, 1200},
            };
            for (const auto& p : kTrail) {
                trail.rings[0].emplace_back(
                    Cartographic(p.lon * kDeg, p.lat * kDeg, p.h));
            }
            trajectoryLayer->store().addFeature(std::move(trail));
            gEngine->addFeatureRenderLayer(std::move(trajectoryLayer));
            LOGI("VectorElevationTrajectory demo: 12 pts 420->1720m "
                 "absolute + per-vertex a_color gradient");
        }
        // Phase 2c P5:GPU 位移已引擎默认开(Engine.h terrainGpuDisplacementEnabled_
        // = true,pool 在首次 scene update 前急切创建)。运行时 A/B 关闭仍走调试面板
        // 的 setTerrainGpuDisplacementEnabled(false)(GLESView.cpp toggle)。
    } else {
        LOGE("Engine initialization failed");
        clearDemoEngineObjects();
    }
    return gEngineReady;
}

// ---- 矢量 P2 demo 编辑流(以下函数仅渲染线程调用) ----

static void clearEditHandles() {
    if (!gEditHandleLayer) return;
    for (FeatureId id : gEditHandleIds) {
        gEditHandleLayer->store().removeFeature(id);
    }
    gEditHandleIds.clear();
}

// 抓取时:被编辑环的每个顶点一个手柄(polygon 闭合末点不重复)。
static void populateEditHandles() {
    if (!gEditHandleLayer || !gEditDrag.active) return;
    clearEditHandles();
    const auto& ring =
        gEditDrag.rings[static_cast<size_t>(gEditDrag.ringIndex)];
    const Feature* feature =
        gDemoFeatureLayer->store().getFeature(gEditDrag.featureId);
    const bool closedDup =
        feature && feature->type == GeometryType::Polygon &&
        ring.size() >= 2 &&
        ring.front().longitude() == ring.back().longitude() &&
        ring.front().latitude() == ring.back().latitude();
    const size_t count = ring.size() - (closedDup ? 1 : 0);
    gEditHandleIds.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Feature handle;
        handle.type = GeometryType::Point;
        handle.rings = {{ring[i]}};
        gEditHandleIds.push_back(
            gEditHandleLayer->store().addFeature(std::move(handle)));
    }
}

// 拖拽中:只更新被拖顶点的手柄(闭合末点映射回首手柄)。
static void updateDraggedHandle(const Cartographic& target) {
    if (!gEditHandleLayer || gEditHandleIds.empty()) return;
    const size_t idx =
        static_cast<size_t>(gEditDrag.vertexIndex) % gEditHandleIds.size();
    const Feature* handle =
        gEditHandleLayer->store().getFeature(gEditHandleIds[idx]);
    if (!handle) return;
    Feature moved = *handle;
    moved.rings = {{target}};
    moved.bounds = Rectangle();
    gEditHandleLayer->store().updateFeature(moved);
}

// 手势起点:pick 顶点 → 抓取(undo 快照 + beginEditPreview)。
static void editTouchDown(float x, float y) {
    if (!gEngine || !gDemoFeatureLayer || gEditDrag.active) return;
    FrameState pickFrame;
    pickFrame.camera = &gEngine->camera();
    pickFrame.viewportWidthPixels = gWidth.load();
    pickFrame.viewportHeightPixels = gHeight.load();
    const FeaturePickResult hit =
        gDemoFeatureLayer->pick(pickFrame, x, y, 48.0f);
    if (hit.part != FeaturePickResult::Part::Vertex) {
        LOGI("EditFlow: no vertex at (%.0f,%.0f) part=%d", x, y,
             static_cast<int>(hit.part));
        return;
    }
    const Feature* feature =
        gDemoFeatureLayer->store().getFeature(hit.featureId);
    if (!feature) return;
    if (!gDemoFeatureLayer->beginEditPreview(hit.featureId)) return;
    // P5c 编辑联动:选中(抓取)要素标签提权,避让时优先显示。
    gDemoFeatureLayer->setLabelPriorityFeature(hit.featureId);
    gEditUndoStack.push_back(*feature);
    gEditDrag.active = true;
    gEditDrag.featureId = hit.featureId;
    gEditDrag.ringIndex = hit.ringIndex;
    gEditDrag.vertexIndex = hit.vertexIndex;
    gEditDrag.vertexHeight = hit.position.height();
    gEditDrag.rings = feature->rings;
    populateEditHandles();
    LOGI("EditFlow: grab feature=%llu ring=%d vertex=%d distPx=%.1f handles=%zu",
         static_cast<unsigned long long>(hit.featureId),
         hit.ringIndex, hit.vertexIndex, hit.distancePx,
         gEditHandleIds.size());
}

// 拖拽:指尖地面坐标 → snap 候选吸附 → 更新预览。
static void editTouchMove(float x, float y) {
    if (!gEngine || !gDemoFeatureLayer || !gEditDrag.active) return;
    const PickResult ground = gEngine->pick(x, y);
    if (!ground.isValid()) return;
    Cartographic target(ground.cartographic.longitude(),
                        ground.cartographic.latitude(),
                        gEditDrag.vertexHeight);
    // snap 容差 = 24px 换算地面米(相机距离 × 每像素弧度),排除自身。
    const double dist =
        (ground.worldPosition - gEngine->camera().position()).length();
    const double tolMeters = std::max(
        5.0, dist * gEngine->camera().verticalFovRadians() /
                 std::max(1, gHeight.load()) * 24.0);
    const auto snap = FeatureSnapQuery::nearest(
        gDemoFeatureLayer->store(), Ellipsoid::WGS84(), target, tolMeters,
        gEditDrag.featureId);
    if (snap) {
        target = Cartographic(snap->position.longitude(),
                              snap->position.latitude(),
                              gEditDrag.vertexHeight);
        LOGI("EditFlow: snap to feature=%llu %s idx=%d dist=%.1fm",
             static_cast<unsigned long long>(snap->featureId),
             snap->part == SnapCandidate::Part::Vertex ? "vertex" : "edge",
             snap->vertexIndex, snap->distanceMeters);
    }
    auto& ring = gEditDrag.rings[gEditDrag.ringIndex];
    ring[static_cast<size_t>(gEditDrag.vertexIndex)] = target;
    // polygon 闭合环:拖首/末点时同步另一端保持闭合。
    const Feature* feature =
        gDemoFeatureLayer->store().getFeature(gEditDrag.featureId);
    if (feature && feature->type == GeometryType::Polygon &&
        ring.size() >= 2) {
        if (gEditDrag.vertexIndex == 0) {
            ring.back() = target;
        } else if (static_cast<size_t>(gEditDrag.vertexIndex) ==
                   ring.size() - 1) {
            ring.front() = target;
        }
    }
    gDemoFeatureLayer->updateEditPreview(gEditDrag.rings);
    updateDraggedHandle(target);
}

// 松手:commit 落库(undo 快照已在抓取时入栈)+ 结束预览。
static void editTouchUp() {
    if (!gDemoFeatureLayer || !gEditDrag.active) return;
    const Feature* feature =
        gDemoFeatureLayer->store().getFeature(gEditDrag.featureId);
    if (feature) {
        Feature edited = *feature;
        edited.rings = gEditDrag.rings;
        edited.bounds = Rectangle();  // store 从 rings 重算
        gDemoFeatureLayer->store().updateFeature(edited);
        LOGI("EditFlow: commit feature=%llu version=%llu undoDepth=%zu",
             static_cast<unsigned long long>(gEditDrag.featureId),
             static_cast<unsigned long long>(
                 gDemoFeatureLayer->store()
                     .getFeature(gEditDrag.featureId)->version),
             gEditUndoStack.size());
    }
    gDemoFeatureLayer->endEditPreview();
    gEditDrag = EditDragState{};
    clearEditHandles();
}

static int gFrameCount = 0;
/// P6c 聚合演示的每帧刷新(应用层职责:引擎只出索引,画什么由这里定)。
/// 相机 zoom 档变化才重建显示层——聚合是层级预聚,同一档内结果不变,
/// 平移不需要重建(300 点直接全量查,不做视口裁剪)。渲染线程调用。
static void refreshClusterDisplay() {
    if (!gClusterLayer || gClusterIndex.empty()) return;
    const double camHeight =
        Ellipsoid::WGS84()
            .cartesianToCartographic(gEngine->camera().position())
            .height();
    // 与引擎 zoom 驱动样式同一换算(web 墨卡托惯例)。
    const double zoom = std::min(
        24.0, std::max(0.0, std::log2(4.0e7 / std::max(1.0, camHeight))));
    const int level = static_cast<int>(std::lround(zoom));
    if (level == gClusterShownLevel) return;
    gClusterShownLevel = level;

    const Rectangle world(-M_PI, -M_PI / 2.0, M_PI, M_PI / 2.0);
    const auto clusters = gClusterIndex.query(world, zoom);
    gClusterLayer->store().clear();
    for (const auto& c : clusters) {
        Feature f;
        f.type = GeometryType::Point;
        f.rings = {{Cartographic(c.longitude, c.latitude)}};
        if (c.isCluster()) {
            f.properties["cluster"] = "1";
            f.properties["name"] = std::to_string(c.count);
        }
        gClusterLayer->store().addFeature(std::move(f));
    }
    LOGI("VectorP6c clusters: zoom=%.2f level=%d entries=%zu", zoom, level,
         clusters.size());
}

// ── 输入 late-latch:fence 门控的 render-ahead cap(C-V8)────────────────
// 弱机 GPU-bound 时 CPU 领跑 GPU 2-3 帧,latch 到的 pose 要等多帧才上屏 →
// 手指→光子滞后 = 流水线深度 × 帧时,而非 latch 时机。把驱动内那段隐式 GPU
// 等待用显式 fence 挪到 latch **之前**:等完再排输入 → latch 到更新鲜的指位,
// 同时把 render-ahead 压到深度 1。等待总量不变(GPU-bound),不掉吞吐;快机
// fence 立即返回 → 0 成本、自适应,无需按机型开关。
// 运行期 A/B:`adb shell setprop debug.ee.latelatch 0` 关(默认开)。
// (gPrevFrameFence 声明上移至 destroyEGL 之前,因其在 teardown 处先被引用。)
static double gFenceWaitMs = 0.0;          // 上一次门控等待耗时(探针)

static bool lateLatchEnabled() {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.ee.latelatch", prop);
    return prop[0] != '0';  // 未设或非 "0" → 开
}

// 影子自检运行时开关(debug 构建默认开 = 收敛漏报捕网,但同步回读会让交互
// 卡——每 idle 段 20 帧 glReadPixels 排空 GPU 管线,见 kShadowVerifyIdle 注释)。
// 运行期切换:`adb shell setprop debug.ee.shadowverify 0` 关 / `1` 开 /
// 清空回落到构建默认。逐帧读(共享内存,亚微秒,与 latelatch 同模式),值变化
// 才调 setShadowVerifyEnabled。
static bool gLastShadowVerifySetting =
    earth_engine::minimal_globe_demo::kShadowVerifyIdle;
static void applyShadowVerifyRuntimeSwitch() {
    char prop[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.ee.shadowverify", prop);
    bool desired = earth_engine::minimal_globe_demo::kShadowVerifyIdle;
    if (prop[0] == '0') {
        desired = false;
    } else if (prop[0] == '1') {
        desired = true;
    }
    if (desired != gLastShadowVerifySetting) {
        gLastShadowVerifySetting = desired;
        if (gEngine) {
            gEngine->setShadowVerifyEnabled(desired);
            LOGI("ShadowVerify runtime switch -> %s",
                 desired ? "on" : "off");
        }
    }
}

// onFrame 顶部、drainTasks 之前调用:等上一帧 GPU 完成(render-ahead≤1),
// 使随后排空的输入尽量新鲜。带超时,GPU 丢失时不挂死。
static void waitPrevFrameFenceForLatch() {
    gFenceWaitMs = 0.0;
    if (!gPrevFrameFence) return;
    if (!lateLatchEnabled()) {
        // 关闭时不等待,但仍回收 fence,避免句柄泄漏(A/B 切换即时生效)。
        glDeleteSync(gPrevFrameFence);
        gPrevFrameFence = nullptr;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    // GL_SYNC_FLUSH_COMMANDS_BIT:确保 fence 已 flush,否则弱驱动下可能永不 signal。
    // 超时 200ms > 单帧 GPU 上界:正常必在此前 signal;超时则放行不挂死。
    glClientWaitSync(gPrevFrameFence, GL_SYNC_FLUSH_COMMANDS_BIT,
                     200ull * 1000ull * 1000ull);
    gFenceWaitMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    glDeleteSync(gPrevFrameFence);
    gPrevFrameFence = nullptr;
}

static void renderFrame() {
    if (!gEngineReady) return;

    const auto frameStart = std::chrono::steady_clock::now();
    static auto previousFrameStart = frameStart;
    const double callbackIntervalMs =
        std::chrono::duration<double, std::milli>(
            frameStart - previousFrameStart).count();
    previousFrameStart = frameStart;

    // 时间步进（实时）
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastTime).count();
    lastTime = now;

    // 环境系统：时间步进，render 中 update() 计算当前帧天空色
    gEngine->advanceTime(dt);
    if (minimal_globe_demo::kEnableVectorDemoLayers) {
        refreshClusterDisplay();
    }
    if (gMvtSource) {
        // P4 MVT 底图驱动(渲染线程契约):地平线视口 + 相机高定 zoom。
        const Ellipsoid& wgs84 = Ellipsoid::WGS84();
        const Cartographic camCarto =
            wgs84.cartesianToCartographic(gEngine->camera().position());
        const Vec3& radii = wgs84.radii();
        const double minRadius =
            std::min(radii.x(), std::min(radii.y(), radii.z()));
        const auto mvtStart = std::chrono::steady_clock::now();
        gMvtSource->update(
            MvtVectorSource::horizonViewRectangle(camCarto, minRadius),
            std::max(1.0, camCarto.height()));
        gFrameMvtMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - mvtStart).count();
        // P6:mvt 超 8ms 就逐帧报构成 —— 尖峰是稀疏事件,周期采样必漏。
        if (gFrameMvtMs >= 8.0) {
            const auto& us = gMvtSource->lastUpdateStats();
            LOGI("MvtSlow %.1fms = ingest %.2f + tree %.2f + dispatch %.2f "
                 "+ commit %.2f | commits=%d drops=%d tess=%d",
                 gFrameMvtMs, us.ingestMs, us.treeMs, us.dispatchMs,
                 us.commitMs, us.commits, us.drops, us.tessellateDispatched);
        }
        // fetch/decode 与 worker 镶嵌各自持有 WorkLedger Landing 票，完成
        // 入箱后释放并唤醒渲染循环；ready commit/retry 则持 Pumped 票。
        // 这里不能再按 pending 每帧 requestRender，否则等待网络也会持续
        // 满帧率渲染，直接抹掉 Landing/Pumped 分治的省帧与交互收益。
        // V27 标注收敛的续帧申报在引擎层(FeatureRenderLayer 的 labelConverge
        // Pumped 票 + Scene::hasConvergingWork ④),app 侧无需置脏。
        static uint64_t mvtLogCounter = 0;
        if (++mvtLogCounter % 120 == 1) {
            // cache 三数是 P2(容量)与 V18(内存有界)的共同判据:
            // refetch 稳态该恒 0(>0 = 容量兜不住工作集,白拉);
            // residentKB 是**实测**常驻字节,别再填"应该很小"。
            const auto cs = gMvtTileCache
                                ? gMvtTileCache->stats()
                                : MvtTileFetchCache::Stats{};
            LOGI("VectorE1 mvt: active=%zu meshes=%zu loaded=%zu pending=%zu "
                 "failed=%zu | cache hit=%llu fetch=%llu refetch=%llu "
                 "resident=%zu/%zuKB raw=%zu/%zuKB rawHit=%llu",
                 gMvtSource->activeTileCount(),
                 gMvtBasemapLayer ? gMvtBasemapLayer->tileMeshCount() : 0,
                 gMvtSource->tree().loadedCount(),
                 gMvtSource->tree().pendingCount(),
                 gMvtSource->tree().failedCount(),
                 static_cast<unsigned long long>(cs.hits),
                 static_cast<unsigned long long>(cs.fetches),
                 static_cast<unsigned long long>(cs.refetches),
                 cs.residentTiles, cs.residentBytes / 1024, cs.rawTiles,
                 cs.rawBytes / 1024,
                 static_cast<unsigned long long>(cs.rawHits));
        }
    }
    // C2/E3:高德矢量几何源驱动(type2 VectorFill + 主源 + POI)。
    if (gAmapRegionsSource || gAmapWater12Source || gAmapMainSource ||
        gAmapPoiSource) {
        amapCleanupCompleted();  // 渲染线程剪除已完成句柄(见该函数注释)
        const Ellipsoid& wgs84 = Ellipsoid::WGS84();
        const Cartographic camCarto =
            wgs84.cartesianToCartographic(gEngine->camera().position());
        const Vec3& radii = wgs84.radii();
        const double minRadius =
            std::min(radii.x(), std::min(radii.y(), radii.z()));
        const Rectangle viewRect =
            MvtVectorSource::horizonViewRectangle(camCarto, minRadius);
        const double camHeight = std::max(1.0, camCarto.height());
        const double amapViewZoom = std::min(
            24.0,
            std::max(0.0, std::log2(4.0e7 / camHeight)));
        if (gAmapRegionsSource) {
            // z10 粗源 LOD 近景让位(与 regions 层 maxZoom=11.5 同口径):
            // zoom > 11.5 时不更新粗源树,不再拉取/镶嵌 z10 面,避免与
            // 主源 z12-14 细面叠加成「破破烂烂」的双层边,也省带宽。
            if (amapViewZoom <= 11.5) {
                gAmapRegionsSource->update(viewRect, camHeight);
            } else {
                gAmapRegionsSource->suspend();
            }
        }
        // z12 粗水层只服务近景：垫在 z14 细块下盖住瓦缝空档。
        // 远景由 regions 的 z3/6/8/10 多档面源承接；固定 z12 若在
        // 全球视野常显，会再次被 256 瓦工作集截成中心孤岛。
        if (gAmapWater12Source && amapViewZoom > 11.5) {
            gAmapWater12Source->update(viewRect, camHeight);
        } else if (gAmapWater12Source) {
            gAmapWater12Source->suspend();
        }
        if (gAmapMainSource) {
            gAmapMainSource->update(viewRect, camHeight);
        }
        if (gAmapPoiSource) {
            gAmapPoiSource->update(viewRect, camHeight);
        }
        // 共享 type1 验收口径：regions/main/water12 必须命中同一
        // typed cache。全球 z3 冷启 fetch 应约为 64，不再是两个
        // profile 各解码一遍。POI(type2) 由独立 pool 统计。
        static uint64_t amapRawLogCounter = 0;
        if (++amapRawLogCounter % 120 == 1) {
            const auto type1 = gAmapRegionCache
                                   ? gAmapRegionCache->stats()
                                   : AmapType1TileCache::Stats{};
            LOGI("AmapType1Cache fetch=%llu refetch=%llu hit=%llu rawHit=%llu "
                 "resident=%zu/%zuKB raw=%zu/%zuKB",
                 static_cast<unsigned long long>(type1.fetches),
                 static_cast<unsigned long long>(type1.refetches),
                 static_cast<unsigned long long>(type1.hits),
                 static_cast<unsigned long long>(type1.rawHits),
                 type1.residentTiles, type1.residentBytes / 1024,
                 type1.rawTiles, type1.rawBytes / 1024);
            auto logPool = [](const char* name,
                              const std::shared_ptr<ThreadPool>& pool) {
                if (!pool) return;
                const auto s = pool->stats();
                const double avgQueue =
                    s.started ? s.totalQueueWaitMs / s.started : 0.0;
                const double avgWork =
                    s.completed ? s.totalWorkMs / s.completed : 0.0;
                LOGI("MvtPool %s threads=%zu queued=%llu active=%llu "
                     "done=%llu queueAvg=%.2f queueMax=%.2f "
                     "workAvg=%.2f workMax=%.2f",
                     name, pool->threadCount(),
                     static_cast<unsigned long long>(s.queued),
                     static_cast<unsigned long long>(s.active),
                     static_cast<unsigned long long>(s.completed), avgQueue,
                     s.maxQueueWaitMs, avgWork, s.maxWorkMs);
            };
            logPool("type1Decode", gMvtDecodePool);
            logPool("poiDecode", gAmapPoiDecodePool);
            logPool("tess", gMvtTessellationPool);
            auto logSource = [](const char* name, const auto* source) {
                if (!source) return;
                const auto& s = source->lastUpdateStats();
                LOGI("AmapSource %s z=%d desired=%lld scanned=%zu render=%zu "
                     "request=%zu pending=%zu tess=%zu ready=%zu active=%zu "
                     "pairs=%zu tree=%.2f commit=%.2f",
                     name, s.selectedZoom,
                     static_cast<long long>(s.desiredTileCount),
                     s.scannedTileCount, s.renderTileCount,
                     s.requestTileCount, s.pendingTileCount,
                     s.tessellatingTileCount, s.readyTileCount,
                     s.activeTileCount, s.activeAncestorPairs, s.treeMs,
                     s.commitMs);
            };
            logSource("regions", gAmapRegionsSource.get());
            logSource("water12", gAmapWater12Source.get());
            logSource("main", gAmapMainSource.get());
            logSource("poi", gAmapPoiSource.get());
        }
    }
    // 阶段 4:假载体在**引擎 update 之前**推进,这样本帧 tether 读到的就是新
    // 位置 —— 放到 render 之后会让相机永远跟着上一帧的载体,表现为恒定滞后,
    // 而画面上看着只是"跟得有点松"。
    gCarrier.step(1.0 / 60.0);

    // P6 分段:`total − engine − post − swap` 这段宿主前奏此前**没有任何
    // 字段覆盖**,于是"慢帧 187ms 而 engine 只有 2-3ms"只能停在
    // 「时间不在引擎」而无法再往下定位。pre 覆盖整段,mvt 单列最大嫌疑。
    const auto engineStart = std::chrono::steady_clock::now();
    const double preMs = std::chrono::duration<double, std::milli>(
        engineStart - frameStart).count();
    const bool presented =
        gEngine->render(0.0);  // auto-delta（内部 update；必要时 beginFrame→render→endFrame）
    const double engineMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - engineStart).count();

    // C-V8 late-latch:scene draw 提交后打栅栏,下一帧顶部 waitPrevFrameFenceForLatch
    // 等它 → render-ahead≤1。仅真出帧时打;旧栅栏理应已在本帧顶部回收,防御性再清。
    if (presented) {
        if (gPrevFrameFence) glDeleteSync(gPrevFrameFence);
        gPrevFrameFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    const auto postEngineStart = std::chrono::steady_clock::now();
    gHeadingRadians = static_cast<float>(gEngine->cameraHeadingRadians());
    const double postEngineMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - postEngineStart).count();

    double swapMs = 0.0;
    EGLBoolean swapOk = EGL_TRUE;
    if (presented) {
        const auto swapStart = std::chrono::steady_clock::now();
        swapOk = eglSwapBuffers(gDisplay, gSurface);
        swapMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - swapStart).count();
        if (swapOk == EGL_TRUE) {
            ++gFrameCount;
        }
    }

    const double frameTotalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - frameStart).count();
    // 回报本帧 CPU 工作耗时。**刻意不含 swapMs** —— eglSwapBuffers 里绝大部分是
    // 等 vsync 的空转,算进去会让系统以为我们每帧都刚好用满预算,反而不提速。
    gRenderThreadPlacement.reportActualWorkDurationMs(engineMs + postEngineMs);
    const uint64_t frameId = gEngine->presentationTrace().camera.frameId;
    // P5c 标签避让诊断(节流):cand=候选 placed=显示 col=碰撞落选
    // horiz=地平线剔除 proj=视锥外/相机背后。
    // P3 水位:图集满 = 永久丢字(无淘汰),必须能看见逼近过程。
    if (frameId % 600 == 0) {
        const GlyphAtlas* ga = gEngine ? gEngine->renderer()->glyphAtlas()
                                       : nullptr;
        if (ga) {
            LOGI("GlyphAtlas glyphs=%zu shelf=%d/%d (%.0f%%) drops=%d",
                 ga->residentGlyphCount(), ga->shelfUsedHeightPx(),
                 GlyphAtlas::kAtlasSize,
                 100.0 * ga->shelfUsedHeightPx() / GlyphAtlas::kAtlasSize,
                 ga->atlasFullDropCount());
        }
    }
    // P4 曲线采样:候选数 vs 全量 placement 耗时(哨兵只报 >4ms 的点)
    if (gMvtBasemapLayer && frameId % 120 == 0) {
        const size_t cand = gMvtBasemapLayer->lastPlacementCandidates();
        if (cand > 0) {
            LOGI("PlaceCurve cand=%zu ms=%.3f", cand,
                 gMvtBasemapLayer->lastPlacementMs());
        }
    }
    // 七态标注 dump 按需触发(免重编译诊断口):
    //   adb shell setprop debug.ee.labeldump <值> && adb shell input tap 620 900
    // 值变化才触发一次(app 清不掉系统属性,记上次值去重;重触发换个值)。
    // 纯数字值或 "all" = 全量(去重要求每次换新值,固定 token 两次就用完
    // ——实测踩过,故数字序列 1/2/3… 都算全量);其余值当作标注名过滤
    // 子串(可给 UTF-8 中文名)。
    // 逐帧轮询而非 %N 节流:按需渲染下 tap 只给短帧串,跨不过 N 边界就
    // 永不触发(实测踩过);__system_property_get 是共享内存读,亚微秒。
    // 仍需至少一帧才会读到(idle 全停时先 tap 顶帧)——诊断口按帧驱动是
    // 故意的:dump 读桶表必须在渲染线程。
    {
        static std::string lastLabelDumpProp;
        char prop[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.ee.labeldump", prop);
        if (prop[0] != '\0' && lastLabelDumpProp != prop) {
            lastLabelDumpProp = prop;
            const bool allDigits =
                lastLabelDumpProp.find_first_not_of("0123456789") ==
                std::string::npos;
            const std::string filter =
                (allDigits || lastLabelDumpProp == "all")
                    ? std::string()
                    : lastLabelDumpProp;
            for (FeatureRenderLayer* layer :
                 {gMvtBasemapLayer, gAmapRegionsLayer, gAmapWater12Layer,
                  gAmapMainLayer, gAmapPoiLayer, gDemoFeatureLayer}) {
                if (!layer) continue;
                // 逐行打(logcat 单条 ~4KB 截断,整段一条会被吞尾)。
                std::istringstream ss(layer->dumpLabelLifecycle(filter));
                std::string line;
                while (std::getline(ss, line)) LOGI("%s", line.c_str());
            }
        }
    }
    if (gDemoFeatureLayer && frameId % 120 == 0) {
        const auto& ls = gDemoFeatureLayer->labelPlacementStats();
        if (ls.candidates > 0) {
            LOGI("LabelPlace frame=%llu cand=%d placed=%d col=%d horiz=%d "
                 "proj=%d",
                 static_cast<unsigned long long>(frameId), ls.candidates,
                 ls.placed, ls.collided, ls.culledHorizon,
                 ls.culledProjection);
        }
    }
    char perflogProp[4] = {0};
    __system_property_get("debug.ee.perflog", perflogProp);
    const bool perFrameLog = perflogProp[0] == '1';
    // 采样模式:perflog=4 → 每 4 帧打一行(swap 分布用,~15 行/秒,不触发
    // 设备 logcat 配额);perflog=1 → 只打可疑帧。2026-08-21 GPU swap 专项。
    const int beatN = perflogProp[0] == '4' ? 4
                     : perflogProp[0] == '8' ? 8
                                            : 0;
    // 逐帧模式只打"可疑帧":帧间隔/swap/latch 超阈或慢帧 —— 避免全量刷屏
    // 触发设备 logcat 配额丢日志(2026-08-20 实测 DROPPED 把暂停帧吞掉)。
    const bool suspiciousFrame =
        callbackIntervalMs >= 40.0 || swapMs >= 8.0 ||
        gFenceWaitMs >= 40.0 || frameTotalMs >= 25.0;
    const bool logFrame =
        frameId <= 3 || frameId % 120 == 0 ||
        frameTotalMs >= 25.0 || swapMs >= 8.0 ||
        (perFrameLog && suspiciousFrame) ||
        (beatN > 0 && frameId % beatN == 0);
    if (logFrame) {
        const auto& stageDiag = gEngine->diagnostics();
        LOGI(
            "FrameLoop frame=%llu total=%.3f pre=%.3f mvt=%.3f engine=%.3f "
            "post=%.3f swap=%.3f latch=%.2f callback=%.3f cpu=%d hint=%d presented=%d swapOk=%d "
            "upd=%.2f build=%.2f submit=%.2f terrUpd=%.2f "
            "cam=%.2f env=%.2f base=%.2f srender=%.2f endf=%.2f",
            static_cast<unsigned long long>(frameId),
            frameTotalMs,
            preMs,
            gFrameMvtMs,
            engineMs,
            postEngineMs,
            swapMs,
            // C-V8 late-latch 门控等待:≈单帧 GPU 时长 → render-ahead 曾≥2、方案成立;
            // ≈0 → 深度已是 1、这条无收益(该转预测)。engine 会相应从阻塞变纯 CPU。
            gFenceWaitMs,
            callbackIntervalMs,
            // 渲染线程当前所在核心。这条线程是裸 std::thread(无优先级/无亲和/
            // 无 ADPF 提示),Android 不知道它有显示截止期,实测 ~91% 的帧被放在
            // 小核簇(cpu0-3),同样的活 ~8ms 涨到 ~21ms → 错过 16.67ms 预算掉到
            // 30fps。判"卡"先看这个字段,不要先怀疑引擎做多了活。
            sched_getcpu(),
            gRenderThreadPlacement.status().mode ==
                    RenderThreadPlacement::Mode::PerformanceHint
                ? 1
                : 0,
            presented ? 1 : 0,
            swapOk == EGL_TRUE ? 1 : 0,
            stageDiag.sceneUpdateMs,
            stageDiag.renderCommandBuildMs,
            stageDiag.renderSubmitMs,
            stageDiag.terrainUpdateMs,
            stageDiag.cameraUpdateMs,
            stageDiag.environmentUpdateMs,
            stageDiag.basemapStackUpdateMs,
            stageDiag.sceneRenderMs,
            stageDiag.engineEndFrameMs);
        // 北极星 Phase 0 测量台:每帧(采样)打相机真实位姿,消除"nadir/oblique"
        // 猜测——用它标注每个 measure stop 的实际视角。
        const auto& camTrace = gEngine->presentationTrace().camera;
        const Cartographic camPos =
            Ellipsoid::WGS84().cartesianToCartographic(
                gEngine->camera().position());
        LOGI("CamPose frame=%llu center=%.5f,%.5f camH=%.1f targetH=%.1f "
             "pitchDeg=%.2f headingDeg=%.2f camPos=%.5f,%.5f,%.1f",
             static_cast<unsigned long long>(frameId),
             camTrace.targetLongitudeDegrees,
             camTrace.targetLatitudeDegrees,
             camTrace.cameraHeightMeters,
             camTrace.targetHeightMeters,
             camTrace.pitchRadians * 180.0 / M_PI,
             camTrace.headingRadians * 180.0 / M_PI,
             camPos.longitude() * 180.0 / M_PI,
             camPos.latitude() * 180.0 / M_PI,
             camPos.height());
    }

    // ---- 阶段 3/4/5 机制信号 ----
    // 与 CamPose 分开、且**不受 frameId%120 采样限制**:飞行只有 2~3 秒,
    // 按 120 帧采样会整段漏掉。
    if (gEngine) {
        CameraSystem& cam = gEngine->cameraSystem();
        if (gFlightProbe.armed) {
            ++gFlightProbe.frames;
            gFlightProbe.maxProgress =
                std::max(gFlightProbe.maxProgress, cam.cameraFlightProgress());
            const double agl = gEngine->camera().getHeight() -
                               cam.groundState().terrainHeightMeters;
            if (gFlightProbe.frames % 5 == 0 || !cam.cameraFlightActive()) {
                LOGI("StageFlight n=%d t=%.3f agl=%.0f clamps=%llu "
                     "active=%d selfAnim=%d",
                     gFlightProbe.frames, cam.cameraFlightProgress(), agl,
                     static_cast<unsigned long long>(
                         cam.constraintClampCount() -
                         gFlightProbe.clampsAtStart),
                     cam.cameraFlightActive() ? 1 : 0,
                     cam.isSelfAnimating() ? 1 : 0);
            }
            if (!cam.cameraFlightActive()) {
                const auto geo = Ellipsoid::WGS84().cartesianToCartographic(
                    gEngine->camera().position());
                LOGI("StageFlight DONE frames=%d maxT=%.3f clamps=%llu "
                     "landed=%.5f,%.5f,%.0f ctrl=%s",
                     gFlightProbe.frames, gFlightProbe.maxProgress,
                     static_cast<unsigned long long>(
                         cam.constraintClampCount() -
                         gFlightProbe.clampsAtStart),
                     geo.longitudeDegrees(), geo.latitudeDegrees(),
                     geo.height(), cam.activeControllerName().c_str());
                // 阶段 3 第四条判据的**落地帧就绪快照**,原子单行 —— 逐帧
                // LoadQual 会被 logd 冲掉(demo 每帧 CamPose/LoadQual/LoadGate
                // 洪泛),而这一行一次性、且是 dump 前最新,不会丢。就绪率定义:
                //   R_t = real/(real+fill+ell+unk)  地形真数据占比
                //   R_i = sharp/(sharp+a1+a2+a3++miss) 影像本级占比
                // 稳态基线从落地后 settle 的 LoadQual 心跳读(同位姿同缓存温度)。
                const auto& fq = gEngine->diagnostics();
                LOGI("FlightReady LANDING vis=%d src=%d/%d/%d/%d "
                     "img=%d/%d/%d/%d/%d",
                     fq.visibleTiles,
                     fq.terrainSurfaceRealCommands,
                     fq.terrainSurfaceFillProxyCommands,
                     fq.terrainSurfaceEllipsoidCommands,
                     fq.terrainSurfaceUnknownCommands,
                     fq.imageryExactAttachments,
                     fq.imageryAncestor1Attachments,
                     fq.imageryAncestor2Attachments,
                     fq.imageryAncestor3PlusAttachments,
                     fq.imageryMissingTiles);
                gFlightProbe.armed = false;
            }
        }
        if (gCarrier.active && frameId % 30 == 0) {
            const auto& t = cam.tetheredController();
            // ⚠️ 必须用 glm::length():`glm::dvec3::length()` 返回的是**分量
            // 个数(恒 3)**,不是模长。第一版就写成了成员版,读数恒 3.0 —— 而
            // range=1500,看着像"相机贴在载体上"的引擎 bug,实际引擎完全正确。
            const double distToCarrier = glm::length(
                gEngine->camera().position().raw() - gCarrier.position);
            LOGI("StageTether h=%.4f p=%.4f r=%.4f range=%.1f dist=%.1f "
                 "resolved=%d selfAnim=%d ctrl=%s",
                 t.localHeading(), t.localPitch(), t.localRoll(), t.range(),
                 distToCarrier, t.frameResolved() ? 1 : 0,
                 cam.isSelfAnimating() ? 1 : 0,
                 cam.activeControllerName().c_str());
        }
        if (gOrthographic && frameId % 120 == 0) {
            LOGI("StageOrtho isOrtho=%d widthM=%.0f near=%.1f",
                 gEngine->camera().isOrthographic() ? 1 : 0,
                 gEngine->camera().orthographicWidthMeters(),
                 gEngine->camera().nearPlaneMeters());
        }
    }

    // 加载体验记分卡:把"糊/露底/台阶"这些观感症状翻成可 A/B 的计数,免去
    // 靠录屏和主观描述定位。采样策略与 FrameLoop 不同——**暂态期逐帧打、
    // 稳态期心跳打**:糊块/露底只在加载暂态出现,120 帧心跳会整段错过。
    //   sharp/a1/a2/a3+/miss  = 底图「糊几级」直方图:贴本级 / 退回祖先差
    //                           1、2、3+ 级上采样 / 地形瓦片压根没影像。
    //                           a*+miss>0 即"屏幕上有糊块或空块"。
    //   src=real/fill/ell/unk = 地形几何来源 → fill/ell>0 即"露代理面或裸椭球"
    //   z / texZ              = 可见几何 LOD 跨度 / 实际贴上的影像层跨度
    const auto& q = gEngine->diagnostics();
    const bool loadDirty = (q.imageryParentFallbackAttachments > 0 ||
                            q.imageryMissingTiles > 0 ||
                            q.terrainSurfaceFillProxyCommands > 0 ||
                            q.terrainSurfaceEllipsoidCommands > 0);
    static bool sLoadDirtyPrev = false;
    // 暂态期逐帧 + 刚回到干净的那一帧(记 settle 落点)+ 稳态心跳
    if (loadDirty || sLoadDirtyPrev || frameId % 120 == 0) {
        LOGI("LoadQual frame=%llu vis=%d sharp=%d a1=%d a2=%d a3+=%d miss=%d "
             "src=%d/%d/%d/%d geoZ=%d-%d texZ=%d-%d z=%d-%d dirty=%d",
             static_cast<unsigned long long>(frameId),
             q.visibleTiles,
             q.imageryExactAttachments,
             q.imageryAncestor1Attachments,
             q.imageryAncestor2Attachments,
             q.imageryAncestor3PlusAttachments,
             q.imageryMissingTiles,
             q.terrainSurfaceRealCommands,
             q.terrainSurfaceFillProxyCommands,
             q.terrainSurfaceEllipsoidCommands,
             q.terrainSurfaceUnknownCommands,
             q.imageryMinTargetZoom, q.imageryMaxTargetZoom,
             q.imageryMinTextureZoom, q.imageryMaxTextureZoom,
             q.minVisibleZoom, q.maxVisibleZoom,
             loadDirty ? 1 : 0);
        // LoadQual 回答"屏幕上糊不糊",回答不了"为什么还没好"。这条补上收敛
        // 速率的那一半:**每帧闸门是否打满**。判据(见诊断文档§二)——
        //   fin/rasUp/ms 逐帧顶到上限 → 瓶颈在每帧提交闸门(候选 2 成立);
        //   长期不满而 pend/net 有积压 → 瓶颈在网络或 mapping,方向完全不同。
        //   fin    = 主线程地形 finalize 次数/上限
        //   rasUp  = 影像上传单元数/上限
        //   ms     = 主线程加载耗时/预算(demo 配 4.0ms)
        //   pend   = 地形 请求/上传/终态 待处理
        //   net    = 地形 起/完 · 影像 起/完(累计计数,看斜率)
        //   inflt  = 地形/影像 worker 在途 · 传输层上限
        //   prog   = frameLoadProgressPercentage
        LOGI("LoadGate frame=%llu fin=%d/%d rasUp=%d/%d ms=%.2f/%.2f "
             "pend=%d/%d/%d net=%d/%d·%d/%d inflt=%d/%d·%d/%d prog=%.1f "
             "mode=%c%c",
             static_cast<unsigned long long>(frameId),
             q.budgetMainThreadFinalizesUsed, q.budgetMainThreadFinalizesLimit,
             q.budgetRasterUploadsUsed, q.budgetRasterUploadsLimit,
             q.budgetMainThreadElapsedMs, q.budgetMainThreadTimeLimitMs,
             q.pendingTerrainRequests, q.pendingGltfTerrainUploads,
             q.pendingGltfTerrainTerminalResults,
             q.terrainProviderRequestsStarted,
             q.terrainProviderRequestsCompleted,
             q.rasterProviderRequestsStarted,
             q.rasterProviderRequestsCompleted,
             q.terrainProviderActiveWorkerBlockingRequests,
             q.terrainTransportActiveRequestLimit,
             q.rasterProviderActiveWorkerBlockingRequests,
             q.rasterTransportActiveRequestLimit,
             q.frameLoadProgressPercentage,
             q.budgetInteractionActive ? 'I' : '-',
             q.budgetSmoothingActive ? 'S' : '-');
    }
    sLoadDirtyPrev = loadDirty;

    // 破洞诊断(假设 A:选中却零绘制 → 屏幕上这块本帧没有任何几何,看到的是
    // 天空/大气而不是地面)。LoadQual 只回答"糊不糊/是不是代理面",回答不了
    // "有没有一块地压根没画"——这条补上那个缺口。
    //   sel/ent   = 选择器要渲染的瓦片数 / 实际拿到 render entry 的条目数
    //               (sel 明显大于 ent = 有瓦片连 entry 都没有 → 必然是洞)
    //   miss      = entry 走完 draw 却零命令(= 下面三个桶之和)
    //   nofill    = 既无真几何也无 fill 兜底 ← A 的头号嫌疑
    //   fillnc/ctnc = 有 fill / 有真几何,但 draw builder 没产出命令
    //   nulls     = entry 的 selectedTile / renderTile 指针为空
    //   defer     = 本帧主动跳过同步 prep(也不出现,但属预期节流)
    // ⚠ sel>ent 本身**不是**洞:一个祖先 entry 可覆盖多个选中瓦片(finalizer
    // dedup)。真的没几何的只有 finalizer 的两条丢弃路径(dropcu/dropnb)。
    const int holeCount = q.terrainRenderEntriesMissed +
                          q.terrainRenderEntriesMissingSelected +
                          q.terrainRenderEntriesMissingRender +
                          q.terrainRenderEntryDropClipUv +
                          q.terrainRenderEntryDropNotBuildable;
    static bool sHolePrev = false;
    const bool holeDirty = holeCount > 0;
    // 破洞只在加载暂态出现,120 帧心跳会整段错过:暂态期(loadDirty)逐帧打。
    // 裁剪回退活跃期(clip>0)同样逐帧打——接缝细缝与它的相关性要逐帧对齐。
    if (holeDirty || sHolePrev || loadDirty ||
        q.terrainRenderEntriesAncestorFallback > 0 || frameId % 120 == 0) {
        // dropwhy = 几何就没有 / 没建 mapping / 建了但无可用纹理(含祖先)/
        //           texcoord 越界 / 其它;dropz = 被丢瓦片的 zoom 跨度。
        //           nomap 占多 = 时序问题;notex 占多 = 真缺常驻粗影像。
        // clip = 走「祖先裁剪回退」的 entry 数(切缝无裙墙,是运动期瓦片
        // 边界天色细缝的头号嫌疑,与截图逐帧对齐用)。
        LOGI("HoleQual frame=%llu sel=%d ent=%d clip=%d drop=%d/%d "
             "dropwhy=%d/%d/%d/%d/%d dropz=%d-%d miss=%d nofill=%d "
             "fillnc=%d ctnc=%d nulls=%d/%d defer=%d drawn=%d "
             "fade=%d fade0=%d opmin=%.3f clipdeg=%d "
             "hlFull=%d hlDenseRej=%d hlEvict=%d hlEpochMiss=%d hlGridMiss=%d "
             "remap=%d plainClip=%d spanMis=%d spanKey=%d/%d/%d spanR=%.3f/%.3f "
             "hlRes=%d/%d hlDense=%d/%d "
             "dark=%.4f dirty=%d",
             static_cast<unsigned long long>(frameId),
             q.terrainSelectedForRenderTiles,
             q.terrainRenderEntriesPlanned,
             q.terrainRenderEntriesAncestorFallback,
             q.terrainRenderEntryDropClipUv,
             q.terrainRenderEntryDropNotBuildable,
             q.terrainRenderEntryDropNoGeometry,
             q.terrainRenderEntryDropNoMapping,
             q.terrainRenderEntryDropNoReadyTexture,
             q.terrainRenderEntryDropTexcoordInvalid,
             q.terrainRenderEntryDropOther,
             q.terrainRenderEntryDropMinZoom,
             q.terrainRenderEntryDropMaxZoom,
             q.terrainRenderEntriesMissed,
             q.terrainZeroDrawNoContentNoFill,
             q.terrainZeroDrawFillNoCommands,
             q.terrainZeroDrawContentNoCommands,
             q.terrainRenderEntriesMissingSelected,
             q.terrainRenderEntriesMissingRender,
             q.terrainRenderEntriesDeferred,
             q.terrainRenderEntriesDrawn,
             q.terrainRenderEntriesFaded,
             q.terrainRenderEntriesFullyTransparent,
             static_cast<double>(q.terrainRenderEntryMinOpacity),
             q.terrainRenderEntriesClipDegenerate,
             q.terrainHeightLayerFull,
             q.terrainHeightDenseRejected,
             q.terrainHeightEvicted,
             q.terrainHeightEpochMiss,
             q.terrainHeightGridMiss,
             q.terrainSurfaceClipRemap,
             q.terrainSurfaceClipPlain,
             q.terrainTemplateSpanMismatch,
             q.terrainTemplateMismatchZ,
             q.terrainTemplateMismatchX,
             q.terrainTemplateMismatchY,
             static_cast<double>(q.terrainTemplateMismatchLatRatio),
             static_cast<double>(q.terrainTemplateMismatchLonRatio),
             q.terrainHeightCoarseResident,
             q.terrainHeightCoarseCapacity,
             q.terrainHeightDenseResident,
             q.terrainHeightDenseCapacity,
             gEngine->lastFrameDarkFraction(),
             holeDirty ? 1 : 0);
        // notex 细分:z=被丢瓦片层级 load/ready=该 mapping 两个 RasterOverlayTile
        // 的 LoadState(-1=空) tex=ready 手上有没有纹理
        // anc=祖先链深度/其中建了 mapping 的/其中能拿出可画纹理的。
        //   anc=0/*/*     → 这片是根,没祖先可借
        //   anc=N/0/0     → 祖先在但从没建过 mapping
        //   anc=N/M>0/0   → mapping 在、纹理没了(淘汰或没上传)← 淘汰假说
        if (q.terrainRenderEntryDropNoTexZoom >= 0) {
            LOGI("HoleNoTex frame=%llu z=%d load=%d ready=%d tex=%d "
                 "anc=%d/%d/%d mstate=%d upd=%llu tload=%d tkind=%d",
                 static_cast<unsigned long long>(frameId),
                 q.terrainRenderEntryDropNoTexZoom,
                 q.terrainRenderEntryDropNoTexLoadingState,
                 q.terrainRenderEntryDropNoTexReadyState,
                 q.terrainRenderEntryDropNoTexReadyHasTexture,
                 q.terrainRenderEntryDropNoTexAncestorDepth,
                 q.terrainRenderEntryDropNoTexAncestorsWithMapping,
                 q.terrainRenderEntryDropNoTexAncestorsWithTexture,
                 q.terrainRenderEntryDropNoTexMappingState,
                 q.terrainRenderEntryDropNoTexAuthoritativeUpdates,
                 q.terrainRenderEntryDropNoTexTileLoadState,
                 q.terrainRenderEntryDropNoTexTileContentKind);
        }
    }
    sHolePrev = holeDirty;
}

// ============================================================
// 渲染线程：EGL + Engine 全部归本线程所有，帧节拍来自 AChoreographer。
// UI 线程只做事件整形，经任务队列投递到本线程执行；引擎与 GPU 资源
// 在线程退出前、EGL context 仍有效时销毁。
// ============================================================

class RenderThread {
public:
    void start(ANativeWindow* window) {
        // SurfaceView can deliver surfaceCreated again before a matching
        // surfaceDestroyed. A joinable std::thread cannot be overwritten, and
        // the old EGL / engine lifetime must finish before a new one begins.
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.clear();  // 丢弃上一轮 surface 生命周期遗留的任务
        }
        running_.store(true);
        paused_.store(false);
        thread_ = std::thread([this, window]() { threadMain(window); });
    }

    /// 停止并 join。线程内先销毁引擎（需有效 context）再拆 EGL。
    void stop() {
        running_.store(false);
        wake();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /// 投递任务并**置引擎脏位**。
    ///
    /// 脏位置在这里而不是逐个调用点上,是整个 gating 设计的关键:任务队列是
    /// UI 线程改变引擎状态的**唯一**通道(输入、surface 变更、demo 各按钮、
    /// 图层增删),所以"有任务进来"与"有事发生"等价。逐站点加 requestRender
    /// 有 16 个调用点,漏一个的症状是"这个按钮按了没反应",而且只在 gating
    /// 开启时才复现 —— 对齐 maplibre 把事件型脏位收成单入口 `_update()` 的
    /// 理由:枚举脏源应该是"审一个函数",不是"审整个代码库"。
    void post(std::function<void()> task) {
        postInternal(std::move(task), /*markDirty=*/true);
    }

    /// 投递任务并等待其在渲染线程执行完（诊断读取等需要返回值的场景）。
    /// 超时返回 false。任务捕获必须按值 / shared_ptr——超时后任务仍可能
    /// 被执行，引用捕获会悬垂。
    bool runSync(std::function<void()> task, std::chrono::milliseconds timeout) {
        if (!running_.load()) return false;
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        // **不置脏位**:runSync 是只读查询通道(调试面板轮询诊断字符串)。
        // 把它算成"有事发生",面板每秒一问就等于永不空闲 —— 测量台自己
        // 把被测对象顶住了。
        postInternal([done, task = std::move(task)]() {
            task();
            done->set_value();
        }, /*markDirty=*/false);
        return future.wait_for(timeout) == std::future_status::ready;
    }

    void setPaused(bool paused) {
        paused_.store(paused);
        if (!paused) {
            // AChoreographer 绑定注册线程，恢复帧回调必须投递过去做
            post([this]() { postFrameIfNeeded(); });
        }
    }

private:
    void postInternal(std::function<void()> task, bool markDirty) {
        if (!running_.load()) return;  // 线程未运行时任务直接丢弃
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        if (markDirty && gEngine) {
            gEngine->requestRender("task");
        }
        wake();
    }

    void wake() {
        // 持锁 wake：TLS looper 在渲染线程退出时释放，线程退出前在同一把
        // 锁下置空 looper_，保证 wake 期间目标存活（否则跨线程 UAF）
        std::lock_guard<std::mutex> lock(looperMutex_);
        if (looper_) {
            ALooper_wake(looper_);
        }
    }

    static void frameCallbackThunk(long /*frameTimeNanos*/, void* data) {
        static_cast<RenderThread*>(data)->onFrame();
    }

    // 仅渲染线程调用
    void postFrameIfNeeded() {
        if (!running_.load() || paused_.load() || framePending_) return;
        if (!choreographer_) return;
        AChoreographer_postFrameCallback(choreographer_, &frameCallbackThunk, this);
        framePending_ = true;
    }

    void onFrame() {
        framePending_ = false;
        if (!running_.load() || paused_.load()) return;
        applyShadowVerifyRuntimeSwitch();
        // C-V8 late-latch:先等上一帧 GPU 完成(render-ahead≤1),再排输入,
        // 使 latch 到的指位尽量新鲜。等待被从驱动内 draw 提交处挪到此处。
        // ⚠️ 2026-08-20 输入门控:只有本帧确有输入要 latch 才等;惯性/无输入
        // 帧直接回收 fence 不等待,CPU 与 GPU 重叠保帧率(PHK110 实测无条件
        // 等待 30fps,门控后 60fps)。
        if (gInputPending.exchange(false, std::memory_order_acq_rel)) {
            waitPrevFrameFenceForLatch();
        } else {
            if (gPrevFrameFence) {
                glDeleteSync(gPrevFrameFence);
                gPrevFrameFence = nullptr;
            }
            gFenceWaitMs = 0.0;
        }
        drainTasks();   // 输入先于渲染，保证事件同帧生效
        renderFrame();
        // needsFrame() 不是纯查询：会 exchange 事件脏位、消费 landed pulse
        // 并推进 settle 计数。统一留给 ALooper 返回后的 threadMain 调一次，
        // 否则每个逻辑帧会重复消费状态并重复执行 WorkLedger 审计。
    }

    void drainTasks() {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks.swap(tasks_);
        }
        for (auto& task : tasks) {
            task();
        }
    }

    void threadMain(ANativeWindow* window) {
        // 上一轮线程可能带着未消费的帧回调退出（flag 留 true），回调已随
        // 旧线程死亡，不复位则本轮 postFrameIfNeeded 永远早退 → 永久冻屏
        framePending_ = false;
        {
            std::lock_guard<std::mutex> lock(looperMutex_);
            looper_ = ALooper_prepare(0);
        }
        // 渲染线程放置策略。必须在本线程内调用 —— ADPF 登记的是 gettid()。
        // 默认 Options 就是这里要的(60Hz 目标 16.6ms / urgent-display nice)。
        const auto placement = gRenderThreadPlacement.applyToCurrentThread();
        LOGI("RenderThreadPlacement mode=%s pinnedCores=%d nice=%d",
             RenderThreadPlacement::modeName(placement.mode),
             placement.pinnedCoreCount,
             placement.threadNice);
        if (!initEGL(window)) {
            LOGE("Failed to initialize EGL on render thread");
        } else if (!createEngine()) {
            LOGE("Failed to create Engine on render thread");
        }
        choreographer_ = AChoreographer_getInstance();
        // Phase B 平台级唤醒钩子(§0):WorkLedger Landing 令牌在 worker/网络线程
        // 释放时,踹醒停在 ALooper_pollOnce(-1) 的渲染线程去消费落地产物。没有
        // 它,ledger gating 下 Landing 挂着会真睡且再也醒不过来。wake() 跨线程安全
        // (looperMutex_ 保护);Engine 析构时清除该回调(见 Engine::~Engine)。
        if (gEngine) {
            gEngine->setFrameRequestCallback([this]() { wake(); });
        }
        postFrameIfNeeded();

        while (running_.load()) {
            int events = 0;
            void* data = nullptr;
            // 帧回调在 pollOnce 内部分发；post()/stop() 经 ALooper_wake 唤醒
            ALooper_pollOnce(-1, nullptr, &events, &data);
            drainTasks();
            // gating 开启后线程会停在上面那句 pollOnce 上,帧回调不再自续。
            // 任务(输入事件等)只把脏位置上,真正把循环重新拉起来的是这里 ——
            // 漏了它,输入进得来但画面不动。这也是每轮 Looper 唯一一次
            // needsFrame() 判定，确保事件型状态恰消费一次。
            if ((!gEngine || !gEngine->frameGatingEnabled() ||
                 gEngine->needsFrame())) {
                postFrameIfNeeded();
            }
        }

        drainTasks();
        // 与 applyToCurrentThread 配对:ADPF session 绑的是本线程 tid,线程退出前
        // 关掉。
        gRenderThreadPlacement.release();
        // destroyEGL 内部先清引擎对象（GPU 资源析构需当前 context），再拆 EGL
        destroyEGL();
        choreographer_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(looperMutex_);
            looper_ = nullptr;
        }
    }

    std::thread thread_;
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::mutex looperMutex_;
    ALooper* looper_ = nullptr;  // looperMutex_ 保护；渲染线程退出前置空
    AChoreographer* choreographer_ = nullptr;  // 仅渲染线程访问
    bool framePending_ = false;                // 仅渲染线程访问
};

static RenderThread gRenderThread;

// ---- C2 步骤5:高德矢量瓦片垂直切片 ----
// 独立线程 + 阻塞请求(getBlocking 是影像/地形已验证路径;异步链在本机
// 调度器上未触发,先绕开):版本探测(GET)→ get_tile(POST)→ 签名 URL(GET)
// → 解码/转换(工作线程,纯 CPU)→ gRenderThread.post 灌 FeatureStore。
static std::vector<uint8_t> amapPostBlocking(
    const std::string& url, const std::string& body,
    const std::string& contentType, HttpRequestOptions opts,
    int timeoutMs = 20000) {
    struct St {
        std::vector<uint8_t> result;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto st = std::make_shared<St>();
    auto req = CurlMultiRequestScheduler::shared().post(
        url, std::vector<uint8_t>(body.begin(), body.end()), contentType,
        [st](int code, std::vector<uint8_t> b) {
            {
                std::lock_guard<std::mutex> lk(st->mutex);
                if (code == 200) st->result = std::move(b);
                st->done = true;
            }
            st->cv.notify_one();
        },
        opts);
    std::unique_lock<std::mutex> lk(st->mutex);
    if (!st->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                         [&] { return st->done; })) {
        if (req) req->cancel();
        return {};
    }
    return std::move(st->result);
}

// regionsOnly=true:只保留 type2(面),用于粗源(z10,水/绿地,低顶点量,
// overzoom 对齐 amapVectorLayers 的粗源);false:只保留 type1/3/4(主源 z14)。
static void amapLoadDemoTile(FeatureRenderLayer* layer, int x, int y, int z,
                             bool regionsOnly) {
    LOGI("AmapDemo: enqueue %d_%d_%d", x, y, z);
    std::thread([layer, x, y, z, regionsOnly]() {
        const std::string key = minimal_globe_demo::kAmapWebKey;
        const std::string referer = minimal_globe_demo::kAmapReferer;
        const std::string initUrl =
            "https://jsapi.amap.com/web/init?key=" + key;
        HttpRequestOptions opts;
        opts.headers = {{"Referer", referer}};
        const auto initBody = CurlMultiRequestScheduler::shared().getBlocking(
            initUrl, opts);
        {
            std::string version;
            try {
                const auto doc = nlohmann::json::parse(initBody.begin(),
                                                       initBody.end());
                const auto inner =
                    nlohmann::json::parse(doc.value("tile", "{}"));
                version = inner.value("v", "");
            } catch (const std::exception&) {
                LOGE("AmapDemo: version probe failed");
                return;
            }
            if (version.empty()) {
                LOGE("AmapDemo: empty version stamp");
                return;
            }
            AmapManifestConfig cfg;
            cfg.key = key;
            cfg.referer = referer;
            cfg.version = version;
            const std::vector<AmapTileRequest> reqs = {{x, y, z, 1}};
            const std::string url = buildGetTileUrl(cfg);
            const std::string bodyStr = buildGetTileBody(reqs, cfg, version);
            HttpRequestOptions postOpts;
            postOpts.headers = {{"Referer", referer}};
            const auto manifestBody =
                amapPostBlocking(url, bodyStr,
                                 "application/x-www-form-urlencoded",
                                 postOpts);
            std::vector<AmapTileUrl> urls;
            std::string err;
            if (!parseTileUrls(std::string(manifestBody.begin(),
                                           manifestBody.end()),
                               urls, &err)) {
                LOGE("AmapDemo: get_tile refused: %s", err.c_str());
                return;
            }
            if (urls.empty()) return;
            AmapTileUrl selected;
            if (!selectAmapTileUrl(urls, reqs[0], selected, &err)) {
                LOGE("AmapDemo: manifest URL mismatch: %s", err.c_str());
                return;
            }
            HttpRequestOptions tileOpts;
            tileOpts.headers = {{"Referer", cfg.referer}};
            const auto tileBody =
                CurlMultiRequestScheduler::shared().getBlocking(
                    selected.url, tileOpts);
            std::vector<AmapDecodedLayerPart> parts;
            if (!decodeAmapTile(tileBody.data(), tileBody.size(), parts)) {
                LOGE("AmapDemo: tile decode failed");
                return;
            }
            std::vector<Feature> feats;
            for (const auto& p : parts) {
                if (regionsOnly ? (p.type != 2) : (p.type == 2)) continue;
                auto fs = amapDecodedPartToFeatures(p, true);
                feats.insert(feats.end(), std::make_move_iterator(fs.begin()),
                             std::make_move_iterator(fs.end()));
            }
            LOGI("AmapDemo: decoded %zu features from %zu layers",
                 feats.size(), parts.size());
            gRenderThread.post([layer, feats = std::move(feats)]() {
                for (auto& f : feats) {
                    layer->store().addFeature(std::move(f));
                }
            });
        }
    }).detach();
}

// UI 线程整形好的输入事件统一从这里投递到渲染线程。
// 屏幕密度（Java surfaceChanged 时设置）。手势阈值以 dp 定义，InputManager
// 用 event.devicePixelRatio 把 dp 换算成物理像素——不填则恒 1，latch 阈值
// 在高密度屏上会偏敏感 density 倍。
static float gDisplayDensity = 1.0f;

static void postInputEvent(const InputEvent& event) {
    InputEvent stamped = event;
    stamped.devicePixelRatio = gDisplayDensity;
    gInputPending.store(true, std::memory_order_release);
    gRenderThread.post([stamped]() {
        if (gEngine) {
            gEngine->onInputEvent(stamped);
            gEngine->requestRender("input");
        }
    });
}

// ============================================================
// JNI 桥接
// ============================================================

extern "C" {

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeInit(
    JNIEnv* env, jclass /* clazz */, jobject appContext) {
    if (gAppContext) {
        env->DeleteGlobalRef(gAppContext);
        gAppContext = nullptr;
    }
    if (appContext) {
        gAppContext = env->NewGlobalRef(appContext);
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceCreated(
    JNIEnv* env, jobject /* this */, jobject surface) {
    // Some devices recreate the Surface without first issuing a matching
    // surfaceDestroyed. Finish the prior native lifetime before replacing the
    // ANativeWindow so no render thread or EGL context remains attached to it.
    gRenderThread.stop();
    if (gWindow) {
        ANativeWindow_release(gWindow);
        gWindow = nullptr;
    }
    gWindow = ANativeWindow_fromSurface(env, surface);
    if (!gWindow) {
        LOGE("ANativeWindow_fromSurface failed");
        return;
    }
    // EGL / Engine 全部在渲染线程内创建
    gRenderThread.start(gWindow);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSetDisplayDensity(
    JNIEnv* /* env */, jobject /* this */, jfloat density) {
    gDisplayDensity = density > 0.1f ? density : 1.0f;
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceChanged(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    gWidth = width;
    gHeight = height;
    gRenderThread.post([width, height]() {
        if (gEngine) {
            gEngine->onSurfaceChanged(width, height, 1.0f);
        }
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSurfaceDestroyed(
    JNIEnv* /* env */, jobject /* this */) {
    cancelInputIfNeeded();
    gRenderThread.stop();  // join；引擎与 EGL 已在线程内销毁
    if (gWindow) {
        ANativeWindow_release(gWindow);
        gWindow = nullptr;
    }
}

// 辅助：通过 JNI 获取 Android 单调时钟（秒）
static double androidUptimeSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) * 1e-9;
}

static void endDebugPinchIfNeeded(float centerX, float centerY) {
    if (!gDebugPinchActive) return;

    InputEvent event;
    event.type = InputEvent::Type::PinchEnd;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
    gDebugPinchActive = false;
}

static void beginDebugPinchIfNeeded(float centerX,
                                    float centerY,
                                    double timestamp) {
    if (gDebugPinchActive) return;

    InputEvent event;
    event.type = InputEvent::Type::PinchStart;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.timestamp = timestamp;
    postInputEvent(event);
    gDebugPinchActive = true;
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeTouchDown(
    JNIEnv* /* env */, jobject /* this */, jfloat x, jfloat y) {
    endDebugPinchIfNeeded(static_cast<float>(gWidth) * 0.5f,
                          static_cast<float>(gHeight) * 0.5f);
    gTouching = true;
    gDragStarted = false;
    gTouchMoved = false;

    // 编辑模式的触摸不喂相机手势流（editTouchDown 由 nativeDrag 首个 move 发）。
    if (gEditMode.load(std::memory_order_relaxed)) {
        return;
    }

    // 真按下即投递 PointerDown。此前只在 nativeDrag 的首个 move 里补发，
    // 于是"按下即抬手"的纯点击只到达一个 PointerUp，InputManager 处在 Idle
    // 直接早退 —— Android 上 Click / DoubleClick 从未触发过（单击选中、
    // 双击缩放全是死的）。iOS 侧 touchesBegan 一直是正常发的。
    InputEvent event;
    event.type = InputEvent::Type::PointerDown;
    event.screenX = x;
    event.screenY = y;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
    gDragStarted = true;  // nativeDrag 不必再补发
}

// 双指抬起一指后，剩余手指续接单指拖拽。刻意不投递 PointerDown：这一段是
// 多指手势的尾巴，不应产生 click/double-click；PointerDown 仍由 nativeDrag
// 的首个 move 补发（=改动前的行为）。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResumePointer(
    JNIEnv* /* env */, jobject /* this */) {
    endDebugPinchIfNeeded(static_cast<float>(gWidth) * 0.5f,
                          static_cast<float>(gHeight) * 0.5f);
    gTouching = true;
    gDragStarted = false;
    gTouchMoved = false;
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDrag(
    JNIEnv* /* env */, jobject /* this */,
    jfloat startX, jfloat startY, jfloat endX, jfloat endY,
    jint /*width*/, jint /*height*/) {
    gTouchMoved = true;

    // 编辑模式:触摸走顶点拖拽编辑流(渲染线程),不喂相机。
    if (gEditMode.load(std::memory_order_relaxed)) {
        const bool first = !gDragStarted;
        gDragStarted = true;
        gRenderThread.post([first, startX, startY, endX, endY]() {
            if (first) editTouchDown(startX, startY);
            editTouchMove(endX, endY);
        });
        return;
    }

    double ts = androidUptimeSeconds();

    if (!gDragStarted) {
        gDragStarted = true;
        InputEvent event;
        event.type = InputEvent::Type::PointerDown;
        event.screenX = startX;
        event.screenY = startY;
        event.pointerType = InputEvent::PointerType::Touch;
        event.timestamp = ts;
        postInputEvent(event);
    }

    InputEvent event;
    event.type = InputEvent::Type::PointerMove;
    event.screenX = endX;
    event.screenY = endY;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = ts;
    postInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeTouchUp(
    JNIEnv* /* env */, jobject /* this */, jfloat x, jfloat y) {
    gTouching = false;

    if (gEditMode.load(std::memory_order_relaxed)) {
        gRenderThread.post([]() { editTouchUp(); });
        return;
    }

    double ts = androidUptimeSeconds();

    InputEvent upEvent;
    upEvent.type = InputEvent::Type::PointerUp;
    upEvent.screenX = x;
    upEvent.screenY = y;
    upEvent.pointerType = InputEvent::PointerType::Touch;
    upEvent.timestamp = ts;
    postInputEvent(upEvent);

    // 诊断日志（pick 和选择由 InputManager → Scene 回调处理）；
    // pick 读渲染态，投递到渲染线程执行
    if (!gTouchMoved) {
        gRenderThread.post([x, y]() {
            if (!gEngine) return;
            PickResult result = gEngine->pick(x, y);
            if (result.isValid()) {
                const double lngDeg = result.cartographic.longitudeDegrees();
                const double latDeg = result.cartographic.latitudeDegrees();
                LOGI("Tap at (%.0f,%.0f) → lng=%.6f lat=%.6f height=%.2f "
                     "layer=%s feature=%s",
                     x, y, lngDeg, latDeg,
                     result.cartographic.height(),
                     result.layerId.c_str(),
                     result.featureId.c_str());
            } else {
                LOGI("Tap at (%.0f,%.0f) → no hit", x, y);
            }
        });
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchStart(
    JNIEnv* /* env */, jobject /* this */, jfloat centerX, jfloat centerY) {
    endDebugPinchIfNeeded(centerX, centerY);
    gTouching = true;
    gDragStarted = false;
    gTouchMoved = true;

    InputEvent event;
    event.type = InputEvent::Type::PinchStart;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchEnd(
    JNIEnv* /* env */, jobject /* this */, jfloat centerX, jfloat centerY) {
    gTouching = false;

    InputEvent event;
    event.type = InputEvent::Type::PinchEnd;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = 1.0f;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePinchRotateTilt(
    JNIEnv* /* env */, jobject /* this */,
    jfloat scale, jfloat rotationRadians,
    jfloat centerX, jfloat centerY, jfloat centerDx, jfloat centerDy,
    jfloat pointer0X, jfloat pointer0Y, jfloat pointer1X, jfloat pointer1Y,
    jint /*width*/, jint /*height*/) {
    InputEvent event;
    event.type = InputEvent::Type::PinchMove;
    event.screenX = centerX;
    event.screenY = centerY;
    event.pinchScale = scale;
    event.rotationRadians = rotationRadians;
    event.centerDeltaX = centerDx;
    event.centerDeltaY = centerDy;
    event.pointerType = InputEvent::PointerType::Touch;
    event.pointerCount = 2;
    event.hasPointerPair = true;
    event.pointer0X = pointer0X;
    event.pointer0Y = pointer0Y;
    event.pointer1X = pointer1X;
    event.pointer1Y = pointer1Y;
    event.timestamp = androidUptimeSeconds();
    postInputEvent(event);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugTilt(
    JNIEnv* /* env */, jobject /* this */,
    jfloat centerDy, jint width, jint height) {
    const float centerX = static_cast<float>(width) * 0.5f;
    const float centerY = static_cast<float>(height) * 0.5f;
    const double timestamp = androidUptimeSeconds();
    beginDebugPinchIfNeeded(centerX, centerY, timestamp);

    InputEvent move;
    move.type = InputEvent::Type::PinchMove;
    move.screenX = centerX;
    move.screenY = centerY;
    move.pinchScale = 1.0f;
    move.centerDeltaY = centerDy;
    move.pointerType = InputEvent::PointerType::Touch;
    move.pointerCount = 2;
    move.timestamp = timestamp + 0.016;
    postInputEvent(move);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugPinchEnd(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    endDebugPinchIfNeeded(
        static_cast<float>(width) * 0.5f,
        static_cast<float>(height) * 0.5f);
}

// 确定性双指路径回放：adb 无法产生多点触控，这里合成固定的两指像素序列
// （含 pointer pair → 走 InputManager latch + 新契约完整链路），供手势回归
// 复测与将来重新插桩时复用。数字键 1-4 触发（见 Java onKeyDown）。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugPinchPath(
    JNIEnv* /* env */, jobject /* this */,
    jint scenario, jint width, jint height) {
    const float cx = static_cast<float>(width) * 0.5f;
    const float cy = static_cast<float>(height) * 0.5f;
    const float halfSpread = 300.0f;
    const double t0 = androidUptimeSeconds();

    auto postMove = [&](int step, float p0x, float p0y, float p1x, float p1y) {
        InputEvent move;
        move.type = InputEvent::Type::PinchMove;
        move.screenX = (p0x + p1x) * 0.5f;
        move.screenY = (p0y + p1y) * 0.5f;
        move.pinchScale = 1.0f;  // 新契约不消费；派生量由 InputManager 计算
        move.pointerType = InputEvent::PointerType::Touch;
        move.pointerCount = 2;
        move.hasPointerPair = true;
        move.pointer0X = p0x;
        move.pointer0Y = p0y;
        move.pointer1X = p1x;
        move.pointer1Y = p1y;
        move.timestamp = t0 + 0.016 * static_cast<double>(step);
        postInputEvent(move);
    };

    {
        InputEvent start;
        start.type = InputEvent::Type::PinchStart;
        start.screenX = cx;
        start.screenY = cy;
        start.pinchScale = 1.0f;
        start.pointerType = InputEvent::PointerType::Touch;
        start.pointerCount = 2;
        start.timestamp = t0;
        postInputEvent(start);
    }

    constexpr int kSteps = 45;
    for (int i = 0; i <= kSteps; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(kSteps);
        switch (scenario) {
            case 0: {  // 纯刚性 pan：质心横移 300px
                const float dx = 300.0f * f;
                postMove(i, cx - halfSpread + dx, cy,
                            cx + halfSpread + dx, cy);
                break;
            }
            case 1: {  // Pitch：双指平行上推 240px
                const float dy = -240.0f * f;
                postMove(i, cx - halfSpread, cy + dy,
                            cx + halfSpread, cy + dy);
                break;
            }
            case 2: {  // 组合：缩放 1.5×+拧 0.5rad+质心斜移
                const float r = halfSpread * (1.0f + 0.5f * f);
                const float a = 0.5f * f;
                const float mx = cx + 150.0f * f;
                const float my = cy - 100.0f * f;
                postMove(i, mx - r * std::cos(a), my - r * std::sin(a),
                            mx + r * std::cos(a), my + r * std::sin(a));
                break;
            }
            default: {  // 3: 慢拧+微缩放（阈值附近，验 latch 后无模式翻转）
                const float a = 0.3f * f;
                const float wobble = 1.0f + 0.02f * ((i % 2 == 0) ? 1.0f : -1.0f);
                const float r = halfSpread * wobble;
                postMove(i, cx - r * std::cos(a), cy - r * std::sin(a),
                            cx + r * std::cos(a), cy + r * std::sin(a));
                break;
            }
        }
    }

    {
        InputEvent end;
        end.type = InputEvent::Type::PinchEnd;
        end.screenX = cx;
        end.screenY = cy;
        end.pinchScale = 1.0f;
        end.pointerType = InputEvent::PointerType::Touch;
        end.pointerCount = 2;
        end.timestamp = t0 + 0.016 * static_cast<double>(kSteps + 1);
        postInputEvent(end);
    }
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugZoom(
    JNIEnv* /* env */, jobject /* this */,
    jfloat scale, jint width, jint height) {
    const float centerX = static_cast<float>(width) * 0.5f;
    const float centerY = static_cast<float>(height) * 0.5f;
    const double ts = androidUptimeSeconds();
    beginDebugPinchIfNeeded(centerX, centerY, ts);

    InputEvent move;
    move.type = InputEvent::Type::PinchMove;
    move.screenX = centerX;
    move.screenY = centerY;
    move.pinchScale = scale;
    move.pointerType = InputEvent::PointerType::Touch;
    move.pointerCount = 2;
    move.timestamp = ts + 0.016;
    postInputEvent(move);

    // 诊断快照读渲染态，投递到渲染线程打印
    gRenderThread.post([scale]() {
    if (!gEngine) return;
    const auto& diag = gEngine->diagnostics();
    const auto& trace = gEngine->presentationTrace();
    const auto& cameraTrace = trace.camera;
    const double cameraRadius = gEngine->camera().position().length();
    const double sphericalAltitude = cameraRadius - 6378137.0;
    const double ellipsoidAltitude =
        Ellipsoid::WGS84().cartesianToCartographic(gEngine->camera().position()).height();
    LOGI("Debug zoom scale=%.2f | tiles vis=%d cached=%d renderSurface=%d "
         "exact=%d parent=%d missing=%d unsupported=%d kicked=%d retained=%d "
         "entry plan=%d/%d draw=%d/%d miss=%d/%d defer=%d/%d fallback=%d prep=%d/%d surface=%d src=%d/%d/%d/%d "
         "z=%d-%d targetZ=%d-%d texZ=%d-%d lod=%.0f eq=%d qRender=%d qWalk=%d qBal=%d "
         "qFrustum=%d qHz=%d qEq2=%d grp=%d/%d/%d "
         "center=%.6f,%.6f targetH=%.2f camH=%.2f pitch=%.6f heading=%.6f vp=%dx%d "
         "ellAlt=%.2f sphAlt=%.2f radius=%.2f FPS=%.1f draw=%d",
         scale,
         diag.visibleTiles,
         diag.cachedTextures,
         diag.renderSurfaceTiles,
         diag.imageryExactAttachments,
         diag.imageryParentFallbackAttachments,
         diag.imageryMissingTiles,
         diag.imageryUnsupportedTiles,
         diag.imageryKickedTiles,
         diag.imageryAncestorRetainedTiles,
         diag.terrainRenderEntriesPlanned,
         diag.terrainRenderEntriesSelectedPlanned,
         diag.terrainRenderEntriesDrawn,
         diag.terrainRenderEntriesSelectedDrawn,
         diag.terrainRenderEntriesMissed,
         diag.terrainRenderEntriesSelectedMissed,
         diag.terrainRenderEntriesDeferred,
         diag.terrainRenderEntriesSelectedDeferred,
         diag.terrainRenderEntriesAncestorFallback,
         diag.terrainRenderEntriesSynchronousPrep,
         diag.terrainRenderEntriesDeferredPrep,
         diag.terrainSurfaceCommandsSubmitted,
         diag.terrainSurfaceRealCommands,
         diag.terrainSurfaceFillProxyCommands,
         diag.terrainSurfaceEllipsoidCommands,
         diag.terrainSurfaceUnknownCommands,
         diag.minVisibleZoom,
         diag.maxVisibleZoom,
         diag.imageryMinTargetZoom,
         diag.imageryMaxTargetZoom,
         diag.imageryMinTextureZoom,
         diag.imageryMaxTextureZoom,
         diag.lodSizePixels,
         diag.quadtreeEqualZoomLayers,
         diag.quadtreeRenderingNodes,
         diag.quadtreeWalkthroughNodes,
         diag.quadtreeNeighborBalancedTiles,
         diag.quadtreeInFrustumNodes,
         diag.quadtreeHorizonTangentPreservedNodes,
         diag.quadtreeEqualZoomSecondPassNodes,
         diag.mercatorTileCount,
         diag.northPolarTileCount,
         diag.southPolarTileCount,
         cameraTrace.targetLongitudeDegrees,
         cameraTrace.targetLatitudeDegrees,
         cameraTrace.targetHeightMeters,
         cameraTrace.cameraHeightMeters,
         cameraTrace.pitchRadians,
         cameraTrace.headingRadians,
         cameraTrace.viewportWidthPixels,
         cameraTrace.viewportHeightPixels,
         ellipsoidAltitude,
         sphericalAltitude,
         cameraRadius,
         diag.fps,
         diag.drawCalls);
    });
}

// ============================================================
// Debug panel JNI
// ============================================================

// 渲染线程上执行：读 gEngine 各状态面拼诊断文本
static std::string buildDiagnosticsText() {
    const auto& diag = gEngine->diagnostics();
    const double cameraRadius = gEngine->camera().position().length();
    const double sphericalAltitude = cameraRadius - 6378137.0;
    const double ellipsoidAltitude =
        Ellipsoid::WGS84().cartesianToCartographic(gEngine->camera().position()).height();
    const double cameraDist = gEngine->camera().position().distanceTo(Vec3::zero());

    char buf[4096];
    snprintf(buf, sizeof(buf),
        "FPS: %.1f  |  Frame: %.1f ms\n"
        "CPU: %.1f ms  |  begin %.1f upd %.1f build %.1f submit %.1f end %.1f\n"
        "Update: cam %.1f env %.1f base %.1f terr %.1f content %.1f\n"
        "Draw calls: %d  |  GPU tex: %d  |  glTF prim: %d\n"
        "Visible tiles: terrain %d content %d/%d  |  Cached: %d\n"
        "Surface meshes: %d (%d terrSurfCmd, %d terrGltfCmd)\n"
        "Terrain surface src: real %d, fill %d, ell %d, unk %d\n"
        "Attachments: %d exact, %d parent, %d missing, %d unsup, %d kicked, %d retained\n"
        "Zoom: %d-%d  |  Img: %d-%d -> tex %d-%d\n"
        "LOD: %.0f px  |  EqZoom: %d\n"
        "QuadTree: %d render, %d walk, %d frustum, %d balanced\n"
        "Occlusion: %d occ, %d wait, %d culled-vis\n"
        "Groups: %d merc, %d N, %d S\n"
        "Camera: ellAlt=%.0fm sphAlt=%.0fm dist=%.0fm\n"
        "LoadQ: %d pre, %d norm, %d urgent  |  Terrain pending: %d req, %d upload, %d terminal\n"
        "Content pending: %d req, %d upload, %d terminal\n"
        "ReqDrop: iss %d%s | key %d dup %d empty %d cls %d upSrc %d upNoC %d disp %d noProv %d stop %d\n"
        "Budget: net %d/%d, terrain-content %d/%d, content %d/%d, raster %d/%d\n"
        "Main budget: fin %d/%d, term %d/%d, rasUp %d/%d, %.1f/%.1f ms, mode %c/%c\n"
        "Provider: terr %d/%d wb %d/%d | cont %d/%d wb %d/%d ext %d/%d | rast %d/%d wb %d/%d\n"
        "Transport limit: terrain %d, content %d, raster %d\n"
        "LoadState: unloading %d, retry %d, unloaded %d, loading %d, loaded %d, done %d, failed %d\n"
        "Content: unknown %d, empty %d, external %d, render %d  |  UnloadQ: %d\n"
        "Raster overlay: missing projections %d\n"
        "Mesh: %d KB  (gen %llu)",
        diag.fps, diag.frameTimeMs,
        diag.engineFrameCpuMs,
        diag.engineBeginFrameMs,
        diag.sceneUpdateMs,
        diag.renderCommandBuildMs,
        diag.renderSubmitMs,
        diag.engineEndFrameMs,
        diag.cameraUpdateMs,
        diag.environmentUpdateMs,
        diag.basemapStackUpdateMs,
        diag.terrainUpdateMs,
        diag.contentTilesetUpdateMs,
        diag.drawCalls, diag.gpuTextureCount, diag.renderGltfPrimitives,
        diag.visibleTiles, diag.contentVisibleTiles, diag.contentTilesets,
        diag.cachedTextures,
        diag.surfaceMeshCount,
        diag.terrainSurfaceTileCommands, diag.terrainGltfPrimitiveCommands,
        diag.terrainSurfaceRealCommands,
        diag.terrainSurfaceFillProxyCommands,
        diag.terrainSurfaceEllipsoidCommands,
        diag.terrainSurfaceUnknownCommands,
        diag.imageryExactAttachments, diag.imageryParentFallbackAttachments,
        diag.imageryMissingTiles, diag.imageryUnsupportedTiles,
        diag.imageryKickedTiles,
        diag.imageryAncestorRetainedTiles,
        diag.minVisibleZoom, diag.maxVisibleZoom,
        diag.imageryMinTargetZoom, diag.imageryMaxTargetZoom,
        diag.imageryMinTextureZoom, diag.imageryMaxTextureZoom,
        diag.lodSizePixels, diag.quadtreeEqualZoomLayers,
        diag.quadtreeRenderingNodes, diag.quadtreeWalkthroughNodes,
        diag.quadtreeInFrustumNodes,
        diag.quadtreeNeighborBalancedTiles,
        diag.quadtreeSelectionOccludedNodes,
        diag.quadtreeSelectionWaitingForOcclusionResultsNodes,
        diag.quadtreeCulledTilesVisited,
        diag.mercatorTileCount, diag.northPolarTileCount,
        diag.southPolarTileCount,
        ellipsoidAltitude, sphericalAltitude, cameraDist,
        diag.loadQueuePreloadRequests,
        diag.loadQueueNormalRequests,
        diag.loadQueueUrgentRequests,
        diag.pendingTerrainRequests,
        diag.pendingGltfTerrainUploads,
        diag.pendingGltfTerrainTerminalResults,
        diag.pendingContentRequests,
        diag.pendingContentUploads,
        diag.pendingContentTerminalResults,
        diag.requestIssued,
        diag.requestBlockedByInflight ? " BLK" : "",
        diag.reqSkipEmptyKey,
        diag.reqSkipAlreadyPending,
        diag.reqSkipEmptyTile,
        diag.reqSkipClassified,
        diag.reqSkipUpsampleSrc,
        diag.reqSkipUpsampleNoContent,
        diag.reqSkipDispatch,
        diag.reqSkipNoProvider,
        diag.reqStopDispatch,
        diag.budgetNetworkRequestsIssued,
        diag.budgetNetworkRequestsLimit,
        diag.budgetTerrainContentNetworkRequestsIssued,
        diag.budgetTerrainContentNetworkRequestsLimit,
        diag.budgetContentNetworkRequestsIssued,
        diag.budgetContentNetworkRequestsLimit,
        diag.budgetRasterNetworkRequestsIssued,
        diag.budgetRasterNetworkRequestsLimit,
        diag.budgetMainThreadFinalizesUsed,
        diag.budgetMainThreadFinalizesLimit,
        diag.budgetTerminalStateTransitionsUsed,
        diag.budgetTerminalStateTransitionsLimit,
        diag.budgetRasterUploadsUsed,
        diag.budgetRasterUploadsLimit,
        diag.budgetMainThreadElapsedMs,
        diag.budgetMainThreadTimeLimitMs,
        diag.budgetInteractionActive ? 'I' : '-',
        diag.budgetSmoothingActive ? 'S' : '-',
        diag.terrainProviderRequestsStarted,
        diag.terrainProviderRequestsCompleted,
        diag.terrainProviderActiveWorkerBlockingRequests,
        diag.terrainProviderPeakWorkerBlockingRequests,
        diag.contentProviderRequestsStarted,
        diag.contentProviderRequestsCompleted,
        diag.contentProviderActiveWorkerBlockingRequests,
        diag.contentProviderPeakWorkerBlockingRequests,
        diag.contentProviderExternalResourceRequestsStarted,
        diag.contentProviderExternalResourceRequestsCompleted,
        diag.rasterProviderRequestsStarted,
        diag.rasterProviderRequestsCompleted,
        diag.rasterProviderActiveWorkerBlockingRequests,
        diag.rasterProviderPeakWorkerBlockingRequests,
        diag.terrainTransportActiveRequestLimit,
        diag.contentTransportActiveRequestLimit,
        diag.rasterTransportActiveRequestLimit,
        diag.terrainLoadUnloadingTiles,
        diag.terrainLoadFailedTemporarilyTiles,
        diag.terrainLoadUnloadedTiles,
        diag.terrainLoadContentLoadingTiles,
        diag.terrainLoadContentLoadedTiles,
        diag.terrainLoadDoneTiles,
        diag.terrainLoadFailedTiles,
        diag.terrainContentUnknownTiles,
        diag.terrainContentEmptyTiles,
        diag.terrainContentExternalTiles,
        diag.terrainContentRenderTiles,
        diag.terrainUnloadQueueTiles,
        diag.missingRasterOverlayProjections,
        diag.surfaceMeshBytes / 1024,
        static_cast<unsigned long long>(diag.terrainGeneration));
    std::string text(buf);
    text += "\n";
    text += minimal_globe_demo::buildRenderEntryDiagnosticsLine(diag);
    text += minimal_globe_demo::buildPresentationTraceSummary(
        gEngine->presentationTrace());
    return text;
}

JNIEXPORT jstring JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetDiagnosticsString(
    JNIEnv* env, jobject /* this */) {
    // 同步投递到渲染线程读取；shared_ptr 捕获防超时后悬垂
    auto text = std::make_shared<std::string>();
    const bool ok = gRenderThread.runSync(
        [text]() {
            *text = gEngine ? buildDiagnosticsText()
                            : std::string("Engine not ready");
        },
        std::chrono::milliseconds(100));
    return env->NewStringUTF(ok ? text->c_str() : "Engine not ready");
}

// 指北针：读取每帧发布的相机方位角(弧度,0=正北,顺时针+)。
JNIEXPORT jfloat JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetHeadingRadians(
    JNIEnv* /* env */, jobject /* this */) {
    return gHeadingRadians.load();
}

// 复位正北朝上（在渲染线程执行，读写相机态）。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResetNorthUp(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (gEngine) gEngine->resetNorthUp();
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeAddDemoVectorLayer(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine || !gRenderDevice) return;
        minimal_globe_demo::addDemoVectorLayer(*gEngine, *gRenderDevice);
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResetCamera(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gSdkFacade) return;
        gSdkFacade->resetCamera();
        LOGI("Camera reset to Chongqing demo viewpoint");
    });
}

// 阶段 3:飞到北京。机制信号 = 飞行期逐帧 progress + 碰撞钳位次数增量,
// 落地打终点位姿误差。⚠️钳位次数**必须为 0** —— 那是"拱高让钳位结构性不
// 触发"的判据;非 0 说明是钳位在兜底(画面上两者完全看不出区别)。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugFlyTo(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        CameraSystem& cam = gEngine->cameraSystem();
        Viewpoint dest;
        dest.eyeGeo = Cartographic::fromDegrees(kFlightDestLng, kFlightDestLat,
                                                kFlightDestAlt);
        dest.headingRadians = 0.0;
        dest.pitchRadians = -0.6;
        dest.rollRadians = 0.0;

        // 先算"直接设过去"的落点作参照:飞过去必须落在同一处
        // (setViewpoint 与 flyTo 共用 resolveViewpoint,分岔就会在这里露出来)。
        const Vec3 before = gEngine->camera().position();
        cam.setViewpoint(dest);
        const Vec3 expected = gEngine->camera().position();
        Viewpoint back;
        back.eyeGeo = Ellipsoid::WGS84().cartesianToCartographic(before);
        cam.setViewpoint(back);

        gFlightProbe = FlightProbe{};
        gFlightProbe.clampsAtStart = cam.constraintClampCount();
        const bool started = cam.flyTo(dest);
        gFlightProbe.armed = started;
        LOGI("StageFlight start=%d expectEye=%.1f,%.1f,%.1f dist=%.0fm",
             started ? 1 : 0, expected.x(), expected.y(), expected.z(),
             (expected - before).length());
    });
}

// 阶段 5 的真实用途:可复现的**正俯视**位姿。掠视下的正交是退化用例
// (正交盒半高远大于相机高度 ⇒ 下半部整个在地下 ⇒ 天空色),俯视才是正交要干的活。
//
// V26 二期:样式文档驱动的换肤。优先读**外置样式文件**(adb push 即热换,
// 不重编译 —— V26 判据本体);文件缺席回落一期的内置 C++ 样式(demo 不依赖
// push 也能演示)。文档路:parse → compile(契约 fail-loud)→ planStyleApply
// (成本类路由:只换线色不重烘场)→ 按 plan 分发三条通路。
// 像素判据(归用户):水深蓝/楼暖棕/路网琥珀 ↔ 日版米白;瞬态=Re-bake 期间
// 面短暂回落纯影像。
// 样式文档候选目录 = kStyleDocDirs(已上移到 mvtFetchTile 前,与
// sources.json 共用同一目录约定与注入方式)。

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugRestyle(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        gNightStyle = !gNightStyle;
        const bool night = gNightStyle;
        // 三期文档路:Engine 一口气分发(parse→契约→成本类路由→三路)。
        // 错误 = 整份拒收逐条 LOGE,继续试下一路径/回落内置 —— 坏文档
        // 不得半应用。
        for (const char* dir : kStyleDocDirs) {
            const std::string docPath = std::string(dir) + "/style-" +
                                        (night ? "night" : "day") + ".json";
            std::ifstream in(docPath, std::ios::binary);
            if (!in) continue;  // 文件缺席不是错误(内置兜底)
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            const std::vector<StyleError> errors =
                gEngine->applyStyleDocument(text);
            if (errors.empty()) {
                LOGI("V26Restyle applied from doc: %s", docPath.c_str());
                return;
            }
            for (const StyleError& e : errors) {
                LOGE("V26Restyle style doc %s: %s: %s", docPath.c_str(),
                     e.where.c_str(), e.message.c_str());
            }
        }
        // 一期内置兜底(真机已验路径,行为不变)。内置路绕过 Engine 的文档
        // 指纹 memo —— 重注册目标清掉它,防下次文档应用按旧指纹误判 diff。
        gEngine->setStyleTargets(gDrapeProviderRaw, gRoadFieldSource,
                                 gMvtBasemapLayer);
        if (gDrapeProviderRaw) {
            gDrapeProviderRaw->setStyle(
                night ? minimal_globe_demo::makeMvtDrapeStyleNight()
                      : minimal_globe_demo::makeMvtDrapeStyle());
            gEngine->invalidateComposedTerrainPages();
        }
        if (gRoadFieldSource) {
            gEngine->setRoadFieldStyleUniforms(
                night ? minimal_globe_demo::kMvtRoadFieldColorNight
                      : minimal_globe_demo::kMvtRoadFieldColor,
                minimal_globe_demo::kMvtRoadFieldWidthRampPx);
            gRoadFieldSource->setStyle(
                minimal_globe_demo::makeMvtRoadFieldStyle());
            gEngine->invalidateRoadFieldPages(
                minimal_globe_demo::kMvtRoadFieldMaxZoom);
        }
        LOGI("V26Restyle applied builtin: %s (drape=%d field=%d)",
             night ? "night" : "day", gDrapeProviderRaw != nullptr,
             gRoadFieldSource != nullptr);
    });
}

// 走 setViewpoint 的「部分 viewpoint」语义,顺带在设备上验阶段 2 的**万向节约定**:
// pitch 恰好 −π/2 是奇点(direction 沿天底,绕它转不改视线 ⇒ heading 只能由 up 定,
// 约定 roll=0)。回读 currentViewpoint() 打出来,位姿往返在真机上也必须闭合。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugNadirView(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        CameraSystem& cam = gEngine->cameraSystem();
        Viewpoint vp;
        vp.targetGeo = Cartographic::fromDegrees(106.508, 29.617, 0.0);
        vp.rangeMeters = 20000.0;
        vp.headingRadians = 0.0;
        vp.pitchRadians = -M_PI / 2.0;   // 正俯视 = 万向节奇点
        vp.rollRadians = 0.0;
        cam.setViewpoint(vp);

        const Viewpoint got = cam.currentViewpoint();
        LOGI("StageNadir set h=%.4f p=%.4f r=%.4f camH=%.1f hasTarget=%d",
             got.headingRadians ? *got.headingRadians : -99.0,
             got.pitchRadians ? *got.pitchRadians : -99.0,
             got.rollRadians ? *got.rollRadians : -99.0,
             got.eyeGeo ? got.eyeGeo->height() : -1.0,
             got.targetGeo ? 1 : 0);
    });
}

// 阶段 4:切系留。第一次按 = 只接 originProvider(跟车但保持北上),
// 第二次 = 加上 orientationProvider(座舱,roll 跟随载体),第三次 = 回 Free。
// 机制信号 = localHPR/range 逐帧不变 + 相机到载体距离恒等于 range。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugTether(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        CameraSystem& cam = gEngine->cameraSystem();
        if (!gCarrier.active) {
            gCarrier.active = true;
            gCarrier.useOrientation = false;
        } else if (!gCarrier.useOrientation) {
            gCarrier.useOrientation = true;
        } else {
            gCarrier.active = false;
            cam.selectController(CameraSystem::kFreeGlobeController);
            LOGI("StageTether off (back to free)");
            return;
        }
        gCarrier.step(0.0);          // 先落一次位置,避免首帧 provider 拿到零

        ViewpointFrame frame;
        frame.originProvider = [](glm::dvec3& out) {
            if (!gCarrier.active) return false;
            out = gCarrier.position;
            return true;
        };
        if (gCarrier.useOrientation) {
            frame.orientationProvider = [](glm::dmat3& out) {
                if (!gCarrier.active) return false;
                out = gCarrier.orientation;
                return true;
            };
        }
        cam.tetheredController().setFrame(frame);
        cam.selectController(CameraSystem::kTetheredController);
        cam.tetheredController().setRange(1500.0);
        LOGI("StageTether on orientationProvider=%d",
             gCarrier.useOrientation ? 1 : 0);
    });
}

// 阶段 5:正交/透视切换。切换瞬间把正交宽度设成"透视在当前地面距离处的
// 足迹",两种投影的画面才可比 —— 否则一切过去尺度全变,看不出别的问题。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeDebugToggleOrtho(
    JNIEnv* /* env */, jobject /* this */, jint width, jint height) {
    const double w = static_cast<double>(width);
    const double h = static_cast<double>(height);
    gRenderThread.post([w, h]() {
        if (!gEngine) return;
        Camera& camera = gEngine->camera();
        if (gOrthographic) {
            camera.setPerspective(camera.verticalFovRadians(),
                                  camera.nearPlaneMeters(),
                                  camera.farPlaneMeters());
            gOrthographic = false;
            LOGI("StageOrtho off isOrtho=%d", camera.isOrthographic() ? 1 : 0);
            return;
        }
        // 视线与椭球求交拿地面距离;不交(看天)则退回相机椭球高。
        const Ray ray(camera.position(), camera.direction());
        const std::optional<Vec3> hit =
            Ellipsoid::WGS84().rayIntersection(ray.origin(), ray.direction());
        // 同上:glm 成员 length() 是分量个数。这里写错会让正交宽度变成
        // 2·3·tan(fov/2)·aspect ≈ 几米,画面直接糊死。
        const double distance =
            hit ? glm::length(hit->raw() - camera.position().raw())
                : camera.getHeight();
        const double aspect = h > 0.0 ? w / h : 1.0;
        const double widthMeters =
            2.0 * distance * std::tan(camera.verticalFovRadians() * 0.5) *
            aspect;
        // ⚠️ near 显式给定:正交下动态 near 已在 SceneFrameUpdateCoordinator
        // 断掉(那套公式治的是透视的 z_ndc 病态区),这里不给就沿用上一次透视
        // 收紧后的值,可能把相机前方整片切掉。
        camera.setOrthographic(widthMeters, 1.0, camera.farPlaneMeters());
        gOrthographic = true;
        LOGI("StageOrtho on isOrtho=%d widthM=%.0f groundDist=%.0f",
             camera.isOrthographic() ? 1 : 0, widthMeters, distance);
    });
}

// 面板按钮文案的回读口。与 GPU 位移开关同一取向:真值只在引擎里,UI 不存镜像。
// ⚠️ 读 camera.isOrthographic() 而不是 gOrthographic —— surface 重建后相机是新
// 造的(回透视),而 gOrthographic 这个 demo 侧变量会留在 true,两者会分叉。
JNIEXPORT jboolean JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetOrtho(
    JNIEnv* /* env */, jobject /* this */) {
    auto on = std::make_shared<bool>(false);
    gRenderThread.runSync(
        [on]() {
            if (gEngine) *on = gEngine->camera().isOrthographic();
        },
        std::chrono::milliseconds(100));
    return *on ? JNI_TRUE : JNI_FALSE;
}

// 系留三态:0=Free,1=跟车(仅 originProvider),2=座舱(加 orientationProvider)。
// ⚠️ 先看当前驱动者是不是 Tethered:引擎重建后选择器回到 Free,而 gCarrier
// 这个 demo 侧结构还留着 active=true —— 只读 gCarrier 会报出不存在的系留态。
JNIEXPORT jint JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetTetherState(
    JNIEnv* /* env */, jobject /* this */) {
    auto state = std::make_shared<int>(0);
    gRenderThread.runSync(
        [state]() {
            if (!gEngine) return;
            if (gEngine->cameraSystem().activeControllerName() !=
                CameraSystem::kTetheredController) {
                return;  // 不是系留在驱动 ⇒ 0,无论 gCarrier 记着什么
            }
            *state = gCarrier.useOrientation ? 2 : 1;
        },
        std::chrono::milliseconds(100));
    return static_cast<jint>(*state);
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeGrazingView(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        // 固定可复现的斜视地平线位姿(性能测量用,复现 horizon-jank 重视野):
        // 相机在重庆上空低空(6km),视线仅比本地水平面下俯 4° → 近景高 LOD、
        // 远景延伸到地平线,把最大数量的地形/底图瓦片纳入考量。
        const auto& ellipsoid = Ellipsoid::WGS84();
        const double centerLng = 106.508, centerLat = 29.617;
        const double camAlt = 6000.0;
        const double pitchDeg = 4.0;
        auto camEcef = ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(centerLng, centerLat, camAlt));
        Vec3 up = ellipsoid.geodeticSurfaceNormal(camEcef);
        Vec3 north = (Vec3::unitZ() - up * up.dot(Vec3::unitZ())).normalized();
        const double p = pitchDeg * 3.14159265358979323846 / 180.0;
        Vec3 dir = (north * std::cos(p) - up * std::sin(p)).normalized();
        gEngine->camera().setView(camEcef, dir, up);
        LOGI("Grazing horizon view set (Chongqing %.0fm alt, pitch %.0f deg down)",
             camAlt, pitchDeg);
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeTerrainGrazingView(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gEngine) return;
        // 低 AGL 贴地掠视(动态 near 换源的复现位姿):相机在缙云山东南麓
        // 低空(~700m 椭球高,当地谷底 ~250m → AGL ~450m),视线朝西北山脊
        // 下俯 3°。旧 near 公式(椭球高×0.5≈350m 起步,但注意旧公式下限
        // 150m、按 nadir 不扣地形,在高原/山地会拉到千米级)会把前景坡体
        // 切出"看到山内部"的截面;新公式按近场最近几何收紧。若起手位姿低
        // 于地形+50m,帧末哨兵会自动抬升——位姿仍可复现。
        const auto& ellipsoid = Ellipsoid::WGS84();
        const double lng = 106.44, lat = 29.70;
        const double camAlt = 700.0;
        const double pitchDeg = 3.0;
        const double headingDeg = 315.0;  // 朝西北(缙云山脊方向)
        auto camEcef = ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(lng, lat, camAlt));
        Vec3 up = ellipsoid.geodeticSurfaceNormal(camEcef);
        Vec3 north = (Vec3::unitZ() - up * up.dot(Vec3::unitZ())).normalized();
        Vec3 east = north.cross(up).normalized();  // ENU: north × up = east
        const double h = headingDeg * 3.14159265358979323846 / 180.0;
        Vec3 horiz = (north * std::cos(h) + east * std::sin(h)).normalized();
        const double p = pitchDeg * 3.14159265358979323846 / 180.0;
        Vec3 dir = (horiz * std::cos(p) - up * std::sin(p)).normalized();
        gEngine->camera().setView(camEcef, dir, up);
        LOGI("Terrain grazing view set (%.2f,%.2f alt=%.0fm heading=%.0f pitch=-%.0f)",
             lng, lat, camAlt, headingDeg, pitchDeg);
    });
}

// 北极星 Phase 2c 地形 GPU 位移 A/B 运行时开关(设备侧前后对比用)。
// on=启用共享位移模板路径(Stage A 贴椭球);off=回现 per-tile baked VBO。
JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSetGpuTerrain(
    JNIEnv* /* env */, jobject /* this */, jboolean enabled) {
    const bool on = (enabled == JNI_TRUE);
    gRenderThread.post([on]() {
        if (!gEngine) return;
        gEngine->setTerrainGpuDisplacementEnabled(on);
        LOGI("Terrain GPU displacement %s", on ? "ENABLED" : "disabled");
    });
}

// ⚠️ 开关的**真值在引擎里**,不在 Java 字段里。surface 重建 = 引擎全重建,
// 这个标志会回到默认 true;Activity 旋转重建则会把 Java 侧字段清回默认。
// 两边各存一份必然静默分叉 ⇒ UI 每次显示前回读这里,不自己记。
JNIEXPORT jboolean JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetGpuTerrain(
    JNIEnv* /* env */, jobject /* this */) {
    // 引擎未就绪时报默认值(Engine.h terrainGpuDisplacementEnabled_ = true),
    // 与"重建后引擎实际处于什么档"一致。
    auto on = std::make_shared<bool>(true);
    gRenderThread.runSync(
        [on]() {
            if (gEngine) *on = gEngine->terrainGpuDisplacementEnabled();
        },
        std::chrono::milliseconds(100));
    return *on ? JNI_TRUE : JNI_FALSE;
}

// 编辑模式的真值是这个 atomic(UI 线程写、两线程读),无需绕渲染线程。
JNIEXPORT jboolean JNICALL
Java_com_earthengine_sdk_GLESView_nativeGetEditMode(
    JNIEnv* /* env */, jobject /* this */) {
    return gEditMode.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeSetEditMode(
    JNIEnv* /* env */, jobject /* this */, jboolean enabled) {
    const bool on = (enabled == JNI_TRUE);
    gEditMode.store(on, std::memory_order_relaxed);
    gRenderThread.post([on]() {
        // 关闭时若拖拽中:cancel(不落库,弹掉抓取时压入的 undo 快照)。
        if (!on && gEditDrag.active && gDemoFeatureLayer) {
            gDemoFeatureLayer->endEditPreview();
            gEditDrag = EditDragState{};
            clearEditHandles();
            if (!gEditUndoStack.empty()) gEditUndoStack.pop_back();
        }
        // 退出编辑模式 = 取消选中,标签提权一并清除。
        if (!on && gDemoFeatureLayer) {
            gDemoFeatureLayer->setLabelPriorityFeature(kInvalidFeatureId);
        }
        LOGI("EditFlow: edit mode %s", on ? "ON" : "OFF");
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeUndoEdit(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (!gDemoFeatureLayer || gEditDrag.active) return;
        if (gEditUndoStack.empty()) {
            LOGI("EditFlow: undo stack empty");
            return;
        }
        Feature snapshot = gEditUndoStack.back();
        gEditUndoStack.pop_back();
        snapshot.bounds = Rectangle();  // store 从 rings 重算
        gDemoFeatureLayer->store().updateFeature(snapshot);
        LOGI("EditFlow: undo feature=%llu → version=%llu undoDepth=%zu",
             static_cast<unsigned long long>(snapshot.id),
             static_cast<unsigned long long>(
                 gDemoFeatureLayer->store().getFeature(snapshot.id)->version),
             gEditUndoStack.size());
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativePause(
    JNIEnv* /* env */, jobject /* this */) {
    cancelInputIfNeeded();
    gRenderThread.setPaused(true);  // 暂停帧回调；任务队列仍在服务
    gRenderThread.post([]() {
        if (gPlatformBridge) {
            gPlatformBridge->onEnterBackground();
        }
    });
}

JNIEXPORT void JNICALL
Java_com_earthengine_sdk_GLESView_nativeResume(
    JNIEnv* /* env */, jobject /* this */) {
    gRenderThread.post([]() {
        if (gPlatformBridge) {
            gPlatformBridge->onEnterForeground();
        }
    });
    gRenderThread.setPaused(false);
}

} // extern "C"
