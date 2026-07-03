#pragma once

#include "../content/GltfContentProvider.h"
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class PlatformBridge;
struct QuantizedMeshMetadataContent;

/// Content provider for the quantized-mesh-1.0 format.
/// Parses binary tiles into first-class glTF terrain load results.
/// Quantized-mesh content enters the tile lifecycle through requestTileContent,
/// matching cesium-native's content-loader ownership model.
///
/// Reference: CesiumQuantizedMeshTerrain/QuantizedMeshLoader
class QuantizedMeshTerrainProvider : public TilesetContentProvider {
public:
    /// @param urlTemplate URL with {z}/{x}/{y} placeholders
    /// @param attribution display credit
    explicit QuantizedMeshTerrainProvider(std::string urlTemplate,
                                          std::string attribution = "");

    ~QuantizedMeshTerrainProvider() override;

    std::string id() const override;
    std::string type() const { return "quantized-mesh-terrain"; }
    bool providesTerrainQuadtree() const override { return true; }
    std::vector<TileKey> rootTiles() const override;
    std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey& key) const override;
    std::vector<TileKey> childTiles(const TileKey& key) const override;

    std::string schemeId() const {
        std::lock_guard<std::recursive_mutex> lock(layersMutex_);
        return schemeId_;
    }

    int minZoom() const {
        std::lock_guard<std::recursive_mutex> lock(layersMutex_);
        return minZoom_;
    }
    int maxZoom() const {
        std::lock_guard<std::recursive_mutex> lock(layersMutex_);
        return maxZoom_;
    }
    int tileSize() const { return tileSize_; }
    std::string attribution() const {
        std::lock_guard<std::recursive_mutex> lock(layersMutex_);
        return attribution_;
    }

    void setZoomRange(int minZ, int maxZ);
    void setTileSize(int ts) { tileSize_ = ts; }
    void setPlatformBridge(PlatformBridge* bridge);
    void setRequestHeaders(std::vector<HttpRequestOptions::Header> headers);
    void setWaterMaskEnabled(bool enabled) { waterMaskEnabled_ = enabled; }
    bool configureFromLayerJsonUrl(const std::string& layerJsonUrl);
    bool configureFromLayerJson(const std::string& layerJson,
                                const std::string& layerJsonUrl);
    std::string urlTemplate() const {
        std::lock_guard<std::recursive_mutex> lock(layersMutex_);
        return urlTemplate_;
    }

    bool supportsTile(const TileKey& key) const override;
    TileAvailabilityState availabilityState(const TileKey& key) const override;
    std::string buildUrl(const TileKey& key) const;
    int estimatedRequestFanout(const TileKey& key) const override;

    /// cesium-native: dynamically add availability from QM metadata
    void addAvailabilityRects(int level, const std::vector<TileAvailabilityRect>& rects);
    /// cesium-native: track loaded subtrees for sparse datasets
    bool isSubtreeLoaded(int subtreeLevel, uint64_t mortonIndex) const;
    void markSubtreeLoaded(int subtreeLevel, uint64_t mortonIndex);
    int availabilityLevels() const {
        std::lock_guard<std::recursive_mutex> lock(layersMutex_);
        return availabilityLevels_;
    }
    bool isAvailabilityBoundaryLevel(int level) const;
    bool isTerrainAvailabilityBoundaryLevel(int level) const override {
        return isAvailabilityBoundaryLevel(level);
    }
    void applyAvailabilityUpdates(
        const std::vector<QuantizedMeshAvailabilityUpdate>& updates) override;
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
    TileContentLoadResult decodeTileContent(const TileKey& key,
                                            const uint8_t* data,
                                            size_t size) const;
    TileContentLoadResult decodeContent(const uint8_t* data,
                                        size_t size) override;

    ProviderRequestDiagnostics requestDiagnostics() const override;

private:
    struct TileRectangleAvailability {
        struct RectangleWithLevel {
            uint32_t level = 0;
            Rectangle rectangle;
        };

        struct Node {
            Node(const TileKey& tileKey,
                 const Rectangle& tileExtent,
                 Node* parentNode);
            Node(const Node& other);
            Node& operator=(const Node& other);

            TileKey key;
            Rectangle extent;
            Node* parent = nullptr;
            std::unique_ptr<Node> ll;
            std::unique_ptr<Node> lr;
            std::unique_ptr<Node> ul;
            std::unique_ptr<Node> ur;
            std::vector<RectangleWithLevel> rectangles;
        };

        TileRectangleAvailability() = default;
        TileRectangleAvailability(std::string schemeId, int maximumLevel);
        TileRectangleAvailability(const TileRectangleAvailability& other);
        TileRectangleAvailability&
        operator=(const TileRectangleAvailability& other);

        void reset(std::string schemeId, int maximumLevel);
        void addAvailableTileRange(int level,
                                   const TileAvailabilityRect& range);
        uint32_t maximumLevelAtTileCenter(const TileKey& key) const;

        std::string schemeId;
        int maximumLevel = 0;
        std::vector<std::unique_ptr<Node>> rootNodes;

        static int rootTileCountX(const std::string& schemeId);
        static int rootTileCountY(const std::string& schemeId);
        static uint64_t tileCountXAtLevel(
            const std::string& schemeId,
            int level);
        static uint64_t tileCountYAtLevel(
            const std::string& schemeId,
            int level);
        static Rectangle availabilityTileRectangle(
            const std::string& schemeId,
            int level,
            uint32_t x,
            uint32_t y);
        static bool rectangleFullyContains(
            const Rectangle& outer,
            const Rectangle& inner);
        static void reparentNodeChildren(Node& node);
        static std::unique_ptr<Node> cloneNode(
            const Node& node,
            Node* parent);
        static void createNodeChildrenIfNecessary(
            Node& node,
            const std::string& schemeId);
        static void putRectangleInQuadtree(
            Node& node,
            const std::string& schemeId,
            int maximumLevel,
            const RectangleWithLevel& rectangle);
        static uint32_t findMaxLevelFromNode(
            Node* stopNode,
            Node& node,
            double x,
            double y);
    };

    struct LayerConfig {
        std::string urlTemplate;
        std::string layerJsonUrl;
        std::string schemeId = "Geographic-TMS";
        std::string version;
        std::string extensionsToRequest;
        std::vector<std::vector<TileAvailabilityRect>> availabilityRanges;
        TileRectangleAvailability contentAvailability;
        std::vector<std::unordered_set<uint64_t>> loadedSubtrees;
        bool hasAvailability = false;
        int availabilityLevels = -1;
        int minZoom = 0;
        int maxZoom = 15;
        std::string attribution;
        bool fallbackLayer = false;
    };
    struct LayerAvailabilityRequest {
        size_t layerIndex = 0;
        TileKey subtreeKey;
        std::string url;
    };
    struct AsyncTileRequestState {
        std::mutex mutex;
        std::vector<std::vector<uint8_t>> metadataBodies;
        size_t remainingMetadata = 0;
        bool metadataFinished = false;
        bool contentFinished = false;
        bool finalized = false;
        bool forceRasterOverlayDetails = false;
        int statusCode = 0;
        std::vector<uint8_t> body;
    };
    struct InFlightMetadataRequest {
        using Waiter = std::function<void(int, std::vector<uint8_t>)>;
        std::vector<Waiter> waiters;
        std::unique_ptr<HttpRequest> handle;
    };
    /// 共享 metadata 请求注册表。独立 shared_ptr 生命周期：传输层完成
    /// 回调（或其兜底 guard）可能在 provider 析构后才触发，届时经由
    /// 强引用的注册表看到 providerAlive == false 安全 no-op，而不是
    /// 触碰已析构的 provider 成员（互斥量/计数器）。
    struct MetadataRequestRegistry {
        std::mutex mutex;
        bool providerAlive = true;
        std::unordered_map<std::string,
                           std::shared_ptr<InFlightMetadataRequest>>
            inFlight;
    };

    bool appendLayerFromJson(const nlohmann::json& j,
                             const std::string& layerJsonUrl,
                             const std::string& forcedSchemeId = {});
    bool appendParentLayers(const nlohmann::json& j,
                            const std::string& layerJsonUrl);
    size_t firstAvailableLayerIndex(const TileKey& key) const;
    const LayerConfig* firstAvailableLayer(const TileKey& key) const;
    LayerConfig* firstAvailableLayer(const TileKey& key);
    std::vector<LayerAvailabilityRequest>
    collectUnderlyingLayerAvailabilityRequests(const TileKey& key) const;
    TileAvailabilityState availabilityStateInLayer(
        const LayerConfig& layer,
        const TileKey& key) const;
    bool isSubtreeLoadedInLayer(
        const LayerConfig& layer,
        int subtreeLevel,
        uint64_t mortonIndex) const;
    void markSubtreeLoadedInLayer(
        LayerConfig& layer,
        int subtreeLevel,
        uint64_t mortonIndex);
    void addAvailabilityRectsToLayer(
        LayerConfig& layer,
        int level,
        const std::vector<TileAvailabilityRect>& rects);
    std::string buildUrlForLayer(const LayerConfig& layer,
                                 const TileKey& key) const;
    TileContentLoadResult loadQuantizedMeshTileContent(
        const TileKey& key,
        const uint8_t* data,
        size_t size,
        bool enableWaterMask,
        const std::vector<QuantizedMeshMetadataContent>& metadata,
        std::optional<QuantizedMeshAvailabilityUpdate>
            currentTileAvailabilityUpdate = std::nullopt,
        bool forceRasterOverlayDetails = false) const;
    void resetFallbackLayerFromFields();
    void syncPublicStateFromLayers();
    void handleAsyncTileBody(
        const TileKey& key,
        int contentLayerIndex,
        bool includeCurrentLayerMetadata,
        std::shared_ptr<std::vector<LayerAvailabilityRequest>>
            availabilityRequests,
        std::shared_ptr<CancellationToken> token,
        std::shared_ptr<ContentCallback> callback,
        std::shared_ptr<AsyncTileRequestState> state,
        HttpRequestPriority priority,
        int statusCode,
        std::vector<uint8_t> body);
    void startAsyncMetadataRequests(
        TileKey key,
        int contentLayerIndex,
        bool includeCurrentLayerMetadata,
        std::shared_ptr<std::vector<LayerAvailabilityRequest>>
            availabilityRequests,
        std::shared_ptr<CancellationToken> token,
        std::shared_ptr<ContentCallback> callback,
        std::shared_ptr<AsyncTileRequestState> state,
        HttpRequestPriority priority,
        bool usePlatformBridge);
    void requestSharedMetadataTile(
        const std::string& url,
        HttpRequestPriority priority,
        bool usePlatformBridge,
        InFlightMetadataRequest::Waiter waiter);
    void completeAsyncTileRequestIfReady(
        TileKey key,
        int contentLayerIndex,
        bool includeCurrentLayerMetadata,
        std::shared_ptr<std::vector<LayerAvailabilityRequest>>
            availabilityRequests,
        std::shared_ptr<CancellationToken> token,
        std::shared_ptr<ContentCallback> callback,
        std::shared_ptr<AsyncTileRequestState> state);
    void finalizeAsyncTileRequest(
        TileKey key,
        int contentLayerIndex,
        bool includeCurrentLayerMetadata,
        std::shared_ptr<std::vector<LayerAvailabilityRequest>>
            availabilityRequests,
        std::shared_ptr<CancellationToken> token,
        std::shared_ptr<ContentCallback> callback,
        std::shared_ptr<std::vector<uint8_t>> body,
        int statusCode,
        std::vector<std::vector<uint8_t>> metadataBodies,
        bool forceRasterOverlayDetails);
    std::vector<QuantizedMeshAvailabilityUpdate>
    parseAvailabilityUpdatesFromMetadataRequests(
        const std::vector<LayerAvailabilityRequest>& availabilityRequests,
        const std::vector<std::vector<uint8_t>>& metadataBodies) const;
    mutable std::recursive_mutex layersMutex_;
    std::vector<LayerConfig> layers_;
    std::string urlTemplate_;
    std::string attribution_;
    std::string layerJsonUrl_;
    std::string schemeId_ = "Geographic-TMS";
    std::string version_;
    std::string extensionsToRequest_;
    std::vector<std::vector<TileAvailabilityRect>> availabilityRanges_;
    std::vector<std::unordered_set<uint64_t>> loadedSubtrees_;
    bool hasAvailability_ = false;
    int availabilityLevels_ = -1;  // -1 = not using subtree mode
    int minZoom_ = 0;
    int maxZoom_ = 15;
    int tileSize_ = 65;   // default 64×64 grid
    bool waterMaskEnabled_ = false;
    PlatformBridge* platformBridge_ = nullptr;
    std::vector<HttpRequestOptions::Header> requestHeaders_;
    std::atomic<int> requestsStarted_{0};
    std::atomic<int> requestsCompleted_{0};
    std::atomic<int> requestsFailed_{0};
    std::atomic<int> activeWorkerBlockingRequests_{0};
    std::atomic<int> peakWorkerBlockingRequests_{0};
    std::shared_ptr<MetadataRequestRegistry> metadataRegistry_ =
        std::make_shared<MetadataRequestRegistry>();
};

} // namespace earth_engine
