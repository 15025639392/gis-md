#pragma once

#include "GltfModel.h"
#include "../core/math/Mat4.h"
#include "../providers/TerrainProvider.h"  // DecodedHeightmap
#include "../platform/bridge/PlatformBridge.h"
#include "../providers/ProviderRequestDiagnostics.h"
#include "../threading/CancellationToken.h"
#include "../tiling/TileAvailabilityState.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileBoundingVolume.h"
#include "../tiling/TileLoadResultMetadata.h"
#include "../tiling/TileLoadStatus.h"
#include "../tiling/TileRefine.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

struct TilesetContentTileMetadata {
    TileKey key;
    std::optional<TileKey> parentKey;
    std::vector<TileKey> childKeys;
    Rectangle bounds;
    bool hasExplicitBounds = false;
    std::optional<TileBoundingVolume> boundingVolume;
    std::optional<TileBoundingVolume> viewerRequestVolume;
    std::optional<TileBoundingVolume> contentBoundingVolume;
    Mat4 transform = Mat4::identity();
    double geometricError = 0.0;
    TileRefine refine = TileRefine::Replace;
    bool unconditionallyRefine = false;
};

enum class TileTerrainRenderSource {
    Generic,
    EllipsoidFallback
};

struct TileContentLoadResult {
    TileLoadStatus status = TileLoadStatus::Failed;
    std::unique_ptr<GltfModel> gltfModel;
    Mat4 contentTransform = Mat4::identity();
    TileLoadResultMetadata metadata;
    bool terrainRenderContent = false;
    TileTerrainRenderSource terrainRenderSource =
        TileTerrainRenderSource::Generic;
    // 北极星 Phase 2c Stage B:保留 worker 解码的原始高度图,供 GL 线程建 per-tile
    // 高度纹理(shader 位移)+ 后续高度查询(P3)。仅真实地形 heightmap 路径填充。
    std::shared_ptr<const DecodedHeightmap> retainedHeightmap;

    static TileContentLoadResult render(std::unique_ptr<GltfModel> model) {
        TileContentLoadResult result;
        result.status = model ? TileLoadStatus::Renderable
                              : TileLoadStatus::Failed;
        if (model && !model->rasterOverlayDetails.empty()) {
            result.metadata.rasterOverlayDetails = model->rasterOverlayDetails;
        }
        result.gltfModel = std::move(model);
        return result;
    }

    static TileContentLoadResult renderTerrain(
        std::unique_ptr<GltfModel> model,
        TileLoadResultMetadata metadata = {},
        Mat4 contentTransform = Mat4::identity(),
        TileTerrainRenderSource source = TileTerrainRenderSource::Generic) {
        TileContentLoadResult result = render(std::move(model));
        if (result.status == TileLoadStatus::Renderable) {
            if (!metadata.rasterOverlayDetails && result.gltfModel &&
                !result.gltfModel->rasterOverlayDetails.empty()) {
                metadata.rasterOverlayDetails =
                    result.gltfModel->rasterOverlayDetails;
            } else if (metadata.rasterOverlayDetails && result.gltfModel) {
                result.gltfModel->rasterOverlayDetails =
                    *metadata.rasterOverlayDetails;
            }
            result.metadata = std::move(metadata);
            result.contentTransform = contentTransform;
            result.terrainRenderContent = true;
            result.terrainRenderSource = source;
        }
        return result;
    }

    static TileContentLoadResult empty() {
        TileContentLoadResult result;
        result.status = TileLoadStatus::Empty;
        return result;
    }

    static TileContentLoadResult retryLater() {
        TileContentLoadResult result;
        result.status = TileLoadStatus::RetryLater;
        return result;
    }

    static TileContentLoadResult failed() {
        TileContentLoadResult result;
        result.status = TileLoadStatus::Failed;
        return result;
    }

    static TileContentLoadResult cancelled() {
        TileContentLoadResult result;
        result.status = TileLoadStatus::Cancelled;
        return result;
    }

    static TileContentLoadResult external() {
        TileContentLoadResult result;
        result.status = TileLoadStatus::External;
        return result;
    }
};

struct TileContentRequestOptions {
    bool generateTerrainRasterOverlayDetails = false;
    std::vector<RasterOverlayProjection> requiredRasterOverlayProjections;
    /// 共享位移模板池是否活跃(摘 glTF 第一级)。true 时地形 provider 对
    /// 「必走模板」的瓦片**不造网格**,只产带元数据的无几何 primitive。
    /// 由 Tileset 从 IPrepareRendererResources::terrainSharedTemplateActive()
    /// 取,与 prepare/draw 两侧的门控同源;运行时关池会重载地形内容
    /// (见 Tileset::reloadGhostReleasedTerrainContent),故不会留下
    /// 「没网格又要按 legacy VBO 画」的瓦片。
    bool terrainSharedTemplateActive = false;
    /// 动态优先级 cell(透传给 HttpRequestOptions.priorityCell;可空=静态)。
    std::shared_ptr<std::atomic<int>> httpPriorityCell;
};

class TilesetContentProvider {
public:
    virtual ~TilesetContentProvider() = default;

    virtual std::string id() const = 0;
    virtual bool supportsTile(const TileKey& key) const = 0;
    virtual std::vector<TileKey> rootTiles() const { return {}; }
    virtual std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey&) const {
        return std::nullopt;
    }
    // Structural metadata is immutable by default, matching cesium-native
    // Tile. Dynamic providers must publish a non-zero revision and only then
    // may an existing Unloaded tile refresh its metadata.
    virtual uint64_t tileMetadataRevision() const { return 0; }
    virtual std::vector<TileKey> childTiles(const TileKey&) const {
        return {};
    }
    virtual bool providesTerrainQuadtree() const { return false; }
    virtual TileAvailabilityState availabilityState(
        const TileKey&) const {
        return TileAvailabilityState::NotAvailable;
    }
    virtual uint64_t childTopologyRevision() const { return 0; }
    virtual bool isTerrainAvailabilityBoundaryLevel(int) const {
        return false;
    }
    virtual int estimatedRequestFanout(const TileKey&) const { return 1; }

    using ContentCallback = std::function<void(
        const TileKey&, TileContentLoadResult)>;

    virtual void requestTileContent(const TileKey& key,
                                    CancellationToken token,
                                    ContentCallback callback,
                                    HttpRequestPriority priority =
                                        HttpRequestPriority::Normal) = 0;

    virtual void requestTileContent(
        const TileKey& key,
        CancellationToken token,
        ContentCallback callback,
        HttpRequestPriority priority,
        TileContentRequestOptions) {
        requestTileContent(
            key,
            std::move(token),
            std::move(callback),
            priority);
    }

    virtual TileContentLoadResult decodeContent(
        const uint8_t* data,
        size_t size) = 0;

    virtual ProviderRequestDiagnostics requestDiagnostics() const {
        return {};
    }
};

/// A real glTF/GLB/B3DM content provider for a single tile. This is the current
/// equivalent of a minimal cesium-native TilesetContentLoader: it produces
/// TileRenderContent data, not terrain meshes.
class SingleGltfContentProvider final : public TilesetContentProvider {
public:
    SingleGltfContentProvider(TileKey contentKey,
                              std::string url,
                              std::string name = "glTF content");
    SingleGltfContentProvider(TileKey contentKey,
                              std::vector<uint8_t> bytes,
                              std::string name = "glTF content");

    std::string id() const override;
    bool supportsTile(const TileKey& key) const override;
    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority =
                                HttpRequestPriority::Normal) override;
    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority,
                            TileContentRequestOptions options) override;
    TileContentLoadResult decodeContent(const uint8_t* data,
                                        size_t size) override;

    const TileKey& contentKey() const { return contentKey_; }
    const std::string& url() const { return url_; }
    void setPlatformBridge(PlatformBridge* bridge) { platformBridge_ = bridge; }
    ProviderRequestDiagnostics requestDiagnostics() const override;
    const Mat4& contentTransform() const { return contentTransform_; }
    void setContentTransform(const Mat4& transform) {
        contentTransform_ = transform;
    }
    void setEastNorthUpPlacementDegrees(double longitudeDegrees,
                                        double latitudeDegrees,
                                        double heightMeters,
                                        double uniformScale = 1.0);

private:
    TileKey contentKey_;
    std::string url_;
    std::string name_;
    std::vector<uint8_t> bytes_;
    Mat4 contentTransform_ = Mat4::identity();
    PlatformBridge* platformBridge_ = nullptr;
    std::atomic<int> requestsStarted_{0};
    std::atomic<int> requestsCompleted_{0};
    std::atomic<int> activeWorkerBlockingRequests_{0};
    std::atomic<int> peakWorkerBlockingRequests_{0};
    std::atomic<int> externalResourceRequestsStarted_{0};
    std::atomic<int> externalResourceRequestsCompleted_{0};
    std::atomic<int> activeExternalResourceBlockingRequests_{0};
    std::atomic<int> peakExternalResourceBlockingRequests_{0};
};

/// 3D Tiles tileset.json content provider aligned with cesium-native
/// TilesetJsonLoader for explicit tile trees. It parses JSON tile transforms,
/// refine modes, geometric errors, bounding volumes, content URI/url, and
/// external tileset JSON content.
class TilesetJsonContentProvider final : public TilesetContentProvider {
public:
    TilesetJsonContentProvider(std::string tilesetJsonUrl,
                               std::vector<uint8_t> tilesetJsonBytes,
                               std::string name = "3D Tiles");

    std::string id() const override;
    bool supportsTile(const TileKey& key) const override;
    std::vector<TileKey> rootTiles() const override;
    std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey& key) const override;
    uint64_t childTopologyRevision() const override {
        return childTopologyRevision_.load(std::memory_order_acquire);
    }
    std::vector<TileKey> childTiles(const TileKey& key) const override;
    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority =
                                HttpRequestPriority::Normal) override;
    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority,
                            TileContentRequestOptions options) override;
    TileContentLoadResult decodeContent(const uint8_t* data,
                                        size_t size) override;

    bool valid() const;
    void setPlatformBridge(PlatformBridge* bridge) { platformBridge_ = bridge; }
    ProviderRequestDiagnostics requestDiagnostics() const override;

private:
    enum class TileRecordContentKind {
        Empty,
        External,
        Uri,
        Unsupported
    };

    struct TileRecord {
        TilesetContentTileMetadata metadata;
        TileRecordContentKind contentKind = TileRecordContentKind::Empty;
        Mat4 gltfUpAxisTransform = Mat4::identity();
        std::string contentUri;
        std::string resolvedContentUrl;
    };

    bool parseTilesetJson(const uint8_t* data,
                          size_t size,
                          const std::string& baseUrl,
                          const Mat4& parentTransform,
                          TileRefine parentRefine,
                          double parentGeometricError,
                          const std::optional<TileKey>& externalParentKey,
                          bool wrapRoot);
    TileContentLoadResult decodeRenderableContent(const uint8_t* data,
                                                  size_t size,
                                                  const Mat4& transform,
                                                  const Mat4& gltfUpAxisTransform,
                                                  const std::string& contentUrl);
    std::vector<uint8_t> httpGet(
        const std::string& url,
        HttpRequestPriority priority = HttpRequestPriority::Normal,
        std::function<bool()> shouldCancel = {}) const;

    std::string tilesetJsonUrl_;
    std::string name_;
    std::string schemeId_;
    PlatformBridge* platformBridge_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<TileKey> rootKeys_;
    std::unordered_map<std::string, TileRecord> records_;
    int nextTileOrdinal_ = 0;
    bool valid_ = false;
    std::atomic<int> requestsStarted_{0};
    std::atomic<int> requestsCompleted_{0};
    std::atomic<int> activeWorkerBlockingRequests_{0};
    std::atomic<int> peakWorkerBlockingRequests_{0};
    std::atomic<int> externalResourceRequestsStarted_{0};
    std::atomic<int> externalResourceRequestsCompleted_{0};
    std::atomic<int> activeExternalResourceBlockingRequests_{0};
    std::atomic<int> peakExternalResourceBlockingRequests_{0};
    std::atomic<uint64_t> childTopologyRevision_{1};
};

} // namespace earth_engine
