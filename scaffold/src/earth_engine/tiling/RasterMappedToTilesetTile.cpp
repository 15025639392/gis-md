#include "RasterMappedToTilesetTile.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/IPrepareRendererResources.h"
#include "../tiling/TileSurface.h"
#include "TilesetTile.h"
#include "TileKey.h"

#include <algorithm>
#include <cmath>

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

bool hasSameOverlayOwner(const RasterOverlayTile& candidate,
                         const RasterOverlayTileProvider& provider) {
    const auto* owner = provider.getOwner();
    return owner != nullptr && candidate.getTileProvider().getOwner() == owner;
}

RasterOverlayTile* findTileOverlay(
    const TilesetTile& tile,
    const RasterOverlayTileProvider& provider) {
    for (const auto& mapped : tile.rasterOverlays) {
        if (!mapped) continue;

        RasterOverlayTile* readyTile = mapped->getReadyTile();
        if (!readyTile || !hasSameOverlayOwner(*readyTile, provider)) {
            continue;
        }

        if (mapped->getLoadingTile()) {
            return mapped->getLoadingTile();
        }
        return readyTile;
    }
    return nullptr;
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
    size_t overlayIndex) {

    // cesium-native: store geometry key + overlay index for attach/detach.
    geometryKey_ = geometryKey;
    overlayIndex_ = static_cast<int32_t>(overlayIndex);

    // ── Step 1: Already-Attached fast path ──
    // cesium-native: if getState() == Attached, report MoreDetailAvailable
    if (state_ == State::Attached) {
        if (_pReadyTile != nullptr &&
            _pReadyTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
            tileProvider.markUsed(*_pReadyTile);
        }
        if (!originalFailed_ && _pReadyTile != nullptr &&
            _pReadyTile->isMoreDetailAvailable() !=
                RasterOverlayTile::MoreDetailAvailable::No) {
            return MoreDetail::Yes;
        }
        return MoreDetail::No;
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
        RasterOverlayTile* overlayTile =
            findTileOverlay(*pGeomTile, _pLoadingTile->getTileProvider());
        if (overlayTile) {
            _pLoadingTile = overlayTile;
            if (_pLoadingTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
                tileProvider.markUsed(*_pLoadingTile);
            }
        }
        pGeomTile = pGeomTile->parent;
    }

    const RasterOverlayProjection projection = tileProvider.getProjection();
    const Rectangle* geometryRectangle =
        overlayDetails.findRectangleForOverlayProjection(projection);

    // If no loading tile yet, create one via the Provider
    if (_pLoadingTile == nullptr) {
        if (geometryRectangle) {
            // cesium-native mapOverlayToTile:
            // ActivatedRasterOverlay::getTile(rectangle, screenPixels) receives
            // the geometry rectangle from TileRenderContent raster details.
            _pLoadingTile = tileProvider.getTile(
                *geometryRectangle,
                targetScreenPixelsX,
                targetScreenPixelsY);
        } else {
            addProjectionToList(missingProjections, projection);
            _pLoadingTile = tileProvider.getPlaceholderTile();
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
            // Promote loading → ready
            if (_pReadyTile != nullptr && state_ != State::Unattached) {
                detachFromTile(pPrepRenderer);
            }
            _pReadyTile = _pLoadingTile;
            _pLoadingTile = nullptr;
            readyTexture_ = _pReadyTile->getTexture();

            // Compute UV transform from the tile's bounds
            if (_pReadyTile->getTexture() && geometryRectangle) {
                computeTranslationAndScale(
                    *geometryRectangle, _pReadyTile->getRectangle());
            }
            // state_ set in Step 6
        } else if (loadState == RasterOverlayTile::LoadState::Failed) {
            originalFailed_ = true;
        }
    }

    // ── Step 4: Ancestor ready-tile substitution ──
    // cesium-native: while loading, find closest ancestor geometry tile
    // with a ready tile as temporary placeholder.
    if (_pLoadingTile != nullptr) {
        RasterOverlayTile* candidate = nullptr;
        pGeomTile = parentTile;

        while (pGeomTile) {
            candidate = findTileOverlay(*pGeomTile, _pLoadingTile->getTileProvider());
            if (candidate &&
                candidate->getState() >= RasterOverlayTile::LoadState::Loaded) {
                break;
            }
            pGeomTile = pGeomTile->parent;
        }

        if (candidate &&
            candidate->getState() >= RasterOverlayTile::LoadState::Loaded &&
            _pReadyTile != candidate) {
            if (_pReadyTile != nullptr && state_ != State::Unattached) {
                detachFromTile(pPrepRenderer);
            }
            _pReadyTile = candidate;
            tileProvider.markUsed(*_pReadyTile);
            readyTexture_ = _pReadyTile->getTexture();
            // cesium-native: recompute UV for CURRENT child bounds.
            if (geometryRectangle) {
                computeTranslationAndScale(
                    *geometryRectangle,
                    _pReadyTile->getRectangle());
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
            state_ = State::Attached;
            readyTexture_ = nullptr;
        }
    }

    // ── Step 6: Attach the ready tile ──
    // cesium-native: loadInMainThread → attachRasterInMainThread
    if (_pReadyTile != nullptr && state_ == State::Unattached) {
        // cesium-native: create GPU resources before attach
        _pReadyTile->loadInMainThread();

        if (pPrepRenderer && _pReadyTile->getRendererResources()) {
            pPrepRenderer->attachRasterInMainThread(
                geometryKey, static_cast<int32_t>(overlayIndex),
                *_pReadyTile,
                static_cast<Texture*>(_pReadyTile->getRendererResources()),
                offsetU_, offsetV_, scaleU_, scaleV_);
        }
        state_ = (_pLoadingTile != nullptr) ? State::TemporarilyAttached
                                             : State::Attached;
    }

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
    if (originalFailed_) return false;
    if (_pReadyTile == nullptr) return false;
    return _pReadyTile->isMoreDetailAvailable() ==
           RasterOverlayTile::MoreDetailAvailable::Yes;
}

void RasterMappedToTilesetTile::detachFromTile(IPrepareRendererResources* pPrepRenderer) {
    if (state_ == State::Unattached) return;
    if (_pReadyTile == nullptr) return;

    // cesium-native: failed tiles are not attached with the renderer, so skip
    // the renderer callback but still reset the attachment state below.
    if (_pReadyTile->getState() != RasterOverlayTile::LoadState::Failed &&
        pPrepRenderer && geometryKey_.z >= 0) {
        pPrepRenderer->detachRasterInMainThread(
            geometryKey_, getTextureCoordinateID());
    }

    state_ = State::Unattached;
}

void RasterMappedToTilesetTile::releaseTileReferences(
    IPrepareRendererResources* pPrepRenderer) {
    detachFromTile(pPrepRenderer);
    _pLoadingTile = nullptr;
    _pReadyTile = nullptr;
    readyTexture_ = nullptr;
    offsetU_ = 0.0f;
    offsetV_ = 0.0f;
    scaleU_ = 1.0f;
    scaleV_ = 1.0f;
    originalFailed_ = false;
}

bool RasterMappedToTilesetTile::loadThrottled(RasterOverlayTileProvider& tileProvider) {
    // cesium-native: if no loading tile, nothing to do
    if (_pLoadingTile == nullptr) return true;
    if (_pLoadingTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
        tileProvider.markUsed(*_pLoadingTile);
    }

    // cesium-native: delegate to Provider's throttled loading
    return tileProvider.loadTileThrottled(*_pLoadingTile);
}

void RasterMappedToTilesetTile::computeTranslationAndScale(
    const Rectangle& geometryBounds,
    const Rectangle& imageryBounds) {
    const TileTextureWindow nativeUv = TileSurface::computeTranslationAndScale(
        geometryBounds, imageryBounds);
    TileTextureWindow uv =
        TileSurface::textureWindowForNorthWestUv(nativeUv);
    offsetU_ = uv.offsetU;
    offsetV_ = uv.offsetV;
    scaleU_ = uv.scaleU;
    scaleV_ = uv.scaleV;
}

} // namespace earth_engine
