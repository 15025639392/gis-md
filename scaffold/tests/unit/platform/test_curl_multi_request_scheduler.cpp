#include "../../../src/earth_engine/platform/bridge/CurlMultiRequestScheduler.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace earth_engine;
using namespace std::chrono_literals;

namespace {

class LocalHttpServer {
public:
    struct SeenRequest {
        std::string method;
        std::string path;
        std::string headers;
        std::string body;
    };

    LocalHttpServer() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(listenFd_ >= 0, "socket");

        int yes = 1;
        require(
            ::setsockopt(
                listenFd_,
                SOL_SOCKET,
                SO_REUSEADDR,
                &yes,
                sizeof(yes)) == 0,
            "setsockopt");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(
            ::bind(
                listenFd_,
                reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) == 0,
            "bind");
        require(::listen(listenFd_, 64) == 0, "listen");

        socklen_t len = sizeof(addr);
        require(
            ::getsockname(
                listenFd_,
                reinterpret_cast<sockaddr*>(&addr),
                &len) == 0,
            "getsockname");
        port_ = ntohs(addr.sin_port);

        acceptThread_ = std::thread([this]() { acceptLoop(); });
    }

    ~LocalHttpServer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
            stopping_ = true;
        }
        cv_.notify_all();
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    bool waitForSeenCount(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&]() {
            return seenPaths_.size() >= count;
        });
    }

    bool waitForPath(
        const std::string& path,
        std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&]() {
            return containsLocked(path);
        });
    }

    bool hasSeen(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return containsLocked(path);
    }

    std::vector<std::string> seenPaths() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return seenPaths_;
    }

    std::vector<SeenRequest> seenRequests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return seenRequests_;
    }

    void releaseOne() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++releaseBudget_;
        }
        cv_.notify_all();
    }

    void releaseAll() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    static void require(bool condition, const char* operation) {
        if (!condition) {
            throw std::runtime_error(
                std::string(operation) + " failed: " + std::strerror(errno));
        }
    }

    void acceptLoop() {
        while (true) {
            int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd < 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    return;
                }
                continue;
            }
            workers_.emplace_back([this, fd]() { handleConnection(fd); });
        }
    }

    void handleConnection(int fd) {
        std::string request;
        char buffer[512];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            request.append(buffer, buffer + n);
        }

        const size_t headerEnd = request.find("\r\n\r\n");
        const std::string headers = request.substr(0, headerEnd + 4);
        const size_t contentLength = parseContentLength(headers);
        while (request.size() < headerEnd + 4 + contentLength) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            request.append(buffer, buffer + n);
        }

        const std::string method = parseMethod(request);
        const std::string path = parsePath(request);
        const std::string body =
            request.substr(headerEnd + 4, contentLength);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            seenPaths_.push_back(path);
            seenRequests_.push_back(SeenRequest{
                method,
                path,
                headers,
                body});
        }
        cv_.notify_all();

        if (path.rfind("/hold/", 0) == 0) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&]() {
                return released_ || releaseBudget_ > 0 || stopping_;
            });
            if (releaseBudget_ > 0) {
                --releaseBudget_;
            }
        }

        const std::string responseBody = path;
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Length: " +
            std::to_string(responseBody.size()) +
            "\r\nConnection: close\r\n\r\n" + responseBody;
        ::send(fd, response.data(), response.size(), 0);
        ::close(fd);
    }

    static std::string parsePath(const std::string& request) {
        const size_t firstSpace = request.find(' ');
        if (firstSpace == std::string::npos) return "/";
        const size_t secondSpace = request.find(' ', firstSpace + 1);
        if (secondSpace == std::string::npos) return "/";
        return request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    }

    static std::string parseMethod(const std::string& request) {
        const size_t firstSpace = request.find(' ');
        if (firstSpace == std::string::npos) return "";
        return request.substr(0, firstSpace);
    }

    static size_t parseContentLength(const std::string& headers) {
        std::string lower = headers;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        const std::string key = "content-length:";
        const size_t start = lower.find(key);
        if (start == std::string::npos) {
            return 0;
        }
        size_t valueStart = start + key.size();
        while (valueStart < lower.size() &&
               std::isspace(static_cast<unsigned char>(lower[valueStart]))) {
            ++valueStart;
        }
        size_t valueEnd = valueStart;
        while (valueEnd < lower.size() &&
               std::isdigit(static_cast<unsigned char>(lower[valueEnd]))) {
            ++valueEnd;
        }
        if (valueEnd == valueStart) {
            return 0;
        }
        return static_cast<size_t>(
            std::stoul(lower.substr(valueStart, valueEnd - valueStart)));
    }

    bool containsLocked(const std::string& path) const {
        return std::find(seenPaths_.begin(), seenPaths_.end(), path) !=
               seenPaths_.end();
    }

    int listenFd_ = -1;
    uint16_t port_ = 0;
    std::thread acceptThread_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> seenPaths_;
    std::vector<SeenRequest> seenRequests_;
    bool released_ = false;
    bool stopping_ = false;
    int releaseBudget_ = 0;
};

} // namespace

TEST(CurlMultiRequestScheduler, StartsHighPriorityBeforeLowWhenSlotOpens) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler;

    std::vector<std::unique_ptr<HttpRequest>> handles;
    std::atomic<int> callbacks{0};
    const int maximumActiveRequests = scheduler.maximumActiveRequests();
    EXPECT_EQ(
        maximumActiveRequests,
        CurlMultiRequestScheduler::kDefaultMaximumActiveRequests);
    EXPECT_EQ(maximumActiveRequests, 20);
    // P2-6:非 High 只能占用 max-2 个槽(2 个保留给 High)。
    const int nonUrgentCapacity = maximumActiveRequests - 2;
    for (int i = 0; i < maximumActiveRequests; ++i) {
        handles.push_back(scheduler.get(
            server.url("/hold/" + std::to_string(i)),
            [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
            {HttpRequestPriority::Normal}));
    }
    ASSERT_TRUE(server.waitForSeenCount(
        static_cast<size_t>(nonUrgentCapacity),
        3s));

    handles.push_back(scheduler.get(
        server.url("/low"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::Low}));
    // High 直接使用保留槽启动,无需等待任何在飞请求完成。
    handles.push_back(scheduler.get(
        server.url("/high"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::High}));

    ASSERT_TRUE(server.waitForPath("/high", 3s));
    EXPECT_FALSE(server.hasSeen("/low"));

    server.releaseAll();
    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, ReservesSlotsForHighPriorityRequests) {
    LocalHttpServer server;
    // max=3,保留 min(2, max-1)=2 → 非 High 并发上限 1。
    CurlMultiRequestScheduler scheduler(3);

    std::vector<std::unique_ptr<HttpRequest>> handles;
    handles.push_back(scheduler.get(
        server.url("/hold/low0"),
        [](int, std::vector<uint8_t>) {},
        {HttpRequestPriority::Normal}));
    ASSERT_TRUE(server.waitForPath("/hold/low0", 3s));

    // 第二个非 High 被保留槽门控,不得启动。
    handles.push_back(scheduler.get(
        server.url("/hold/low1"),
        [](int, std::vector<uint8_t>) {},
        {HttpRequestPriority::Normal}));

    // 两个 High 用满保留槽,与持槽的 low0 并发。
    handles.push_back(scheduler.get(
        server.url("/hold/high0"),
        [](int, std::vector<uint8_t>) {},
        {HttpRequestPriority::High}));
    handles.push_back(scheduler.get(
        server.url("/hold/high1"),
        [](int, std::vector<uint8_t>) {},
        {HttpRequestPriority::High}));
    ASSERT_TRUE(server.waitForPath("/hold/high0", 3s));
    ASSERT_TRUE(server.waitForPath("/hold/high1", 3s));
    EXPECT_FALSE(server.hasSeen("/hold/low1"));

    // 持槽请求释放后,被门控的 low1 不会被饿死。
    server.releaseAll();
    ASSERT_TRUE(server.waitForPath("/hold/low1", 3s));

    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, PreservesFifoWithinSamePriority) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(1);

    std::vector<std::unique_ptr<HttpRequest>> handles;
    std::atomic<int> callbacks{0};
    handles.push_back(scheduler.get(
        server.url("/hold/active"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::Normal}));
    ASSERT_TRUE(server.waitForPath("/hold/active", 3s));

    handles.push_back(scheduler.get(
        server.url("/high/first"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::High}));
    handles.push_back(scheduler.get(
        server.url("/high/second"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::High}));

    server.releaseOne();
    ASSERT_TRUE(server.waitForPath("/high/first", 3s));
    EXPECT_FALSE(server.hasSeen("/high/second"));

    server.releaseAll();
    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, PostsBodyWithContentType) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(1);

    std::mutex mutex;
    std::condition_variable cv;
    int statusCode = 0;
    std::string responseBody;
    bool callbackReturned = false;

    const std::string payload = "{\"mapType\":\"satellite\"}";
    auto handle = scheduler.post(
        server.url("/v1/createSession?key=api-key"),
        std::vector<uint8_t>(payload.begin(), payload.end()),
        "application/json",
        [&](int code, std::vector<uint8_t> body) {
            std::lock_guard<std::mutex> lock(mutex);
            statusCode = code;
            responseBody.assign(body.begin(), body.end());
            callbackReturned = true;
            cv.notify_one();
        },
        {HttpRequestPriority::Normal});

    ASSERT_TRUE(server.waitForPath("/v1/createSession?key=api-key", 3s));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 3s, [&]() {
            return callbackReturned;
        }));
    }

    const std::vector<LocalHttpServer::SeenRequest> seen =
        server.seenRequests();
    ASSERT_EQ(1u, seen.size());
    EXPECT_EQ("POST", seen[0].method);
    EXPECT_EQ("/v1/createSession?key=api-key", seen[0].path);
    EXPECT_NE(
        std::string::npos,
        seen[0].headers.find("Content-Type: application/json"));
    EXPECT_EQ(payload, seen[0].body);
    EXPECT_EQ(200, statusCode);
    EXPECT_EQ("/v1/createSession?key=api-key", responseBody);

    handle.reset();
    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, SendsConfiguredRequestHeaders) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(1);

    std::mutex mutex;
    std::condition_variable cv;
    bool callbackReturned = false;

    auto handle = scheduler.get(
        server.url("/headers"),
        [&](int, std::vector<uint8_t>) {
            std::lock_guard<std::mutex> lock(mutex);
            callbackReturned = true;
            cv.notify_one();
        },
        {HttpRequestPriority::Normal,
         {{"Authorization", "Bearer test-token-123"},
          {"X-Custom-Header", "custom-value"}}});

    ASSERT_TRUE(server.waitForPath("/headers", 3s));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 3s, [&]() {
            return callbackReturned;
        }));
    }

    const std::vector<LocalHttpServer::SeenRequest> seen =
        server.seenRequests();
    ASSERT_EQ(1u, seen.size());
    EXPECT_NE(
        std::string::npos,
        seen[0].headers.find("Authorization: Bearer test-token-123"));
    EXPECT_NE(
        std::string::npos,
        seen[0].headers.find("X-Custom-Header: custom-value"));

    handle.reset();
    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, UsesConfiguredMaximumActiveRequests) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(2);

    EXPECT_EQ(scheduler.maximumActiveRequests(), 2);

    // max=2,保留 min(2, max-1)=1 → 非 High 并发上限 1,总并发上限 2
    // (High 用满时)。
    std::vector<std::unique_ptr<HttpRequest>> handles;
    std::atomic<int> callbacks{0};
    for (int i = 0; i < 3; ++i) {
        handles.push_back(scheduler.get(
            server.url("/hold/" + std::to_string(i)),
            [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
            {HttpRequestPriority::Normal}));
    }

    ASSERT_TRUE(server.waitForSeenCount(1, 3s));
    EXPECT_FALSE(server.hasSeen("/hold/1"));
    EXPECT_FALSE(server.hasSeen("/hold/2"));

    handles.push_back(scheduler.get(
        server.url("/hold/high"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::High}));
    ASSERT_TRUE(server.waitForPath("/hold/high", 3s));

    handles.push_back(scheduler.get(
        server.url("/high2"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::High}));
    // 总并发已满(low0+high 各持一槽),第二个 High 也必须排队。
    EXPECT_FALSE(server.hasSeen("/high2"));

    server.releaseAll();
    ASSERT_TRUE(server.waitForPath("/high2", 3s));
    ASSERT_TRUE(server.waitForPath("/hold/1", 3s));
    ASSERT_TRUE(server.waitForPath("/hold/2", 3s));

    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, ClampsConfiguredMaximumActiveRequestsToOne) {
    CurlMultiRequestScheduler scheduler(0);

    EXPECT_EQ(scheduler.maximumActiveRequests(), 1);

    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, CancelledQueuedRequestNeverStartsOrCallbacks) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler;

    std::vector<std::unique_ptr<HttpRequest>> blockers;
    const int maximumActiveRequests = scheduler.maximumActiveRequests();
    // 非 High 只能占 max-2 个槽(P2-6 保留),填满即可阻住后续 Normal。
    const int nonUrgentCapacity = maximumActiveRequests - 2;
    for (int i = 0; i < nonUrgentCapacity; ++i) {
        blockers.push_back(scheduler.get(
            server.url("/hold/" + std::to_string(i)),
            [](int, std::vector<uint8_t>) {},
            {HttpRequestPriority::Normal}));
    }
    ASSERT_TRUE(server.waitForSeenCount(
        static_cast<size_t>(nonUrgentCapacity),
        3s));

    std::atomic<int> queuedCallbacks{0};
    auto queued = scheduler.get(
        server.url("/queued"),
        [&](int, std::vector<uint8_t>) { queuedCallbacks.fetch_add(1); },
        {HttpRequestPriority::Normal});
    queued->cancel();

    server.releaseOne();
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(server.hasSeen("/queued"));
    EXPECT_EQ(queuedCallbacks.load(), 0);

    server.releaseAll();
    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, CancelledActiveRequestFreesSlotForQueuedRequest) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(1);

    std::atomic<int> activeCallbacks{0};
    auto active = scheduler.get(
        server.url("/hold/active-cancel"),
        [&](int, std::vector<uint8_t>) { activeCallbacks.fetch_add(1); },
        {HttpRequestPriority::Normal});
    ASSERT_TRUE(server.waitForPath("/hold/active-cancel", 3s));

    std::atomic<int> queuedCallbacks{0};
    auto queued = scheduler.get(
        server.url("/queued-after-active-cancel"),
        [&](int, std::vector<uint8_t>) { queuedCallbacks.fetch_add(1); },
        {HttpRequestPriority::Normal});

    active->cancel();
    ASSERT_TRUE(server.waitForPath("/queued-after-active-cancel", 3s));
    EXPECT_EQ(activeCallbacks.load(), 0);

    server.releaseAll();
    for (int i = 0; i < 100 && queuedCallbacks.load() == 0; ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(queuedCallbacks.load(), 1);
    scheduler.shutdown();
}

TEST(CurlMultiRequestScheduler, RequestHandleMayOutliveScheduler) {
    LocalHttpServer server;
    std::unique_ptr<HttpRequest> handle;
    std::atomic<int> callbacks{0};

    {
        CurlMultiRequestScheduler scheduler(1);
        handle = scheduler.get(
            server.url("/hold/outlive"),
            [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
            {HttpRequestPriority::Normal});
        ASSERT_TRUE(server.waitForPath("/hold/outlive", 3s));
    }

    handle.reset();
    server.releaseAll();
    EXPECT_EQ(callbacks.load(), 0);
}

TEST(CurlMultiRequestScheduler, ShutdownMayBeRequestedFromCallback) {
    LocalHttpServer server;
    auto scheduler = std::make_unique<CurlMultiRequestScheduler>(1);

    std::mutex mutex;
    std::condition_variable cv;
    bool callbackReturned = false;

    auto handle = scheduler->get(
        server.url("/callback-shutdown"),
        [&](int code, std::vector<uint8_t>) {
            EXPECT_EQ(code, 200);
            scheduler->shutdown();
            {
                std::lock_guard<std::mutex> lock(mutex);
                callbackReturned = true;
            }
            cv.notify_one();
        },
        {HttpRequestPriority::Normal});

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 3s, [&]() {
            return callbackReturned;
        }));
    }

    handle.reset();
    scheduler.reset();
}

TEST(CurlMultiRequestScheduler, ShutdownWakesQueuedBlockingRequest) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(1);

    auto active = scheduler.get(
        server.url("/hold/blocking-active"),
        [](int, std::vector<uint8_t>) {},
        {HttpRequestPriority::Normal});
    ASSERT_TRUE(server.waitForPath("/hold/blocking-active", 3s));

    std::atomic<bool> returned{false};
    std::thread blockingThread([&]() {
        const std::vector<uint8_t> body = scheduler.getBlocking(
            server.url("/queued-blocking"),
            {HttpRequestPriority::Normal},
            5s);
        EXPECT_TRUE(body.empty());
        returned.store(true);
    });

    std::this_thread::sleep_for(100ms);
    scheduler.shutdown();
    blockingThread.join();
    server.releaseAll();

    EXPECT_TRUE(returned.load());
    EXPECT_FALSE(server.hasSeen("/queued-blocking"));
}

TEST(CurlMultiRequestScheduler, ShutdownWakesActiveBlockingRequest) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(1);

    std::atomic<bool> returned{false};
    std::thread blockingThread([&]() {
        const std::vector<uint8_t> body = scheduler.getBlocking(
            server.url("/hold/active-blocking"),
            {HttpRequestPriority::Normal},
            5s);
        EXPECT_TRUE(body.empty());
        returned.store(true);
    });

    ASSERT_TRUE(server.waitForPath("/hold/active-blocking", 3s));
    scheduler.shutdown();
    blockingThread.join();
    server.releaseAll();

    EXPECT_TRUE(returned.load());
}

TEST(CurlMultiRequestScheduler, SchedulersDoNotCleanupGlobalCurlForEachOther) {
    LocalHttpServer server;
    std::atomic<int> callbacks{0};

    CurlMultiRequestScheduler owner(1);
    auto active = owner.get(
        server.url("/hold/owner"),
        [&](int code, std::vector<uint8_t> body) {
            if (code == 200 && std::string(body.begin(), body.end()) ==
                                   "/hold/owner") {
                callbacks.fetch_add(1);
            }
        },
        {HttpRequestPriority::Normal});
    ASSERT_TRUE(server.waitForPath("/hold/owner", 3s));

    {
        CurlMultiRequestScheduler temporary(1);
    }

    server.releaseAll();
    for (int i = 0; i < 100 && callbacks.load() == 0; ++i) {
        std::this_thread::sleep_for(20ms);
    }
    owner.shutdown();
    EXPECT_EQ(callbacks.load(), 1);
}

// ---------------------------------------------------------------------------
// externalCancel 桥接(HttpRequestOptions.cancelFlag):上层不持句柄也能真取
// 消。两条契约:①传输被中止、槽位腾出;②回调恰好一次(code=-1)——引擎侧
// retired-token 清账依赖它,丢了会在销毁时死等。
// ---------------------------------------------------------------------------

TEST(CurlMultiRequestScheduler, ExternalCancelFlagAbortsActiveFiresCallbackOnce) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(1);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::atomic<int> cancelledCallbacks{0};
    std::atomic<int> lastCode{0};
    HttpRequestOptions options{HttpRequestPriority::Normal};
    options.cancelFlag = flag;
    auto active = scheduler.get(
        server.url("/hold/external-cancel"),
        [&](int code, std::vector<uint8_t>) {
            cancelledCallbacks.fetch_add(1);
            lastCode.store(code);
        },
        options);
    ASSERT_TRUE(server.waitForPath("/hold/external-cancel", 3s));

    std::atomic<int> queuedCallbacks{0};
    auto queued = scheduler.get(
        server.url("/queued-after-external-cancel"),
        [&](int, std::vector<uint8_t>) { queuedCallbacks.fetch_add(1); },
        {HttpRequestPriority::Normal});

    flag->store(true, std::memory_order_release);
    // 无 wake 兜底:工作线程 poll 上限 50ms,3s 内必然传播。
    ASSERT_TRUE(server.waitForPath("/queued-after-external-cancel", 3s))
        << "外部旗标未腾出唯一并发槽";
    for (int i = 0; i < 100 && cancelledCallbacks.load() == 0; ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(1, cancelledCallbacks.load()) << "桥接取消必须回调恰好一次";
    EXPECT_EQ(-1, lastCode.load());

    server.releaseAll();
    scheduler.shutdown();
    EXPECT_EQ(1, cancelledCallbacks.load()) << "shutdown 不得二次回调";
}

TEST(CurlMultiRequestScheduler, ExternalCancelFlagDropsQueuedWithSingleCallback) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler;

    std::vector<std::unique_ptr<HttpRequest>> blockers;
    const int nonUrgentCapacity = scheduler.maximumActiveRequests() - 2;
    for (int i = 0; i < nonUrgentCapacity; ++i) {
        blockers.push_back(scheduler.get(
            server.url("/hold/ext/" + std::to_string(i)),
            [](int, std::vector<uint8_t>) {},
            {HttpRequestPriority::Normal}));
    }
    ASSERT_TRUE(server.waitForSeenCount(
        static_cast<size_t>(nonUrgentCapacity), 3s));

    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::atomic<int> callbacks{0};
    HttpRequestOptions options{HttpRequestPriority::Normal};
    options.cancelFlag = flag;
    auto queued = scheduler.get(
        server.url("/queued-external-cancel"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        options);
    flag->store(true, std::memory_order_release);

    for (int i = 0; i < 100 && callbacks.load() == 0; ++i) {
        std::this_thread::sleep_for(20ms);
    }
    // 排队项被丢弃:回调一次(与 cancelQueuedRequests 同契约),且从未上网。
    EXPECT_EQ(1, callbacks.load());
    EXPECT_FALSE(server.hasSeen("/queued-external-cancel"));

    server.releaseAll();
    scheduler.shutdown();
    EXPECT_EQ(1, callbacks.load());
}
