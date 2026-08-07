#pragma once

#include "../threading/CancellationToken.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

struct PendingRequestCounts {
    size_t terrainRequests = 0;
    size_t contentRequests = 0;
    size_t totalRequests = 0;
};

class TilePendingRequestState {
public:
    bool destroying() const;
    bool empty() const;
    bool callbacksDrained() const;
    bool contains(const std::string& cacheKey) const;

    size_t totalRequestCount() const;
    PendingRequestCounts counts() const;

    bool beginTerrainRequest(const std::string& cacheKey,
                             const CancellationToken& token);
    bool beginContentRequest(const std::string& cacheKey,
                             const CancellationToken& token);

    void completeTerrainRequest(const std::string& cacheKey);
    void completeContentRequest(const std::string& cacheKey);
    void completeTerrainRequest(
        const std::string& cacheKey,
        const CancellationToken& token);
    void completeContentRequest(
        const std::string& cacheKey,
        const CancellationToken& token);
    void cancelAndErase(const std::string& cacheKey);

    void markDestroyingAndCancelRequests();
    void clearAfterCallbacksComplete();

    /// 帧级"仍被需要"标记(请求侧降级/取消欠账)。派发/去重路径对每个本帧
    /// 仍在要的在飞 key 逐帧调用;begin* 视为首帧标记。非在飞 key 忽略。
    void markNeeded(const std::string& cacheKey);

    /// 推进内部帧序,返回连续 maxAgeFrames 个"推进帧"里都没被 markNeeded
    /// 的在飞 key。只在调度器真的处理了请求的帧被调用 —— reuse/空闲帧不
    /// 推进帧序,静止相机下的长传输不计龄、不会被误杀。调用方拿返回值走
    /// cancelAndErase 单一出口(此时 token.cancel 经 cancelFlag 桥接真正
    /// 中止 curl 传输,腾出并发槽)。
    std::vector<std::string> advanceFrameAndCollectStale(uint64_t maxAgeFrames);

private:
    std::unordered_set<std::string> pendingRequests_;
    std::unordered_set<std::string> pendingContentRequestKeys_;
    std::unordered_map<std::string, CancellationToken> pendingRequestTokens_;
    std::vector<CancellationToken> retiredCallbackTokens_;
    /// key → 最后一次被标记"仍需要"的帧序。恒 ⊆ pendingRequests_
    /// (PendingRequestParity 契约一并看住)。
    std::unordered_map<std::string, uint64_t> lastNeededSeq_;
    uint64_t neededFrameSeq_ = 0;
    bool destroying_ = false;
};

} // namespace earth_engine
