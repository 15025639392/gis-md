#include "CurlMultiRequestScheduler.h"

#include "CaCertBundle.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../../debug/PlatformLog.h"

namespace earth_engine {
namespace {

int priorityValue(HttpRequestPriority priority) {
    return static_cast<int>(priority);
}

size_t priorityBucket(HttpRequestPriority priority) {
    const int value = std::clamp(priorityValue(priority), 0, 2);
    return static_cast<size_t>(value);
}

struct CurlGlobalLifetime {
    CurlGlobalLifetime() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~CurlGlobalLifetime() {
        curl_global_cleanup();
    }
};

CurlGlobalLifetime& curlGlobalLifetime() {
    static CurlGlobalLifetime lifetime;
    return lifetime;
}

} // namespace

struct CurlMultiRequestScheduler::Impl {
    struct RequestState {
        uint64_t sequence = 0;
        std::string url;
        std::string method = "GET";
        std::vector<uint8_t> uploadBody;
        std::string contentType;
        std::vector<HttpRequestOptions::Header> requestHeaders;
        std::function<void(int, std::vector<uint8_t>)> callback;
        HttpRequestPriority priority = HttpRequestPriority::Normal;
        std::vector<uint8_t> body;
        std::array<char, CURL_ERROR_SIZE> errorBuffer{};
        CURL* easy = nullptr;
        curl_slist* headers = nullptr;
        bool active = false;
        bool notifyCallbackOnShutdown = false;
        std::atomic<bool> cancelled{false};
        std::chrono::steady_clock::time_point queuedAt;
        std::chrono::steady_clock::time_point startedAt;
    };

    struct WakeState {
        std::mutex mutex;
        std::condition_variable* cv = nullptr;
        CURLM* multi = nullptr;
        bool alive = false;
    };

    class RequestHandle : public HttpRequest {
    public:
        RequestHandle(std::weak_ptr<WakeState> wakeState,
                      std::weak_ptr<RequestState> state)
            : wakeState_(std::move(wakeState)), state_(std::move(state)) {}
        ~RequestHandle() override { cancel(); }
        void cancel() override {
            if (auto state = state_.lock()) {
                state->cancelled.store(true, std::memory_order_release);
                wake(wakeState_);
            }
        }

    private:
        static void wake(const std::weak_ptr<WakeState>& weakWakeState) {
            if (auto wakeState = weakWakeState.lock()) {
                std::lock_guard<std::mutex> lock(wakeState->mutex);
                if (!wakeState->alive) {
                    return;
                }
                if (wakeState->cv) {
                    wakeState->cv->notify_one();
                }
                if (wakeState->multi) {
                    curl_multi_wakeup(wakeState->multi);
                }
            }
        }

        std::weak_ptr<WakeState> wakeState_;
        std::weak_ptr<RequestState> state_;
    };

    static size_t writeBody(void* contents,
                            size_t size,
                            size_t nmemb,
                            void* userp) {
        auto* state = static_cast<RequestState*>(userp);
        const size_t total = size * nmemb;
        std::vector<uint8_t>& body = state->body;
        if (body.empty() && state->easy) {
            // 首块数据到达时按 Content-Length 一次性 reserve，避免大响应
            // 按 16KB 块 ~8-10 次 realloc 串在唯一网络线程上（P2-3）。
            // 注意启用压缩后该值是压缩后大小（解压数据仍会二次增长，但
            // 已消掉多数 realloc）；上限防御恶意超大声明。
            constexpr curl_off_t kMaxReserveBytes = 64LL * 1024 * 1024;
            curl_off_t contentLength = -1;
            if (curl_easy_getinfo(state->easy,
                                  CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                                  &contentLength) == CURLE_OK &&
                contentLength > 0 && contentLength <= kMaxReserveBytes) {
                body.reserve(static_cast<size_t>(contentLength));
            }
        }
        const auto* begin = static_cast<const uint8_t*>(contents);
        body.insert(body.end(), begin, begin + total);
        return total;
    }

    explicit Impl(int maximumActiveRequestsValue)
        : maximumActiveRequests(std::max(1, maximumActiveRequestsValue)) {
        (void)curlGlobalLifetime();
        multi = curl_multi_init();
        {
            std::lock_guard<std::mutex> lock(wakeState->mutex);
            wakeState->cv = &cv;
            wakeState->multi = multi;
            wakeState->alive = true;
        }
        worker = std::thread([this]() { run(); });
    }

    ~Impl() {
        shutdown();
    }

    std::unique_ptr<HttpRequest> request(
        std::string method,
        const std::string& url,
        std::vector<uint8_t> uploadBody,
        std::string contentType,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions options,
        bool notifyCallbackOnShutdown = false) {
        auto state = std::make_shared<RequestState>();
        state->sequence = nextSequence++;
        state->method = std::move(method);
        state->url = url;
        state->uploadBody = std::move(uploadBody);
        state->contentType = std::move(contentType);
        state->requestHeaders = std::move(options.headers);
        state->callback = std::move(callback);
        state->priority = options.priority;
        state->notifyCallbackOnShutdown = notifyCallbackOnShutdown;
        state->queuedAt = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lk(mutex);
            if (stopping) {
                state->cancelled.store(true, std::memory_order_release);
            } else {
                pending[priorityBucket(state->priority)].push_back(state);
            }
        }

        if (state->cancelled.load(std::memory_order_acquire)) {
            if (state->callback) {
                state->callback(-1, {});
            }
        } else {
            wake();
        }

        return std::make_unique<RequestHandle>(wakeState, state);
    }

    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions options,
        bool notifyCallbackOnShutdown = false) {
        return request(
            "GET",
            url,
            {},
            {},
            std::move(callback),
            options,
            notifyCallbackOnShutdown);
    }

    std::unique_ptr<HttpRequest> post(
        const std::string& url,
        std::vector<uint8_t> body,
        const std::string& contentType,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions options) {
        return request(
            "POST",
            url,
            std::move(body),
            contentType,
            std::move(callback),
            options);
    }

    void cancelQueuedRequests() {
        // 回调交给 curl 工作线程执行（与正常完成回调同一线程语义）。
        // 原实现在调用方线程内联执行全部排队回调——该入口由
        // onEnterBackground 等 UI 线程事件触发，会把回调链阻塞在 UI
        // 线程上（P2-22）。
        {
            std::lock_guard<std::mutex> lk(mutex);
            for (auto& bucket : pending) {
                for (auto& request : bucket) {
                    request->cancelled.store(true, std::memory_order_release);
                    cancelledQueuedCallbacks.push_back(std::move(request));
                }
                bucket.clear();
            }
        }
        wake();
    }

    void shutdown() {
        std::array<std::deque<std::shared_ptr<RequestState>>, 3>
            cancelledPending;
        std::vector<std::shared_ptr<RequestState>> cancelledActiveCallbacks;
        bool shouldJoin = false;
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (!stopping) {
                stopping = true;
                for (auto& bucket : pending) {
                    for (auto& request : bucket) {
                        request->cancelled.store(true, std::memory_order_release);
                    }
                }
                cancelledPending.swap(pending);
                for (auto& [easy, request] : active) {
                    request->cancelled.store(true, std::memory_order_release);
                    if (request->notifyCallbackOnShutdown) {
                        cancelledActiveCallbacks.push_back(request);
                    }
                }
            }
            shouldJoin = worker.joinable() &&
                         worker.get_id() != std::this_thread::get_id();
        }
        wake();

        if (shouldJoin) {
            worker.join();
        }

        for (auto& bucket : cancelledPending) {
            for (auto& request : bucket) {
                if (request->callback) {
                    request->callback(-1, {});
                }
            }
        }
        for (auto& request : cancelledActiveCallbacks) {
            if (request->callback) {
                request->callback(-1, {});
            }
        }
    }

    void wake() {
        std::lock_guard<std::mutex> lock(wakeState->mutex);
        if (!wakeState->alive) {
            return;
        }
        if (wakeState->cv) {
            wakeState->cv->notify_one();
        }
        if (wakeState->multi) {
            curl_multi_wakeup(wakeState->multi);
        }
    }

    CURLM* multi = nullptr;
    std::shared_ptr<WakeState> wakeState = std::make_shared<WakeState>();
    std::mutex mutex;
    std::condition_variable cv;
    std::array<std::deque<std::shared_ptr<RequestState>>, 3> pending;
    // cancelQueuedRequests 摘下的请求，回调由 curl 工作线程统一执行。
    std::deque<std::shared_ptr<RequestState>> cancelledQueuedCallbacks;
    std::unordered_map<CURL*, std::shared_ptr<RequestState>> active;
    std::thread worker;
    std::atomic<uint64_t> nextSequence{1};
    bool stopping = false;
    int maximumActiveRequests =
        CurlMultiRequestScheduler::kDefaultMaximumActiveRequests;

private:
    bool pendingEmptyLocked() const {
        return pending[0].empty() && pending[1].empty() &&
               pending[2].empty();
    }

    // P2-6:为 High 保留并发槽——低优先级预取不再占满全部槽位,让新视野
    // 的 High 请求排队等完整轮转(cesium 每帧重排优先级的折衷替代)。
    // High 桶只受总并发上限约束;非 High 只能占用 max - reserved 个槽。
    static constexpr size_t kReservedUrgentSlots = 2;

    std::shared_ptr<RequestState> popNextPendingGatedLocked() {
        auto& urgentBucket =
            pending[priorityBucket(HttpRequestPriority::High)];
        if (!urgentBucket.empty()) {
            auto request = urgentBucket.front();
            urgentBucket.pop_front();
            return request;
        }

        const size_t reserved = std::min(
            kReservedUrgentSlots,
            static_cast<size_t>(maximumActiveRequests) - 1);
        size_t nonUrgentActive = 0;
        for (const auto& entry : active) {
            if (entry.second->priority != HttpRequestPriority::High) {
                ++nonUrgentActive;
            }
        }
        if (nonUrgentActive >=
            static_cast<size_t>(maximumActiveRequests) - reserved) {
            return nullptr;
        }

        for (size_t i = pending.size() - 1; i > 0; --i) {
            auto& bucket = pending[i - 1];
            if (bucket.empty()) {
                continue;
            }
            auto request = bucket.front();
            bucket.pop_front();
            return request;
        }
        return nullptr;
    }

    // 在 curl 工作线程上执行 cancelQueuedRequests 摘下的请求回调。
    void drainCancelledQueuedCallbacks() {
        std::deque<std::shared_ptr<RequestState>> drained;
        {
            std::lock_guard<std::mutex> lk(mutex);
            drained.swap(cancelledQueuedCallbacks);
        }
        for (auto& request : drained) {
            if (request->callback) {
                request->callback(-1, {});
            }
        }
    }

    void run() {
        while (true) {
            drainCancelledQueuedCallbacks();
            startPendingRequests();
            cancelActiveRequests();
            startPendingRequests();

            int running = 0;
            if (multi) {
                curl_multi_perform(multi, &running);
                drainCompletedRequests();
            }

            {
                std::unique_lock<std::mutex> lk(mutex);
                if (stopping && pendingEmptyLocked() && active.empty() &&
                    cancelledQueuedCallbacks.empty()) {
                    break;
                }
                if (active.empty() && pendingEmptyLocked() &&
                    cancelledQueuedCallbacks.empty()) {
                    cv.wait(lk, [this]() {
                        return stopping || !pendingEmptyLocked() ||
                               !cancelledQueuedCallbacks.empty();
                    });
                    continue;
                }
            }

            if (multi) {
                // curl_multi_poll 而非 curl_multi_wait：
                // 1) curl_multi_wakeup 只能唤醒 poll，wait 收不到唤醒信号；
                // 2) numFds==0（DNS/建连窗口）时 wait 立即返回导致忙旋，
                //    poll 会真正睡满超时。
                int numFds = 0;
                curl_multi_poll(multi, nullptr, 0, 50, &numFds);
            }
        }

        cleanupPendingWithoutCallbacks();
        cleanupActiveWithoutCallbacks();
        {
            std::lock_guard<std::mutex> lock(wakeState->mutex);
            wakeState->cv = nullptr;
            wakeState->multi = nullptr;
            wakeState->alive = false;
        }
        if (multi) {
            curl_multi_cleanup(multi);
            multi = nullptr;
        }
    }

    void startPendingRequests() {
        while (true) {
            std::shared_ptr<RequestState> request;
            {
                std::lock_guard<std::mutex> lk(mutex);
                if (stopping || active.size() >=
                        static_cast<size_t>(maximumActiveRequests)) {
                    return;
                }

                request = popNextPendingGatedLocked();
            }

            if (!request) {
                // 队列为空,或非 High 请求被保留槽门控(等在飞请求轮转)。
                return;
            }

            if (request->cancelled.load(std::memory_order_acquire)) {
                continue;
            }
            if (!startRequest(request)) {
                request->cancelled.store(true, std::memory_order_release);
                if (request->callback) {
                    request->callback(-1, {});
                }
            }
        }
    }

    bool startRequest(const std::shared_ptr<RequestState>& request) {
        CURL* easy = curl_easy_init();
        if (!easy) {
            return false;
        }

        request->easy = easy;
        request->active = true;
        request->startedAt = std::chrono::steady_clock::now();
        curl_easy_setopt(easy, CURLOPT_URL, request->url.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Impl::writeBody);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, request.get());
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "earth-md/0.1");
        // 空字符串 = 声明 libcurl 支持的全部压缩编码并自动解压。
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(easy, CURLOPT_PRIVATE, request.get());
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, request->errorBuffer.data());
        // vcpkg libcurl 无内置 CA 路径；Android app 沙箱又无法可靠使用系统 CA
        // 目录(/system/etc/security/cacerts) —— OpenSSL 报 curl=60 "unable to
        // get local issuer certificate"(SELinux/哈希目录格式)。故内嵌 Mozilla
        // CA 包用内存 blob 做证书校验，一次性修复所有 HTTPS(cesium 及任何 HTTPS
        // 源)；HTTP 源不受影响。VERIFYPEER 保持默认开启(安全)。NOCOPY：数据是
        // 静态存储，curl 只引用不拷贝。
        curl_blob caBlob;
        caBlob.data = const_cast<char*>(kCaCertBundlePem);
        caBlob.len = sizeof(kCaCertBundlePem) - 1;
        caBlob.flags = CURL_BLOB_NOCOPY;
        curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &caBlob);
        for (const auto& header : request->requestHeaders) {
            const std::string value = header.first + ": " + header.second;
            request->headers =
                curl_slist_append(request->headers, value.c_str());
        }
        if (request->method == "POST") {
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            curl_easy_setopt(
                easy,
                CURLOPT_POSTFIELDS,
                request->uploadBody.empty()
                    ? ""
                    : reinterpret_cast<const char*>(
                          request->uploadBody.data()));
            curl_easy_setopt(
                easy,
                CURLOPT_POSTFIELDSIZE,
                static_cast<long>(request->uploadBody.size()));
            if (!request->contentType.empty()) {
                const std::string header =
                    "Content-Type: " + request->contentType;
                request->headers =
                    curl_slist_append(request->headers, header.c_str());
            }
        }
        if (request->headers) {
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request->headers);
        }

        if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
            cleanupEasy(request);
            request->easy = nullptr;
            request->active = false;
            return false;
        }

        size_t activeCount = 0;
        {
            std::lock_guard<std::mutex> lk(mutex);
            active[easy] = request;
            activeCount = active.size();
        }
        const double queueMs =
            std::chrono::duration<double, std::milli>(
                request->startedAt - request->queuedAt)
                .count();
        platformLog(
            LogLevel::Info,
            "EarthNet",
            "start seq=%llu %s p=%d queue=%.1fms active=%zu url=%s",
            static_cast<unsigned long long>(request->sequence),
            request->method.c_str(),
            static_cast<int>(request->priority),
            queueMs,
            activeCount,
            request->url.c_str());
        return true;
    }

    void cancelActiveRequests() {
        std::vector<std::shared_ptr<RequestState>> cancelled;
        {
            std::lock_guard<std::mutex> lk(mutex);
            for (auto it = active.begin(); it != active.end();) {
                auto request = it->second;
                if (!request->cancelled.load(std::memory_order_acquire)) {
                    ++it;
                    continue;
                }
                cancelled.push_back(request);
                it = active.erase(it);
            }
        }

        for (auto& request : cancelled) {
            if (request->easy) {
                curl_multi_remove_handle(multi, request->easy);
                cleanupEasy(request);
                request->easy = nullptr;
            }
            request->active = false;
        }
    }

    void drainCompletedRequests() {
        int messages = 0;
        while (CURLMsg* message = curl_multi_info_read(multi, &messages)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }

            CURL* easy = message->easy_handle;
            std::shared_ptr<RequestState> request;
            {
                std::lock_guard<std::mutex> lk(mutex);
                auto it = active.find(easy);
                if (it != active.end()) {
                    request = it->second;
                    active.erase(it);
                }
            }

            curl_multi_remove_handle(multi, easy);

            if (!request) {
                curl_easy_cleanup(easy);
                continue;
            }

            long httpCode = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);
            const CURLcode curlResult = message->data.result;

            double dnsSeconds = 0.0;
            double connectSeconds = 0.0;
            double tlsSeconds = 0.0;
            double firstByteSeconds = 0.0;
            double transferSeconds = 0.0;
            curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME, &dnsSeconds);
            curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &connectSeconds);
            curl_easy_getinfo(easy, CURLINFO_APPCONNECT_TIME, &tlsSeconds);
            curl_easy_getinfo(
                easy, CURLINFO_STARTTRANSFER_TIME, &firstByteSeconds);
            curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &transferSeconds);
            const auto now = std::chrono::steady_clock::now();
            const double queueMs =
                std::chrono::duration<double, std::milli>(
                    request->startedAt - request->queuedAt)
                    .count();
            const double wallMs =
                std::chrono::duration<double, std::milli>(
                    now - request->queuedAt)
                    .count();

            if (request->cancelled.load(std::memory_order_acquire)) {
                cleanupEasy(request);
                request->easy = nullptr;
                request->active = false;
                continue;
            }

            const int statusCode = curlResult == CURLE_OK
                ? static_cast<int>(httpCode)
                : -1;
            platformLog(
                LogLevel::Info,
                "EarthNet",
                "seq=%llu %s p=%d http=%ld curl=%d queue=%.1fms "
                "dns=%.1f conn=%.1f tls=%.1f ttfb=%.1f xfer=%.1f "
                "wall=%.1f bytes=%zu url=%s",
                static_cast<unsigned long long>(request->sequence),
                request->method.c_str(),
                static_cast<int>(request->priority),
                httpCode,
                static_cast<int>(curlResult),
                queueMs,
                dnsSeconds * 1000.0,
                connectSeconds * 1000.0,
                tlsSeconds * 1000.0,
                firstByteSeconds * 1000.0,
                transferSeconds * 1000.0,
                wallMs,
                request->body.size(),
                request->url.c_str());
            if (statusCode != 200) {
                const char* detail = request->errorBuffer[0] != '\0'
                    ? request->errorBuffer.data()
                    : curl_easy_strerror(curlResult);
                platformLog(
                    LogLevel::Warning,
                    "CurlScheduler",
                    "request failed curl=%d (%s) http=%ld bytes=%zu url=%s",
                    static_cast<int>(curlResult),
                    detail,
                    httpCode,
                    request->body.size(),
                    request->url.c_str());
            }
            cleanupEasy(request);
            request->easy = nullptr;
            request->active = false;

            std::vector<uint8_t> result =
                statusCode == 200 ? std::move(request->body)
                                  : std::vector<uint8_t>{};
            if (request->callback) {
                request->callback(statusCode, std::move(result));
            }
        }
    }

    void cleanupPendingWithoutCallbacks() {
        std::lock_guard<std::mutex> lk(mutex);
        for (auto& bucket : pending) {
            bucket.clear();
        }
    }

    void cleanupActiveWithoutCallbacks() {
        std::vector<std::shared_ptr<RequestState>> requests;
        {
            std::lock_guard<std::mutex> lk(mutex);
            for (auto& [easy, request] : active) {
                (void)easy;
                requests.push_back(request);
            }
            active.clear();
        }
        for (auto& request : requests) {
            if (request->easy) {
                curl_multi_remove_handle(multi, request->easy);
                cleanupEasy(request);
                request->easy = nullptr;
            }
            request->active = false;
        }
    }

    void cleanupEasy(const std::shared_ptr<RequestState>& request) {
        if (request->easy) {
            curl_easy_cleanup(request->easy);
        }
        if (request->headers) {
            curl_slist_free_all(request->headers);
            request->headers = nullptr;
        }
    }
};

CurlMultiRequestScheduler& CurlMultiRequestScheduler::shared() {
    static CurlMultiRequestScheduler scheduler;
    return scheduler;
}

CurlMultiRequestScheduler::CurlMultiRequestScheduler(
    int maximumActiveRequests)
    : impl_(std::make_unique<Impl>(maximumActiveRequests)) {}

CurlMultiRequestScheduler::~CurlMultiRequestScheduler() = default;

std::unique_ptr<HttpRequest> CurlMultiRequestScheduler::get(
    const std::string& url,
    std::function<void(int, std::vector<uint8_t>)> callback,
    HttpRequestOptions options) {
    return impl_->get(url, std::move(callback), options);
}

std::unique_ptr<HttpRequest> CurlMultiRequestScheduler::post(
    const std::string& url,
    std::vector<uint8_t> body,
    const std::string& contentType,
    std::function<void(int, std::vector<uint8_t>)> callback,
    HttpRequestOptions options) {
    return impl_->post(
        url,
        std::move(body),
        contentType,
        std::move(callback),
        options);
}

std::vector<uint8_t> CurlMultiRequestScheduler::getBlocking(
    const std::string& url,
    HttpRequestOptions options,
    std::chrono::milliseconds timeout,
    std::function<bool()> shouldCancel) {
    struct BlockingState {
        std::vector<uint8_t> result;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<BlockingState>();

    auto request = impl_->get(url, [state](int code, std::vector<uint8_t> body) {
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            if (code == 200) {
                state->result = std::move(body);
            }
            state->done = true;
        }
        state->cv.notify_one();
    }, options, true);

    bool done = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    {
        std::unique_lock<std::mutex> lk(state->mutex);
        while (!state->done) {
            if (shouldCancel && shouldCancel()) {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }
            state->cv.wait_until(
                lk,
                std::min(deadline, now + std::chrono::milliseconds(20)));
        }
        done = state->done;
    }

    if ((!done || (shouldCancel && shouldCancel())) && request) {
        request->cancel();
    }
    return done ? std::move(state->result) : std::vector<uint8_t>{};
}

void CurlMultiRequestScheduler::cancelQueuedRequests() {
    impl_->cancelQueuedRequests();
}

void CurlMultiRequestScheduler::shutdown() {
    impl_->shutdown();
}

int CurlMultiRequestScheduler::maximumActiveRequests() const {
    return impl_->maximumActiveRequests;
}

} // namespace earth_engine
