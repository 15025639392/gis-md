#pragma once

#include "AmapVectorTile.h"
#include "Feature.h"
#include "../core/math/Rectangle.h"
#include "../tiling/TileKey.h"
#include "../threading/CancellationToken.h"

#include <functional>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace earth_engine {

class Engine;
class FeatureRenderLayer;
class RenderDevice;
class ThreadPool;
class AmapClassicRuntime;
class SceneFrameResourceArbiter;
struct Feature;
#if defined(EARTH_ENGINE_TESTING)
struct AmapDecodedTileDecodeTraits;
struct AmapDecodedTile;
#endif

/// 高德瓦片地理矩形(弧度,4326 等距圆柱 2:1)。
/// 与 AmapGeographicScheme::tileToRectangle 同数学;GLESView 的
/// workerTessellationContextForArea 需要按瓦片矩形取高度范围。
Rectangle amapTileRectangle(const TileKey& key);

#if defined(EARTH_ENGINE_TESTING)
std::vector<Feature> amapRegionsToFeaturesForContractTest(
    std::shared_ptr<const AmapDecodedTile> tile);
std::vector<Feature> amapMainToFeaturesForContractTest(
    std::shared_ptr<const AmapDecodedTile> tile);
std::vector<Feature> amapPoiToFeaturesForContractTest(
    std::shared_ptr<const AmapDecodedTile> tile);
#endif

/// Atomic owner of the official AMap decoded-data path. It is the only
/// production constructor that pairs the sealed profiles with the AMap typed
/// decoders, geographic scheme, discrete data zooms and shared type-1 cache.
/// Layers remain Scene-owned through Engine, while this bundle owns their
/// sources and removes the layers during teardown.
class AmapClassicSourceBundle {
private:
    friend class AmapClassicRuntime;
    using FetchCallback = std::function<void(int, std::vector<uint8_t>)>;
    using Type1Fetch = std::function<void(const TileKey&, FetchCallback)>;
    using PoiFetch = std::function<void(const TileKey&, FetchCallback)>;
    using SurfaceFeatures = std::vector<Feature>;
    using SurfaceFeaturesCallback =
        std::function<void(std::shared_ptr<const SurfaceFeatures>)>;

public:

    struct CacheStats {
        uint64_t hits = 0;
        uint64_t fetches = 0;
        uint64_t refetches = 0;
        uint64_t rawHits = 0;
        size_t residentTiles = 0;
        size_t rawTiles = 0;
        size_t rawBytes = 0;
        size_t residentBytes = 0;
        uint64_t failureSkips = 0;
    };

    struct SourceStats {
        double ingestMs = 0.0;
        double treeMs = 0.0;
        double dispatchMs = 0.0;
        double commitMs = 0.0;
        int commits = 0;
        int drops = 0;
        int tessellateDispatched = 0;
        int selectedZoom = 0;
        int64_t desiredTileCount = 0;
        size_t scannedTileCount = 0;
        size_t renderTileCount = 0;
        size_t requestTileCount = 0;
        size_t pendingTileCount = 0;
        size_t tessellatingTileCount = 0;
        size_t readyTileCount = 0;
        size_t activeTileCount = 0;
        size_t activeAncestorPairs = 0;
    };

    struct Options {
        size_t decodedCacheTiles = 48;
        size_t rawCacheTiles = 256;
        size_t maximumTilesPerView = 256;
        size_t maximumTessellationsInFlight = 8;
        bool collectDiagnostics = true;
#if defined(EARTH_ENGINE_TESTING)
        size_t failAfterSourceConstruction = 0;
#endif
    };

    ~AmapClassicSourceBundle();

    AmapClassicSourceBundle(const AmapClassicSourceBundle&) = delete;
    AmapClassicSourceBundle& operator=(const AmapClassicSourceBundle&) = delete;

    const FeatureRenderLayer* regionsLayer() const { return regionsLayer_; }
    const FeatureRenderLayer* mainLayer() const { return mainLayer_; }
    const FeatureRenderLayer* poiLayer() const { return poiLayer_; }
    CacheStats type1CacheStats() const;
    SourceStats regionsSourceStats() const;
    SourceStats mainSourceStats() const;
    SourceStats poiSourceStats() const;
    bool hasPendingWork() const;
    void setOfficialSurfaceFillBaked(bool enabled);

private:
#if defined(EARTH_ENGINE_TESTING)
    friend struct AmapDecodedTileDecodeTraits;
    friend bool decodeAmapTile(
        const uint8_t*, size_t, std::vector<AmapDecodedLayerPart>&,
        std::string*);
    friend bool decodeAmapPoiTile(
        const uint8_t*, size_t, std::vector<AmapDecodedLayerPart>&,
        std::string*);
    friend std::vector<Feature> amapDecodedPartToFeatures(
        const AmapDecodedLayerPart&, bool);
    friend std::vector<Feature> amapRegionsToFeaturesForContractTest(
        std::shared_ptr<const AmapDecodedTile>);
    friend std::vector<Feature> amapMainToFeaturesForContractTest(
        std::shared_ptr<const AmapDecodedTile>);
    friend std::vector<Feature> amapPoiToFeaturesForContractTest(
        std::shared_ptr<const AmapDecodedTile>);
#endif
    AmapClassicSourceBundle(
        Engine& engine, RenderDevice& renderDevice, Type1Fetch type1Fetch,
        PoiFetch poiFetch,
        std::shared_ptr<ThreadPool> type1DecodePool,
        std::shared_ptr<ThreadPool> poiDecodePool,
        std::shared_ptr<ThreadPool> tessellationPool,
        Options options);
    void update(const Rectangle& viewRectangle, double cameraHeightMeters,
                SceneFrameResourceArbiter& resourceArbiter);
    /// Resolve all ordinary surface polygons needed to paint one geographic
    /// or WebMercator 256 page. The request shares the official type-1 cache with
    /// regions/main, converts geometry to WGS84, and reports nullptr on any
    /// incomplete/failed source tile so the raster overlay keeps its loading
    /// or ancestor fallback state instead of caching a transparent success.
    void requestSurfaceFeatures(const TileKey& webMercatorKey,
                                CancellationToken token,
                                SurfaceFeaturesCallback callback) const;
    struct Impl;
    Engine& engine_;
    std::unique_ptr<Impl> impl_;
    FeatureRenderLayer* regionsLayer_ = nullptr;
    FeatureRenderLayer* mainLayer_ = nullptr;
    FeatureRenderLayer* poiLayer_ = nullptr;
    size_t updateCursor_ = 0;
};

} // namespace earth_engine
