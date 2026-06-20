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

        const std::string path = parsePath(request);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            seenPaths_.push_back(path);
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

        const std::string body = path;
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Length: " +
            std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
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
    for (int i = 0; i < maximumActiveRequests; ++i) {
        handles.push_back(scheduler.get(
            server.url("/hold/" + std::to_string(i)),
            [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
            {HttpRequestPriority::Normal}));
    }
    ASSERT_TRUE(server.waitForSeenCount(
        static_cast<size_t>(maximumActiveRequests),
        3s));

    handles.push_back(scheduler.get(
        server.url("/low"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::Low}));
    handles.push_back(scheduler.get(
        server.url("/high"),
        [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
        {HttpRequestPriority::High}));

    server.releaseOne();
    ASSERT_TRUE(server.waitForPath("/high", 3s));
    EXPECT_FALSE(server.hasSeen("/low"));

    server.releaseAll();
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

TEST(CurlMultiRequestScheduler, UsesConfiguredMaximumActiveRequests) {
    LocalHttpServer server;
    CurlMultiRequestScheduler scheduler(2);

    EXPECT_EQ(scheduler.maximumActiveRequests(), 2);

    std::vector<std::unique_ptr<HttpRequest>> handles;
    std::atomic<int> callbacks{0};
    for (int i = 0; i < 3; ++i) {
        handles.push_back(scheduler.get(
            server.url("/hold/" + std::to_string(i)),
            [&](int, std::vector<uint8_t>) { callbacks.fetch_add(1); },
            {HttpRequestPriority::Normal}));
    }

    ASSERT_TRUE(server.waitForSeenCount(2, 3s));
    EXPECT_FALSE(server.hasSeen("/hold/2"));

    server.releaseOne();
    ASSERT_TRUE(server.waitForPath("/hold/2", 3s));

    server.releaseAll();
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
    for (int i = 0; i < maximumActiveRequests; ++i) {
        blockers.push_back(scheduler.get(
            server.url("/hold/" + std::to_string(i)),
            [](int, std::vector<uint8_t>) {},
            {HttpRequestPriority::Normal}));
    }
    ASSERT_TRUE(server.waitForSeenCount(
        static_cast<size_t>(maximumActiveRequests),
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
