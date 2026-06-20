#include "CurlMultiRequestScheduler.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace earth_engine {
namespace {

size_t writeBody(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* body = static_cast<std::vector<uint8_t>*>(userp);
    const size_t total = size * nmemb;
    const auto* begin = static_cast<const uint8_t*>(contents);
    body->insert(body->end(), begin, begin + total);
    return total;
}

int priorityValue(HttpRequestPriority priority) {
    return static_cast<int>(priority);
}

size_t priorityBucket(HttpRequestPriority priority) {
    const int value = std::clamp(priorityValue(priority), 0, 2);
    return static_cast<size_t>(value);
}

} // namespace

struct CurlMultiRequestScheduler::Impl {
    struct RequestState {
        uint64_t sequence = 0;
        std::string url;
        std::function<void(int, std::vector<uint8_t>)> callback;
        HttpRequestPriority priority = HttpRequestPriority::Normal;
        std::vector<uint8_t> body;
        std::array<char, CURL_ERROR_SIZE> errorBuffer{};
        CURL* easy = nullptr;
        bool active = false;
        std::atomic<bool> cancelled{false};
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

    explicit Impl(int maximumActiveRequestsValue)
        : maximumActiveRequests(std::max(1, maximumActiveRequestsValue)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
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
        curl_global_cleanup();
    }

    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions options) {
        auto state = std::make_shared<RequestState>();
        state->sequence = nextSequence++;
        state->url = url;
        state->callback = std::move(callback);
        state->priority = options.priority;

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

    void cancelQueuedRequests() {
        std::array<std::deque<std::shared_ptr<RequestState>>, 3> cancelled;
        {
            std::lock_guard<std::mutex> lk(mutex);
            cancelled.swap(pending);
        }

        for (auto& bucket : cancelled) {
            for (auto& request : bucket) {
                request->cancelled.store(true, std::memory_order_release);
                if (request->callback) {
                    request->callback(-1, {});
                }
            }
        }
        wake();
    }

    void shutdown() {
        bool shouldJoin = false;
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (!stopping) {
                stopping = true;
                for (auto& bucket : pending) {
                    for (auto& request : bucket) {
                        request->cancelled.store(true, std::memory_order_release);
                    }
                    bucket.clear();
                }
                for (auto& [easy, request] : active) {
                    request->cancelled.store(true, std::memory_order_release);
                }
                shouldJoin = worker.joinable();
            }
        }
        wake();

        if (shouldJoin) {
            worker.join();
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

    std::shared_ptr<RequestState> popNextPendingLocked() {
        for (size_t i = pending.size(); i > 0; --i) {
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

    void run() {
        while (true) {
            startPendingRequests();
            cancelActiveRequests();

            int running = 0;
            if (multi) {
                curl_multi_perform(multi, &running);
                drainCompletedRequests();
            }

            {
                std::unique_lock<std::mutex> lk(mutex);
                if (stopping && pendingEmptyLocked() && active.empty()) {
                    break;
                }
                if (active.empty() && pendingEmptyLocked()) {
                    cv.wait(lk, [this]() {
                        return stopping || !pendingEmptyLocked();
                    });
                    continue;
                }
            }

            if (multi) {
                int numFds = 0;
                curl_multi_wait(multi, nullptr, 0, 50, &numFds);
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
                        static_cast<size_t>(maximumActiveRequests) ||
                    pendingEmptyLocked()) {
                    return;
                }

                request = popNextPendingLocked();
            }

            if (!request) {
                continue;
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
        curl_easy_setopt(easy, CURLOPT_URL, request->url.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeBody);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &request->body);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "earth-md/0.1");
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(easy, CURLOPT_PRIVATE, request.get());
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, request->errorBuffer.data());

        if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
            curl_easy_cleanup(easy);
            request->easy = nullptr;
            request->active = false;
            return false;
        }

        std::lock_guard<std::mutex> lk(mutex);
        active[easy] = request;
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
                curl_easy_cleanup(request->easy);
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

            if (request->cancelled.load(std::memory_order_acquire)) {
                curl_easy_cleanup(easy);
                request->easy = nullptr;
                request->active = false;
                continue;
            }

            const int statusCode = curlResult == CURLE_OK
                ? static_cast<int>(httpCode)
                : -1;
#ifdef __ANDROID__
            if (statusCode != 200) {
                const char* detail = request->errorBuffer[0] != '\0'
                    ? request->errorBuffer.data()
                    : curl_easy_strerror(curlResult);
                __android_log_print(
                    ANDROID_LOG_WARN,
                    "CurlScheduler",
                    "request failed curl=%d (%s) http=%ld bytes=%zu url=%s",
                    static_cast<int>(curlResult),
                    detail,
                    httpCode,
                    request->body.size(),
                    request->url.c_str());
            }
#endif
            curl_easy_cleanup(easy);
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
        std::vector<CURL*> handles;
        {
            std::lock_guard<std::mutex> lk(mutex);
            for (auto& [easy, request] : active) {
                handles.push_back(easy);
                request->easy = nullptr;
                request->active = false;
            }
            active.clear();
        }
        for (CURL* easy : handles) {
            curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);
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

    auto request = get(url, [state](int code, std::vector<uint8_t> body) {
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            if (code == 200) {
                state->result = std::move(body);
            }
            state->done = true;
        }
        state->cv.notify_one();
    }, options);

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
