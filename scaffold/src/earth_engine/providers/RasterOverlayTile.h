#pragma once

#include "../tiling/TileKey.h"
#include "../core/math/Rectangle.h"

#include <cstdint>
#include <memory>
#include <string>

namespace earth_engine {

class RasterOverlayTileProvider;
class Texture;

/// cesium-native RasterOverlayTile equivalent.
/// Represents a single imagery tile in a quadtree with its own
/// async loading lifecycle and LoadState.
///
/// Owned by RasterOverlayTileProvider and active mappings through shared_ptr.
/// Raw pointers returned by accessors are non-owning views only.
class RasterOverlayTile {
public:
    /// cesium-native RasterOverlayTile::LoadState aligned.
    /// Values match cesium-native for direct comparison compatibility.
    enum class LoadState {
        Placeholder = -2,  /// Placeholder tile (provider not ready)
        Failed      = -1,  /// Image request or creation failed
        Unloaded    =  0,  /// Initial state, not yet requested
        Loading     =  1,  /// Request pending (HTTP or GPU upload)
        Loaded      =  2,  /// Image data loaded, GPU texture created
        Done        =  3   /// Renderer resources created (attached)
    };

    /// cesium-native RasterOverlayTile::MoreDetailAvailable aligned.
    enum class MoreDetailAvailable {
        No      = 0,
        Yes     = 1,
        Unknown = 2
    };

    /// Construct a real tile (starts in Unloaded).
    /// @param provider The owning tile provider (must outlive this tile).
    /// @param key The tile's quadtree key.
    /// @param bounds The geographic rectangle covered by this tile.
    RasterOverlayTile(RasterOverlayTileProvider& provider,
                      const TileKey& key,
                      const Rectangle& bounds,
                      std::string cacheKey = {});

    /// Construct a placeholder tile (starts in Placeholder).
    /// Used when the provider is not yet ready.
    explicit RasterOverlayTile(RasterOverlayTileProvider& provider);

    ~RasterOverlayTile();

    RasterOverlayTile(const RasterOverlayTile&) = delete;
    RasterOverlayTile& operator=(const RasterOverlayTile&) = delete;

    // ── cesium-native aligned accessors ──

    /// The current load state.
    LoadState getState() const { return state_; }
    void setState(LoadState s) { state_ = s; }

    /// The tile's quadtree key.
    const TileKey& getTileID() const { return key_; }

    /// Provider cache key. Real quadtree tiles use scheme/z/x/y. Rectangle
    /// tiles use a key derived from rectangle bounds and selected source level.
    const std::string& getCacheKey() const { return cacheKey_; }

    /// The geographic rectangle covered by this tile.
    const Rectangle& getRectangle() const { return bounds_; }
    void setRectangle(const Rectangle& bounds) { bounds_ = bounds; }

    /// cesium-native RasterOverlayTile::getTargetScreenPixels equivalent.
    /// This is the maximum number of screen pixels the geometry rectangle is
    /// expected to cover when the selector switches to its children.
    double getTargetScreenPixelsX() const { return targetScreenPixelsX_; }
    double getTargetScreenPixelsY() const { return targetScreenPixelsY_; }
    void setTargetScreenPixels(double x, double y) {
        targetScreenPixelsX_ = x;
        targetScreenPixelsY_ = y;
    }

    /// The owning tile provider.
    RasterOverlayTileProvider& getTileProvider() { return provider_; }
    const RasterOverlayTileProvider& getTileProvider() const { return provider_; }

    /// The GPU texture (valid when state >= Loaded).
    Texture* getTexture() const { return texture_.get(); }

    /// Set the GPU texture (takes ownership, called after GPU upload).
    void setTexture(std::unique_ptr<Texture> tex);

    /// cesium-native: a raster request may complete with an empty image when
    /// all quadtree source images are ancestor fallbacks. This tile is loaded
    /// for lifecycle purposes but has no renderer resources to attach.
    void markLoadedWithoutTexture();

    /// cesium-native: opaque renderer resources handle.
    /// Points to the owned GPU Texture.
    void* getRendererResources() const { return rendererResources_; }
    void setRendererResources(void* res) { rendererResources_ = res; }

    /// cesium-native: creates GPU resources and transitions Loaded→Done.
    /// In our architecture, the texture may already be created by the
    /// Provider's processPendingUploads. This method formalizes the
    /// transition and ensures state consistency.
    void loadInMainThread();

    /// cesium-native: whether a more detailed version of this tile exists.
    MoreDetailAvailable isMoreDetailAvailable() const;
    void setMoreDetailAvailable(MoreDetailAvailable moreDetailAvailable) {
        moreDetailAvailable_ = moreDetailAvailable;
    }

    /// The maximum zoom level for the tile scheme.
    int getMaxZoom() const { return maxZoom_; }
    void setMaxZoom(int maxZoom) { maxZoom_ = maxZoom; }

    /// cesium-native mapOverlayToTile rectangle path: this tile is not a
    /// provider quadtree tile. It represents a raster image for an arbitrary
    /// geometry rectangle.
    bool isRectangleTile() const { return rectangleTile_; }
    int getSourceZoom() const { return sourceZoom_; }
    void setRectangleTileLevel(int sourceZoom) {
        rectangleTile_ = true;
        sourceZoom_ = sourceZoom;
    }

    /// For imagery atlas: UV offset/scale within the atlas texture.
    float getAtlasOffsetU() const { return atlasOffsetU_; }
    float getAtlasOffsetV() const { return atlasOffsetV_; }
    float getAtlasScaleU() const { return atlasScaleU_; }
    float getAtlasScaleV() const { return atlasScaleV_; }
    void setAtlasUV(float ou, float ov, float su, float sv) {
        atlasOffsetU_ = ou; atlasOffsetV_ = ov;
        atlasScaleU_ = su; atlasScaleV_ = sv;
    }

private:
    RasterOverlayTileProvider& provider_;
    TileKey key_;
    std::string cacheKey_;
    Rectangle bounds_;
    LoadState state_ = LoadState::Unloaded;
    std::unique_ptr<Texture> texture_;
    void* rendererResources_ = nullptr;
    int maxZoom_ = 22;
    MoreDetailAvailable moreDetailAvailable_ = MoreDetailAvailable::Unknown;
    bool rectangleTile_ = false;
    int sourceZoom_ = 0;
    double targetScreenPixelsX_ = 256.0;
    double targetScreenPixelsY_ = 256.0;
    float atlasOffsetU_ = 0.0f, atlasOffsetV_ = 0.0f;
    float atlasScaleU_ = 1.0f, atlasScaleV_ = 1.0f;

public:
    /// cesium-native: last frame this tile was referenced.
    /// Used by the provider's trimUnusedTiles to evict stale tiles.
    uint64_t lastUsedFrame = 0;
};

} // namespace earth_engine
