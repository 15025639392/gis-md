#pragma once

#include "../renderer/RenderCommand.h"
#include "../tiling/TilePlan.h"
#include "../renderer/TileTextureCache.h"
#include "../providers/ImageryProvider.h"
#include "../tiling/TileScheme.h"

#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <atomic>

namespace earth_engine {

class RenderDevice;
class Renderer;
struct FrameState;
class TileScheme;

/// 底图图层。
/// 管理从 TilePlan 计算 → Provider 请求 → TextureCache → RenderCommands 的完整链路。
class BasemapLayer {
public:
    /// @param provider 瓦片数据源（所有权转移）
    /// @param tileScheme 瓦片体系（与 provider 的 scheme 匹配）
    /// @param renderDevice 用于创建 GPU 纹理
    BasemapLayer(std::unique_ptr<ImageryProvider> provider,
                 std::unique_ptr<TileScheme> tileScheme,
                 RenderDevice* renderDevice);
    ~BasemapLayer();

    BasemapLayer(const BasemapLayer&) = delete;
    BasemapLayer& operator=(const BasemapLayer&) = delete;

    /// 图层 ID
    const std::string& id() const { return id_; }

    /// 可见性
    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }

    /// 透明度
    float opacity() const { return opacity_; }
    void setOpacity(float o) { opacity_ = std::max(0.0f, std::min(1.0f, o)); }

    /// 每帧更新（计算 TilePlan、请求缺失瓦片）
    /// 独立模式：图层自己计算 TilePlan。
    /// 当被 BasemapLayerStack 管理时，使用 applyPlan + loadMissingTiles 替代。
    void update(const FrameState& frameState);

    /// 从外部注入 TilePlan（由 BasemapLayerStack 提供共享计划）
    void applyPlan(const TilePlan& plan);

    /// 请求缺失的瓦片（通常在 applyPlan 之后调用）
    void loadMissingTiles();

    /// 获取当前 TilePlan（供外部读取）
    const TilePlan& tilePlan() const { return tilePlan_; }

    /// 生成渲染命令
    /// @param renderer 共享渲染器（用于 makeTileCommand）
    /// @param commands 输出命令列表
    void buildRenderCommands(Renderer& renderer,
                             RenderCommandList& commands);

    /// 获取统计信息
    int visibleTileCount() const { return static_cast<int>(tilePlan_.visibleTiles.size()); }
    int cachedTileCount() const { return static_cast<int>(textureCache_.count()); }

    /// 获取当前可见瓦片（供调试叠加层使用）
    const std::vector<TileKey>& visibleTiles() const { return tilePlan_.visibleTiles; }

    /// 获取瓦片体系
    const TileScheme& tileScheme() const { return *tileScheme_; }

private:
    /// 发起异步瓦片加载（回调中入队待上传）
    void loadTile(const TileKey& key);
    /// 处理待上传队列（在 update() 中调用，主线程安全）
    void processPendingUploads();

    std::string id_;
    bool visible_ = true;
    float opacity_ = 1.0f;

    std::unique_ptr<ImageryProvider> provider_;
    std::unique_ptr<TileScheme> tileScheme_;
    RenderDevice* renderDevice_;
    TileTextureCache textureCache_;
    TilePlan tilePlan_;

    // 待上传队列（后台线程解码 → 主线程上传 GPU）
    struct PendingUpload {
        TileKey key;
        uint64_t generation = 0;
        std::unique_ptr<DecodedImage> image;
    };
    // 使用 shared_ptr 保护回调生命周期（防止 layer 销毁后 detach 线程访问野指针）
    struct PendingQueue {
        std::deque<PendingUpload> queue;
        std::mutex mutex;
    };
    std::shared_ptr<PendingQueue> pendingQueue_;

    // 失败瓦片重试追踪
    struct FailedTile {
        double firstFailTime = 0.0;
        int retries = 0;
    };
    std::unordered_map<std::string, FailedTile> failedTiles_;
    uint64_t generation_ = 0;
    std::unordered_map<std::string, uint64_t> requestedGeneration_;
};

} // namespace earth_engine
