#pragma once

#include "TilesetTile.h"
#include "TileScheme.h"
#include "TilePlan.h"
#include "TileContentAccess.h"
#include "TileContentCacheManager.h"
#include "TileCacheOwnershipManager.h"
#include "TileContentLifecycleManager.h"
#include "TileContentResourceInvalidator.h"
#include "TileContentRuntime.h"
#include "TileLoadDiagnostics.h"
#include "TileLoadQueue.h"
#include "TileLoadTypes.h"
#include "TileOcclusionCallback.h"
#include "TileMeshPreparationManager.h"
#include "TileOcclusionState.h"
#include "TileRasterUpsampledChildCoordinator.h"
#include "TileRenderCommandManager.h"
#include "TileSelectionCounters.h"
#include "TileSelectionMetrics.h"
#include "TileSelectionReuseState.h"
#include "TilesetTileRegistry.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../core/math/Vec3.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderCommand.h"
#include "../scene/FrameState.h"

#include <memory>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_set>

namespace earth_engine {

class Renderer;
class RenderDevice;
struct SelectorFrame;
class ActivatedRasterOverlay;
struct TilesetTestAccess;
class TilesetRenderFrameFacade;
class TilesetSelectionFrameFacade;
class TilesetContentLifecycleFacade;
class TilesetOcclusionFacade;
class TilesetQueryFacade;
class TilesetUpdateFrameFacade;

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
    using OcclusionCallback = TileOcclusionCallback;

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
    int cachedTerrainTiles() const;
    int pendingRequests() const;
    int64_t totalBytesUsed() const;
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
    friend class TilesetContentLifecycleFacade;
    friend class TilesetOcclusionFacade;
    friend class TilesetQueryFacade;
    friend class TilesetRenderFrameFacade;
    friend class TilesetSelectionFrameFacade;
    friend class TilesetUpdateFrameFacade;

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
    uint64_t frameNumber_ = 0;

    TilesetTileRegistry tileRegistry_;
    TileContentLifecycleManager contentLifecycle_;
    TileContentAccess contentAccess_;
    TileContentCacheManager contentCache_;
    uint64_t resourceRevision_ = 1;
    TileSelectionReuseState selectionReuseState_;
    TileContentResourceInvalidator resourceInvalidator_;
    TileCacheOwnershipManager cacheOwnership_;
    TileRasterUpsampledChildCoordinator rasterUpsampledChildren_;
    std::unordered_set<std::string> tilesFadingOut_;
    uint64_t generation_ = 0;
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
    TileMeshPreparationManager meshPreparation_;
    TileContentRuntime contentRuntime_;
    TileRenderCommandManager renderCommands_;
};

} // namespace earth_engine
