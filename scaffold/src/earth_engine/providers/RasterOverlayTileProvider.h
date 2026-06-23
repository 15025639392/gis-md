#pragma once

#include "ProviderRequestDiagnostics.h"
#include "RasterOverlayTile.h"
#include "RasterTextureUploader.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileScheme.h"
#include "../tiling/SurfaceTile.h"
#include "../core/math/Rectangle.h"

#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <functional>
#include <atomic>
#include <cstdint>
#include <future>

namespace earth_engine {

class ImageryProvider;
class FrameResourceBudget;
struct DecodedImage;

/// cesium-native RasterOverlayTileProvider equivalent.
///
/// Owns the lifecycle of RasterOverlayTile instances. Handles tile creation,
/// async loading dispatch, upload scheduling, throttling, and cache management.
///
/// This owns raster tile runtime state: cache, pending uploads, failed tiles,
/// request throttling, and frame-based trimming.
class RasterOverlayTileProvider {
public:
    using TilePtr = std::shared_ptr<RasterOverlayTile>;
    using ConstTilePtr = std::shared_ptr<const RasterOverlayTile>;

    // TileLoadedCallback removed — textures are now owned directly by
    // RasterOverlayTile (unique_ptr<Texture>). No external callback needed.

    /// @param provider The imagery data source (HTTP fetcher).
    /// @param scheme The tile coordinate scheme.
    /// @param textureUploader Resource-prep adapter for GPU texture creation.
    ///                        May be null for headless lifecycle tests.
    RasterOverlayTileProvider(ImageryProvider& provider,
                              const TileScheme& scheme,
                              std::unique_ptr<RasterTextureUploader> textureUploader =
                                  nullptr);
    ~RasterOverlayTileProvider();

    RasterOverlayTileProvider(const RasterOverlayTileProvider&) = delete;
    RasterOverlayTileProvider& operator=(const RasterOverlayTileProvider&) = delete;

    struct QuadtreeSourceImage {
        QuadtreeSourceImage() = default;
        QuadtreeSourceImage(
            TileKey key_,
            Rectangle bounds_,
            std::unique_ptr<DecodedImage> image_,
            std::optional<Rectangle> sourceSubset_,
            RasterOverlayTile::MoreDetailAvailable moreDetailAvailable_,
            std::vector<std::string> diagnostics_ = {})
            : key(std::move(key_)),
              bounds(bounds_),
              image(std::move(image_)),
              sourceSubset(std::move(sourceSubset_)),
              moreDetailAvailable(moreDetailAvailable_),
              diagnostics(std::move(diagnostics_)) {}

        TileKey key;
        Rectangle bounds;
        std::unique_ptr<DecodedImage> image;
        std::optional<Rectangle> sourceSubset;
        RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::Unknown;
        std::vector<std::string> diagnostics;
    };

    struct CompositeImageResult {
        std::unique_ptr<DecodedImage> image;
        Rectangle rectangle;
        RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        std::vector<std::string> diagnostics;
    };

    struct RasterSourceTileMapping {
        int sourceZoom = 0;
        Rectangle sourceBounds = Rectangle::MAXIMUM;
        std::vector<TileKey> sourceKeys;
        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;

        int budgetUnits() const {
            return static_cast<int>(sourceKeys.size());
        }

        bool empty() const { return sourceKeys.empty(); }
    };

    struct RasterTileMapping {
        TilePtr tile;
        bool directTile = false;
        RasterSourceTileMapping sourceTiles;
    };

    static CompositeImageResult composeQuadtreeSourceImagesWithDetails(
        const TileScheme& scheme,
        const Rectangle& targetBounds,
        int sourceZoom,
        std::vector<QuadtreeSourceImage>&& sources,
        int maximumSourceZoom);

    static double projectedVForLatitude(
        const TileScheme& scheme,
        const Rectangle& bounds,
        double lat);

    // ── Tile lifecycle ──

    /// cesium-native: get or create a tile for a key.
    /// Returns the shared placeholder tile if the provider is not yet ready.
    TilePtr getTile(const TileKey& key);

    /// cesium-native QuadtreeRasterOverlayTileProvider::
    /// mapRasterTilesToGeometryTile equivalent. Maps the geometry tile's
    /// projected rectangle to this provider's quadtree raster cache. Exact
    /// single-source mappings return the direct quadtree tile; multi-source
    /// mappings return a composed implementation tile backed by the same
    /// source-tile plan.
    RasterTileMapping mapRasterTilesToGeometryTile(
        const Rectangle& geometryRectangle,
        double targetScreenPixelsX,
        double targetScreenPixelsY);

    /// cesium-native: returns whether the provider is ready to serve tiles.
    bool isReady() const { return ready_; }
    void setReady(bool ready) { ready_ = ready; }

    /// cesium-native: returns the shared placeholder tile.
    TilePtr getPlaceholderTile();

    /// cesium-native: find the best available tile covering the given bounds
    /// at ≤ desiredZoom. Returns nullptr if no tile is available.
    TilePtr resolveTile(const Rectangle& bounds, int desiredZoom);

    /// cesium-native: returns the owner RasterOverlay.
    class RasterOverlay* getOwner() const { return owner_; }
    void setOwner(RasterOverlay* owner);
    std::shared_future<void> getAsyncDestructionCompleteEvent();
    void applyOwnerOptions();
    const Rectangle& getCoverageRectangle() const { return coverageRectangle_; }
    void setCoverageRectangle(const Rectangle& coverageRectangle);

    /// Direct access to the imagery provider.
    ImageryProvider& getImageryProvider() { return provider_; }
    ProviderRequestDiagnostics requestDiagnostics() const;

    /// Returns the tile scheme.
    const TileScheme& getTileScheme() const { return scheme_; }

    /// Projection used when mapping geometry rectangles to raster tiles.
    RasterOverlayProjection getProjection() const {
        return projection_;
    }

    // ── Async loading ──

    /// cesium-native: initiate async load for a tile.
    /// Transitions state to Loading and issues HTTP request.
    /// @return true if the load was initiated, false if already loading/loaded.
    bool loadTile(RasterOverlayTile& tile,
                  FrameResourceBudget* budget = nullptr);

    /// cesium-native: throttled load. Returns false if at concurrent limit.
    bool loadTileThrottled(RasterOverlayTile& tile,
                           FrameResourceBudget* budget = nullptr);

    /// Maximum concurrent tile loads.
    int maximumSimultaneousTileLoads = 20;

    /// Current number of tiles in Loading state.
    int getThrottledTilesCurrentlyLoading() const;
    int getActiveRasterSourceRequests() const {
        return static_cast<int>(
            asyncState_->activeRasterSourceRequests.load(
                std::memory_order_relaxed));
    }
    int getPendingUploadCount() const;

    double getMaximumScreenSpaceError() const {
        return maximumScreenSpaceError_;
    }
    void setMaximumScreenSpaceError(double maximumScreenSpaceError);
    int getMaximumTextureSize() const { return maximumTextureSize_; }
    void setMaximumTextureSize(int maximumTextureSize);
    int64_t getSubTileCacheBytes() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return asyncState_->subTileCacheBytes;
    }
    int64_t getCachedSourceTileBytes() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return asyncState_->sourceTileDepotCacheBytes;
    }
    int getCachedSourceTileCount() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return static_cast<int>(asyncState_->sourceTileDepotCache.size());
    }
    void setSubTileCacheBytes(int64_t subTileCacheBytes);
    int getMinimumLevel() const;
    int getMaximumLevel() const;
    void setLevelRange(int minimumLevel, int maximumLevel);

    /// Process completed uploads on the main thread.
    /// Should be called once per frame.
    int processPendingUploads(bool interactionActive,
                              FrameResourceBudget* budget = nullptr);

    /// True while HTTP requests, queued raster source fanout, or main-thread
    /// texture uploads are outstanding.
    bool hasPendingWork() const;

    /// Monotonic state revision. Increments when raster tile load state or GPU
    /// texture readiness changes, so diagnostics and cache users can observe
    /// provider-side progress without walking every mapped tile.
    uint64_t revision() const {
        return asyncState_->revision.load(std::memory_order_relaxed);
    }

    // ── Texture cache ──

    /// Direct texture cache lookup by key.
    Texture* getTexture(const TileKey& key) const;

    /// Total cached tiles.
    int getCachedTileCount() const { return static_cast<int>(tiles_.size()); }

    // ── Eviction ──

    /// Set the current frame number (called BEFORE tile access each frame).
    /// Subsequent getTile() calls will stamp tiles with this frame number.
    void setFrameNumber(uint64_t frame) { frameNumber_ = frame; }

    /// Mark a tile as used in the current frame (updates lastUsedFrame).
    void markUsed(const std::string& cacheKey);
    void markUsed(const TileKey& key);
    void markUsed(const RasterOverlayTile& tile);

    /// True when this tile is still the provider-owned cache entry for its
    /// identity. Mapped raster cache keys include a provider mapping epoch, so
    /// option changes invalidate old geometry-to-raster compositions.
    bool ownsCurrentTile(const RasterOverlayTile& tile) const;

    /// Evict tiles that have not been referenced recently.
    /// Called once per frame from Tileset::buildRenderCommands,
    /// AFTER all tile access for the frame is complete.
    void trimUnusedTiles();

    // Texture ownership: RasterOverlayTile owns its GPU texture
    // via unique_ptr<Texture>. No external callback needed.

private:
    struct QuadtreeSourcePlan {
        int sourceZoom = 0;
        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;
        std::vector<TileKey> sourceKeys;

        int budgetUnits() const {
            return static_cast<int>(sourceKeys.size());
        }

        bool empty() const { return sourceKeys.empty(); }
    };

    struct MappedSourceImageSet;
    struct QuadtreeSourceAssetDepot;

    static QuadtreeSourcePlan buildQuadtreeSourcePlan(
        const TileScheme& scheme,
        const ImageryProvider& provider,
        const RasterTextureUploader* uploader,
        const Rectangle& geometryBounds,
        const Rectangle& sourceBounds,
        double targetScreenPixelsX,
        double targetScreenPixelsY,
        double maximumScreenSpaceError,
        int maximumTextureSize,
        int minimumLevel,
        int maximumLevel);

    bool loadSourceTileList(RasterOverlayTile& tile,
                            RasterSourceTileMapping sourceTiles,
                            const Rectangle& targetBounds,
                            const std::string& cacheKey,
                            FrameResourceBudget* budget);
    bool loadSourceImageSet(RasterOverlayTile& tile,
                            RasterSourceTileMapping sourceTiles,
                            const Rectangle& targetBounds,
                            const std::string& cacheKey,
                            FrameResourceBudget* budget);
    void refreshSourceAssetDepot();

    /// Internal: load a mapped raster tile by combining the provider's
    /// quadtree imagery tiles that overlap its geometry rectangle.
    bool loadMappedRasterTile(RasterOverlayTile& tile,
                              FrameResourceBudget* budget = nullptr);
    int issueMappedSourceImageSet(
        const std::shared_ptr<MappedSourceImageSet>& sourceSet,
        FrameResourceBudget* budget);

    /// Tile cache key from TileKey.
    std::string tileCacheKey(const TileKey& key) const;
    void invalidateDirectRasterTileCache();
    void invalidateMappedRasterTileCache();
    void invalidateSourceAssetDepotCache();

    ImageryProvider& provider_;
    const TileScheme& scheme_;
    RasterOverlayProjection projection_ = RasterOverlayProjection::Geographic;
    std::unique_ptr<RasterTextureUploader> textureUploader_;
    class RasterOverlay* owner_ = nullptr;
    Rectangle coverageRectangle_ = Rectangle::MAXIMUM;

    /// All cached tiles retained by this provider (key → shared_ptr).
    std::unordered_map<std::string, TilePtr> tiles_;

    /// cesium-native: shared placeholder tile returned when provider is not ready.
    TilePtr placeholderTile_;
    bool ready_ = true;

    /// Pending GPU uploads (HTTP completed, awaiting main-thread upload).
    struct PendingUpload {
        std::string cacheKey;
        std::unique_ptr<DecodedImage> image;
        std::shared_ptr<const DecodedImage> sharedImage;
        Rectangle rectangle;
        RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::Unknown;
        std::vector<std::string> diagnostics;
    };

    /// Provider-level source imagery depot, matching cesium-native
    /// SharedAssetDepot ownership. Geometry requests may compose different
    /// output tiles, but the underlying quadtree source tile is shared by
    /// TileKey here rather than owned by an individual mapped source request.
    struct SourceTileAsset {
        TileKey key;
        Rectangle bounds;
        std::shared_ptr<const DecodedImage> image;
        std::optional<Rectangle> sourceSubset;
        RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::Unknown;
        std::vector<std::string> diagnostics;
        bool terminalFailure = false;
        int64_t sizeBytes = 0;
        uint64_t generation = 0;
    };
    struct InFlightSourceTileAsset {
        using Result = std::shared_ptr<const SourceTileAsset>;
        std::vector<std::function<void(Result)>> waiters;
    };

    /// Shared runtime state touched by async raster callbacks. It intentionally
    /// outlives RasterOverlayTileProvider when a source request completes
    /// after overlay/provider destruction, matching cesium-native's depot
    /// lifetime model without letting callbacks dereference a dead provider.
    struct ProviderAsyncState {
        std::deque<PendingUpload> pendingUploads;
        mutable std::mutex mutex;
        std::unordered_map<std::string, SourceTileAsset>
            sourceTileDepotCache;
        std::unordered_map<std::string, InFlightSourceTileAsset>
            sourceTileDepotInFlight;
        std::deque<std::pair<std::string, uint64_t>>
            sourceTileDepotCacheLru;
        int64_t sourceTileDepotCacheBytes = 0;
        int64_t subTileCacheBytes = 16 * 1024 * 1024;
        uint64_t sourceTileDepotGeneration = 0;
        uint64_t sourceTileDepotEpoch = 0;
        std::unordered_set<std::string> inFlightRequests;
        std::atomic<uint32_t> activeRasterTileLoads{0};
        std::atomic<uint32_t> activeRasterSourceRequests{0};
        std::atomic<uint32_t> activeRasterComposeTasks{0};
        std::atomic<uint32_t> peakRasterSourceRequests{0};
        std::atomic<int> rasterSourceRequestsStarted{0};
        std::atomic<int> rasterSourceRequestsCompleted{0};
        std::atomic<int> rasterSourceRequestsFailed{0};
        std::atomic<uint64_t> revision{0};
        std::atomic<bool> alive{true};
        mutable std::mutex destructionMutex;
        std::shared_ptr<std::promise<void>> destructionCompletePromise;
        std::shared_future<void> destructionCompleteFuture;
        bool destructionCompleteResolved = false;

        bool hasActiveAsyncWork() const {
            return activeRasterTileLoads.load(std::memory_order_acquire) > 0 ||
                   activeRasterSourceRequests.load(std::memory_order_acquire) > 0 ||
                   activeRasterComposeTasks.load(std::memory_order_acquire) > 0;
        }

        void resolveDestructionIfComplete() {
            std::shared_ptr<std::promise<void>> promise;
            {
                std::lock_guard<std::mutex> lock(destructionMutex);
                if (alive.load(std::memory_order_acquire) ||
                    hasActiveAsyncWork() ||
                    !destructionCompletePromise ||
                    destructionCompleteResolved) {
                    return;
                }
                destructionCompleteResolved = true;
                promise = destructionCompletePromise;
            }
            promise->set_value();
        }
    };
    std::shared_ptr<ProviderAsyncState> asyncState_ =
        std::make_shared<ProviderAsyncState>();
    std::shared_ptr<QuadtreeSourceAssetDepot> sourceAssetDepot_;

    /// Monotonic frame counter, updated by trimUnusedTiles.
    /// Used to stamp lastUsedFrame on tiles in getTile().
    uint64_t frameNumber_ = 0;
    uint64_t mappedRasterTileEpoch_ = 0;
    double maximumScreenSpaceError_ = 2.0;
    int maximumTextureSize_ = 2048;
    int minimumLevel_ = 0;
    int maximumLevel_ = 0;
    bool hasAppliedOwnerOptions_ = false;
    Rectangle appliedOwnerCoverageRectangle_ = Rectangle::MAXIMUM;
    double appliedOwnerMaximumScreenSpaceError_ = 2.0;
    int appliedOwnerMaximumTextureSize_ = 2048;
    int64_t appliedOwnerSubTileCacheBytes_ = -1;
    int appliedOwnerMinimumLevel_ = 0;
    int appliedOwnerMaximumLevel_ = 0;

};

} // namespace earth_engine
