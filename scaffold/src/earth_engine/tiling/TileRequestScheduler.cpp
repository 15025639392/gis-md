#include "TileRequestScheduler.h"

#include <algorithm>
#include <deque>

namespace earth_engine {

TileRequestScheduler::TileRequestScheduler(ImageryProvider* provider,
                                             int maxConcurrent)
    : provider_(provider), maxConcurrent_(maxConcurrent) {}

TileRequestScheduler::~TileRequestScheduler() {
    cancelAll();
}

void TileRequestScheduler::requestTile(const TileKey& key, int priority) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 跳过已在队列或飞行中的请求
    std::string cacheKey = key.schemeId + "/" +
                           std::to_string(key.z) + "/" +
                           std::to_string(key.x) + "/" +
                           std::to_string(key.y);
    if (inFlight_.count(cacheKey)) return;

    // 检查是否已在 pending 队列中
    // 简化：允许重复（priority_queue 不支持遍历）

    pending_.push({key, priority, 0});
}

void TileRequestScheduler::update() {
    // 仅主线程调用，无需锁
    startNext();
}

void TileRequestScheduler::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    currentToken_.cancel();

    // 清空 pending 队列
    while (!pending_.empty()) pending_.pop();

    // 重置取消令牌
    currentToken_ = CancellationToken();

    inFlight_.clear();
}

int TileRequestScheduler::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(pending_.size() + inFlight_.size());
}

void TileRequestScheduler::startNext() {
    std::lock_guard<std::mutex> lock(mutex_);

    while (static_cast<int>(inFlight_.size()) < maxConcurrent_ && !pending_.empty()) {
        Request req = pending_.top();
        pending_.pop();

        std::string cacheKey = req.key.schemeId + "/" +
                               std::to_string(req.key.z) + "/" +
                               std::to_string(req.key.x) + "/" +
                               std::to_string(req.key.y);

        if (inFlight_.count(cacheKey)) continue;  // 已在飞行中

        inFlight_.insert(cacheKey);

        // 发起异步请求
        auto token = currentToken_.createChild();
        auto callback = callback_;
        TileKey key = req.key;

        provider_->requestTile(key, token,
            [this, key, cacheKey, callback = std::move(callback)]
            (const TileKey& k, std::unique_ptr<DecodedImage> image) {

            {
                std::lock_guard<std::mutex> lk(mutex_);
                inFlight_.erase(cacheKey);
            }

            if (callback) {
                callback(k, std::move(image));
            }
        });

        // 注意：在锁内调用了 provider_->requestTile，可能触发回调
        // 但回调在后台线程中执行，不会死锁（mutex_ 是独立锁）
    }
}

} // namespace earth_engine
