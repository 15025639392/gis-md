#include "TileRasterOverlayPrefetcher.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TileRasterOverlayMappingPolicy.h"
#include "TileRasterOverlaySignature.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {
namespace {

bool canReuseStableUpdate(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    if (tile.rasterOverlayState.hasMissingProjections() ||
        tile.rasterOverlayState.mappingCount() != rasterOverlays.size()) {
        return false;
    }

    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        if (!activeOverlay || !activeOverlay->visible()) {
            if (mapped) {
                return false;
            }
            continue;
        }
        if (!mapped || !mapped->hasStableUpdateState()) {
            return false;
        }
    }
    return true;
}

void markStableMappingsUsed(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    for (size_t i = 0;
         i < rasterOverlays.size() &&
         i < tile.rasterOverlayState.mappingCount();
         ++i) {
        if (!rasterOverlays[i] || !rasterOverlays[i]->visible()) {
            continue;
        }
        RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        if (mapped) {
            mapped->markStableReadyTileUsed();
        }
    }
}

} // namespace

TileRasterOverlayPrefetchAction TileRasterOverlayPrefetcher::prefetch(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    double maximumScreenSpaceError,
    FrameResourceBudget& frameResourceBudget,
    IPrepareRendererResources* pPrepRenderer,
    uint64_t frameNumber) {
    const uint64_t mappingIdentity =
        TileRasterOverlaySignature::mappingIdentity(rasterOverlays);
    const uint64_t configuration =
        TileRasterOverlaySignature::configuration(rasterOverlays);
    const uint64_t providerMappingRevision =
        TileRasterOverlaySignature::mappingRevision(rasterOverlays);
    const uint64_t contentRevision =
        tile.content.renderContent.retainedResourcesRevision();
    const uint64_t runtimeStateSignature =
        tile.rasterOverlayState.runtimeStateSignature();
    const bool stableAcrossFrames =
        canReuseStableUpdate(tile, rasterOverlays);

    TileRasterOverlayPrefetchAction action;
    bool rendererMaterialized = false;
    if (tile.rasterOverlayState.tryReuseFrameUpdate(
            frameNumber,
            mappingIdentity,
            configuration,
            providerMappingRevision,
            contentRevision,
            runtimeStateSignature,
            stableAcrossFrames,
            action,
            rendererMaterialized)) {
        if (pPrepRenderer && !rendererMaterialized &&
            !action.unloadTileContent) {
            tile.rasterOverlayState.attachReadyMappingsInMainThread(
                pPrepRenderer);
            tile.rasterOverlayState.recordFrameUpdate(
                frameNumber,
                mappingIdentity,
                TileRasterOverlaySignature::configuration(rasterOverlays),
                TileRasterOverlaySignature::mappingRevision(rasterOverlays),
                tile.content.renderContent.retainedResourcesRevision(),
                tile.rasterOverlayState.runtimeStateSignature(),
                canReuseStableUpdate(tile, rasterOverlays),
                action,
                true);
        }
        if (stableAcrossFrames) {
            markStableMappingsUsed(tile, rasterOverlays);
        }
        return action;
    }

    tile.rasterOverlayState.countAuthoritativeUpdate();
    auto finish = [&]() {
        tile.rasterOverlayState.recordFrameUpdate(
            frameNumber,
            mappingIdentity,
            TileRasterOverlaySignature::configuration(rasterOverlays),
            TileRasterOverlaySignature::mappingRevision(rasterOverlays),
            tile.content.renderContent.retainedResourcesRevision(),
            tile.rasterOverlayState.runtimeStateSignature(),
            canReuseStableUpdate(tile, rasterOverlays),
            action,
            pPrepRenderer != nullptr);
        return action;
    };

    if (TileRasterOverlayReadinessPolicy::doneTileCannotHoldRasterOverlays(
            tile)) {
        tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
        return finish();
    }
    tile.rasterOverlayState.synchronizeMappingIdentity(
        mappingIdentity,
        pPrepRenderer);
    tile.rasterOverlayState.resizeMappingSlots(
        rasterOverlays.size(),
        pPrepRenderer);
    tile.rasterOverlayState.clearMissingProjections();

    if (rasterOverlays.empty()) {
        return finish();
    }

    const TileRasterOverlayMappingContext mappingContext =
        TileRasterOverlayMappingPolicy::contextFor(tile);
    const RasterOverlayDetails& overlayDetails = mappingContext.details();

    std::optional<size_t> firstMoreDetailAvailable;
    std::optional<size_t> firstUnknownAvailability;
    for (size_t i : overlayProcessingOrder) {
        if (i >= tile.rasterOverlayState.mappingCount()) {
            continue;
        }

        ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            tile.rasterOverlayState.releaseMapping(i, pPrepRenderer);
            continue;
        }

        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device);
        if (!activeProvider) {
            continue;
        }

        const RasterOverlayProjection projection =
            activeProvider->getProjection();
        RasterMappedToTilesetTile& mapped =
            tile.rasterOverlayState.ensureMapping(i);
        // 闸3:目标几何缓存(computeDesiredScreenPixels 等三角开销加载时一次)。
        // cesium-native: pass the TILESET maximumScreenSpaceError (16.0) to
        // computeDesiredScreenPixels, NOT the overlay's MSE (2.0).
        // The overlay's MSE is divided again inside
        // computeLevelFromTargetScreenPixels, creating intentional headroom
        // for higher zoom overlays.  Using overlay MSE in both places
        // underestimates the desired zoom by ~3 levels (factor 16/2 = 8).
        const TileRasterOverlayMappingPolicy::ResolvedTargetGeometry
            resolvedGeometry =
                TileRasterOverlayMappingPolicy::resolveTargetGeometry(
                    tile,
                    mappingContext,
                    projection,
                    mapped,
                    maximumScreenSpaceError);
        const std::optional<Rectangle>& boundingVolumeRectangle =
            resolvedGeometry.boundingVolumeRectangle;
        std::vector<RasterOverlayProjection> localMissingProjections;
        const bool mapAsRenderContent =
            mappingContext.mapsLoadedRenderContent;
        std::vector<RasterOverlayProjection>& missingProjections =
            mapAsRenderContent
                ? tile.rasterOverlayState.missingProjections()
                : localMissingProjections;

        // cesium-native updates RasterMappedTo3DTile before giving any
        // throttled raster request another chance to run. Keep that order so
        // loaded/failed/stale tiles are consumed before request fanout.
        const RasterMappedToTilesetTile::MoreDetail moreDetail =
            mapped.update(
                tile.key,
                overlayDetails,
                resolvedGeometry.screenPixelsX,
                resolvedGeometry.screenPixelsY,
                *activeProvider,
                pPrepRenderer,
                missingProjections,
                tile.parent,
                i,
                mapAsRenderContent,
                boundingVolumeRectangle);
        if (tile.rasterOverlayState.hasMissingProjections()) {
            if (tile.content.loadState == TileLoadState::Done) {
                action.unloadTileContent = true;
            } else {
                tile.rasterOverlayState.releaseAndClearReferences(
                    pPrepRenderer);
            }
            return finish();
        }

        if (moreDetail == RasterMappedToTilesetTile::MoreDetail::Yes &&
            tile.rasterOverlayState.hasReadyMapping(i)) {
            if (!firstMoreDetailAvailable || i < *firstMoreDetailAvailable) {
                firstMoreDetailAvailable = i;
            }
        } else if (
            moreDetail == RasterMappedToTilesetTile::MoreDetail::Unknown) {
            if (!firstUnknownAvailability || i < *firstUnknownAvailability) {
                firstUnknownAvailability = i;
            }
        }

        RasterOverlayTile* loadingTile = mapped.getLoadingTile();
        if (loadingTile &&
            loadingTile->getState() !=
                RasterOverlayTile::LoadState::Placeholder) {
            mapped.loadThrottled(*activeProvider, &frameResourceBudget);
        }
    }
    action.createRasterOverlayUpsampledChildren =
        tile.content.loadState == TileLoadState::Done &&
        firstMoreDetailAvailable &&
        (!firstUnknownAvailability ||
         *firstUnknownAvailability > *firstMoreDetailAvailable);
    return finish();
}

void TileRasterOverlayPrefetcher::advanceThrottledLoads(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    FrameResourceBudget& frameResourceBudget,
    IPrepareRendererResources* pPrepRenderer) {
    for (size_t i : overlayProcessingOrder) {
        if (i >= tile.rasterOverlayState.mappingCount()) {
            continue;
        }
        ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        if (!mapped) {
            continue;
        }
        // 这条泵路服务的正是「有 mapping、却到不了 Done」的瓦片,而它们拿不到
        // mapped->update()。所以泵必须既发也收:先消费已经加载完的影像(不做
        // 几何解算),否则影像下载完也永远上不了屏(破洞真因,见
        // promoteLoadedTileWithoutGeometryWork)。
        if (mapped->promoteLoadedTileWithoutGeometryWork(pPrepRenderer)) {
            tile.rasterOverlayState.invalidateFrameUpdateCache();
        }
        // 提升出来的 ready 瓦片没有任何 update 路径替它 markUsed,会被 provider
        // 当成无人引用淘汰掉。这片瓦片本帧可见,保活。
        mapped->markStableReadyTileUsed();

        RasterOverlayTile* loadingTile = mapped->getLoadingTile();
        if (!loadingTile ||
            loadingTile->getState() ==
                RasterOverlayTile::LoadState::Placeholder) {
            continue;
        }
        // ensureTileProvider is cached after first creation; no geometry work.
        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device);
        if (!activeProvider) {
            continue;
        }
        mapped->loadThrottled(*activeProvider, &frameResourceBudget);
    }
}

} // namespace earth_engine
