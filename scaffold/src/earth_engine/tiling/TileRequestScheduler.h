#pragma once

#include "../tiling/TileKey.h"
#include "../threading/CancellationToken.h"
#include "../providers/ImageryProvider.h"

#include <functional>
#include <vector>
#include <queue>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <string>

namespace earth_engine {

/// 异步瓦片请求调度器。
/// 管理请求优先级、并发限制、取消和重试。
///
/// 使用方式：
///   1. 每帧调用 requestTile(key, priority) 入队
///   2. 每帧调用 update() 处理已完成请求
///   3. 相机大幅移动时调用 cancelAll()
class TileRequestScheduler {
public:
    /// 瓦片加载完成回调
    /// @param key 瓦片键
    /// @param image 解码后的图片（失败为 nullptr）
    using ReadyCallback = std::function<void(const TileKey& key,
                                              std::unique_ptr<DecodedImage> image)>;

    /// @param provider 瓦片数据源
    /// @param maxConcurrent 最大并发请求数（默认 4）
    TileRequestScheduler(ImageryProvider* provider, int maxConcurrent = 4);
    ~TileRequestScheduler();

    TileRequestScheduler(const TileRequestScheduler&) = delete;
    TileRequestScheduler& operator=(const TileRequestScheduler&) = delete;

    /// 设置完成回调
    void setCallback(ReadyCallback callback) { callback_ = std::move(callback); }

    /// 入队瓦片请求
    /// @param key 瓦片键
    /// @param priority 优先级（越小越优先，0 = 最高）
    void requestTile(const TileKey& key, int priority = 10);

    /// 每帧更新：检查已完成请求并触发回调
    void update();

    /// 取消所有未完成请求（相机移动后调用）
    void cancelAll();

    /// 当前在队/在飞的请求数
    int pendingCount() const;

private:
    struct Request {
        TileKey key;
        int priority;
        int retries = 0;
    };

    struct PriorityCompare {
        bool operator()(const Request& a, const Request& b) const {
            return a.priority > b.priority;  // 小值优先
        }
    };

    void startNext();

    ImageryProvider* provider_;
    int maxConcurrent_;

    std::priority_queue<Request, std::vector<Request>, PriorityCompare> pending_;
    std::unordered_set<std::string> inFlight_;
    ReadyCallback callback_;

    mutable std::mutex mutex_;
    CancellationToken currentToken_;
};

} // namespace earth_engine
