#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <queue>
#include <condition_variable>
#include <future>
#include <atomic>

namespace earth_engine {

/// A simple thread pool. Aligned with cesium-native CesiumAsync::ThreadPool
/// for offloading tile decode work to background threads.
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads = 0) : stop_(false) {
        if (numThreads == 0) {
            numThreads = std::max(1u, std::thread::hardware_concurrency());
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// Enqueue a task; returns a std::future for the result.
    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>> {
        using ReturnType = typename std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<ReturnType> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return result;
    }

    size_t threadCount() const { return workers_.size(); }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;
};

/// A simple Future<T> wrapper over std::future<T> with .then() chaining.
/// Aligned with cesium-native CesiumAsync::Future for ergonomic async.
template <typename T>
class Future {
public:
    Future() = default;
    explicit Future(std::future<T> f) : impl_(std::make_shared<std::future<T>>(std::move(f))) {}

    /// Block until the value is available.
    T get() {
        if (!impl_) throw std::runtime_error("Future: no value");
        return impl_->get();
    }

    /// Returns true if the value is ready (non-blocking).
    bool isReady() const {
        return impl_ && impl_->valid() &&
               impl_->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    /// Chain a transformation on the result.  Returns a new Future.
    template <typename F>
    auto then(F&& func) -> Future<decltype(func(std::declval<T>()))> {
        using R = decltype(func(std::declval<T>()));
        auto shared = impl_;
        auto p = std::make_shared<std::promise<R>>();
        auto f = p->get_future();
        std::thread([shared = std::move(shared), p = std::move(p),
                     func = std::forward<F>(func)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    shared->get();
                    func();
                    p->set_value();
                } else {
                    p->set_value(func(shared->get()));
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        }).detach();
        return Future<R>(std::move(f));
    }

    explicit operator bool() const { return impl_ && impl_->valid(); }

private:
    std::shared_ptr<std::future<T>> impl_;
};

/// Singleton thread pool for the engine.
class AsyncSystem {
public:
    static ThreadPool& pool() {
        static ThreadPool instance(std::max(1u, std::thread::hardware_concurrency()));
        return instance;
    }

    /// Run a callable on the thread pool and return a Future.
    template <typename F, typename... Args>
    static auto run(F&& f, Args&&... args) {
        return Future<typename std::invoke_result_t<F, Args...>>(
            pool().enqueue(std::forward<F>(f), std::forward<Args>(args)...));
    }
};

} // namespace earth_engine
