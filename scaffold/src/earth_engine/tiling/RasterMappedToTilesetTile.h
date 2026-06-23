#pragma once

#include "TileKey.h"
#include "SurfaceTile.h"
#include "../core/math/Rectangle.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {

class RasterOverlayTile;
class RasterOverlayTileProvider;
class FrameResourceBudget;
class IPrepareRendererResources;
class Texture;
struct TileBoundingVolume;
struct TilesetTile;

/// cesium-native RasterMappedTo3DTile equivalent.
/// Links a geometry tile to an imagery tile with state management.
///
/// Holds RasterOverlayTile shared handles for the desired loading tile and the
/// currently renderable ready tile. Raw pointers returned by accessors are
/// non-owning views; the mapping keeps tile/texture lifetime stable through
/// the shared handles.
///
/// State machine:
///   Unattached → (loading starts) → TemporarilyAttached (parent fallback)
///   → (loading completes) → Attached (own texture ready)
///
/// Key invariants:
///   - _pLoadingTile : the tile we WANT (may be Loading/Failed) — may be nullptr
///   - _pReadyTile   : the tile we RENDER (always Loaded/Done) — may be nullptr
///   - When _pLoadingTile == nullptr && _pReadyTile != nullptr, state is Attached
///   - When both non-null, state is TemporarilyAttached
///   - When both nullptr, state is Unattached
///   - _originalFailed is set once on original tile failure, never cleared
class RasterMappedToTilesetTile {
public:
    /// Aligned with cesium-native AttachmentState values
    enum class State {
        Unattached = 0,
        TemporarilyAttached = 1,
        Attached = 2
    };

    /// Aligned with cesium-native RasterOverlayTile::MoreDetailAvailable
    enum class MoreDetail {
        No = 0,
        Yes = 1,
        Unknown = 2
    };

    enum class ReadyTileSource {
        None = 0,
        Real = 1,
        Ancestor = 2
    };

    struct SourceTileList {
        int sourceZoom = 0;
        Rectangle sourceBounds = Rectangle::MAXIMUM;
        std::vector<TileKey> sourceKeys;
        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;

        bool empty() const { return sourceKeys.empty(); }
    };

    RasterMappedToTilesetTile();
    ~RasterMappedToTilesetTile();

    RasterMappedToTilesetTile(const RasterMappedToTilesetTile&) = delete;
    RasterMappedToTilesetTile& operator=(const RasterMappedToTilesetTile&) = delete;

    /// cesium-native RasterMappedTo3DTile::update — 7-step flow.
    /// @param geometryKey   The geometry tile's quadtree key.
    /// @param overlayDetails The render content's raster overlay details.
    /// @param tileProvider   The imagery tile provider (for tile creation/lookup).
    /// @param pPrepRenderer Resource lifecycle notification sink.
    /// @param parentTile     Parent geometry tile (for step 2 failure fallback).
    /// @param overlayIndex   Our index in parent's rasterOverlays vector.
    /// @return MoreDetailAvailable status.
    MoreDetail update(const TileKey& geometryKey,
                      const RasterOverlayDetails& overlayDetails,
                      double targetScreenPixelsX,
                      double targetScreenPixelsY,
                      RasterOverlayTileProvider& tileProvider,
                      IPrepareRendererResources* pPrepRenderer,
                      std::vector<RasterOverlayProjection>& missingProjections,
                      const TilesetTile* parentTile = nullptr,
                      size_t overlayIndex = 0,
                      bool hasRenderContentDetails = true,
                      std::optional<Rectangle> boundingVolumeRectangle =
                          std::nullopt);

    /// cesium-native: check if a higher-resolution tile could be loaded.
    bool isMoreDetailAvailable() const;

    /// cesium-native: detach this raster from the geometry tile.
    void detachFromTile(IPrepareRendererResources* pPrepRenderer);

    /// Drop provider tile handles after the geometry tile stops rendering.
    /// This lets the provider evict unreferenced raster tiles safely.
    void releaseTileReferences(IPrepareRendererResources* pPrepRenderer);

    /// cesium-native: throttled load via the Provider.
    bool loadThrottled(RasterOverlayTileProvider& tileProvider,
                       FrameResourceBudget* budget = nullptr);

    /// Compute UV translation/scale from geometry↔imagery rectangles.
    void computeTranslationAndScale(const Rectangle& geometryBounds,
                                    const Rectangle& imageryBounds,
                                    bool invertedVCoordinate = false);

    // ── Accessors (aligned with cesium-native naming) ──

    /// The loading tile (may be nullptr). Aligned with getLoadingTile().
    const RasterOverlayTile* getLoadingTile() const { return _pLoadingTile.get(); }
    RasterOverlayTile* getLoadingTile() { return _pLoadingTile.get(); }
    const std::shared_ptr<RasterOverlayTile>& getLoadingTileHandle() const {
        return _pLoadingTile;
    }

    /// The ready tile (may be nullptr). Aligned with getReadyTile().
    const RasterOverlayTile* getReadyTile() const { return _pReadyTile.get(); }
    RasterOverlayTile* getReadyTile() { return _pReadyTile.get(); }
    const std::shared_ptr<RasterOverlayTile>& getReadyTileHandle() const {
        return _pReadyTile;
    }
    ReadyTileSource getReadyTileSource() const { return readyTileSource_; }
    const SourceTileList& getMappedSourceTiles() const {
        return mappedSourceTiles_;
    }
    bool usesDirectRasterTile() const { return directRasterTile_; }

    /// Aligned with getTextureCoordinateID().
    /// Returns the raster-overlay texture coordinate index for this projection.
    int32_t getTextureCoordinateID() const { return textureCoordinateID_; }

    /// Aligned with getState().
    State getState() const { return state_; }

    /// Aligned with getTranslation() / getScale().
    float getTranslationU() const { return offsetU_; }
    float getTranslationV() const { return offsetV_; }
    float getScaleU() const { return scaleU_; }
    float getScaleV() const { return scaleV_; }

    // Retained renderer/cache accessors.
    State state() const { return state_; }
    Texture* texture() const;
    float offsetU() const { return offsetU_; }
    float offsetV() const { return offsetV_; }
    float scaleU() const { return scaleU_; }
    float scaleV() const { return scaleV_; }

private:
    void clearTileOwnershipState();

    State state_ = State::Unattached;

    /// The tile we WANT at the desired zoom. May be Loading/Failed.
    /// Aligned with cesium-native _pLoadingTile and retains tile lifetime.
    std::shared_ptr<RasterOverlayTile> _pLoadingTile;

    /// The tile we RENDER (always Loaded/Done, texture available).
    /// Aligned with cesium-native _pReadyTile and retains texture lifetime.
    std::shared_ptr<RasterOverlayTile> _pReadyTile;

    /// Cached pointer to the ready texture (from _pReadyTile->getTexture()).
    Texture* readyTexture_ = nullptr;

    ReadyTileSource loadingTileSource_ = ReadyTileSource::None;
    ReadyTileSource readyTileSource_ = ReadyTileSource::None;
    SourceTileList mappedSourceTiles_;
    bool directRasterTile_ = false;

    /// UV transform: rasterUV = geometryUV * scale + offset.
    float offsetU_ = 0.0f, offsetV_ = 0.0f;
    float scaleU_ = 1.0f, scaleV_ = 1.0f;

    /// Set once when the original desired-zoom tile fails.
    /// Never cleared — suppresses MoreDetailAvailable::Yes.
    /// Aligned with cesium-native _originalFailed.
    bool originalFailed_ = false;

    /// The geometry tile key this raster is mapped to.
    /// Used for attachRaster/detachRaster notification key consistency.
    TileKey geometryKey_;

    /// Raster-overlay texture coordinate index for this projection.
    /// Aligned with cesium-native _textureCoordinateID.
    int32_t textureCoordinateID_ = -1;

    /// Our layer slot in the owning tile's rasterOverlays vector.
    /// This is resource notification identity, not a texture-coordinate ID.
    int32_t overlaySlot_ = 0;
};

} // namespace earth_engine
