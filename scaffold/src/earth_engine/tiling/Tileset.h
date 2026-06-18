#pragma once

#include "TilesetTile.h"
#include "RasterMappedToTilesetTile.h"
#include "TileScheme.h"
#include "TilePlan.h"
#include "TileFrameState.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadDiagnostics.h"
#include "TileLoadLifecycle.h"
#include "TileLoadPriorityPolicy.h"
#include "TileLoadQueue.h"
#include "TileLoadTypes.h"
#include "TileOcclusionState.h"
#include "TileIndexState.h"
#include "TileSelectionCounters.h"
#include "TileSelectionMetrics.h"
#include "TileSelectionReuseState.h"
#include "TileTraversalDetails.h"
#include "TileSubtreeTraversal.h"
#include "TileUnloadQueue.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../core/math/Vec3.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderCommand.h"
#include "../scene/FrameState.h"

#include <memory>
#include <array>
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <limits>

namespace earth_engine {

class Renderer;
class RenderDevice;
class IPrepareRendererResources;
struct SelectorFrame;
class ActivatedRasterOverlay;
struct TilesetTestAccess;
enum class TileCacheUnloadContentResult;

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

    struct TilePlanFinalizeTimings {
        double dedupeMs = 0.0;
        double transitionMs = 0.0;
        double summaryMs = 0.0;
    };

    void selectTiles(const FrameState& frameState);
    TileTraversalDetails visitTileIfNeeded(TilesetTile& tile,
                                           const SelectorFrame& selectorFrame,
                                           uint32_t depth,
                                           bool ancestorMeetsSse);
    TileTraversalDetails visitTile(TilesetTile& tile,
                                   const SelectorFrame& selectorFrame,
                                   uint32_t depth,
                                   bool meetsSse,
                                   bool ancestorMeetsSse,
                                   double tilePriority,
                                   double tileSse);
    TileTraversalDetails createTraversalDetailsForSingleTile(
        const TilesetTile& tile) const;
    TileTraversalDetails createTraversalDetailsForCulledTile(
        const TilesetTile& tile) const;
    void addTileToCurrentPlan(TilesetTile& tile,
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
    bool hasLoadedTerrainContent(const TilesetTile& tile) const;
    bool isAvailabilityBoundaryTile(const TilesetTile& tile) const;
    bool canRefine(const TilesetTile& tile) const;
    bool prepareUpsampleSourceTile(
        TilesetTile& tile,
        double priority = std::numeric_limits<double>::max());
    bool hasLodTransitionRenderContent(const TilesetTile& tile) const;
    TileOcclusionState checkSingleTileOcclusion(
        const TilesetTile& tile) const;
    TileOcclusionState checkOcclusion(const TilesetTile& tile) const;

    TileLoadRequestOutcome requestMissingTiles(
        const std::vector<TileLoadRequest>& loadRequests,
        FrameResourceBudget* budget = nullptr);
    bool processPendingUploads(bool interactionActive,
                               bool resourceSmoothingActive,
                               FrameResourceBudget* budget = nullptr);
    bool hasTilesetPendingWork() const;
    void markTileResourcesDirty();
    TilesetTile* ensureTile(const TileKey& key);
    void ensureTileMesh(TilesetTile& tile);
    TileCacheUnloadContentResult unloadTileContent(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer);
    void buildTileDrawCommand(Renderer& renderer, TilesetTile& tile,
                              RenderCommandList& commands,
                              float transitionOpacity,
                              bool allowSynchronousMeshPrep = true,
                              const std::optional<std::array<float, 4>>&
                                  surfaceClipUv = std::nullopt);
    TilePlanFinalizeTimings finalizeSelectedTilePlan(
        const FrameState& frameState);
    void refreshTilePlanRenderEntries();
    void updateLodTransitions(double deltaSeconds);

    // ── cesium-native cache alignment ──
    /// cesium-native: byte-budget-based unload (replaces fixed tile count).
    /// Unloads tiles until totalBytesUsed <= maximumCachedBytes.
    void unloadCachedBytes(int64_t maximumCachedBytes,
                           IPrepareRendererResources* pPrepRenderer);
    /// cesium-native: recompute totalBytesUsed_ from current tile state.
    /// Called before unload to ensure overlay attach/detach is accounted for.
    void updateTotalBytesUsed();
    /// Track unload eligibility: tiles not used this frame are eligible.
    void markEligibleForUnloading(const std::string& key);
    void markIneligibleForUnloading(const std::string& key);
    bool subtreeHasActiveContentWork(const TilesetTile& tile);
    void eraseTileIndexState(const std::string& key);
    /// cesium-native: recursively clear children when parent is unloaded.
    void clearChildrenRecursively(TilesetTile* tile,
                                  IPrepareRendererResources* pPrepRenderer);

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

    // cesium-native: unload queue - LRU-ordered eligible tile keys.
    TileUnloadQueue unloadQueue_;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles_;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>> terrainCache_;
    TileEmptyContentRegistry emptyContentRegistry_;
    std::unordered_set<std::string> tilesFadingOut_;
    TileLoadLifecycle loadLifecycle_;
    uint64_t generation_ = 0;
    uint64_t resourceRevision_ = 1;
    TileSelectionReuseState selectionReuseState_;
    bool cacheBytesDirty_ = true;
    bool interactionActiveForFrame_ = false;
    bool resourceSmoothingActiveForFrame_ = false;
    FrameResourceBudget frameResourceBudget_;
    double lastInteractionActiveTimeSeconds_ = -1.0;
    Vec3 lastCameraPosition_ = Vec3::zero();
    Vec3 lastCameraDirection_ = Vec3::zero();  // for view-weighted priority
    double currentFrameTimeSeconds_ = 0.0;
    OcclusionCallback occlusionCallback_;
    bool cameraMoving_ = false;
    TileLoadQueue loadQueue_;
    TileSelectionCounters selectionCounters_;
};

} // namespace earth_engine
