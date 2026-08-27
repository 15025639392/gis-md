#pragma once

#include "../../debug/PlatformLog.h"

#include <functional>
#include <chrono>
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
    struct Stats {
        uint64_t enqueued = 0;
        uint64_t started = 0;
        uint64_t completed = 0;
        uint64_t queued = 0;
        uint64_t active = 0;
        double totalQueueWaitMs = 0.0;
        double maxQueueWaitMs = 0.0;
        double totalWorkMs = 0.0;
        double maxWorkMs = 0.0;
    };

    explicit ThreadPool(size_t numThreads = 0) : stop_(false) {
        if (numThreads == 0) {
            numThreads = std::max(1u, std::thread::hardware_concurrency());
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    QueuedTask queuedTask;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        queuedTask = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    const auto workStart = Clock::now();
                    const uint64_t queueWaitNs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            workStart - queuedTask.enqueuedAt)
                            .count());
                    started_.fetch_add(1, std::memory_order_relaxed);
                    active_.fetch_add(1, std::memory_order_relaxed);
                    totalQueueWaitNs_.fetch_add(queueWaitNs,
                                               std::memory_order_relaxed);
                    updateMax(maxQueueWaitNs_, queueWaitNs);
                    // An escaped exception would std::terminate the whole
                    // process and strand any completion callbacks the task
                    // owned. cesium-native's async++ swallows into the
                    // task state; we log and keep the worker alive.
                    try {
                        queuedTask.task();
                    } catch (const std::exception& e) {
                        platformLog(LogLevel::Error, "AsyncSystem",
                            "worker task threw: %s", e.what());
                    } catch (...) {
                        platformLog(LogLevel::Error, "AsyncSystem",
                            "worker task threw unknown exception");
                    }
                    const uint64_t workNs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now() - workStart)
                            .count());
                    totalWorkNs_.fetch_add(workNs, std::memory_order_relaxed);
                    updateMax(maxWorkNs_, workNs);
                    active_.fetch_sub(1, std::memory_order_relaxed);
                    completed_.fetch_add(1, std::memory_order_relaxed);
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
            tasks_.push(QueuedTask{[task]() { (*task)(); }, Clock::now()});
            enqueued_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
        return result;
    }

    size_t threadCount() const { return workers_.size(); }

    Stats stats() const {
        constexpr double kNsToMs = 1.0e-6;
        Stats result;
        result.enqueued = enqueued_.load(std::memory_order_relaxed);
        result.started = started_.load(std::memory_order_relaxed);
        result.completed = completed_.load(std::memory_order_relaxed);
        result.queued = result.enqueued > result.started
                            ? result.enqueued - result.started
                            : 0;
        result.active = active_.load(std::memory_order_relaxed);
        result.totalQueueWaitMs =
            static_cast<double>(
                totalQueueWaitNs_.load(std::memory_order_relaxed)) *
            kNsToMs;
        result.maxQueueWaitMs =
            static_cast<double>(maxQueueWaitNs_.load(std::memory_order_relaxed)) *
            kNsToMs;
        result.totalWorkMs =
            static_cast<double>(totalWorkNs_.load(std::memory_order_relaxed)) *
            kNsToMs;
        result.maxWorkMs =
            static_cast<double>(maxWorkNs_.load(std::memory_order_relaxed)) *
            kNsToMs;
        return result;
    }

private:
    using Clock = std::chrono::steady_clock;
    struct QueuedTask {
        std::function<void()> task;
        Clock::time_point enqueuedAt{};
    };

    static void updateMax(std::atomic<uint64_t>& target, uint64_t value) {
        uint64_t current = target.load(std::memory_order_relaxed);
        while (current < value &&
               !target.compare_exchange_weak(current, value,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
        }
    }

    std::vector<std::thread> workers_;
    std::queue<QueuedTask> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;
    std::atomic<uint64_t> enqueued_{0};
    std::atomic<uint64_t> started_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> active_{0};
    std::atomic<uint64_t> totalQueueWaitNs_{0};
    std::atomic<uint64_t> maxQueueWaitNs_{0};
    std::atomic<uint64_t> totalWorkNs_{0};
    std::atomic<uint64_t> maxWorkNs_{0};
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
