#pragma once

#include "ProviderRequestDiagnostics.h"
#include "RasterAsset.h"
#include "RasterOverlayTile.h"
#include "RasterTextureUploader.h"
#include "../tiling/TileRasterOverlayUploadResult.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileScheme.h"
#include "../tiling/SurfaceTile.h"
#include "../core/math/Rectangle.h"
#include "../core/async/WorkLedger.h"

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
#include <list>

namespace earth_engine {

class ImageryProvider;
class FrameResourceBudget;
struct DecodedImage;
class RasterAssetDepot;

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
    using ExternalSourceImageRetainer = std::function<
        std::shared_ptr<const DecodedImage>(
            const std::shared_ptr<const DecodedImage>&)>;

    // TileLoadedCallback removed — textures are now owned directly by
    // RasterOverlayTile (unique_ptr<Texture>). No external callback needed.

    /// @param provider The imagery data source (HTTP fetcher).
    /// @param scheme The tile coordinate scheme.
    /// @param textureUploader Resource-prep adapter for GPU texture creation.
    ///                        May be null for headless lifecycle tests.
    RasterOverlayTileProvider(ImageryProvider& provider,
                              const TileScheme& scheme,
                              std::unique_ptr<RasterTextureUploader> textureUploader =
                                  nullptr,
                              RasterOverlayGeoreference georeference =
                                  RasterOverlayGeoreference::Standard);
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
        std::vector<std::string> credits;
    };

    struct CompositeImageResult {
        std::unique_ptr<DecodedImage> image;
        Rectangle rectangle;
        RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        std::vector<std::string> diagnostics;
        std::vector<std::string> credits;
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
        std::vector<QuadtreeSourceImage>&& sources);

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
    void setReady(bool ready);

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
    const ImageryProvider& getImageryProvider() const { return provider_; }
    ProviderRequestDiagnostics requestDiagnostics() const;

    /// Runtime-owned decoded-source coordinator. The provider retains this
    /// shared handle so a provider can outlive its Tileset runtime safely;
    /// source mapping remains provider-local until the next depot migration.
    void setAssetDepot(std::shared_ptr<RasterAssetDepot> depot) {
        assetDepot_ = std::move(depot);
    }
    std::shared_ptr<RasterAssetDepot> assetDepot() const {
        return assetDepot_;
    }

    /// Returns the tile scheme.
    const TileScheme& getTileScheme() const { return scheme_; }

    /// Projection used when mapping geometry rectangles to raster tiles.
    RasterOverlayProjection getProjection() const {
        return projection_;
    }

    /// Returns an immutable exact source image already decoded by the shared
    /// provider depot. This is a read-through cache facade only: it never
    /// starts or joins transport and never applies ancestor fallback.
    std::optional<RasterAssetSnapshot> tryGetCachedExactSource(
        const TileKey& sourceKey);

    /// Return an aliasing image pointer whose lifetime is charged as an
    /// external consumer pin. The provider itself remains the accounting
    /// owner; callers must retain the returned pointer for the duration of
    /// their decoded-image use.
    std::shared_ptr<const DecodedImage> retainExternalSourceImage(
        const std::shared_ptr<const DecodedImage>& image);
    /// Lifetime-safe retainer for asynchronous depot callbacks. The returned
    /// closure captures only ProviderAsyncState, never the provider object.
    ExternalSourceImageRetainer externalSourceImageRetainer() const;

    /// Acquire one exact source through the provider's shared decoded-asset
    /// depot. Direct and PageStore callers join the same in-flight transport.
    /// Only a newly started transport invokes tryAdmitTransport; cache hits
    /// and joins consume no additional frame network grant.
    RasterAssetAcquireResult acquireExactSource(
        const TileKey& sourceKey,
        std::function<bool()> tryAdmitTransport,
        std::function<void(RasterAssetResponse)> onReady);

    /// Current backend-neutral identity for a source key (diagnostics/tests).
    RasterAssetKey rasterAssetKey(const TileKey& sourceKey);

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

    /// 根层常驻(见 TileBaseCoveragePin.h):开启后 z ≤ 钉扎线的影像瓦片
    /// 不被 trimUnusedTiles 驱逐。由 ActivatedRasterOverlay 按 overlay
    /// options 转发,仅底图 overlay 开启。
    bool pinBaseCoverage = false;

    /// Current number of tiles in Loading state.
    int getThrottledTilesCurrentlyLoading() const;
    int getActiveRasterSourceRequests() const {
        return static_cast<int>(
            asyncState_->activeRasterSourceRequests.load(
                std::memory_order_relaxed));
    }
    int getPendingUploadCount() const;
    int64_t getPendingUploadBytes() const;
    int64_t getPeakPendingUploadBytes() const;
    int64_t getPendingUploadBudgetBytes() const;
    int64_t getPeakPendingUploadBudgetBytes() const;

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
    /// CPU bytes retained by decoded source assets that are still pinned by a
    /// consumer (for example PageStore composition) after the source cache
    /// entry has been evicted. This is a category metric, not an additive
    /// budget: use getSourceDepotResidentBytes() for unique physical bytes.
    int64_t getExternalPinnedSourceBytes() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return asyncState_->externalPinnedSourceBytes;
    }
    /// Unique decoded-image bytes still physically resident through the
    /// provider depot, including source-cache, pending-upload and external
    /// consumer pins. A shared image is counted once even when several
    /// lifetimes overlap.
    int64_t getSourceDepotResidentBytes() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        int64_t bytes = 0;
        for (const auto& [_, refs] : asyncState_->sharedRasterImageRefs) {
            if (refs.sourceCacheRefs > 0 || refs.pendingUploadRefs > 0 ||
                refs.externalPinRefs > 0) {
                bytes += refs.sizeBytes;
            }
        }
        return bytes;
    }
    int64_t getPeakCachedSourceTileBytes() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return asyncState_->peakSourceTileDepotCacheBytes;
    }
    int getCachedSourceTileCount() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return static_cast<int>(asyncState_->sourceTileDepotCache.size());
    }
    int getCachedSourceTileLruEntryCount() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return static_cast<int>(asyncState_->sourceTileDepotCacheLru.size());
    }
    int getInFlightSourceTileCount() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return static_cast<int>(asyncState_->sourceTileDepotInFlight.size());
    }
    int getInFlightSourceWaiterCount() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        int total = 0;
        for (const auto& [_, entry] : asyncState_->sourceTileDepotInFlight) {
            total += static_cast<int>(entry.waiters.size());
        }
        return total;
    }
    int getActiveMappedSourceSetOrderCount() const {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        return static_cast<int>(asyncState_->activeMappedSourceSetOrder.size());
    }
    int getPendingSourceFallbackCount() const {
        return static_cast<int>(
            asyncState_->pendingSourceFallbackCount.load(
                std::memory_order_acquire));
    }
    void setSubTileCacheBytes(int64_t subTileCacheBytes);
    int getMinimumLevel() const;
    int getMaximumLevel() const;
    void setLevelRange(int minimumLevel, int maximumLevel);

    /// Process completed uploads on the main thread.
    /// Should be called once per frame.
    TileRasterOverlayUploadResult processPendingUploads(
        bool interactionActive,
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

    /// Monotonic mapping identity revision. Unlike revision(), this changes
    /// only when existing geometry-to-raster mappings may point at stale
    /// provider tiles because provider readiness or mapping options changed.
    uint64_t mappingRevision() const {
        return mappingRevision_;
    }

    // ── Texture cache ──

    /// Direct texture cache lookup by key.
    Texture* getTexture(const TileKey& key) const;

    /// Total cached tiles.
    int getCachedTileCount() const { return static_cast<int>(tiles_.size()); }
    int64_t tileTextureBytesUsed() const {
        return textureByteLedger_->bytes.load(std::memory_order_relaxed);
    }

    // ── Eviction ──

    /// Set the current frame number (called BEFORE tile access each frame).
    /// Subsequent getTile() calls will stamp tiles with this frame number.
    void setFrameNumber(uint64_t frame) {
        frameNumber_ = frame;
        // 每帧每 overlay 无条件调(TileRenderFrameBuilder)——用作账本对账的
        // 稳定每帧钩子:把本 provider 的在途状态同步进 WorkLedger 供 gating 审计。
        syncWorkTickets();
    }

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
    void trimUnusedTiles(bool cachePressure = false);

    /// Generation fence for a Direct backend replacement. Cancels mapped
    /// source sets, drops mapped and direct uploads/tiles and advances
    /// mappingRevision, while preserving backend-neutral exact-source cache
    /// entries that a PageStore consumer may still be using.
    void invalidateDirectExecutionState();

    // Texture ownership: RasterOverlayTile owns its GPU texture
    // via unique_ptr<Texture>. No external callback needed.

private:
    friend class RasterOverlayTile;
    using TileCache = std::unordered_map<std::string, TilePtr>;

    /// 把本 provider 的在途状态对账进 WorkLedger(setFrameNumber 每帧调)。
    /// 在 asyncState_->mutex 下读两个布尔谓词后释放锁再 reconcile,避免
    /// 账本锁与 provider 锁嵌套(WorkLedger 从不回调本类,单向即可)。
    void syncWorkTickets();
    struct ProviderAsyncState;  // 前向声明:下方 helper 的参数类型(定义见本类后文)
    /// [2026-08-21 冻屏根修] worker 完成/派发路径的 Landing 票同步(线程安全)。
    /// 见 syncWorkTickets 注释;state 由 worker 回调持有(alive 守卫)。
    static void syncRasterLandingTicketFromAnyThread(
        const std::shared_ptr<ProviderAsyncState>& state);
    /// 调用方已持 state->mutex 时的变体(避免自死锁)。
    static void syncRasterLandingTicketLocked(
        const std::shared_ptr<ProviderAsyncState>& state);

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
    void syncProviderContentRevision();
    static uint64_t nextSourceWaiterOwnerToken();

    /// Internal: load a mapped raster tile by combining the provider's
    /// quadtree imagery tiles that overlap its geometry rectangle.
    bool loadMappedRasterTile(RasterOverlayTile& tile,
                              FrameResourceBudget* budget = nullptr);
    bool pumpLoadingMappedRasterTile(RasterOverlayTile& tile,
                                     FrameResourceBudget* budget);
    int issueMappedSourceImageSet(
        const std::shared_ptr<MappedSourceImageSet>& sourceSet,
        FrameResourceBudget* budget);
    int issuePendingSourceFallbacks(FrameResourceBudget* budget);
    int issueActiveMappedSourceImageSets(
        FrameResourceBudget* budget,
        double* fallbackMs,
        double* snapshotMs,
        double* issueMs);
    int estimateNewSourceRequestsForSourceKeys(
        const std::vector<TileKey>& sourceKeys) const;
    bool mappedTileWouldIssueNewSourceRequests(
        const RasterOverlayTile& tile) const;

    /// Tile cache key from TileKey.
    std::string tileCacheKey(const TileKey& key) const;
    void insertCachedTile(const std::string& cacheKey, TilePtr tile);
    void touchCachedTile(const std::string& cacheKey);
    void touchCachedTile(RasterOverlayTile& tile);
    TileCache::iterator eraseCachedTile(TileCache::iterator it);
    // 按谓词批量清除已缓存瓦:命中 predicate 的条目在 eraseCachedTile 之前先跑
    // beforeErase(如置败)。四路失效与 setCoverageRectangle 共用,消除三处重复循环。
    void eraseCachedTilesMatching(
        const std::function<bool(const std::string&, const TilePtr&)>&
            predicate,
        const std::function<void(const TilePtr&)>& beforeErase = {});
    void clearCachedTiles();
    std::shared_ptr<RasterTextureByteLedger> textureByteLedgerForTiles() const {
        return textureByteLedger_;
    }
    void invalidateDirectRasterTileCache();
    void invalidateMappedRasterTileCache();
    void invalidateSourceAssetDepotCache();
    void abandonActiveSourceSets(bool mappedOnly);
    void discardPendingUploadsForMissingTiles();
    bool pendingUploadBackpressureActive() const;

    ImageryProvider& provider_;
    const TileScheme& scheme_;
    RasterOverlayProjection projection_ = RasterOverlayProjection::Geographic;
    std::unique_ptr<RasterTextureUploader> textureUploader_;
    class RasterOverlay* owner_ = nullptr;
    Rectangle coverageRectangle_ = Rectangle::MAXIMUM;
    Rectangle sourceCoverageRectangle_ = Rectangle::MAXIMUM;

    /// All cached tiles retained by this provider (key → shared_ptr).
    TileCache tiles_;
    std::list<RasterOverlayTile*> tileCacheLru_;
    std::shared_ptr<RasterTextureByteLedger> textureByteLedger_ =
        std::make_shared<RasterTextureByteLedger>();

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
        std::vector<std::string> credits;
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
        std::vector<std::string> credits;
        bool terminalFailure = false;
        // A failed exact transport is cacheable for ExactOnly consumers, but
        // Direct must treat it as the start of its ancestor-resolution policy
        // rather than as an already exhausted fallback chain.
        bool exactTransportFailure = false;
        int64_t sizeBytes = 0;
        uint64_t generation = 0;
    };
    struct InFlightSourceTileAsset {
        using Result = std::shared_ptr<const SourceTileAsset>;
        struct WaiterEntry {
            uint64_t ownerToken = 0;
            std::function<void(Result)> callback;
            bool usesAncestorFallback = false;
            // Fallback is a consumer policy, not a property of the shared
            // exact transport. Exact-only consumers are completed as soon as
            // that transport fails; Direct consumers remain attached to the
            // original key while this continuation starts/join the parent
            // chain.
            std::function<void()> continueWithParentFallback;
        };
        std::vector<WaiterEntry> waiters;
    };
    struct PendingSourceFallback {
        TileKey originalKey;
        TileKey requestedKey;
        uint64_t ownerToken = 0;
        // Returns -1 when Scene/local admission denied the transport before
        // provider.requestTile, 0 for cache/in-flight reuse, or 1 when a new
        // source request was issued.
        std::function<int(std::function<bool()>)> issue;
    };
    struct RetiredAsyncResources {
        RetiredAsyncResources() {
            pendingUploads.reserve(8);
            sourceAssets.reserve(8);
            inFlightSources.reserve(8);
            sourceSets.reserve(8);
        }

        std::vector<PendingUpload> pendingUploads;
        std::vector<SourceTileAsset> sourceAssets;
        std::vector<InFlightSourceTileAsset> inFlightSources;
        std::vector<std::shared_ptr<MappedSourceImageSet>> sourceSets;
    };

    /// Shared runtime state touched by async raster callbacks. It intentionally
    /// outlives RasterOverlayTileProvider when a source request completes
    /// after overlay/provider destruction, matching cesium-native's depot
    /// lifetime model without letting callbacks dereference a dead provider.
    struct ProviderAsyncState {
        struct SharedRasterImageRefs {
            int64_t sizeBytes = 0;
            uint32_t sourceCacheRefs = 0;
            uint32_t pendingUploadRefs = 0;
            uint32_t externalPinRefs = 0;
        };
        std::deque<PendingUpload> pendingUploads;
        mutable std::mutex mutex;
        std::unordered_map<std::string, SourceTileAsset>
            sourceTileDepotCache;
        std::unordered_map<std::string, InFlightSourceTileAsset>
            sourceTileDepotInFlight;
        std::unordered_map<uint64_t, std::vector<TileKey>>
            sourceTileDepotFallbackKeysByOwner;
        std::unordered_set<uint64_t> activeMappedSourceOwnerTokens;
        std::unordered_map<std::string, std::shared_ptr<MappedSourceImageSet>>
            activeMappedSourceSets;
        std::deque<std::string> activeMappedSourceSetOrder;
        std::deque<PendingSourceFallback> pendingSourceFallbacks;
        std::atomic<uint32_t> pendingSourceFallbackCount{0};
        // Multi-source raster composition becomes ready from provider
        // callbacks, but the Scene arbiter is frame-scoped and must never be
        // captured by those callbacks. Keep ready work here until the render
        // thread pumps it with the current frame's Raster/WorkerDispatch
        // grant.
        std::deque<std::function<void()>> pendingRasterComposeTasks;
        // Once a load is started with a Scene frame budget, keep subsequent
        // multi-source composition callbacks on the Scene-managed handoff
        // path. This is sticky for the provider lifetime so callbacks never
        // retain a frame-scoped arbiter pointer.
        std::atomic<bool> sceneResourceManaged{false};
        std::deque<std::pair<std::string, uint64_t>>
            sourceTileDepotCacheLru;
        std::unordered_map<const DecodedImage*, SharedRasterImageRefs>
            sharedRasterImageRefs;
        // Category metric for images retained by a consumer lease. It may
        // overlap sourceTileDepotCacheBytes while the cache entry is live;
        // getSourceDepotResidentBytes() performs unique-image accounting.
        int64_t externalPinnedSourceBytes = 0;
        int64_t pendingUploadBytes = 0;
        int64_t pinnedSharedPendingUploadBytes = 0;
        int64_t peakPendingUploadBytes = 0;
        int64_t peakPendingUploadBudgetBytes = 0;
        int64_t sourceTileDepotCacheBytes = 0;
        int64_t peakSourceTileDepotCacheBytes = 0;
        int64_t subTileCacheBytes = 16 * 1024 * 1024;
        std::atomic<bool> pendingUploadBackpressure{false};
        uint64_t sourceTileDepotGeneration = 0;
        uint64_t sourceTileDepotEpoch = 0;
        std::unordered_set<std::string> inFlightRequests;
        std::atomic<uint32_t> activeRasterTileLoads{0};
        std::atomic<uint32_t> activeRasterSourceRequests{0};
        std::atomic<uint32_t> activeRasterComposeTasks{0};
        std::atomic<uint32_t> activeDeferredUploadReleases{0};
        // [2026-08-21 冻屏根修] 票槽入共享状态:worker 完成/派发路径直接
        // reconcile(生命周期随 state 的 alive 守卫,不依赖 provider 存活)。
        WorkTicketSlot loadSlot_;    ///< Landing: inFlight/compose/source/depot/...
        WorkTicketSlot uploadSlot_;  ///< Pumped: pendingUploads(每帧限量 drain)
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
                   activeRasterComposeTasks.load(std::memory_order_acquire) > 0 ||
                   activeDeferredUploadReleases.load(
                       std::memory_order_acquire) > 0;
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
    static void enforceSourceDepotBudgetLocked(
        ProviderAsyncState& state,
        RetiredAsyncResources& retired);
    // 按谓词从 pendingUploads 清除条目(释放字节→retired→末尾 enforce budget)。
    // 调用方须已持 state.mutex。invalidateMapped 与 discardPending... 共用。
    static void erasePendingUploadsMatchingLocked(
        ProviderAsyncState& state,
        const std::function<bool(const PendingUpload&)>& predicate,
        RetiredAsyncResources& retired);
    static void clearSourceDepotInFlightLocked(
        ProviderAsyncState& state,
        RetiredAsyncResources& retired);
    static void compactSourceDepotCacheLruLocked(ProviderAsyncState& state);
    static void compactActiveMappedSourceSetOrderLocked(
        ProviderAsyncState& state);
    static void retainPendingUploadImageBytesLocked(
        ProviderAsyncState& state,
        const PendingUpload& upload);
    static void releasePendingUploadImageBytesLocked(
        ProviderAsyncState& state,
        const PendingUpload& upload);
    static void releaseOwnedPendingUploadImageBytesLocked(
        ProviderAsyncState& state,
        int64_t imageBytes);
    static void retainSourceCacheImageBytesLocked(
        ProviderAsyncState& state,
        const std::shared_ptr<const DecodedImage>& image);
    static void releaseSourceCacheImageBytesLocked(
        ProviderAsyncState& state,
        const std::shared_ptr<const DecodedImage>& image);
    static int64_t externalOnlyResidentBytesLocked(
        const ProviderAsyncState& state);
    std::shared_ptr<const DecodedImage> pinDecodedImage(
        const std::shared_ptr<const DecodedImage>& image);
    static std::shared_ptr<const DecodedImage> pinDecodedImage(
        const std::shared_ptr<ProviderAsyncState>& state,
        const std::shared_ptr<const DecodedImage>& image);
    static void trackPendingUploadBudgetPeakLocked(ProviderAsyncState& state);
    static void updatePendingUploadBackpressureLocked(
        ProviderAsyncState& state);
    static void clearSourceDepotCacheLocked(
        ProviderAsyncState& state,
        RetiredAsyncResources& retired);
    static int64_t pendingUploadSizeBytes(const PendingUpload& upload);
    static void clearPendingUploadsLocked(
        ProviderAsyncState& state,
        RetiredAsyncResources& retired);
    std::shared_ptr<ProviderAsyncState> asyncState_ =
        std::make_shared<ProviderAsyncState>();
    std::shared_ptr<RasterAssetDepot> assetDepot_;
    std::shared_ptr<QuadtreeSourceAssetDepot> sourceAssetDepot_;

    /// Monotonic frame counter, updated by trimUnusedTiles.
    /// Used to stamp lastUsedFrame on tiles in getTile().
    uint64_t frameNumber_ = 0;

    uint64_t mappedRasterTileEpoch_ = 0;
    uint64_t mappingRevision_ = 0;
    uint64_t observedProviderContentRevision_ = 0;
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
