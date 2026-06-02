#pragma once

#include "../providers/TerrainProvider.h"
#include "../tiling/TileScheme.h"
#include "../tiling/TilePlan.h"
#include "../globe/Globe.h"
#include "../renderer/RenderCommand.h"
#include "../terrain/TerrainTile.h"

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <deque>

namespace earth_engine {

class RenderDevice;
class Renderer;
struct FrameState;
class Camera;

/// 地形图层。
///
/// 管理地形瓦片加载、地形网格生成和渲染。
/// 替换平坦椭球，使用位移后的地形 mesh 渲染地球表面。
class TerrainLayer {
public:
    /// @param provider 地形数据源（所有权转移）
    /// @param tileScheme 瓦片体系
    TerrainLayer(std::unique_ptr<TerrainProvider> provider,
                 std::unique_ptr<TileScheme> tileScheme);
    ~TerrainLayer();

    TerrainLayer(const TerrainLayer&) = delete;
    TerrainLayer& operator=(const TerrainLayer&) = delete;

    // ---- 基本信息 ----

    const std::string& id() const { return id_; }
    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }
    bool enabled() const { return enabled_; }
    void setEnabled(bool e) { enabled_ = e; }

    // ---- 数据访问 ----

    /// 采样指定地理坐标的地形高度
    /// @return ellipsoid height（meter），无数据返回 0
    float sampleHeight(double lngRad, double latRad) const;

    // ---- 渲染 ----

    /// 每帧更新（加载缺失瓦片）
    void update(const FrameState& frameState);

    /// 生成地形渲染命令（替换 Globe 背景）
    /// @param baseGlobeMesh 基础椭球网格
    /// @param renderer 共享渲染器
    /// @param commands 输出命令列表
    void buildRenderCommands(const GlobeMesh& baseGlobeMesh,
                             const FrameState& frameState,
                             Renderer& renderer,
                             RenderCommandList& commands);

    /// 获取当前可见瓦片（供调试用）
    const std::vector<TileKey>& visibleTiles() const { return tilePlan_.visibleTiles; }
    const TileScheme& tileScheme() const { return *tileScheme_; }

private:
    void loadTile(const TileKey& key);
    void processPendingUploads();
    const TerrainTile* findBestTile(double lngRad, double latRad) const;

    std::string id_;
    bool visible_ = true;
    bool enabled_ = false;  // 默认关闭，由 Scene 启用

    std::unique_ptr<TerrainProvider> provider_;
    std::unique_ptr<TileScheme> tileScheme_;
    TilePlan tilePlan_;

    // 瓦片缓存
    std::unordered_map<std::string, std::unique_ptr<TerrainTile>> tileCache_;
    std::unordered_set<std::string> requestedTiles_;

    // 待上传队列
    struct PendingUpload {
        TileKey key;
        std::unique_ptr<DecodedHeightmap> heightmap;
    };
    struct PendingQueue {
        std::deque<PendingUpload> queue;
        std::mutex mutex;
    };
    std::shared_ptr<PendingQueue> pendingQueue_;

    // 当前渲染的地形网格（缓存避免每帧重建）
    GlobeMesh cachedMesh_;
    mutable bool meshDirty_ = true;
};

} // namespace earth_engine
