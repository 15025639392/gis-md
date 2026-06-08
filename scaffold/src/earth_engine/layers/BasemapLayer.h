#pragma once

#include "../renderer/RenderCommand.h"
#include "../tiling/TilePlan.h"
#include "../renderer/TileTextureCache.h"
#include "../providers/ImageryProvider.h"
#include "../tiling/TileScheme.h"
#include "../tiling/SurfaceTile.h"
#include "../core/math/Vec3.h"

#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <cstddef>

namespace earth_engine {

class RenderDevice;
class Renderer;
class Buffer;
struct FrameState;
class TileScheme;
class TerrainLayer;

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
    void setNormalMapDebugEnabled(bool enabled) { normalMapDebugEnabled_ = enabled; }
    bool normalMapDebugEnabled() const { return normalMapDebugEnabled_; }

    /// 每帧更新（计算 TilePlan、请求缺失瓦片）
    /// 独立模式：图层自己计算 TilePlan。
    /// 当被 BasemapLayerStack 管理时，使用 applyPlan + loadMissingTiles 替代。
    void update(const FrameState& frameState);

    /// 从外部注入 TilePlan（由 BasemapLayerStack 提供共享计划）
    void applyPlan(const TilePlan& plan, const Vec3& cameraPosition);

    /// 请求缺失的瓦片（通常在 applyPlan 之后调用）
    void loadMissingTiles();

    /// 获取当前 TilePlan（供外部读取）
    const TilePlan& tilePlan() const { return tilePlan_; }

    /// 生成渲染命令
    /// @param renderer 共享渲染器（用于 makeSurfaceTileCommand）
    /// @param commands 输出命令列表
    void buildRenderCommands(Renderer& renderer,
                             const TerrainLayer* terrainLayer,
                             RenderCommandList& commands);

    /// 获取统计信息
    int visibleTileCount() const { return static_cast<int>(tilePlan_.visibleTiles.size()); }
    int desiredTileCount() const { return static_cast<int>(layerPlan_.desiredTiles.size()); }
    int requestTileCount() const { return static_cast<int>(layerPlan_.requestTiles.size()); }
    int renderTileCount() const { return static_cast<int>(layerPlan_.renderTiles.size()); }
    int cachedTileCount() const { return static_cast<int>(textureCache_.count()); }
    int surfaceMeshCount() const { return static_cast<int>(surfaceMeshCache_.size()); }
    size_t surfaceMeshBytes() const;
    int terrainSurfaceMeshCount() const;
    int terrainParentFallbackMeshCount() const;
    int ellipsoidSurfaceMeshCount() const;
    int exactAttachmentCount() const;
    int parentFallbackAttachmentCount() const;
    int missingImageryTileCount() const { return layerPlan_.missingTileCount; }
    int transitionTileCount() const { return layerPlan_.transitionTileCount; }
    int terrainReadySurfaceMeshCount() const;
    int terrainTransitionSurfaceMeshCount() const;
    int normalMapTextureCount() const;

    /// 获取当前可见瓦片（供调试叠加层使用）
    const std::vector<TileKey>& visibleTiles() const { return tilePlan_.visibleTiles; }
    const LayerTilePlan& layerPlan() const { return layerPlan_; }

    /// 获取瓦片体系
    const TileScheme& tileScheme() const { return *tileScheme_; }

private:
    /// 发起异步瓦片加载（回调中入队待上传）
    void loadTile(const TileKey& key);
    /// 处理待上传队列（在 update() 中调用，主线程安全）
    void processPendingUploads();
    void rebuildLayerPlan();
    Texture* findFallbackTexture(const TileKey& target, TileKey& textureKey);
    bool isCurrentDesiredTile(const TileKey& key) const;
    struct SurfaceGpuMesh {
        std::unique_ptr<Buffer> vertexBuffer;
        std::unique_ptr<Buffer> indexBuffer;
        std::unique_ptr<Texture> normalMapTexture;
        Vec3 localOriginEcef = Vec3::zero();
        int indexCount = 0;
        bool usesTerrain = false;
        bool usesParentTerrain = false;
        bool terrainReady = false;
        bool terrainTransition = false;
    };
    SurfaceGpuMesh* getOrCreateSurfaceGpuMesh(const TileKey& key,
                                              const Rectangle& bounds,
                                              const TerrainLayer* terrainLayer);
    void evictUnusedSurfaceMeshes();
    std::string tileCacheKey(const TileKey& key) const;

    std::string id_;
    bool visible_ = true;
    float opacity_ = 1.0f;
    bool normalMapDebugEnabled_ = false;

    std::unique_ptr<ImageryProvider> provider_;
    std::unique_ptr<TileScheme> tileScheme_;
    RenderDevice* renderDevice_;
    TileTextureCache textureCache_;
    std::unordered_map<std::string, SurfaceGpuMesh> surfaceMeshCache_;
    TilePlan tilePlan_;
    LayerTilePlan layerPlan_;
    Vec3 lastCameraPosition_ = Vec3::zero();

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
