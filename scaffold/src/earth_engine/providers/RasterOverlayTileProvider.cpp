#include "RasterOverlayTileProvider.h"
#include "RasterOverlayImageCompositing.h"
#include "RasterOverlaySourceDepot.h"
#include "ImageryProvider.h"
#include "../layers/RasterOverlay.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../tiling/TileBaseCoveragePin.h"
#include "../tiling/TileScheme.h"
#include "RasterTextureUploader.h"
#include "../renderer/RenderDevice.h"
#include "../threading/CancellationToken.h"
#include "../core/async/AsyncSystem.h"
#include "../debug/PerfTimer.h"
#include "../tiling/TileRasterOverlayUploadResult.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/math/MathUtils.h"
#include "../debug/PlatformLog.h"
#include "../debug/Policies.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

constexpr uint64_t kRetainedUnusedFrames = 120;
constexpr int kMaximumCombinedTextureSizeFallback = 2048;
std::atomic<uint64_t> gNextRasterSourceWaiterOwnerToken{1};


int maximumCombinedTextureSize(const RasterTextureUploader* uploader,
                               int configuredMaximumTextureSize) {
    const int configuredMaxTextureSize =
        configuredMaximumTextureSize > 0
            ? configuredMaximumTextureSize
            : kMaximumCombinedTextureSizeFallback;
    if (!uploader) return configuredMaxTextureSize;
    const int backendMaxTextureSize = uploader->maxTextureSize();
    if (backendMaxTextureSize <= 0) {
        return configuredMaxTextureSize;
    }
    return std::max(1, std::min(backendMaxTextureSize,
                                configuredMaxTextureSize));
}

double normalizeRectangleCacheCoordinate(double value) {
    return std::abs(value) < 1e-15 ? 0.0 : value;
}

std::string rectangleCacheKey(const Rectangle& rectangle) {
    char bounds[256];
    std::snprintf(bounds,
                  sizeof(bounds),
                  "%.15g/%.15g/%.15g/%.15g",
                  normalizeRectangleCacheCoordinate(rectangle.west()),
                  normalizeRectangleCacheCoordinate(rectangle.south()),
                  normalizeRectangleCacheCoordinate(rectangle.east()),
                  normalizeRectangleCacheCoordinate(rectangle.north()));
    return bounds;
}

std::string directCompositeTileCacheKey(
    const TileScheme& scheme,
    const Rectangle& geometryRectangle,
    const Rectangle& sourceBounds,
    const RasterOverlaySourcePlan& sourceTiles,
    uint64_t epoch) {
    std::string key = "direct-composite/epoch/" + std::to_string(epoch) + "/" +
                      scheme.id() + "/srcz/" +
                      std::to_string(sourceTiles.sourceZoom) + "/geom/" +
                      rectangleCacheKey(geometryRectangle) + "/src/" +
                      rectangleCacheKey(sourceBounds) + "/range/" +
                      std::to_string(sourceTiles.minX) + "/" +
                      std::to_string(sourceTiles.minY) + "/" +
                      std::to_string(sourceTiles.maxX) + "/" +
                      std::to_string(sourceTiles.maxY) + "/tiles";
    for (const TileKey& sourceKey : sourceTiles.sourceKeys) {
        key += "/" + std::to_string(sourceKey.z) + "/" +
               std::to_string(sourceKey.x) + "/" +
               std::to_string(sourceKey.y);
    }
    return key;
}

bool isDirectCompositeCacheKey(const std::string& cacheKey) {
    return cacheKey.rfind("direct-composite/", 0) == 0;
}

bool isEpochDirectCompositeCacheKey(const std::string& cacheKey) {
    return cacheKey.rfind("direct-composite/epoch/", 0) == 0;
}

} // namespace

uint64_t RasterOverlayTileProvider::nextSourceWaiterOwnerToken() {
    return gNextRasterSourceWaiterOwnerToken.fetch_add(
        1, std::memory_order_relaxed);
}

int RasterOverlayTileProvider::effectiveMaximumTextureSize() const {
    return maximumCombinedTextureSize(
        textureUploader_.get(), maximumTextureSize_);
}

RasterOverlayTileProvider::CompositeImageResult
RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    std::vector<QuadtreeSourceImage>&& publicSources) {
    std::vector<RasterSourceResult> sources;
    sources.reserve(publicSources.size());
    for (auto& source : publicSources) {
        sources.push_back(RasterSourceResult{
            source.key,
            source.bounds,
            std::shared_ptr<const DecodedImage>(std::move(source.image)),
            source.sourceSubset,
            source.moreDetailAvailable,
            std::move(source.diagnostics),
            std::move(source.credits),
            false});
    }
    if (!hasNonAncestorRasterSourceImage(sources)) {
        CompositeImageResult result;
        result.image = std::make_unique<DecodedImage>();
        result.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        for (RasterSourceResult& source : sources) {
            result.diagnostics.insert(
                result.diagnostics.end(),
                std::make_move_iterator(source.diagnostics.begin()),
                std::make_move_iterator(source.diagnostics.end()));
        }
        return result;
    }
    return combineQuadtreeSourceImages(
        scheme,
        targetBounds,
        std::move(sources));
}

double RasterOverlayTileProvider::projectedVForLatitude(
    const TileScheme& scheme,
    const Rectangle& bounds,
    double lat) {
    return projectedVForLatitudeInternal(scheme, bounds, lat);
}


RasterOverlayTileProvider::RasterOverlayTileProvider(ImageryProvider& provider,
                                                     const TileScheme& scheme,
                                                     std::unique_ptr<RasterTextureUploader> textureUploader,
                                                     RasterOverlayGeoreference georeference)
    : provider_(provider)
    , scheme_(scheme)
    , projection_(projectionForScheme(scheme, georeference))
    , textureUploader_(std::move(textureUploader))
    , observedProviderContentRevision_(provider.contentRevision()) {
    sourceCoverageRectangle_ =
        worldRectangleToRasterSource(coverageRectangle_, projection_);
    refreshSourceAssetDepot();
}

RasterOverlayTileProvider::~RasterOverlayTileProvider() {
    clearCachedTiles();
    placeholderTile_.reset();
    asyncState_->alive.store(false, std::memory_order_release);
    RetiredAsyncResources retired;
    std::vector<std::shared_ptr<DirectCompositeSourceImageSet>> abandonedSourceSets;
    std::deque<std::function<void()>> abandonedComposeTasks;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        // pendingUploads 的节流名额已在加载完成入队时释放，直接丢弃
        clearPendingUploadsLocked(*asyncState_, retired);
        for (auto& [_, sourceSet] : asyncState_->activeDirectCompositeSourceSets) {
            if (sourceSet) {
                abandonedSourceSets.push_back(std::move(sourceSet));
            }
        }
        asyncState_->activeDirectCompositeSourceSets.clear();
        asyncState_->activeDirectCompositeSourceSetOrder.clear();
        asyncState_->sourceTileDepotFallbackKeysByOwner.clear();
        asyncState_->activeDirectCompositeSourceOwnerTokens.clear();
        asyncState_->pendingSourceFallbacks.clear();
        asyncState_->pendingSourceFallbackCount.store(
            0,
            std::memory_order_release);
        abandonedComposeTasks.swap(
            asyncState_->pendingRasterComposeTasks);
        asyncState_->inFlightRequests.clear();
    }
    for (const auto& sourceSet : abandonedSourceSets) {
        sourceSet->markAbandoned();
        sourceSet->releaseThrottleSlotOnce();
    }
    for (std::function<void()>& task : abandonedComposeTasks) {
        if (task) {
            task();
        }
    }
    asyncState_->resolveDestructionIfComplete();
}

std::shared_future<void>
RasterOverlayTileProvider::getAsyncDestructionCompleteEvent() {
    {
        std::lock_guard<std::mutex> lock(asyncState_->destructionMutex);
        if (!asyncState_->destructionCompletePromise) {
            asyncState_->destructionCompletePromise =
                std::make_shared<std::promise<void>>();
            asyncState_->destructionCompleteFuture =
                asyncState_->destructionCompletePromise->get_future().share();
        }
    }
    asyncState_->resolveDestructionIfComplete();
    return asyncState_->destructionCompleteFuture;
}

void RasterOverlayTileProvider::setReady(bool ready) {
    if (ready_ == ready) {
        return;
    }

    ready_ = ready;
    ++mappingRevision_;
    if (ready_) {
        asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    abandonActiveSourceSets(false);

    RetiredAsyncResources retired;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        ++asyncState_->sourceTileDepotEpoch;
        clearSourceDepotCacheLocked(*asyncState_, retired);
        clearSourceDepotInFlightLocked(*asyncState_, retired);
        asyncState_->inFlightRequests.clear();
        asyncState_->activeDirectCompositeSourceSetOrder.clear();
        asyncState_->sourceTileDepotFallbackKeysByOwner.clear();
        asyncState_->activeDirectCompositeSourceOwnerTokens.clear();
        asyncState_->pendingSourceFallbacks.clear();
        asyncState_->pendingSourceFallbackCount.store(
            0,
            std::memory_order_release);
        // pendingUploads 的节流名额已在加载完成入队时释放，直接丢弃
        clearPendingUploadsLocked(*asyncState_, retired);
    }

    for (auto& entry : tiles_) {
        if (!entry.second) {
            continue;
        }
        entry.second->setMoreDetailAvailable(
            RasterOverlayTile::MoreDetailAvailable::No);
        entry.second->setState(RasterOverlayTile::LoadState::Failed);
    }
    clearCachedTiles();

    refreshSourceAssetDepot();
    asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
    asyncState_->resolveDestructionIfComplete();
}

void RasterOverlayTileProvider::setOwner(RasterOverlay* owner) {
    owner_ = owner;
    applyOwnerOptions();
}

void RasterOverlayTileProvider::applyOwnerOptions() {
    syncProviderContentRevision();
    if (!owner_) {
        return;
    }
    const RasterOverlay::Options& options = owner_->getOptions();
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerCoverageRectangle_ != options.coverageRectangle) {
        setCoverageRectangle(options.coverageRectangle);
        appliedOwnerCoverageRectangle_ = options.coverageRectangle;
    }
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerMaximumScreenSpaceError_ !=
            options.maximumScreenSpaceError) {
        setMaximumScreenSpaceError(options.maximumScreenSpaceError);
        appliedOwnerMaximumScreenSpaceError_ =
            options.maximumScreenSpaceError;
    }
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerMaximumTextureSize_ != options.maximumTextureSize) {
        setMaximumTextureSize(options.maximumTextureSize);
        appliedOwnerMaximumTextureSize_ = options.maximumTextureSize;
    }
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerSubTileCacheBytes_ != options.subTileCacheBytes) {
        setSubTileCacheBytes(options.subTileCacheBytes);
        appliedOwnerSubTileCacheBytes_ = options.subTileCacheBytes;
    }
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerMinimumLevel_ != options.minimumZoom ||
        appliedOwnerMaximumLevel_ != options.maximumZoom) {
        setLevelRange(options.minimumZoom, options.maximumZoom);
        appliedOwnerMinimumLevel_ = options.minimumZoom;
        appliedOwnerMaximumLevel_ = options.maximumZoom;
    }
    hasAppliedOwnerOptions_ = true;
}

void RasterOverlayTileProvider::setCoverageRectangle(
    const Rectangle& coverageRectangle) {
    if (coverageRectangle_ == coverageRectangle) {
        return;
    }
    coverageRectangle_ = coverageRectangle;
    sourceCoverageRectangle_ =
        worldRectangleToRasterSource(coverageRectangle_, projection_);
    const Rectangle effectiveCoverage =
        effectiveCoverageRectangle(scheme_, sourceCoverageRectangle_);
    invalidateSourceAssetDepotCache();
    eraseCachedTilesMatching(
        [&](const std::string& cacheKey, const TilePtr& tile) {
            if (isDirectCompositeCacheKey(cacheKey)) {
                return false;
            }
            const Rectangle tileGeographicBounds =
                unprojectProviderToGeographic(tile->getRectangle(), projection_);
            const bool stillCovered =
                tileGeographicBounds.computeIntersection(effectiveCoverage)
                    .has_value();
            const bool loading =
                tile->getState() == RasterOverlayTile::LoadState::Loading;
            return !stillCovered && !loading;
        });
    discardPendingUploadsForMissingTiles();
    invalidateDirectCompositeTileCache();
}

void RasterOverlayTileProvider::setMaximumScreenSpaceError(
    double maximumScreenSpaceError) {
    const double nextMaximumScreenSpaceError =
        maximumScreenSpaceError > 0.0 ? maximumScreenSpaceError : 2.0;
    if (maximumScreenSpaceError_ == nextMaximumScreenSpaceError) {
        return;
    }
    maximumScreenSpaceError_ = nextMaximumScreenSpaceError;
    invalidateDirectCompositeTileCache();
}

void RasterOverlayTileProvider::setMaximumTextureSize(int maximumTextureSize) {
    const int nextMaximumTextureSize =
        maximumTextureSize > 0 ? maximumTextureSize : 2048;
    if (maximumTextureSize_ == nextMaximumTextureSize) {
        return;
    }
    maximumTextureSize_ = nextMaximumTextureSize;
    invalidateDirectCompositeTileCache();
    invalidateSourceAssetDepotCache();
}

void RasterOverlayTileProvider::setSubTileCacheBytes(int64_t subTileCacheBytes) {
    RetiredAsyncResources retired;
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    asyncState_->subTileCacheBytes = std::max<int64_t>(0, subTileCacheBytes);
    enforceSourceDepotBudgetLocked(*asyncState_, retired);
    if (asyncState_->subTileCacheBytes == 0 ||
        asyncState_->sourceTileDepotCacheBytes < 0) {
        if (asyncState_->subTileCacheBytes == 0) {
            clearSourceDepotCacheLocked(*asyncState_, retired);
        }
        asyncState_->sourceTileDepotCacheBytes = 0;
    }
    updatePendingUploadBackpressureLocked(*asyncState_);
}

void RasterOverlayTileProvider::setLevelRange(int minimumLevel,
                                              int maximumLevel) {
    const int nextMinimumLevel = minimumLevel > 0 ? minimumLevel : 0;
    const int nextMaximumLevel = maximumLevel > 0 ? maximumLevel : 0;
    if (minimumLevel_ == nextMinimumLevel &&
        maximumLevel_ == nextMaximumLevel) {
        return;
    }
    minimumLevel_ = nextMinimumLevel;
    maximumLevel_ = nextMaximumLevel;
    invalidateDirectCompositeTileCache();
    invalidateSourceAssetDepotCache();
}

int RasterOverlayTileProvider::getMinimumLevel() const {
    return std::max({scheme_.minZoom(), provider_.minZoom(), minimumLevel_});
}

void RasterOverlayTileProvider::refreshSourceAssetDepot() {
    sourceAssetDepot_ = std::make_shared<QuadtreeSourceAssetDepot>(
        provider_,
        scheme_,
        asyncState_,
        getMinimumLevel(),
        getMaximumLevel());
}

void RasterOverlayTileProvider::syncProviderContentRevision() {
    const uint64_t revision = provider_.contentRevision();
    if (revision == observedProviderContentRevision_) {
        return;
    }
    observedProviderContentRevision_ = revision;
    invalidateDirectCompositeTileCache();
    invalidateSourceAssetDepotCache();
}

RasterAssetKey RasterOverlayTileProvider::rasterAssetKey(
    const TileKey& sourceKey) {
    syncProviderContentRevision();
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return RasterAssetKey{
        provider_.instanceId(),
        observedProviderContentRevision_,
        asyncState_->sourceTileDepotEpoch,
        scheme_.id(),
        projection_,
        sourceKey};
}

std::shared_ptr<const DecodedImage>
RasterOverlayTileProvider::pinDecodedImage(
    const std::shared_ptr<const DecodedImage>& image) {
    return pinDecodedImage(asyncState_, image);
}

std::shared_ptr<const DecodedImage>
RasterOverlayTileProvider::pinDecodedImage(
    const std::shared_ptr<ProviderAsyncState>& state,
    const std::shared_ptr<const DecodedImage>& image) {
    if (!image) {
        return {};
    }

    // The aliasing shared_ptr keeps the original decoded image alive through
    // the pin object. The pin object owns the accounting transition, so the
    // source cache may evict its entry without making the physical bytes
    // disappear from diagnostics while PageStore still composes them.
    struct ExternalImagePin {
        std::shared_ptr<const DecodedImage> retainedImage;
        std::shared_ptr<ProviderAsyncState> state;
        const DecodedImage* imageKey = nullptr;

        ~ExternalImagePin() {
            if (!state || !imageKey) {
                return;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            auto it = state->sharedRasterImageRefs.find(imageKey);
            if (it == state->sharedRasterImageRefs.end()) {
                return;
            }
            auto& refs = it->second;
            if (refs.externalPinRefs > 0) {
                --refs.externalPinRefs;
            }
            if (refs.externalPinRefs == 0) {
                state->externalPinnedSourceBytes = std::max<int64_t>(
                    0,
                    state->externalPinnedSourceBytes - refs.sizeBytes);
            }
            if (refs.sourceCacheRefs == 0 && refs.pendingUploadRefs == 0 &&
                refs.externalPinRefs == 0) {
                state->sharedRasterImageRefs.erase(it);
            }
            int64_t externalOnlyBytes = 0;
            for (const auto& [_, tracked] :
                 state->sharedRasterImageRefs) {
                if (tracked.externalPinRefs > 0 &&
                    tracked.sourceCacheRefs == 0 &&
                    tracked.pendingUploadRefs == 0) {
                    externalOnlyBytes += tracked.sizeBytes;
                }
            }
            state->pendingUploadBackpressure.store(
                state->subTileCacheBytes > 0 &&
                    state->pendingUploadBytes +
                            state->pinnedSharedPendingUploadBytes +
                            externalOnlyBytes >=
                        state->subTileCacheBytes,
                std::memory_order_release);
        }
    };

    const DecodedImage* imageKey = image.get();
    const int64_t sizeBytes = decodedImageSizeBytes(*image);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto& refs = state->sharedRasterImageRefs[imageKey];
        if (refs.sizeBytes <= 0) {
            refs.sizeBytes = sizeBytes;
        }
        if (refs.externalPinRefs == 0) {
            state->externalPinnedSourceBytes += refs.sizeBytes;
        }
        ++refs.externalPinRefs;
    }

    auto pin = std::make_shared<ExternalImagePin>();
    pin->retainedImage = image;
    pin->state = state;
    pin->imageKey = imageKey;
    return std::shared_ptr<const DecodedImage>(std::move(pin), imageKey);
}

std::shared_ptr<const DecodedImage>
RasterOverlayTileProvider::retainExternalSourceImage(
    const std::shared_ptr<const DecodedImage>& image) {
    return pinDecodedImage(image);
}

RasterOverlayTileProvider::ExternalSourceImageRetainer
RasterOverlayTileProvider::externalSourceImageRetainer() const {
    std::shared_ptr<ProviderAsyncState> state = asyncState_;
    return [state = std::move(state)](
               const std::shared_ptr<const DecodedImage>& image) {
        return RasterOverlayTileProvider::pinDecodedImage(state, image);
    };
}

std::optional<RasterAssetSnapshot>
RasterOverlayTileProvider::tryGetCachedExactSource(
    const TileKey& sourceKey) {
    syncProviderContentRevision();
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    const uint64_t depotEpoch = asyncState_->sourceTileDepotEpoch;
    auto it = asyncState_->sourceTileDepotCache.find(
        sourceCacheKey(depotEpoch, sourceKey));
    if (it == asyncState_->sourceTileDepotCache.end() ||
        !it->second.image ||
        !isDecodedImageUploadable(*it->second.image) ||
        it->second.terminalFailure ||
        it->second.sourceSubset.has_value() ||
        it->second.key != sourceKey) {
        return std::nullopt;
    }

    SourceTileAsset& source = it->second;
    source.generation = ++asyncState_->sourceTileDepotGeneration;
    asyncState_->sourceTileDepotCacheLru.emplace_back(
        sourceCacheKey(depotEpoch, sourceKey), source.generation);
    compactSourceDepotCacheLruLocked(*asyncState_);

    RasterAssetSnapshot snapshot;
    snapshot.key = RasterAssetKey{
        provider_.instanceId(),
        observedProviderContentRevision_,
        depotEpoch,
        scheme_.id(),
        projection_,
        sourceKey};
    snapshot.resolvedKey = source.key;
    snapshot.bounds = source.bounds;
    snapshot.image = source.image;
    snapshot.credits = source.credits;
    snapshot.moreDetailAvailable = source.moreDetailAvailable;
    return snapshot;
}

RasterAssetAcquireResult RasterOverlayTileProvider::acquireExactSource(
    const TileKey& sourceKey,
    std::function<bool()> tryAdmitTransport,
    std::function<void(RasterAssetResponse)> onReady) {
    syncProviderContentRevision();
    if (!sourceAssetDepot_) {
        refreshSourceAssetDepot();
    }

    const RasterAssetKey assetKey = rasterAssetKey(sourceKey);
    const uint64_t ownerToken = nextSourceWaiterOwnerToken();
    auto handleState =
        std::make_shared<RasterAssetRequestHandle::State>();
    std::shared_ptr<ProviderAsyncState> state = asyncState_;
    std::shared_ptr<QuadtreeSourceAssetDepot> depot = sourceAssetDepot_;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->activeDirectCompositeSourceOwnerTokens.insert(ownerToken);
    }
    handleState->detach = [state, depot, sourceKey, ownerToken]() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->activeDirectCompositeSourceOwnerTokens.erase(ownerToken);
        }
        depot->detachInFlightWaiters({sourceKey}, ownerToken);
    };

    auto sourceIssued = [state]() {
        state->rasterSourceRequestsStarted.fetch_add(
            1, std::memory_order_relaxed);
        state->activeRasterSourceRequests.fetch_add(
            1, std::memory_order_relaxed);
        RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(
            state);
    };
    auto sourceFinished = [state]() {
        state->rasterSourceRequestsCompleted.fetch_add(
            1, std::memory_order_relaxed);
        uint32_t current = state->activeRasterSourceRequests.load(
            std::memory_order_relaxed);
        while (current > 0 &&
               !state->activeRasterSourceRequests.compare_exchange_weak(
                   current,
                   current - 1,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        state->resolveDestructionIfComplete();
        RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(
            state);
    };
    auto sourceFailed = [state]() {
        state->rasterSourceRequestsFailed.fetch_add(
            1, std::memory_order_relaxed);
    };

    const RasterAssetAcquireStatus status = depot->requestSource(
        sourceKey,
        sourceKey,
        false,
        true,
        ownerToken,
        sourceIssued,
        sourceFinished,
        sourceFailed,
        [state, handleState, ownerToken, assetKey, sourceKey,
         onReady = std::move(onReady)](RasterSourceResult&& source) mutable {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->activeDirectCompositeSourceOwnerTokens.erase(ownerToken);
            }
            if (!handleState->active.exchange(
                    false, std::memory_order_acq_rel)) {
                return;
            }

            RasterAssetResponse response;
            response.diagnostics = std::move(source.diagnostics);
            const bool exactImage =
                source.image &&
                isDecodedImageUploadable(*source.image) &&
                !source.terminalFailure &&
                !source.sourceSubset.has_value() &&
                source.key == sourceKey;
            if (exactImage) {
                RasterAssetSnapshot snapshot;
                snapshot.key = assetKey;
                snapshot.resolvedKey = source.key;
                snapshot.bounds = source.bounds;
                snapshot.image = std::move(source.image);
                snapshot.credits = std::move(source.credits);
                snapshot.moreDetailAvailable =
                    source.moreDetailAvailable;
                response.asset = std::move(snapshot);
            } else {
                response.terminalFailure = source.terminalFailure ||
                    (source.image != nullptr);
                if (source.image && !isDecodedImageUploadable(*source.image)) {
                    response.diagnostics.push_back(
                        "Raster source decoded an invalid image");
                }
            }
            if (onReady) {
                onReady(std::move(response));
            }
        },
        {},
        std::move(tryAdmitTransport),
        {},
        false);

    RasterAssetAcquireResult result;
    result.status = status;
    if (status == RasterAssetAcquireStatus::AdmissionDenied) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->activeDirectCompositeSourceOwnerTokens.erase(ownerToken);
        }
        handleState->active.store(false, std::memory_order_release);
        return result;
    }
    result.handle = RasterAssetRequestHandle(std::move(handleState));
    return result;
}

void RasterOverlayTileProvider::eraseCachedTilesMatching(
    const std::function<bool(const std::string&, const TilePtr&)>& predicate,
    const std::function<void(const TilePtr&)>& beforeErase) {
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (it->second && predicate(it->first, it->second)) {
            if (beforeErase) {
                beforeErase(it->second);
            }
            it = eraseCachedTile(it);
        } else {
            ++it;
        }
    }
}

void RasterOverlayTileProvider::erasePendingUploadsMatchingLocked(
    ProviderAsyncState& state,
    const std::function<bool(const PendingUpload&)>& predicate,
    RetiredAsyncResources& retired) {
    auto& pendingUploads = state.pendingUploads;
    for (auto it = pendingUploads.begin(); it != pendingUploads.end();) {
        if (!predicate(*it)) {
            ++it;
            continue;
        }
        releasePendingUploadImageBytesLocked(state, *it);
        retired.pendingUploads.push_back(std::move(*it));
        it = pendingUploads.erase(it);
    }
    enforceSourceDepotBudgetLocked(state, retired);
}

void RasterOverlayTileProvider::invalidateDirectCompositeTileCache() {
    ++directCompositeTileEpoch_;
    ++mappingRevision_;
    abandonActiveSourceSets(true);
    RetiredAsyncResources retired;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        erasePendingUploadsMatchingLocked(
            *asyncState_,
            [](const PendingUpload& upload) {
                return isDirectCompositeCacheKey(upload.cacheKey);
            },
            retired);
    }
    eraseCachedTilesMatching(
        [](const std::string& cacheKey, const TilePtr&) {
            return isDirectCompositeCacheKey(cacheKey);
        });
    asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
}

void RasterOverlayTileProvider::invalidateDirectExecutionState() {
    // A backend generation fence owns every Direct mapping request, including
    // aligned exact tiles. Detach their waiters before clearing tile/upload
    // state so a late transport may still populate the backend-neutral source
    // cache for PageStore, but cannot resurrect an old Direct PendingUpload.
    abandonActiveSourceSets(false);
    invalidateDirectCompositeTileCache();
    invalidateDirectRasterTileCache();
}

void RasterOverlayTileProvider::invalidateDirectRasterTileCache() {
    ++mappingRevision_;
    eraseCachedTilesMatching(
        [](const std::string& cacheKey, const TilePtr&) {
            return !isDirectCompositeCacheKey(cacheKey);
        },
        [](const TilePtr& tile) {
            tile->setMoreDetailAvailable(
                RasterOverlayTile::MoreDetailAvailable::No);
            tile->setState(RasterOverlayTile::LoadState::Failed);
        });
    discardPendingUploadsForMissingTiles();
    asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
}

void RasterOverlayTileProvider::invalidateSourceAssetDepotCache() {
    abandonActiveSourceSets(false);
    RetiredAsyncResources retired;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        ++asyncState_->sourceTileDepotEpoch;
        clearSourceDepotCacheLocked(*asyncState_, retired);
        clearSourceDepotInFlightLocked(*asyncState_, retired);
        asyncState_->sourceTileDepotFallbackKeysByOwner.clear();
        asyncState_->activeDirectCompositeSourceOwnerTokens.clear();
    }
    invalidateDirectRasterTileCache();
    refreshSourceAssetDepot();
}

void RasterOverlayTileProvider::abandonActiveSourceSets(
    bool directCompositeOnly) {
    std::vector<std::pair<std::string, std::shared_ptr<DirectCompositeSourceImageSet>>>
        activeSets;
    std::vector<TileKey> abandonedFallbackSources;
    std::vector<std::pair<std::vector<TileKey>, uint64_t>>
        detachedInFlightWaiters;
    std::unordered_set<uint64_t> abandonedOwnerTokens;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        activeSets.reserve(asyncState_->activeDirectCompositeSourceSets.size());
        detachedInFlightWaiters.reserve(
            asyncState_->activeDirectCompositeSourceSets.size());
        for (auto it = asyncState_->activeDirectCompositeSourceSets.begin();
             it != asyncState_->activeDirectCompositeSourceSets.end();) {
            if (directCompositeOnly &&
                !isDirectCompositeCacheKey(it->first)) {
                ++it;
                continue;
            }
            if (it->second) {
                const uint64_t ownerToken = it->second->getWaiterOwnerToken();
                abandonedOwnerTokens.insert(ownerToken);
                asyncState_->activeDirectCompositeSourceOwnerTokens.erase(ownerToken);
                detachedInFlightWaiters.emplace_back(
                    it->second->getSourceKeys(),
                    ownerToken);
                auto fallbackKeysIt =
                    asyncState_->sourceTileDepotFallbackKeysByOwner.find(
                        ownerToken);
                if (fallbackKeysIt !=
                    asyncState_->sourceTileDepotFallbackKeysByOwner.end()) {
                    detachedInFlightWaiters.emplace_back(
                        std::move(fallbackKeysIt->second),
                        ownerToken);
                    asyncState_->sourceTileDepotFallbackKeysByOwner.erase(
                        fallbackKeysIt);
                }
            }
            asyncState_->inFlightRequests.erase(it->first);
            activeSets.emplace_back(it->first, std::move(it->second));
            it = asyncState_->activeDirectCompositeSourceSets.erase(it);
        }
        if (directCompositeOnly) {
            std::deque<PendingSourceFallback> retainedFallbacks;
            for (auto& fallback : asyncState_->pendingSourceFallbacks) {
                if (abandonedOwnerTokens.count(fallback.ownerToken) == 0) {
                    retainedFallbacks.push_back(std::move(fallback));
                    continue;
                }
                abandonedFallbackSources.push_back(fallback.originalKey);
            }
            asyncState_->pendingSourceFallbacks.swap(retainedFallbacks);
            asyncState_->pendingSourceFallbackCount.store(
                static_cast<uint32_t>(
                    asyncState_->pendingSourceFallbacks.size()),
                std::memory_order_release);
        } else {
            abandonedFallbackSources.reserve(
                asyncState_->pendingSourceFallbacks.size());
            for (const PendingSourceFallback& fallback :
                 asyncState_->pendingSourceFallbacks) {
                abandonedFallbackSources.push_back(fallback.originalKey);
            }
            asyncState_->pendingSourceFallbacks.clear();
            asyncState_->pendingSourceFallbackCount.store(
                0,
                std::memory_order_release);
        }
        compactActiveDirectCompositeSourceSetOrderLocked(*asyncState_);
    }

    if (sourceAssetDepot_) {
        for (const auto& [sourceKeys, waiterOwnerToken] :
             detachedInFlightWaiters) {
            sourceAssetDepot_->detachInFlightWaiters(
                sourceKeys,
                waiterOwnerToken);
        }
        for (const TileKey& key : abandonedFallbackSources) {
            sourceAssetDepot_->abandonInFlightSource(key);
        }
    }

    for (const auto& [cacheKey, sourceSet] : activeSets) {
        if (sourceSet) {
            sourceSet->markAbandoned();
            sourceSet->releaseThrottleSlotOnce();
        }
        auto tileIt = tiles_.find(cacheKey);
        if (tileIt != tiles_.end() && tileIt->second) {
            tileIt->second->setMoreDetailAvailable(
                RasterOverlayTile::MoreDetailAvailable::No);
            tileIt->second->setState(RasterOverlayTile::LoadState::Failed);
        }
    }
    if (!activeSets.empty()) {
        asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
        asyncState_->resolveDestructionIfComplete();
    }
}

void RasterOverlayTileProvider::discardPendingUploadsForMissingTiles() {
    RetiredAsyncResources retired;
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    erasePendingUploadsMatchingLocked(
        *asyncState_,
        [this](const PendingUpload& upload) {
            return tiles_.find(upload.cacheKey) == tiles_.end();
        },
        retired);
}

int RasterOverlayTileProvider::getMaximumLevel() const {
    const int configuredMaximumLevel =
        maximumLevel_ > 0
            ? maximumLevel_
            : std::min(scheme_.maxZoom(), provider_.maxZoom());
    return std::min({scheme_.maxZoom(), provider_.maxZoom(),
                     configuredMaximumLevel});
}

std::string RasterOverlayTileProvider::tileCacheKey(const TileKey& key) const {
    return key.schemeId.str() + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

void RasterOverlayTileProvider::insertCachedTile(
    const std::string& cacheKey,
    TilePtr tile) {
    auto existing = tiles_.find(cacheKey);
    if (existing != tiles_.end()) {
        eraseCachedTile(existing);
    }
    auto [inserted, didInsert] =
        tiles_.emplace(cacheKey, std::move(tile));
    if (!didInsert || !inserted->second) {
        return;
    }
    RasterOverlayTile& cachedTile = *inserted->second;
    tileCacheLru_.push_back(&cachedTile);
    cachedTile.cacheLruLinked_ = true;
    cachedTile.cacheLruPosition_ = std::prev(tileCacheLru_.end());
}

void RasterOverlayTileProvider::touchCachedTile(
    const std::string& cacheKey) {
    auto tileIt = tiles_.find(cacheKey);
    if (tileIt == tiles_.end() || !tileIt->second) {
        return;
    }
    touchCachedTile(*tileIt->second);
}

void RasterOverlayTileProvider::touchCachedTile(
    RasterOverlayTile& tile) {
    if (tile.lastUsedFrame == frameNumber_ && tile.cacheLruLinked_) {
        return;
    }
    tile.lastUsedFrame = frameNumber_;
    if (!tile.cacheLruLinked_) {
        tileCacheLru_.push_back(&tile);
        tile.cacheLruLinked_ = true;
        tile.cacheLruPosition_ = std::prev(tileCacheLru_.end());
        return;
    }
    if (std::next(tile.cacheLruPosition_) == tileCacheLru_.end()) {
        return;
    }
    tileCacheLru_.splice(
        tileCacheLru_.end(),
        tileCacheLru_,
        tile.cacheLruPosition_);
    tile.cacheLruPosition_ = std::prev(tileCacheLru_.end());
}

RasterOverlayTileProvider::TileCache::iterator
RasterOverlayTileProvider::eraseCachedTile(TileCache::iterator it) {
    if (it == tiles_.end()) {
        return it;
    }
    RasterOverlayTile* tile = it->second.get();
    if (tile && tile->cacheLruLinked_) {
        tileCacheLru_.erase(tile->cacheLruPosition_);
        tile->cacheLruLinked_ = false;
    }
    return tiles_.erase(it);
}

void RasterOverlayTileProvider::clearCachedTiles() {
    for (auto& [_, tile] : tiles_) {
        if (tile) {
            tile->cacheLruLinked_ = false;
        }
    }
    tileCacheLru_.clear();
    tiles_.clear();
}

RasterOverlayTileProvider::TilePtr RasterOverlayTileProvider::getPlaceholderTile() {
    if (!placeholderTile_) {
        placeholderTile_ = std::make_shared<RasterOverlayTile>(*this);
    }
    return placeholderTile_;
}

RasterOverlayTileProvider::TilePtr RasterOverlayTileProvider::getTile(
    const TileKey& key) {
    syncProviderContentRevision();
    // cesium-native: return placeholder if provider is not yet ready
    if (!ready_) {
        return getPlaceholderTile();
    }

    if (key.z < getMinimumLevel() || key.z > getMaximumLevel()) return nullptr;
    if (!provider_.supportsTile(key)) return nullptr;
    Rectangle geographicBounds = scheme_.tileToRectangle(key);
    const Rectangle effectiveCoverage =
        effectiveCoverageRectangle(scheme_, sourceCoverageRectangle_);
    if (!geographicBounds.computeIntersection(effectiveCoverage)) return nullptr;
    Rectangle bounds =
        projectGeographicToProvider(geographicBounds, projection_);

    std::string ck = tileCacheKey(key);
    auto it = tiles_.find(ck);
    if (it != tiles_.end()) {
        // 已持有命中迭代器,直接调对象重载,省去 touchCachedTile(string)
        // 内部第二次 tiles_.find(ck)。留 null 守卫:it->second 会作返回值,
        // 可能为空,须与 string 重载的 !tileIt->second no-op 语义一致。
        if (it->second) touchCachedTile(*it->second);
        return it->second;
    }

    // Create new tile in Unloaded state
    auto tile = std::make_shared<RasterOverlayTile>(*this, key, bounds, ck);
    tile->setMaxZoom(getMaximumLevel());
    insertCachedTile(ck, tile);
    touchCachedTile(ck);
    return tile;
}

RasterOverlayTileProvider::RasterTileMapping
RasterOverlayTileProvider::mapRasterTilesToGeometryTile(
    const Rectangle& providerGeometryBounds,
    double targetScreenPixelsX,
    double targetScreenPixelsY) {
    syncProviderContentRevision();
    if (!ready_) {
        return {getPlaceholderTile(), false, {}};
    }

    const Rectangle geometryBounds =
        unprojectProviderToGeographic(providerGeometryBounds, projection_);
    const Rectangle effectiveCoverage =
        effectiveCoverageRectangle(scheme_, sourceCoverageRectangle_);
    const std::optional<Rectangle> sourceBounds =
        mapGeometryBoundsToImageryCoverage(
            geometryBounds,
            effectiveCoverage,
            shouldClampOutsideCoverage(owner_));
    if (!sourceBounds) {
        return {nullptr, false, {}};
    }

    RasterOverlaySourcePlan sourcePlan = buildRasterOverlaySourcePlan(
        scheme_,
        provider_,
        geometryBounds,
        *sourceBounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError_,
        effectiveMaximumTextureSize(),
        getMinimumLevel(),
        getMaximumLevel());

    if (sourcePlan.empty()) {
        return {getPlaceholderTile(), false, {}};
    }

    RasterOverlaySourcePlan sourceTiles = sourcePlan;

    if (sourcePlan.exactSingleSource) {
        const TileKey& sourceKey = sourcePlan.sourceKeys.front();
        return {getTile(sourceKey), true, std::move(sourceTiles)};
    }

    const std::string ck = directCompositeTileCacheKey(
        scheme_,
        providerGeometryBounds,
        *sourceBounds,
        sourceTiles,
        directCompositeTileEpoch_);
    auto existing = tiles_.find(ck);
    if (existing != tiles_.end()) {
        // 已持有命中迭代器,直接调对象重载省去内部第二次 find。免 null 守卫:
        // 紧接着的 setDirectCompositeSourceList 无条件解引用 existing->second,调用点
        // 本已假定非空,string 重载的 null no-op 分支在此不可达。
        touchCachedTile(*existing->second);
        existing->second->setDirectCompositeSourceList(
            sourcePlan.sourceZoom,
            *sourceBounds,
            sourcePlan.sourceKeys,
            sourcePlan.minX,
            sourcePlan.minY,
            sourcePlan.maxX,
            sourcePlan.maxY);
        existing->second->setTargetScreenPixels(
            targetScreenPixelsX,
            targetScreenPixelsY);
        return {existing->second, false, std::move(sourceTiles)};
    }

    const double centerLng =
        geometryBounds.west() + geometryBounds.width() * 0.5;
    const double centerLat =
        geometryBounds.south() + geometryBounds.height() * 0.5;
    TileKey representativeKey = scheme_.positionToTile(
        centerLng, centerLat, sourcePlan.sourceZoom);

    auto tile = std::make_shared<RasterOverlayTile>(
        *this, representativeKey, providerGeometryBounds, ck);
    tile->setMaxZoom(getMaximumLevel());
    tile->setDirectCompositeSourceList(
        sourcePlan.sourceZoom,
        *sourceBounds,
        sourcePlan.sourceKeys,
        sourcePlan.minX,
        sourcePlan.minY,
        sourcePlan.maxX,
        sourcePlan.maxY);
    tile->setTargetScreenPixels(targetScreenPixelsX, targetScreenPixelsY);
    insertCachedTile(ck, tile);
    touchCachedTile(ck);
    return {tile, false, std::move(sourceTiles)};
}

RasterOverlayTileProvider::TilePtr RasterOverlayTileProvider::resolveTile(
    const Rectangle& bounds,
    int desiredZoom) {
    // cesium-native: find the best tile covering the bounds at ≤ desiredZoom.
    // Tries desiredZoom first, then walks up the tree.
    const double centerLng = (bounds.west() + bounds.east()) * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;

    for (int z = std::min(desiredZoom, getMaximumLevel());
         z >= getMinimumLevel();
         --z) {
        TileKey key = scheme_.positionToTile(centerLng, centerLat, z);
        TilePtr tile = getTile(key);

        // Loaded-without-texture tiles are cover-ready, not drawable fallback
        // sources. Surface raster binding requires a real texture.
        if (tile &&
            tile->getState() >= RasterOverlayTile::LoadState::Loaded &&
            tile->getTexture()) {
            return tile;
        }
    }
    return nullptr;
}

ProviderRequestDiagnostics
RasterOverlayTileProvider::requestDiagnostics() const {
    ProviderRequestDiagnostics diag = provider_.requestDiagnostics();
    diag.externalResourceRequestsStarted +=
        asyncState_->rasterSourceRequestsStarted.load(
            std::memory_order_relaxed);
    diag.externalResourceRequestsCompleted +=
        asyncState_->rasterSourceRequestsCompleted.load(
            std::memory_order_relaxed);
    diag.externalResourceRequestsFailed +=
        asyncState_->rasterSourceRequestsFailed.load(
            std::memory_order_relaxed);
    diag.activeExternalResourceBlockingRequests +=
        static_cast<int>(
            asyncState_->activeRasterSourceRequests.load(
                std::memory_order_relaxed));
    diag.peakExternalResourceBlockingRequests =
        std::max(
            diag.peakExternalResourceBlockingRequests,
            static_cast<int>(
                asyncState_->peakRasterSourceRequests.load(
                    std::memory_order_relaxed)));
    diag.peakExternalResourceBlockingRequests =
        std::max(diag.peakExternalResourceBlockingRequests,
                 diag.activeExternalResourceBlockingRequests);
    return diag;
}

Texture* RasterOverlayTileProvider::getTexture(const TileKey& key) const {
    std::string ck = tileCacheKey(key);
    auto it = tiles_.find(ck);
    if (it != tiles_.end() && it->second->getTexture()) {
        return it->second->getTexture();
    }
    return nullptr;
}

int RasterOverlayTileProvider::getThrottledTilesCurrentlyLoading() const {
    return static_cast<int>(
        asyncState_->activeRasterTileLoads.load(std::memory_order_relaxed));
}

int RasterOverlayTileProvider::getPendingUploadCount() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return static_cast<int>(asyncState_->pendingUploads.size());
}

int64_t RasterOverlayTileProvider::getPendingUploadBytes() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return asyncState_->pendingUploadBytes;
}

int64_t RasterOverlayTileProvider::getPeakPendingUploadBytes() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return asyncState_->peakPendingUploadBytes;
}

int64_t RasterOverlayTileProvider::getPendingUploadBudgetBytes() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return asyncState_->pendingUploadBytes +
           asyncState_->pinnedSharedPendingUploadBytes +
           externalOnlyResidentBytesLocked(*asyncState_);
}

int64_t RasterOverlayTileProvider::getPeakPendingUploadBudgetBytes() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return asyncState_->peakPendingUploadBudgetBytes;
}

bool RasterOverlayTileProvider::pendingUploadBackpressureActive() const {
    return asyncState_->pendingUploadBackpressure.load(
        std::memory_order_acquire);
}

bool RasterOverlayTileProvider::loadTile(RasterOverlayTile& tile,
                                         FrameResourceBudget* budget) {
    syncProviderContentRevision();
    if (budget && budget->sceneArbiter() != nullptr) {
        asyncState_->sceneResourceManaged.store(
            true,
            std::memory_order_release);
    }
    if (tile.isDirectCompositeTile()) {
        return loadDirectCompositeTile(tile, budget);
    }

    // cesium-native ActivatedRasterOverlay::doLoad: only Unloaded tiles start
    // a load. Loading/Loaded/Done/Failed/Placeholder are terminal or already
    // in progress from this entry point.
    auto loadState = tile.getState();
    switch (loadState) {
        case RasterOverlayTile::LoadState::Unloaded:
            break;  // OK to load
        case RasterOverlayTile::LoadState::Loading:
        case RasterOverlayTile::LoadState::Loaded:
        case RasterOverlayTile::LoadState::Done:
        case RasterOverlayTile::LoadState::Failed:
        case RasterOverlayTile::LoadState::Placeholder:
            return true;  // Already in progress or complete
    }

    const TileKey& key = tile.getTileID();
    const Rectangle outputBounds = tile.getRectangle();
    const Rectangle targetBounds =
        unprojectProviderToGeographic(outputBounds, projection_);
    return loadSourceTileList(
        tile,
        RasterOverlaySourcePlan{
            key.z,
            targetBounds,
            {key},
            key.x,
            key.y,
            key.x,
            key.y},
        targetBounds,
        tileCacheKey(key),
        budget);
}

bool RasterOverlayTileProvider::loadTileThrottled(RasterOverlayTile& tile,
                                                  FrameResourceBudget* budget) {
    syncProviderContentRevision();
    if (budget && budget->sceneArbiter() != nullptr) {
        asyncState_->sceneResourceManaged.store(
            true,
            std::memory_order_release);
    }
    // cesium-native: loadTileThrottled only starts Unloaded tiles. Once a
    // A Direct composite tile may remain Loading with unissued source futures
    // waiting for raster request budget. Keep pumping those shared source
    // assets so large rectangle compositions converge over multiple frames.
    if (tile.isDirectCompositeTile() &&
        tile.getState() == RasterOverlayTile::LoadState::Loading) {
        pumpLoadingDirectCompositeTile(tile, budget);
        return true;
    }
    if (tile.getState() != RasterOverlayTile::LoadState::Unloaded) {
        return true;
    }
    if (pendingUploadBackpressureActive()) {
        return false;
    }

    const bool canJoinExistingDirectCompositeSources =
        tile.isDirectCompositeTile() &&
        !directCompositeTileWouldIssueNewSourceRequests(tile);
    if (!canJoinExistingDirectCompositeSources &&
        getThrottledTilesCurrentlyLoading() >= maximumSimultaneousTileLoads) {
        return false;  // Throttled
    }

    return loadTile(tile, budget);
}

void RasterOverlayTileProvider::syncWorkTickets() {
    // hasPendingWork 的 8 个子信号按 gating 语义拆两票。在锁下只读两个布尔,
    // 释放锁后再 reconcile(不在 provider 锁内嵌套账本锁)。
    // [2026-08-21] 槽已移入 ProviderAsyncState(线程安全),本函数渲染线程
    // 每帧调;worker 完成/派发路径用 syncRasterLandingTicketFromAnyThread。
    bool landing = false;
    bool pumped = false;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        pumped = !asyncState_->pendingUploads.empty();
        landing =
            !asyncState_->inFlightRequests.empty() ||
            !asyncState_->activeDirectCompositeSourceSets.empty() ||
            !asyncState_->pendingSourceFallbacks.empty() ||
            !asyncState_->sourceTileDepotInFlight.empty() ||
            asyncState_->activeRasterComposeTasks.load(
                std::memory_order_relaxed) > 0 ||
            asyncState_->activeDeferredUploadReleases.load(
                std::memory_order_relaxed) > 0 ||
            asyncState_->activeRasterSourceRequests.load(
                std::memory_order_relaxed) > 0;
    }
    asyncState_->loadSlot_.reconcile(
        WorkLedger::Kind::Landing, "rasterOverlayLoad", landing);
    asyncState_->uploadSlot_.reconcile(
        WorkLedger::Kind::Pumped, "rasterOverlayUpload", pumped);
}

/// [2026-08-21 冻屏根修] worker 侧 Landing 票同步:在锁下读 Landing 谓词,
/// 锁外 reconcile(槽线程安全)。完成路径在最后一件落地时释放 → 触发 Landing
/// 落地唤醒;派发路径确保持有(睡死期间 worker 起的新活不能无票)。
/// Locked 变体供已持 state->mutex 的调用方(避免自死锁)。
void RasterOverlayTileProvider::syncRasterLandingTicketLocked(
    const std::shared_ptr<ProviderAsyncState>& state) {
    const bool busy =
        !state->inFlightRequests.empty() ||
        !state->activeDirectCompositeSourceSets.empty() ||
        !state->pendingSourceFallbacks.empty() ||
        !state->sourceTileDepotInFlight.empty() ||
        state->activeRasterComposeTasks.load(
            std::memory_order_relaxed) > 0 ||
        state->activeDeferredUploadReleases.load(
            std::memory_order_relaxed) > 0 ||
        state->activeRasterSourceRequests.load(
            std::memory_order_relaxed) > 0;
    state->loadSlot_.reconcile(
        WorkLedger::Kind::Landing, "rasterOverlayLoad", busy);
}

void RasterOverlayTileProvider::syncRasterLandingTicketFromAnyThread(
    const std::shared_ptr<ProviderAsyncState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    syncRasterLandingTicketLocked(state);
}

void RasterOverlayTileProvider::markUsed(const std::string& cacheKey) {
    touchCachedTile(cacheKey);
}

void RasterOverlayTileProvider::markUsed(const TileKey& key) {
    markUsed(tileCacheKey(key));
}

void RasterOverlayTileProvider::markUsed(const RasterOverlayTile& tile) {
    if (&tile.getTileProvider() != this ||
        tile.getState() == RasterOverlayTile::LoadState::Placeholder) {
        return;
    }
    RasterOverlayTile& mutableTile =
        const_cast<RasterOverlayTile&>(tile);
    if (mutableTile.cacheLruLinked_) {
        touchCachedTile(mutableTile);
    }
}

bool RasterOverlayTileProvider::ownsCurrentTile(
    const RasterOverlayTile& tile) const {
    if (tile.getState() == RasterOverlayTile::LoadState::Placeholder) {
        return placeholderTile_ && placeholderTile_.get() == &tile;
    }

    const std::string& cacheKey = tile.getCacheKey();
    if (!cacheKey.empty()) {
        if (isEpochDirectCompositeCacheKey(cacheKey)) {
            const std::string currentEpochPrefix =
                "direct-composite/epoch/" +
                std::to_string(directCompositeTileEpoch_) + "/";
            if (cacheKey.rfind(currentEpochPrefix, 0) != 0) {
                return false;
            }
        }
        auto it = tiles_.find(cacheKey);
        return it != tiles_.end() && it->second.get() == &tile;
    }

    auto it = tiles_.find(tileCacheKey(tile.getTileID()));
    return it != tiles_.end() && it->second.get() == &tile;
}

void RasterOverlayTileProvider::trimUnusedTiles(bool cachePressure) {
    // Keep recently referenced tiles for a short window. cesium-native retains
    // raster tiles via intrusive references and a cache budget; this local
    // provider owns tiles directly, so immediate one-frame eviction would
    // churn active mapping handles and waste in-flight IO.
    // 一次加锁快照 in-flight/待上传 key 集合后无锁遍历。原实现每瓦片一次
    // mutex 往返 + O(待上传) 线性字符串比较（300 缓存 × 50 待上传 ≈ 1.5 万
    // 次比较/帧，P2-8）。
    std::unordered_set<std::string> busyKeys;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        busyKeys.reserve(asyncState_->inFlightRequests.size() +
                         asyncState_->pendingUploads.size());
        busyKeys.insert(asyncState_->inFlightRequests.begin(),
                        asyncState_->inFlightRequests.end());
        for (const PendingUpload& upload : asyncState_->pendingUploads) {
            busyKeys.insert(upload.cacheKey);
        }
    }
    auto lruIt = tileCacheLru_.begin();
    while (lruIt != tileCacheLru_.end()) {
        RasterOverlayTile* lruTile = *lruIt;
        auto nextLruIt = std::next(lruIt);
        if (!lruTile) {
            tileCacheLru_.erase(lruIt);
            lruIt = nextLruIt;
            continue;
        }
        const std::string& cacheKey = lruTile->getCacheKey();
        auto it = tiles_.find(cacheKey);
        if (it == tiles_.end() || !it->second ||
            it->second.get() != lruTile) {
            lruTile->cacheLruLinked_ = false;
            tileCacheLru_.erase(lruIt);
            lruIt = nextLruIt;
            continue;
        }
        RasterOverlayTile& tile = *it->second;
        const uint64_t age = frameNumber_ > tile.lastUsedFrame
            ? frameNumber_ - tile.lastUsedFrame
            : 0;
        if (!cachePressure && age <= kRetainedUnusedFrames) {
            break;
        }
        const bool inFlight = busyKeys.count(cacheKey) > 0;
        const bool retainedOutsideProvider = it->second.use_count() > 1;
        // 根层常驻(与几何侧同一条钉扎线,按 overlay 选择加入):兜底层影像
        // 一经加载不再被 trim 驱逐 —— 祖先 remap 兜底要求祖先影像纹理还在,
        // 几何钉住影像被换出的话,兜出来的是白板。见 TileBaseCoveragePin.h。
        const bool pinnedBaseCoverage =
            pinBaseCoverage && isBaseCoveragePinnedZoom(tile.getTileID().z);
        if (inFlight || retainedOutsideProvider || pinnedBaseCoverage) {
            lruIt = nextLruIt;
            continue;
        }
        eraseCachedTile(it);
        lruIt = nextLruIt;
    }
}

} // namespace earth_engine
