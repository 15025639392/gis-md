#include "CurlMultiRequestScheduler.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace earth_engine {
namespace {

constexpr int kMaxActiveRequests = 8;

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

} // namespace

struct CurlMultiRequestScheduler::Impl {
    struct RequestState {
        uint64_t sequence = 0;
        std::string url;
        std::function<void(int, std::vector<uint8_t>)> callback;
        HttpRequestPriority priority = HttpRequestPriority::Normal;
        std::vector<uint8_t> body;
        CURL* easy = nullptr;
        bool active = false;
        std::atomic<bool> cancelled{false};
    };

    class RequestHandle : public HttpRequest {
    public:
        RequestHandle(Impl* scheduler, std::weak_ptr<RequestState> state)
            : scheduler_(scheduler), state_(std::move(state)) {}
        ~RequestHandle() override { cancel(); }
        void cancel() override {
            if (auto state = state_.lock()) {
                state->cancelled.store(true, std::memory_order_release);
                if (scheduler_) {
                    scheduler_->wake();
                }
            }
        }

    private:
        Impl* scheduler_ = nullptr;
        std::weak_ptr<RequestState> state_;
    };

    Impl() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        multi = curl_multi_init();
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
                pending.push_back(state);
            }
        }

        if (state->cancelled.load(std::memory_order_acquire)) {
            if (state->callback) {
                state->callback(-1, {});
            }
        } else {
            wake();
        }

        return std::make_unique<RequestHandle>(this, state);
    }

    void cancelQueuedRequests() {
        std::deque<std::shared_ptr<RequestState>> cancelled;
        {
            std::lock_guard<std::mutex> lk(mutex);
            cancelled.swap(pending);
        }

        for (auto& request : cancelled) {
            request->cancelled.store(true, std::memory_order_release);
            if (request->callback) {
                request->callback(-1, {});
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
                for (auto& request : pending) {
                    request->cancelled.store(true, std::memory_order_release);
                }
                pending.clear();
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
        cv.notify_one();
        if (multi) {
            curl_multi_wakeup(multi);
        }
    }

    CURLM* multi = nullptr;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<RequestState>> pending;
    std::unordered_map<CURL*, std::shared_ptr<RequestState>> active;
    std::thread worker;
    std::atomic<uint64_t> nextSequence{1};
    bool stopping = false;

private:
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
                if (stopping && pending.empty() && active.empty()) {
                    break;
                }
                if (active.empty() && pending.empty()) {
                    cv.wait(lk, [this]() {
                        return stopping || !pending.empty();
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
                if (stopping || active.size() >= kMaxActiveRequests ||
                    pending.empty()) {
                    return;
                }

                auto best = std::max_element(
                    pending.begin(),
                    pending.end(),
                    [](const auto& a, const auto& b) {
                        const int pa = priorityValue(a->priority);
                        const int pb = priorityValue(b->priority);
                        if (pa != pb) {
                            return pa < pb;
                        }
                        return a->sequence > b->sequence;
                    });
                request = *best;
                pending.erase(best);
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
            curl_easy_cleanup(easy);
            request->easy = nullptr;
            request->active = false;

            if (request->cancelled.load(std::memory_order_acquire)) {
                continue;
            }

            const int statusCode = message->data.result == CURLE_OK
                ? static_cast<int>(httpCode)
                : -1;
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
        pending.clear();
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

CurlMultiRequestScheduler::CurlMultiRequestScheduler()
    : impl_(std::make_unique<Impl>()) {}

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

} // namespace earth_engine
