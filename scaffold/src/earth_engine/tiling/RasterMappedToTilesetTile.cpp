#include "RasterMappedToTilesetTile.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/IPrepareRendererResources.h"
#include "../tiling/TileSurface.h"
#include "TilesetTile.h"
#include "TileKey.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace earth_engine {
namespace {

int32_t addProjectionToList(std::vector<RasterOverlayProjection>& projections,
                            RasterOverlayProjection projection) {
    auto it = std::find(projections.begin(), projections.end(), projection);
    if (it == projections.end()) {
        projections.push_back(projection);
        return static_cast<int32_t>(projections.size()) - 1;
    }
    return static_cast<int32_t>(it - projections.begin());
}

RasterMappedToTilesetTile::MoreDetail toMappedMoreDetail(
    RasterOverlayTile::MoreDetailAvailable moreDetailAvailable) {
    switch (moreDetailAvailable) {
        case RasterOverlayTile::MoreDetailAvailable::No:
            return RasterMappedToTilesetTile::MoreDetail::No;
        case RasterOverlayTile::MoreDetailAvailable::Yes:
            return RasterMappedToTilesetTile::MoreDetail::Yes;
        case RasterOverlayTile::MoreDetailAvailable::Unknown:
            return RasterMappedToTilesetTile::MoreDetail::Unknown;
    }
    return RasterMappedToTilesetTile::MoreDetail::Unknown;
}

RasterMappedToTilesetTile::SourceTileList toMappedSourceTileList(
    RasterOverlayTileProvider::RasterSourceTileMapping&& sourceTiles) {
    return RasterMappedToTilesetTile::SourceTileList{
        sourceTiles.sourceZoom,
        sourceTiles.sourceBounds,
        std::move(sourceTiles.sourceKeys),
        sourceTiles.minX,
        sourceTiles.minY,
        sourceTiles.maxX,
        sourceTiles.maxY};
}

bool hasSameOverlayOwner(const RasterOverlayTile& candidate,
                         const RasterOverlayTileProvider& provider) {
    const auto* owner = provider.getOwner();
    if (owner != nullptr) {
        return candidate.getTileProvider().getOwner() == owner;
    }
    return &candidate.getTileProvider() == &provider;
}

std::shared_ptr<RasterOverlayTile> findLoadedTileOverlay(
    const TilesetTile& tile,
    const RasterOverlayTileProvider& provider) {
    std::shared_ptr<RasterOverlayTile> result;
    tile.rasterOverlayState.forEachMapping([&](const auto* mapped) {
        if (result) return;
        if (!mapped) return;

        std::shared_ptr<RasterOverlayTile> candidate =
            mapped->getLoadingTileHandle();
        if (!candidate || !hasSameOverlayOwner(*candidate, provider)) {
            candidate = mapped->getReadyTileHandle();
        }
        if (!candidate || !hasSameOverlayOwner(*candidate, provider)) return;

        const RasterOverlayTile::LoadState state = candidate->getState();
        if (state == RasterOverlayTile::LoadState::Loaded ||
            state == RasterOverlayTile::LoadState::Done) {
            result = candidate;
        }
    });
    return result;
}

std::shared_ptr<RasterOverlayTile> findParentTileOverlayPreferLoading(
    const TilesetTile& tile,
    const RasterOverlayTileProvider& provider) {
    std::shared_ptr<RasterOverlayTile> result;
    tile.rasterOverlayState.forEachMapping([&](const auto* mapped) {
        if (result) return;
        if (!mapped) return;

        std::shared_ptr<RasterOverlayTile> readyTile =
            mapped->getReadyTileHandle();
        if (!readyTile || !hasSameOverlayOwner(*readyTile, provider)) {
            return;
        }

        std::shared_ptr<RasterOverlayTile> loadingTile =
            mapped->getLoadingTileHandle();
        result = loadingTile ? loadingTile : readyTile;
    });
    return result;
}

const Rectangle* findRectangleForProjection(
    const RasterOverlayDetails& overlayDetails,
    RasterOverlayProjection projection,
    int32_t* textureCoordinateID = nullptr,
    bool* invertedV = nullptr) {
    for (size_t i = 0; i < overlayDetails.rasterOverlayProjections.size(); ++i) {
        if (overlayDetails.rasterOverlayProjections[i] == projection &&
            i < overlayDetails.rasterOverlayRectangles.size()) {
                if (textureCoordinateID) {
                    *textureCoordinateID = static_cast<int32_t>(i);
                }
                if (invertedV) {
                    *invertedV =
                        i < overlayDetails
                                .rasterOverlayInvertedVCoordinates.size() &&
                        overlayDetails.rasterOverlayInvertedVCoordinates[i];
                }
            return &overlayDetails.rasterOverlayRectangles[i];
        }
    }
    return nullptr;
}

bool isCurrentProviderTile(const std::shared_ptr<RasterOverlayTile>& tile,
                           const RasterOverlayTileProvider& provider) {
    return !tile || provider.ownsCurrentTile(*tile);
}

} // namespace

RasterMappedToTilesetTile::RasterMappedToTilesetTile() = default;
RasterMappedToTilesetTile::~RasterMappedToTilesetTile() = default;

RasterMappedToTilesetTile::MoreDetail RasterMappedToTilesetTile::update(
    const TileKey& geometryKey,
    const RasterOverlayDetails& overlayDetails,
    double targetScreenPixelsX,
    double targetScreenPixelsY,
    RasterOverlayTileProvider& tileProvider,
    IPrepareRendererResources* pPrepRenderer,
    std::vector<RasterOverlayProjection>& missingProjections,
    const TilesetTile* parentTile,
    size_t overlayIndex,
    bool hasRenderContentDetails,
    std::optional<Rectangle> boundingVolumeRectangle) {

    // cesium-native: store geometry key + overlay slot for attach/detach.
    geometryKey_ = geometryKey;
    overlaySlot_ = static_cast<int32_t>(overlayIndex);

    // Provider option/coverage/level changes invalidate mapped-raster cache
    // entries. Existing mappings may still retain shared handles, so drop
    // stale handles before the attached fast path can keep rendering an old
    // composition indefinitely.
    const bool readyTileCurrent =
        isCurrentProviderTile(_pReadyTile, tileProvider);
    const bool loadingTileCurrent =
        isCurrentProviderTile(_pLoadingTile, tileProvider);
    if (!readyTileCurrent) {
        releaseTileReferences(pPrepRenderer);
    } else if (!loadingTileCurrent) {
        _pLoadingTile = nullptr;
        loadingTileSource_ = ReadyTileSource::None;
        mappedSourceTiles_ = {};
        directRasterTile_ = false;
        originalFailed_ = false;
        if (_pReadyTile != nullptr) {
            state_ = readyTileSource_ == ReadyTileSource::Ancestor
                ? State::TemporarilyAttached
                : State::Attached;
        } else {
            state_ = State::Unattached;
        }
    }

    // ── Step 1: Already-Attached fast path ──
    // cesium-native: if getState() == Attached, report MoreDetailAvailable
    if (state_ == State::Attached) {
        if (_pReadyTile != nullptr &&
            _pReadyTile->getState() != RasterOverlayTile::LoadState::Failed &&
            _pReadyTile->getRendererResources() == nullptr) {
            state_ = State::Unattached;
            readyTexture_ = nullptr;
        } else {
            if (_pReadyTile != nullptr &&
                _pReadyTile->getState() !=
                    RasterOverlayTile::LoadState::Placeholder) {
                tileProvider.markUsed(*_pReadyTile);
            }
            if (!originalFailed_ && _pReadyTile != nullptr &&
                _pReadyTile->isMoreDetailAvailable() !=
                    RasterOverlayTile::MoreDetailAvailable::No) {
                return MoreDetail::Yes;
            }
            return MoreDetail::No;
        }
    }

    // ── Step 2: Failure fallback — walk parent geometry tile chain ──
    // cesium-native: if loading tile has Failed state, find a sibling mapped
    // raster on an ancestor geometry tile.
    if (_pLoadingTile != nullptr &&
        _pLoadingTile->getState() == RasterOverlayTile::LoadState::Failed) {
        originalFailed_ = true;
    }
    if (_pLoadingTile != nullptr &&
        _pLoadingTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
        tileProvider.markUsed(*_pLoadingTile);
    }
    if (_pReadyTile != nullptr &&
        _pReadyTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
        tileProvider.markUsed(*_pReadyTile);
    }

    const TilesetTile* pGeomTile = parentTile;
    while (_pLoadingTile != nullptr &&
           _pLoadingTile->getState() == RasterOverlayTile::LoadState::Failed &&
           pGeomTile != nullptr) {
        originalFailed_ = true;
        std::shared_ptr<RasterOverlayTile> overlayTile =
            findParentTileOverlayPreferLoading(
                *pGeomTile,
                _pLoadingTile->getTileProvider());
        if (overlayTile) {
            _pLoadingTile = overlayTile;
            loadingTileSource_ = ReadyTileSource::Ancestor;
            if (_pLoadingTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
                tileProvider.markUsed(*_pLoadingTile);
            }
        }
        pGeomTile = pGeomTile->parent;
    }

    const RasterOverlayProjection projection = tileProvider.getProjection();
    int32_t readyTextureCoordinateID = textureCoordinateID_;
    bool readyInvertedV = false;
    const Rectangle* geometryRectangle = hasRenderContentDetails
        ? findRectangleForProjection(
              overlayDetails,
              projection,
              &readyTextureCoordinateID,
              &readyInvertedV)
        : nullptr;
    const Rectangle* mappingRectangle = geometryRectangle;
    bool mappingRectangleInvertedV = readyInvertedV;
    if (!mappingRectangle && !hasRenderContentDetails &&
        boundingVolumeRectangle && !boundingVolumeRectangle->isEmpty()) {
        mappingRectangle = &*boundingVolumeRectangle;
        mappingRectangleInvertedV = false;
    }
    // 记下本次用的 mapping 矩形,供 promoteLoadedTileWithoutGeometryWork 在
    // 拿不到 update() 调用的瓦片上复算 UV(见该函数注释)。
    if (mappingRectangle) {
        lastMappingRectangle_ = *mappingRectangle;
        lastMappingRectangleInvertedV_ = mappingRectangleInvertedV;
    }

    // cesium-native RasterOverlayCollection::updateTileOverlays:
    // placeholder mappings are retried once the provider is ready.
    if (_pLoadingTile != nullptr &&
        _pLoadingTile->getState() == RasterOverlayTile::LoadState::Placeholder &&
        tileProvider.isReady()) {
        _pLoadingTile = nullptr;
        loadingTileSource_ = ReadyTileSource::None;
        if (_pReadyTile == nullptr) {
            state_ = State::Unattached;
        }
    }

    // If this mapping already owns a ready tile, keep that geometry-to-raster
    // identity stable. Rectangle provider calls create per-mapping tiles, so a
    // ready-but-unattached mapping must not ask the provider for a fresh tile
    // on the next update.
    if (_pLoadingTile == nullptr &&
        (_pReadyTile == nullptr ||
         readyTileSource_ == ReadyTileSource::Ancestor)) {
        if (!tileProvider.isReady()) {
            // cesium-native mapOverlayToTile: provider not created/ready yet,
            // so use the overlay placeholder and do not invent a projection.
            textureCoordinateID_ = -1;
            _pLoadingTile = tileProvider.getPlaceholderTile();
            mappedSourceTiles_ = {};
            directRasterTile_ = false;
            loadingTileSource_ = ReadyTileSource::None;
        } else if (hasRenderContentDetails && geometryRectangle) {
            // cesium-native mapOverlayToTile:
            // mapRasterTilesToGeometryTile receives the geometry rectangle
            // from TileRenderContent raster details.
            textureCoordinateID_ = readyTextureCoordinateID;
            RasterOverlayTileProvider::RasterTileMapping mapping =
                tileProvider.mapRasterTilesToGeometryTile(
                    *geometryRectangle,
                    targetScreenPixelsX,
                    targetScreenPixelsY);
            _pLoadingTile = mapping.tile;
            mappedSourceTiles_ =
                toMappedSourceTileList(std::move(mapping.sourceTiles));
            directRasterTile_ = mapping.directTile;
            loadingTileSource_ = ReadyTileSource::Real;
        } else if (hasRenderContentDetails) {
            // Render content is loaded, but it has no texture coordinates for
            // this overlay projection. Match cesium-native by recording the
            // projection at an index after the existing projection list, and
            // return a placeholder until the tile is reloaded/reprojected.
            const int32_t existingIndex =
                static_cast<int32_t>(
                    overlayDetails.rasterOverlayProjections.size());
            textureCoordinateID_ =
                existingIndex + addProjectionToList(
                    missingProjections,
                    projection);
            _pLoadingTile = tileProvider.getPlaceholderTile();
            mappedSourceTiles_ = {};
            directRasterTile_ = false;
            loadingTileSource_ = ReadyTileSource::None;
        } else {
            textureCoordinateID_ =
                addProjectionToList(missingProjections, projection);
            if (boundingVolumeRectangle &&
                !boundingVolumeRectangle->isEmpty()) {
                RasterOverlayTileProvider::RasterTileMapping mapping =
                    tileProvider.mapRasterTilesToGeometryTile(
                        *boundingVolumeRectangle,
                        targetScreenPixelsX,
                        targetScreenPixelsY);
                _pLoadingTile = mapping.tile;
                mappedSourceTiles_ =
                    toMappedSourceTileList(std::move(mapping.sourceTiles));
                directRasterTile_ = mapping.directTile;
                loadingTileSource_ = ReadyTileSource::Real;
            } else {
                // No precise rectangle yet, so return a placeholder for now.
                _pLoadingTile = tileProvider.getPlaceholderTile();
                mappedSourceTiles_ = {};
                directRasterTile_ = false;
                loadingTileSource_ = ReadyTileSource::None;
            }
        }
    }

    // cesium-native: placeholder tile — nothing to load, return early
    if (_pLoadingTile != nullptr &&
        _pLoadingTile->getState() == RasterOverlayTile::LoadState::Placeholder) {
        return MoreDetail::No;
    }

    // ── Step 3: Loading→Ready promotion ──
    // cesium-native: if loading tile state >= Loaded, promote to ready.
    if (_pLoadingTile != nullptr) {
        RasterOverlayTile::LoadState loadState = _pLoadingTile->getState();

        if (loadState == RasterOverlayTile::LoadState::Loaded ||
            loadState == RasterOverlayTile::LoadState::Done) {
            const bool canPromoteReadyRaster =
                pPrepRenderer != nullptr || state_ == State::Unattached ||
                _pReadyTile == nullptr;
            if (canPromoteReadyRaster) {
                // Promote loading -> ready. Without renderer resources, this
                // may make the raster cover-ready for selection, but it must
                // stay Unattached until buildTileDrawCommand can attach it.
                if (_pReadyTile != nullptr && state_ != State::Unattached) {
                    detachFromTile(pPrepRenderer);
                }
                _pReadyTile = _pLoadingTile;
                _pLoadingTile = nullptr;
                readyTileSource_ = loadingTileSource_;
                loadingTileSource_ = ReadyTileSource::None;
                readyTexture_ = _pReadyTile->getTexture();

                // Compute UV transform from the tile's bounds
                if (mappingRectangle) {
                    computeTranslationAndScale(
                        *mappingRectangle,
                        _pReadyTile->getRectangle(),
                        mappingRectangleInvertedV);
                }
            }
        } else if (loadState == RasterOverlayTile::LoadState::Failed) {
            originalFailed_ = true;
        }
    }

    // ── Step 4: Ancestor ready-tile substitution ──
    // cesium-native: while loading, find closest ancestor geometry tile
    // with a ready tile as temporary placeholder.
    if (_pLoadingTile != nullptr) {
        std::shared_ptr<RasterOverlayTile> candidate;
        pGeomTile = parentTile;

        while (pGeomTile) {
            candidate = findLoadedTileOverlay(
                *pGeomTile,
                _pLoadingTile->getTileProvider());
            if (candidate) {
                break;
            }
            pGeomTile = pGeomTile->parent;
        }

        if (candidate && _pReadyTile != candidate) {
            const bool canPromoteReadyRaster =
                pPrepRenderer != nullptr || state_ == State::Unattached ||
                _pReadyTile == nullptr;
            if (canPromoteReadyRaster) {
                if (_pReadyTile != nullptr && state_ != State::Unattached) {
                    detachFromTile(pPrepRenderer);
                }
                _pReadyTile = candidate;
                readyTileSource_ = ReadyTileSource::Ancestor;
                tileProvider.markUsed(*_pReadyTile);
                readyTexture_ = _pReadyTile->getTexture();
                // cesium-native: recompute UV for CURRENT child bounds.
                if (mappingRectangle) {
                    computeTranslationAndScale(
                        *mappingRectangle,
                        _pReadyTile->getRectangle(),
                        mappingRectangleInvertedV);
                }
            }
        }

        // ── Step 5: Failed-and-no-fallback safety valve ──
        else if (candidate == nullptr && _pReadyTile == nullptr &&
                 _pLoadingTile->getState() == RasterOverlayTile::LoadState::Failed) {
            // cesium-native: a failed overlay tile with no usable ancestor is
            // marked ready so raster failure does not block geometry
            // rendering. We intentionally do not substitute an unrelated
            // center-point imagery tile here.
            _pReadyTile = _pLoadingTile;
            _pLoadingTile = nullptr;
            readyTileSource_ = ReadyTileSource::None;
            loadingTileSource_ = ReadyTileSource::None;
            state_ = State::Attached;
            readyTexture_ = nullptr;
        }
    }

    // ── Step 6: Attach the ready tile ──
    // cesium-native: loadInMainThread → attachRasterInMainThread
    attachReadyTileInMainThread(pPrepRenderer);

    // ── Step 7: Return MoreDetailAvailable ──
    if (_pLoadingTile != nullptr) {
        if (_pLoadingTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
            tileProvider.markUsed(*_pLoadingTile);
        }
        return MoreDetail::Unknown;
    }
    if (_pReadyTile != nullptr &&
        _pReadyTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
        tileProvider.markUsed(*_pReadyTile);
    }
    if (!originalFailed_ && _pReadyTile != nullptr) {
        return toMappedMoreDetail(_pReadyTile->isMoreDetailAvailable());
    }
    return MoreDetail::No;
}

bool RasterMappedToTilesetTile::isMoreDetailAvailable() const {
    // cesium-native: !_pLoadingTile && !_originalFailed && _pReadyTile
    //   && _pReadyTile->isMoreDetailAvailable() == Yes
    if (_pLoadingTile != nullptr) return false;
    if (originalFailed_) return false;
    if (_pReadyTile == nullptr) return false;
    return _pReadyTile->isMoreDetailAvailable() ==
           RasterOverlayTile::MoreDetailAvailable::Yes;
}

bool RasterMappedToTilesetTile::hasPendingNonPlaceholderLoadingTile() const {
    return _pLoadingTile != nullptr &&
           _pLoadingTile->getState() !=
               RasterOverlayTile::LoadState::Placeholder;
}

bool RasterMappedToTilesetTile::hasStableUpdateState() const {
    return _pLoadingTile == nullptr &&
           _pReadyTile != nullptr &&
           state_ == State::Attached;
}

bool RasterMappedToTilesetTile::promoteLoadedTileWithoutGeometryWork(
    IPrepareRendererResources* pPrepRenderer) {
    if (_pLoadingTile == nullptr) {
        return false;
    }
    const RasterOverlayTile::LoadState loadState = _pLoadingTile->getState();
    if (loadState != RasterOverlayTile::LoadState::Loaded &&
        loadState != RasterOverlayTile::LoadState::Done) {
        return false;
    }
    const bool canPromoteReadyRaster =
        pPrepRenderer != nullptr || state_ == State::Unattached ||
        _pReadyTile == nullptr;
    if (!canPromoteReadyRaster) {
        return false;
    }
    if (_pReadyTile != nullptr && state_ != State::Unattached) {
        detachFromTile(pPrepRenderer);
    }
    _pReadyTile = _pLoadingTile;
    _pLoadingTile = nullptr;
    readyTileSource_ = loadingTileSource_;
    loadingTileSource_ = ReadyTileSource::None;
    readyTexture_ = _pReadyTile->getTexture();
    // UV 用上一次 update() 记下的 mapping 矩形重算 —— 这里刻意不碰 overlayDetails
    // / 包围体,因为本函数存在的前提就是"没人替这片瓦片跑 update()"。矩形只在
    // update() 里更新,内容重载会重新走 update() 并覆盖它,不会读到陈旧值。
    if (lastMappingRectangle_) {
        computeTranslationAndScale(
            *lastMappingRectangle_,
            _pReadyTile->getRectangle(),
            lastMappingRectangleInvertedV_);
    }
    return true;
}

void RasterMappedToTilesetTile::markStableReadyTileUsed() {
    if (_pReadyTile == nullptr ||
        _pReadyTile->getState() ==
            RasterOverlayTile::LoadState::Placeholder) {
        return;
    }
    _pReadyTile->getTileProvider().markUsed(*_pReadyTile);
}

uint64_t RasterMappedToTilesetTile::runtimeStateSignature() const {
    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t signature = kOffset;
    auto mix = [&](uint64_t value) {
        signature ^= value;
        signature *= kPrime;
    };
    auto mixTile = [&](const std::shared_ptr<RasterOverlayTile>& tile) {
        mix(reinterpret_cast<uintptr_t>(tile.get()));
        if (!tile) {
            return;
        }
        mix(static_cast<uint64_t>(tile->getState()));
        mix(static_cast<uint64_t>(tile->isMoreDetailAvailable()));
        mix(reinterpret_cast<uintptr_t>(tile->getRendererResources()));
    };

    mix(static_cast<uint64_t>(state_));
    mix(static_cast<uint64_t>(loadingTileSource_));
    mix(static_cast<uint64_t>(readyTileSource_));
    mix(static_cast<uint64_t>(originalFailed_ ? 1 : 0));
    mixTile(_pLoadingTile);
    mixTile(_pReadyTile);
    return signature;
}

Texture* RasterMappedToTilesetTile::texture() const {
    return _pReadyTile ? _pReadyTile->getTexture() : nullptr;
}

void RasterMappedToTilesetTile::detachFromTile(IPrepareRendererResources* pPrepRenderer) {
    if (state_ == State::Unattached) return;
    if (_pReadyTile == nullptr) return;

    // cesium-native: failed tiles are not attached with the renderer, so skip
    // the renderer callback but still reset the attachment state below.
    if (_pReadyTile->getState() != RasterOverlayTile::LoadState::Failed &&
        pPrepRenderer && geometryKey_.z >= 0) {
        pPrepRenderer->detachRasterInMainThread(
            geometryKey_, overlaySlot_);
    }

    state_ = State::Unattached;
}

void RasterMappedToTilesetTile::releaseTileReferences(
    IPrepareRendererResources* pPrepRenderer) {
    detachFromTile(pPrepRenderer);
    clearTileOwnershipState();
}

void RasterMappedToTilesetTile::attachReadyTileInMainThread(
    IPrepareRendererResources* pPrepRenderer) {
    if (!pPrepRenderer || _pReadyTile == nullptr ||
        state_ != State::Unattached) {
        return;
    }

    _pReadyTile->loadInMainThread();
    if (!_pReadyTile->getRendererResources()) {
        return;
    }

    pPrepRenderer->attachRasterInMainThread(
        geometryKey_,
        overlaySlot_,
        _pReadyTile,
        static_cast<Texture*>(_pReadyTile->getRendererResources()),
        offsetU_,
        offsetV_,
        scaleU_,
        scaleV_);
    state_ = _pLoadingTile != nullptr
        ? State::TemporarilyAttached
        : State::Attached;
}

void RasterMappedToTilesetTile::clearTileOwnershipState() {
    _pLoadingTile = nullptr;
    _pReadyTile = nullptr;
    readyTexture_ = nullptr;
    loadingTileSource_ = ReadyTileSource::None;
    readyTileSource_ = ReadyTileSource::None;
    mappedSourceTiles_ = {};
    directRasterTile_ = false;
    offsetU_ = 0.0f;
    offsetV_ = 0.0f;
    scaleU_ = 1.0f;
    scaleV_ = 1.0f;
    originalFailed_ = false;
    textureCoordinateID_ = -1;
    overlaySlot_ = 0;
    state_ = State::Unattached;
}

bool RasterMappedToTilesetTile::loadThrottled(
    RasterOverlayTileProvider& tileProvider,
    FrameResourceBudget* budget) {
    // cesium-native: if no loading tile, nothing to do
    if (_pLoadingTile == nullptr) return true;
    if (_pLoadingTile->getState() == RasterOverlayTile::LoadState::Placeholder) {
        return true;
    }
    tileProvider.markUsed(*_pLoadingTile);

    // cesium-native: delegate to Provider's throttled loading
    return tileProvider.loadTileThrottled(*_pLoadingTile, budget);
}

void RasterMappedToTilesetTile::computeTranslationAndScale(
    const Rectangle& geometryBounds,
    const Rectangle& imageryBounds,
    bool invertedVCoordinate) {
    if (imageryBounds.isEmpty()) {
        offsetU_ = 0.0f;
        offsetV_ = 0.0f;
        scaleU_ = 1.0f;
        scaleV_ = 1.0f;
        return;
    }

    // cesium-native: pure rectangle-ratio UV transform, no tile-size assumption.
    // Edge bleed is handled at the GL level (CLAMP_TO_EDGE + optional padding
    // in RenderDeviceRasterTextureUploader) rather than via UV shrinkage.
    const TileTextureWindow nativeUv = TileSurface::computeTranslationAndScale(
        geometryBounds, imageryBounds);
    TileTextureWindow uv = invertedVCoordinate
        ? nativeUv
        : TileSurface::textureWindowForNorthWestUv(nativeUv);
    offsetU_ = uv.offsetU;
    offsetV_ = uv.offsetV;
    scaleU_ = uv.scaleU;
    scaleV_ = uv.scaleV;
}

} // namespace earth_engine
