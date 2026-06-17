#pragma once

#include "TilesetTile.h"
#include "RasterMappedToTilesetTile.h"
#include "TileScheme.h"
#include "TilePlan.h"
#include "../core/math/Vec3.h"
#include "../core/math/OrientedBoundingBox.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"
#include "../providers/ImageryProvider.h"
#include "../renderer/RenderCommand.h"
#include "../scene/FrameState.h"

#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <list>
#include <limits>

namespace earth_engine {

class Renderer;
class RenderDevice;
class Frustum;
struct SelectorFrame;
class ActivatedRasterOverlay;
struct TilesetTestAccess;

enum class TileOcclusionState {
    NotOccluded,
    Occluded,
    OcclusionUnavailable
};

/// cesium-native TilesetOptions::FogDensityAtHeight equivalent.
struct FogDensityAtHeight {
    double cameraHeight;  // meters above ellipsoid
    double fogDensity;    // density value
};

/// cesium-native TilesetOptions subset used by the unified terrain tileset.
/// Defaults intentionally mirror native where the local renderer has the
/// corresponding capability.
struct TilesetOptions {
    double maximumScreenSpaceError = 16.0;
    uint32_t maximumSimultaneousTileLoads = 20;
    bool preloadAncestors = true;
    bool preloadSiblings = true;
    uint32_t loadingDescendantLimit = 20;
    bool forbidHoles = false;
    bool enableFrustumCulling = true;
    bool enableOcclusionCulling = true;
    bool delayRefinementForOcclusion = true;
    bool enableFogCulling = true;
    bool enforceCulledScreenSpaceError = true;
    double culledScreenSpaceError = 64.0;
    bool renderTilesUnderCamera = true;
    int64_t maximumCachedBytes = 512LL * 1024 * 1024;
    bool enableLodTransitionPeriod = false;
    float lodTransitionLength = 1.0f;
    bool kickDescendantsWhileFadingIn = true;
    double mainThreadLoadingTimeLimit = 0.0;
    double tileCacheUnloadTimeLimit = 0.0;
    std::vector<FogDensityAtHeight> fogDensityTable = {
        {359.393, 2.0e-5},     {800.749, 2.0e-4},
        {1275.6501, 1.0e-4},   {2151.1192, 7.0e-5},
        {3141.7763, 5.0e-5},   {4777.5198, 4.0e-5},
        {6281.2493, 3.0e-5},   {12364.307, 1.9e-5},
        {15900.765, 1.0e-5},   {49889.0549, 8.5e-6},
        {78026.8259, 6.2e-6},  {99260.7344, 5.8e-6},
        {120036.3873, 5.3e-6}, {151011.0158, 5.2e-6},
        {156091.1953, 5.1e-6}, {203849.3112, 4.2e-6},
        {274866.9803, 4.0e-6}, {319916.3149, 3.4e-6},
        {493552.0528, 2.6e-6}, {628733.5874, 2.2e-6},
        {1000000.0, 0.0},
    };
};

struct TilesetLoadDiagnostics {
    int loadQueuePreloadRequests = 0;
    int loadQueueNormalRequests = 0;
    int loadQueueUrgentRequests = 0;
    int pendingTerrainRequests = 0;
    int pendingTerrainUploads = 0;
    int pendingTerrainTerminalResults = 0;
    int pendingContentRequests = 0;
    int pendingContentUploads = 0;
    int pendingContentTerminalResults = 0;
    int unloadQueueTiles = 0;
    int loadUnloadingTiles = 0;
    int loadFailedTemporarilyTiles = 0;
    int loadUnloadedTiles = 0;
    int loadContentLoadingTiles = 0;
    int loadContentLoadedTiles = 0;
    int loadDoneTiles = 0;
    int loadFailedTiles = 0;
    int contentUnknownTiles = 0;
    int contentEmptyTiles = 0;
    int contentExternalTiles = 0;
    int contentRenderTiles = 0;
    int missingRasterOverlayProjections = 0;

    int loadQueueTotal() const {
        return loadQueuePreloadRequests +
               loadQueueNormalRequests +
               loadQueueUrgentRequests;
    }

    int pendingTerrainTotal() const {
        return pendingTerrainRequests +
               pendingTerrainUploads +
               pendingTerrainTerminalResults;
    }

    int pendingContentTotal() const {
        return pendingContentRequests +
               pendingContentUploads +
               pendingContentTerminalResults;
    }
};

/// cesium-native Tileset equivalent.
/// Manages a unified quadtree of terrain and raster overlay tiles.
class Tileset {
public:
    using OcclusionCallback =
        std::function<TileOcclusionState(const TilesetTile&)>;

    Tileset(std::unique_ptr<TerrainProvider> terrainProvider,
            std::unique_ptr<TileScheme> tileScheme,
            std::vector<ActivatedRasterOverlay*> rasterOverlays,
            RenderDevice* device,
            TilesetOptions options,
            std::unique_ptr<TilesetContentProvider> contentProvider = nullptr);
    ~Tileset();

    void update(const FrameState& frameState);
    void buildRenderCommands(Renderer& renderer, RenderCommandList& commands);

    const TilePlan& tilePlan() const { return tilePlan_; }
    const TileScheme& tileScheme() const { return *tileScheme_; }
    int cachedTerrainTiles() const { return static_cast<int>(terrainCache_.size()); }
    int pendingRequests() const;
    int64_t totalBytesUsed() const { return totalBytesUsed_; }
    TilesetLoadDiagnostics loadDiagnostics() const;

    /// Sample the best loaded terrain height at longitude/latitude.
    /// Returns ellipsoid height in meters, or 0 when no loaded terrain tile
    /// currently covers the position.
    float sampleHeight(double lngRad, double latRad) const;

    /// cesium-native: release all render references after GPU submit.
    /// Called by Scene after renderer_->submit(commands) so that
    /// reference counts protect tiles until the GPU has consumed them.
    void releaseRenderReferences();

    /// cesium-native TileOcclusionRendererProxyPool equivalent input hook.
    /// Renderer/platform code may provide real per-tile occlusion results.
    /// Do not return OcclusionUnavailable for tiles whose result will never
    /// arrive; return NotOccluded so traversal does not wait forever.
    void setOcclusionCallback(OcclusionCallback callback);
    void clearOcclusionCallback();

private:
    friend struct TilesetTestAccess;

    enum class TileLoadPriorityGroup {
        Preload = 0,
        Normal = 1,
        Urgent = 2
    };
    enum class UnloadTileContentResult {
        Keep,
        Remove,
        RemoveAndClearChildren
    };

    struct TileLoadRequest {
        TileKey key;
        TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
        double priority = std::numeric_limits<double>::max();
    };

    struct PendingTerrainUpload {
        TileKey key;
        std::string cacheKey;
        TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
        double priority = 0.0;
        std::unique_ptr<DecodedHeightmap> heightmap;
    };

    struct PendingTerrainTerminalResult {
        TileKey key;
        std::string cacheKey;
        TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
        double priority = 0.0;
        TerrainTileLoadStatus status = TerrainTileLoadStatus::Failed;
    };

    struct PendingContentUpload {
        TileKey key;
        std::string cacheKey;
        TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
        double priority = 0.0;
        TileContentLoadResult result;
    };

    struct PendingContentTerminalResult {
        TileKey key;
        std::string cacheKey;
        TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
        double priority = 0.0;
        TileContentLoadStatus status = TileContentLoadStatus::Failed;
    };

    struct TraversalDetails {
        bool allAreRenderable = true;
        bool anyWereRenderedLastFrame = false;
        uint32_t notYetRenderableCount = 0;
    };

    void selectTiles(const FrameState& frameState);
    TraversalDetails visitTileIfNeeded(TilesetTile& tile,
                                       const SelectorFrame& selectorFrame,
                                       uint32_t depth,
                                       bool ancestorMeetsSse);
    TraversalDetails visitTile(TilesetTile& tile,
                               const SelectorFrame& selectorFrame,
                               uint32_t depth,
                               bool meetsSse,
                               bool ancestorMeetsSse,
                               double tilePriority,
                               double tileSse);
    TraversalDetails createTraversalDetailsForSingleTile(const TilesetTile& tile) const;
    TraversalDetails createTraversalDetailsForCulledTile(const TilesetTile& tile) const;
    void renderSelectedTile(TilesetTile& tile,
                            double tileSse,
                            bool queueForLoad,
                            double tilePriority =
                                std::numeric_limits<double>::max());
    void queueTileLoad(
        const TileKey& key,
        TileLoadPriorityGroup group,
        double priority = std::numeric_limits<double>::max());
    void ensureTileChildren(TilesetTile& tile);
    void createRasterOverlayUpsampledChildren(TilesetTile& tile);
    void resetTileSelectionState();
    bool hasSurfaceDrawable(const TilesetTile& tile) const;
    bool isTileCompleteRenderable(const TilesetTile& tile) const;
    bool isTileRenderable(const TilesetTile& tile) const;
    std::vector<size_t> rasterOverlayProcessingOrder() const;
    void prepareRasterOverlaysForSelection(TilesetTile& tile);
    bool hasLoadedTerrainContent(const TilesetTile& tile) const;
    bool isAvailabilityBoundaryTile(const TilesetTile& tile) const;
    bool canRefine(const TilesetTile& tile) const;
    const TilesetTile* findUpsampleSourceTile(
        const TilesetTile& tile,
        bool allowUnloadingSource = false) const;
    bool prepareUpsampleSourceTile(
        TilesetTile& tile,
        double priority = std::numeric_limits<double>::max());
    bool wasRenderedLastFrame(const TilesetTile& tile) const;
    bool childWasRefinedLastFrame(const TilesetTile& tile) const;
    bool anyDescendantWasRenderedLastFrame(const TilesetTile& tile) const;
    bool hasLodTransitionRenderContent(const TilesetTile& tile) const;
    TileOcclusionState checkSingleTileOcclusion(
        const TilesetTile& tile) const;
    TileOcclusionState checkOcclusion(const TilesetTile& tile) const;
    TileOcclusionState checkSoftwareOcclusion(const TilesetTile& tile) const;
    double computeTileSse(const TilesetTile& tile,
                          const SelectorFrame& selectorFrame,
                          const std::vector<double>& distances) const;

    void requestMissingTiles(const std::vector<TileLoadRequest>& loadRequests);
    void processPendingUploads();
    TilesetTile* ensureTile(const TileKey& key);
    void prefetchRasterOverlays(TilesetTile& tile);
    void ingestQuantizedMeshAvailability(const TileKey& key,
                                         DecodedHeightmap& heightmap);
    void ensureTileMesh(TilesetTile& tile);
    void ensureGltfRenderResources(TilesetTile& tile);
    void buildGltfDrawCommands(Renderer& renderer,
                               TilesetTile& tile,
                               RenderCommandList& commands,
                               float transitionOpacity);
    UnloadTileContentResult unloadTileContent(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer);
    void buildTileDrawCommand(Renderer& renderer, TilesetTile& tile,
                              RenderCommandList& commands,
                              float transitionOpacity);
    void updateLodTransitions(double deltaSeconds);
    bool wasRenderedInPreviousSelection(const TilesetTile& tile) const;

    // ── cesium-native cache alignment ──
    /// cesium-native: byte-budget-based unload (replaces fixed tile count).
    /// Unloads tiles until totalBytesUsed <= maximumCachedBytes.
    void unloadCachedBytes(int64_t maximumCachedBytes,
                           IPrepareRendererResources* pPrepRenderer);
    /// Estimate bytes used by a tile (mesh VBO + IBO + heightmap + raster textures).
    static int64_t estimateTileBytes(const TilesetTile& tile);
    /// cesium-native: recompute totalBytesUsed_ from current tile state.
    /// Called before unload to ensure overlay attach/detach is accounted for.
    void updateTotalBytesUsed();
    /// Track unload eligibility: tiles not used this frame are eligible.
    void markEligibleForUnloading(const std::string& key);
    void markIneligibleForUnloading(const std::string& key);
    bool hasReferencedDescendant(const TilesetTile& tile) const;
    bool subtreeHasActiveContentWork(const TilesetTile& tile);
    void eraseTileIndexState(const std::string& key);
    /// cesium-native: recursively clear children when parent is unloaded.
    void clearChildrenRecursively(TilesetTile* tile,
                                  IPrepareRendererResources* pPrepRenderer);
    /// Detach overlays on geometry tiles that are no longer rendered this frame.
    void detachInactiveRasterOverlays(IPrepareRendererResources* pPrepRenderer);

    // ── cesium-native fog alignment ──
    /// cesium-native: compute fog density from camera height using density table.
    static double computeFogDensity(
        const std::vector<FogDensityAtHeight>& fogDensityTable,
        double cameraHeightMeters);
    /// cesium-native: exponential fog visibility test.
    static bool isVisibleInFog(double distance, double fogDensity);
    /// cesium-native BoundingRegion approximation: terrain tiles use their
    /// current min/max height range when estimating culling/fog bounds.
    static Vec3 tileBoundsCenter(const Rectangle& bounds);
    static double tileBoundsRadius(const TilesetTile& tile,
                                   const Vec3& center);
    static std::optional<OrientedBoundingBox> tileBoundingRegionObb(
        const TilesetTile& tile);
    static bool tileIntersectsFrustum(const TilesetTile& tile,
                                      const Frustum& frustum);
    static double approximateDistanceToTileBounds(
        const TilesetTile& tile,
        const Vec3& cameraPosition);

    // ── cesium-native priority alignment ──
    /// cesium-native: view-direction-weighted tile priority.
    /// Lower = higher priority. Formula: (1-dot(tileDir,viewDir)) * distance.
    static double computeTilePriority(const Vec3& tileCenter,
                                       const Vec3& cameraPos,
                                       const Vec3& cameraDir,
                                       double distance);

    std::string terrainCacheKey(const TileKey& key) const;

    std::unique_ptr<TerrainProvider> terrainProvider_;
    std::unique_ptr<TilesetContentProvider> contentProvider_;
    std::unique_ptr<TileScheme> tileScheme_;
    std::vector<ActivatedRasterOverlay*> rasterOverlays_;
    RenderDevice* device_ = nullptr;
    TilesetOptions options_;

    TilePlan tilePlan_;

    // cesium-native: byte budget instead of fixed tile count.
    // cesium-native TilesetOptions::maximumCachedBytes default.
    static constexpr int64_t kMaximumCachedBytes = 512LL * 1024 * 1024;
    int64_t totalBytesUsed_ = 0;
    uint64_t frameNumber_ = 0;

    // cesium-native: unload queue — LRU-ordered list of eligible tile keys.
    std::list<std::string> unloadQueue_;
    std::unordered_map<std::string, std::list<std::string>::iterator> unloadQueueMap_;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles_;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>> terrainCache_;
    std::unordered_set<std::string> pendingRequests_;
    std::unordered_set<std::string> pendingContentRequestKeys_;
    std::unordered_map<std::string, CancellationToken> pendingRequestTokens_;
    std::unordered_set<std::string> pendingUploadKeys_;
    std::unordered_set<std::string> pendingContentUploadKeys_;
    std::unordered_set<std::string> emptyTiles_;
    std::unordered_set<std::string> tilesFadingOut_;
    std::deque<PendingTerrainUpload> pendingUploads_;
    std::deque<PendingTerrainTerminalResult> pendingTerminalResults_;
    std::deque<PendingContentUpload> pendingContentUploads_;
    std::deque<PendingContentTerminalResult> pendingContentTerminalResults_;
    mutable std::mutex pendingMutex_;
    std::condition_variable pendingCondition_;
    bool destroying_ = false;
    uint64_t generation_ = 0;
    Vec3 lastCameraPosition_ = Vec3::zero();
    Vec3 lastCameraDirection_ = Vec3::zero();  // for view-weighted priority
    std::vector<FrameState::SelectorView> lastSelectorViews_;
    double currentFrameTimeSeconds_ = 0.0;
    OcclusionCallback occlusionCallback_;
    bool cameraMoving_ = false;
    std::vector<TileLoadRequest> loadQueue_;
    int selectedTilesVisited_ = 0;
    int selectedTilesCulled_ = 0;
    int selectedTilesKicked_ = 0;
    int selectedFogCulled_ = 0;
    int selectedNotYetRenderable_ = 0;
    int selectedCulledTilesVisited_ = 0;
    int selectedTilesOccluded_ = 0;
    int selectedTilesWaitingForOcclusionResults_ = 0;
};

} // namespace earth_engine
