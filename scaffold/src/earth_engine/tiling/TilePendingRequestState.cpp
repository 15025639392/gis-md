#include "TilePendingRequestState.h"

#include "../debug/Contracts.h"

#include <algorithm>

namespace earth_engine {

namespace {

/// 三张表的成对变异自检(PendingRequestParity 契约的判定点)。分歧的静默
/// 后果:键残留 → 该 cacheKey 永判"在飞"、调度器永不重发、瓦片永久缺失;
/// 令牌残留 → 令牌表泄漏、换代取消形同虚设。今天由同一组方法成对增删,
/// 契约防的是未来某次改动只动其中一张。
void checkParity(const char* site,
                 size_t pending,
                 size_t content,
                 size_t tokens,
                 size_t needed,
                 size_t cells) {
    GE_CONTRACT(contracts::Id::PendingRequestParity,
                pending == tokens && content <= pending &&
                    needed <= pending && cells <= pending,
                "site=%s pending=%zu tokens=%zu content=%zu needed=%zu "
                "cells=%zu",
                site, pending, tokens, content, needed, cells);
}

}  // namespace

bool TilePendingRequestState::destroying() const {
    return destroying_;
}

bool TilePendingRequestState::empty() const {
    return pendingRequests_.empty();
}

bool TilePendingRequestState::callbacksDrained() const {
    return pendingRequests_.empty() &&
           retiredCallbackTokens_.empty();
}

bool TilePendingRequestState::contains(const std::string& cacheKey) const {
    if (cacheKey.empty()) {
        return false;
    }
    return pendingRequests_.count(cacheKey) != 0;
}

size_t TilePendingRequestState::totalRequestCount() const {
    return pendingRequests_.size();
}

PendingRequestCounts TilePendingRequestState::counts() const {
    PendingRequestCounts result;
    result.totalRequests = pendingRequests_.size();
    result.contentRequests = pendingContentRequestKeys_.size();
    result.terrainRequests =
        result.totalRequests > result.contentRequests
            ? result.totalRequests - result.contentRequests
            : 0;
    return result;
}

bool TilePendingRequestState::beginTerrainRequest(
    const std::string& cacheKey,
    const CancellationToken& token,
    std::shared_ptr<std::atomic<int>> priorityCell) {
    if (destroying_ || cacheKey.empty() || pendingRequests_.count(cacheKey)) {
        return false;
    }
    pendingRequests_.insert(cacheKey);
    pendingRequestTokens_[cacheKey] = token;
    lastNeededSeq_[cacheKey] = neededFrameSeq_;  // begin 即首帧"仍需要"
    if (priorityCell) {
        priorityCells_[cacheKey] = std::move(priorityCell);
    }
    checkParity("beginTerrain", pendingRequests_.size(),
                pendingContentRequestKeys_.size(),
                pendingRequestTokens_.size(),
                lastNeededSeq_.size(),
                priorityCells_.size());
    return true;
}

bool TilePendingRequestState::beginContentRequest(
    const std::string& cacheKey,
    const CancellationToken& token,
    std::shared_ptr<std::atomic<int>> priorityCell) {
    if (destroying_ || cacheKey.empty() || pendingRequests_.count(cacheKey)) {
        return false;
    }
    pendingRequests_.insert(cacheKey);
    pendingContentRequestKeys_.insert(cacheKey);
    pendingRequestTokens_[cacheKey] = token;
    lastNeededSeq_[cacheKey] = neededFrameSeq_;  // begin 即首帧"仍需要"
    if (priorityCell) {
        priorityCells_[cacheKey] = std::move(priorityCell);
    }
    checkParity("beginContent", pendingRequests_.size(),
                pendingContentRequestKeys_.size(),
                pendingRequestTokens_.size(),
                lastNeededSeq_.size(),
                priorityCells_.size());
    return true;
}

void TilePendingRequestState::completeTerrainRequest(
    const std::string& cacheKey) {
    pendingRequests_.erase(cacheKey);
    pendingContentRequestKeys_.erase(cacheKey);
    pendingRequestTokens_.erase(cacheKey);
    lastNeededSeq_.erase(cacheKey);
    priorityCells_.erase(cacheKey);
    checkParity("complete", pendingRequests_.size(),
                pendingContentRequestKeys_.size(),
                pendingRequestTokens_.size(),
                lastNeededSeq_.size(),
                priorityCells_.size());
}

void TilePendingRequestState::completeContentRequest(
    const std::string& cacheKey) {
    pendingRequests_.erase(cacheKey);
    pendingContentRequestKeys_.erase(cacheKey);
    pendingRequestTokens_.erase(cacheKey);
    lastNeededSeq_.erase(cacheKey);
    priorityCells_.erase(cacheKey);
    checkParity("complete", pendingRequests_.size(),
                pendingContentRequestKeys_.size(),
                pendingRequestTokens_.size(),
                lastNeededSeq_.size(),
                priorityCells_.size());
}

void TilePendingRequestState::completeTerrainRequest(
    const std::string& cacheKey,
    const CancellationToken& token) {
    const auto active = pendingRequestTokens_.find(cacheKey);
    if (active != pendingRequestTokens_.end() &&
        active->second.sharesStateWith(token)) {
        completeTerrainRequest(cacheKey);
        return;
    }
    const auto retired = std::find_if(
        retiredCallbackTokens_.begin(),
        retiredCallbackTokens_.end(),
        [&token](const CancellationToken& candidate) {
            return candidate.sharesStateWith(token);
        });
    if (retired != retiredCallbackTokens_.end()) {
        retiredCallbackTokens_.erase(retired);
    }
}

void TilePendingRequestState::completeContentRequest(
    const std::string& cacheKey,
    const CancellationToken& token) {
    completeTerrainRequest(cacheKey, token);
}

void TilePendingRequestState::cancelAndErase(const std::string& cacheKey) {
    if (auto tokenIt = pendingRequestTokens_.find(cacheKey);
        tokenIt != pendingRequestTokens_.end()) {
        tokenIt->second.cancel();
        retiredCallbackTokens_.push_back(tokenIt->second);
    }
    pendingRequests_.erase(cacheKey);
    pendingContentRequestKeys_.erase(cacheKey);
    pendingRequestTokens_.erase(cacheKey);
    lastNeededSeq_.erase(cacheKey);
    priorityCells_.erase(cacheKey);
    checkParity("cancelAndErase", pendingRequests_.size(),
                pendingContentRequestKeys_.size(),
                pendingRequestTokens_.size(),
                lastNeededSeq_.size(),
                priorityCells_.size());
}

void TilePendingRequestState::markNeeded(const std::string& cacheKey,
                                         int httpPriority) {
    if (cacheKey.empty() || pendingRequests_.count(cacheKey) == 0) {
        return;  // 只标在飞 key,防 lastNeededSeq_ 长成孤儿表
    }
    lastNeededSeq_[cacheKey] = neededFrameSeq_;
    if (httpPriority >= 0) {
        // promotion 重排:写动态优先级 cell,curl 工作线程下一轮 wake 据此
        // 把仍在排队的请求搬桶(升档插队/降档让路)。在飞传输不受影响。
        const auto it = priorityCells_.find(cacheKey);
        if (it != priorityCells_.end() && it->second) {
            it->second->store(httpPriority, std::memory_order_release);
        }
    }
}

std::vector<std::string> TilePendingRequestState::advanceFrameAndCollectStale(
    uint64_t maxAgeFrames) {
    ++neededFrameSeq_;
    std::vector<std::string> stale;
    for (const std::string& cacheKey : pendingRequests_) {
        const auto it = lastNeededSeq_.find(cacheKey);
        // 无记录按"本帧刚需要"处理 —— 失败方向偏向不取消(误杀比漏杀贵:
        // 误杀 = 半途丢弃已花的带宽再重拉;漏杀 = 多占一个槽到自然完成)。
        const uint64_t last =
            it != lastNeededSeq_.end() ? it->second : neededFrameSeq_;
        if (neededFrameSeq_ - last > maxAgeFrames) {
            stale.push_back(cacheKey);
        }
    }
    return stale;
}

void TilePendingRequestState::markDestroyingAndCancelRequests() {
    destroying_ = true;
    for (auto& [cacheKey, token] : pendingRequestTokens_) {
        token.cancel();
        (void)cacheKey;
    }
}

void TilePendingRequestState::clearAfterCallbacksComplete() {
    if (!callbacksDrained()) {
        return;
    }
    pendingContentRequestKeys_.clear();
    pendingRequestTokens_.clear();
    retiredCallbackTokens_.clear();
    lastNeededSeq_.clear();
    priorityCells_.clear();
    destroying_ = false;
}

} // namespace earth_engine
