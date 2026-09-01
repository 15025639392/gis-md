#pragma once

#include "AmapClassicAssets.h"
#include "../data/AmapVectorSource.h"
#include "../renderer/AmapTerrainFillMaskStore.h"

#include <memory>
#include <utility>

namespace earth_engine {

class Engine;
class EarthEngineSdkFacade;
class PlatformBridge;
class RenderDevice;
class Scene;
class SceneFrameResourceArbiter;
class ThreadPool;
class AmapSurfaceMaskStyleState;
class AmapTerrainFillMaskStore;

/// Single production owner for the sealed AMap classic runtime.
///
/// Assets and typed vector sources cannot be installed, pumped, or destroyed
/// independently. Member order is intentional: sources are destroyed before
/// assets, so no live source can publish symbols after the official asset
/// lifecycle has been cancelled.
class AmapClassicRuntime {
public:
    struct Options {
        AmapClassicAssets::Credentials credentials;
        AmapClassicSourceBundle::Options sources;
    };

    AmapClassicRuntime(const AmapClassicRuntime&) = delete;
    AmapClassicRuntime& operator=(const AmapClassicRuntime&) = delete;

    const AmapClassicAssets& assets() const { return assets_; }
    const AmapClassicSourceBundle& sources() const { return sources_; }

    /// Diagnostics probe for the terrain-fill mask request path.  Reading it
    /// does not change rendering; it resets the store's rolling counters.
    /// Returns empty counters when no mask store is installed.
    AmapTerrainFillMaskStore::Probe maskProbe() const;
#if defined(EARTH_ENGINE_TESTING)
    void requireAtlasForContractTest(int atlas) {
        assets_.requireAtlasForContractTest(atlas);
    }
    bool installAtlasForContractTest(int atlas, std::vector<uint8_t> body) {
        return assets_.installAtlasForContractTest(atlas, std::move(body));
    }
    FeatureRenderLayer* poiLayerForContractTest() {
        return sources_.poiLayer_;
    }
    void installAtlasManifestForContractTest(
        std::string version, std::string path, std::string type) {
        assets_.installManifest(
            std::move(version), std::move(path), std::move(type));
    }
#endif

private:
    class Transport {
    public:
        struct Credentials {
            std::string webKey;
        };

        using ManifestCallback =
            std::function<void(std::string, std::string, std::string)>;
        Transport(PlatformBridge& platformBridge, Credentials credentials,
                  ManifestCallback manifestCallback);
        ~Transport();
        Transport(const Transport&) = delete;
        Transport& operator=(const Transport&) = delete;
        void fetchType1(const TileKey& key,
                        AmapClassicSourceBundle::FetchCallback callback);
        void fetchPoi(const TileKey& key,
                      AmapClassicSourceBundle::FetchCallback callback);
        void update();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    friend class Engine;
    friend class EarthEngineSdkFacade;
    friend class Scene;
    friend struct std::default_delete<AmapClassicRuntime>;
    ~AmapClassicRuntime();
    AmapClassicRuntime(
        Engine& engine, RenderDevice& renderDevice,
        PlatformBridge& platformBridge,
        std::shared_ptr<ThreadPool> type1DecodePool,
        std::shared_ptr<ThreadPool> poiDecodePool,
        std::shared_ptr<ThreadPool> tessellationPool,
        Options options);
    void update(const Rectangle& viewRectangle, double cameraHeightMeters,
                SceneFrameResourceArbiter& resourceArbiter);
    void requestSurfaceFeatures(
        const TileKey& key, CancellationToken token,
        AmapClassicSourceBundle::SurfaceFeaturesCallback callback) const {
        sources_.requestSurfaceFeatures(
            key, std::move(token), std::move(callback));
    }

    /// Enable the terrain-baked ordinary surface fill after the sealed
    /// 256x256 overlay has been installed. Buildings and line/label paths are
    /// intentionally unaffected.
    void setOfficialSurfaceFillBaked(bool enabled);
    void setSurfaceMaskStyleState(
        std::shared_ptr<AmapSurfaceMaskStyleState> state);
    AmapTerrainFillMaskStore* terrainFillMaskStore() {
        return terrainFillMaskStore_.get();
    }

    AmapClassicAssets assets_;
    std::unique_ptr<Transport> transport_;
    AmapClassicSourceBundle sources_;
    std::shared_ptr<AmapSurfaceMaskStyleState> surfaceMaskStyleState_;
    // Declared after sources_: destruction runs in reverse order, so every
    // pending mask callback is cancelled before the shared decoded cache dies.
    std::unique_ptr<AmapTerrainFillMaskStore> terrainFillMaskStore_;
};

} // namespace earth_engine
