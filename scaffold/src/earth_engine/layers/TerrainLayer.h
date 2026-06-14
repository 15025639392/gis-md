#pragma once

#include "../providers/TerrainProvider.h"
#include "../core/math/Vec3.h"
#include "../tiling/TileScheme.h"
#include "../tiling/TilePlan.h"
#include "../tiling/TileQuadTree.h"
#include "../terrain/TerrainTile.h"

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <deque>

namespace earth_engine {

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
    const TerrainTile* findBestTile(double lngRad, double latRad) const;
    const TerrainTile* findBestTileForKey(const TileKey& targetKey) const;
    /// Cross-projection lookup by geographic bounds (when basemap CRS ≠ terrain CRS).
    const TerrainTile* findBestTileForBounds(const Rectangle& geoBounds) const;
    uint64_t terrainGeneration() const { return terrainGeneration_; }
    int cachedTileCount() const { return static_cast<int>(tileCache_.size()); }
    std::vector<const TerrainTile*> visibleLoadedTiles() const;

    /// 每帧更新（加载缺失瓦片）
    void update(const FrameState& frameState);

    /// 获取当前可见瓦片（供调试用）
    const std::vector<TileKey>& visibleTiles() const { return tilePlan_.visibleTiles; }
    const TileScheme& tileScheme() const { return *tileScheme_; }

private:
    void loadTile(const TileKey& key);
    void processPendingUploads();
    const std::vector<TileKey>& requestCandidatesForFrame(const FrameState& frameState);
    bool shouldReuseRequestCandidateSnapshot(const FrameState& frameState) const;
    std::string id_;
    bool visible_ = true;
    bool enabled_ = false;  // 默认关闭，由 Scene 启用

    std::unique_ptr<TerrainProvider> provider_;
    std::unique_ptr<TileScheme> tileScheme_;
    TilePlan tilePlan_;

    // 瓦片缓存
    std::unordered_map<std::string, std::unique_ptr<TerrainTile>> tileCache_;
    std::unordered_set<std::string> requestedTiles_;

    // cesium-native availability tracking (see CesiumGeometry/QuadtreeRectangleAvailability).
    // Tracks tiles confirmed to have NO data to avoid redundant requests.
    std::unordered_set<std::string> emptyTiles_;

    // Persistent quad tree (avoids rebuilding every frame)
    std::unique_ptr<TileQuadTree> quadTree_;
    Vec3 lastCameraPosition_ = Vec3::zero();
    Vec3 lastCameraDirection_ = Vec3::zero();
    bool hasCameraState_ = false;
    bool cameraMoving_ = false;
    uint64_t lastPlanFrameId_ = 0;

    struct RequestCandidateSnapshot {
        std::vector<TileKey> tiles;
        uint64_t planFrameId = 0;
        uint64_t terrainGeneration = 0;
        size_t requestedCount = 0;
        size_t emptyCount = 0;
        Vec3 cameraPosition = Vec3::zero();
        Vec3 cameraDirection = Vec3::zero();
        bool hasInteractionFocus = false;
        Vec3 interactionFocusDirection = Vec3::zero();
        bool valid = false;
    };
    RequestCandidateSnapshot requestCandidateSnapshot_;

    // 待上传队列
    struct PendingUpload {
        TileKey key;
        std::unique_ptr<DecodedHeightmap> heightmap;
    };
    struct PendingQueue {
        std::deque<PendingUpload> queue;
        std::unordered_set<std::string> emptyTiles;
        std::mutex mutex;
    };
    std::shared_ptr<PendingQueue> pendingQueue_;

    uint64_t terrainGeneration_ = 0;
    std::string terrainCacheKey(const TileKey& key) const;

    /// Returns false if this tile or any ancestor has been confirmed empty.
    /// Prevents redundant requests to coverage gaps (cesium-native availability).
    bool isTilePossiblyAvailable(const TileKey& key) const;
};

} // namespace earth_engine
