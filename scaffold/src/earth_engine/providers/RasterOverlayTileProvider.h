#pragma once

#include "RasterOverlayTile.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileScheme.h"
#include "../tiling/SurfaceTile.h"
#include "../core/math/Rectangle.h"
#include "../renderer/RenderDevice.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <functional>
#include <chrono>

namespace earth_engine {

class ImageryProvider;
struct DecodedImage;

/// cesium-native RasterOverlayTileProvider equivalent.
///
/// Owns the lifecycle of RasterOverlayTile instances. Handles tile creation,
/// async loading dispatch, texture upload, throttling, and cache management.
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
    /// @param device GPU device for texture creation (may be null for headless).
    RasterOverlayTileProvider(ImageryProvider& provider,
                              const TileScheme& scheme,
                              RenderDevice* device);
    ~RasterOverlayTileProvider();

    RasterOverlayTileProvider(const RasterOverlayTileProvider&) = delete;
    RasterOverlayTileProvider& operator=(const RasterOverlayTileProvider&) = delete;

    // ── Tile lifecycle ──

    /// cesium-native: get or create a tile for a key.
    /// Returns the shared placeholder tile if the provider is not yet ready.
    TilePtr getTile(const TileKey& key);

    /// cesium-native mapOverlayToTile rectangle path: get or create a raster
    /// tile for the geometry rectangle.
    TilePtr getTile(const Rectangle& rectangle,
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
    void setOwner(RasterOverlay* owner) { owner_ = owner; }

    /// Direct access to the imagery provider.
    ImageryProvider& getImageryProvider() { return provider_; }

    /// Returns the tile scheme.
    const TileScheme& getTileScheme() const { return scheme_; }

    /// Projection used by rectangle overlay tiles. The current provider
    /// contract composes source imagery into geographic rectangles.
    RasterOverlayProjection getProjection() const {
        return RasterOverlayProjection::Geographic;
    }

    /// Returns the render device (may be null).
    RenderDevice* getRenderDevice() const { return device_; }

    // ── Async loading ──

    /// cesium-native: initiate async load for a tile.
    /// Transitions state to Loading and issues HTTP request.
    /// @return true if the load was initiated, false if already loading/loaded.
    bool loadTile(RasterOverlayTile& tile);

    /// cesium-native: throttled load. Returns false if at concurrent limit.
    bool loadTileThrottled(RasterOverlayTile& tile);

    /// Maximum concurrent tile loads.
    int maximumSimultaneousTileLoads = 20;

    /// Current number of tiles in Loading state.
    int getThrottledTilesCurrentlyLoading() const;

    double getMaximumScreenSpaceError() const {
        return maximumScreenSpaceError_;
    }
    void setMaximumScreenSpaceError(double maximumScreenSpaceError) {
        maximumScreenSpaceError_ =
            maximumScreenSpaceError > 0.0 ? maximumScreenSpaceError : 2.0;
    }

    /// Process completed uploads on the main thread.
    /// Should be called once per frame.
    void processPendingUploads();

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

    /// Evict tiles that have not been referenced recently.
    /// Called once per frame from Tileset::buildRenderCommands,
    /// AFTER all tile access for the frame is complete.
    void trimUnusedTiles();

    // Texture ownership: RasterOverlayTile owns its GPU texture
    // via unique_ptr<Texture>. No external callback needed.

#ifdef __ANDROID__
    // DIAGNOSTIC ONLY: lets Android crash/perf probes detect stale raster
    // texture raw pointers before dereferencing them.
    static void registerLiveTextureForDiagnostics(const Texture* texture);
    static void unregisterLiveTextureForDiagnostics(const Texture* texture);
    static bool isLiveTextureForDiagnostics(const Texture* texture);
    static void registerLiveTileForDiagnostics(const RasterOverlayTile* tile);
    static void unregisterLiveTileForDiagnostics(const RasterOverlayTile* tile);
    static bool isLiveTileForDiagnostics(const RasterOverlayTile* tile);
#endif

private:
    /// Internal: create GPU texture from decoded image.
    std::unique_ptr<Texture> uploadTexture(const DecodedImage& image,
                                           bool generateMipmaps);

    /// Internal: load a rectangle raster tile by combining the provider's
    /// quadtree imagery tiles that overlap its rectangle.
    bool loadRectangleTile(RasterOverlayTile& tile);

    /// Tile cache key from TileKey.
    std::string tileCacheKey(const TileKey& key) const;

    ImageryProvider& provider_;
    const TileScheme& scheme_;
    RenderDevice* device_;
    class RasterOverlay* owner_ = nullptr;

    /// All cached tiles retained by this provider (key → shared_ptr).
    std::unordered_map<std::string, TilePtr> tiles_;

    /// cesium-native: shared placeholder tile returned when provider is not ready.
    TilePtr placeholderTile_;
    bool ready_ = true;

    /// Pending GPU uploads (HTTP completed, awaiting main-thread upload).
    struct PendingUpload {
        std::string cacheKey;
        std::unique_ptr<DecodedImage> image;
    };
    std::deque<PendingUpload> pendingUploads_;
    std::mutex pendingMutex_;

    /// Tiles currently in-flight (requested but not yet responded).
    std::unordered_set<std::string> inFlightRequests_;

    /// Failed tiles (key → first fail timestamp, for retry logic).
    struct FailedRecord { double firstFailTime = 0.0; int retries = 0; };
    std::unordered_map<std::string, FailedRecord> failedTiles_;

    /// Monotonic frame counter, updated by trimUnusedTiles.
    /// Used to stamp lastUsedFrame on tiles in getTile().
    uint64_t frameNumber_ = 0;
    double maximumScreenSpaceError_ = 2.0;

};

} // namespace earth_engine
