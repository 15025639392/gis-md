#pragma once

#include "EarthSceneConfig.h"

#include <memory>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class Engine;
class ImageryProvider;
class PlatformBridge;
class RasterOverlay;
class RenderDevice;
class TileScheme;

/// Thin SDK entry point for installing a configured earth scene into an
/// already-created Engine.
///
/// The caller still owns Engine, RenderDevice, and PlatformBridge. This facade
/// owns scene-level runtime objects created from EarthSceneConfig, such as
/// RasterOverlay / ActivatedRasterOverlay instances, and releases them when the
/// facade is destroyed or a new scene is installed.
class EarthEngineSdkFacade {
public:
    EarthEngineSdkFacade(Engine& engine,
                         RenderDevice& renderDevice,
                         PlatformBridge& platformBridge);
    ~EarthEngineSdkFacade();

    EarthEngineSdkFacade(const EarthEngineSdkFacade&) = delete;
    EarthEngineSdkFacade& operator=(const EarthEngineSdkFacade&) = delete;

    /// Install terrain, raster overlays, optional glTF content, initial camera,
    /// and fixed simulation time from a complete scene config.
    void installScene(EarthSceneConfig config);
    /// Restore the configured initial camera without rebuilding scene sources.
    void resetCamera();

    const EarthSceneConfig& config() const { return config_; }

private:
    void addActivatedRasterOverlay(
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        std::unique_ptr<ImageryProvider> provider,
        std::unique_ptr<TileScheme> scheme,
        RasterOverlay::Options options);

    Engine& engine_;
    RenderDevice& renderDevice_;
    PlatformBridge& platformBridge_;
    EarthSceneConfig config_;
    std::vector<std::unique_ptr<RasterOverlay>> rasterOverlays_;
    std::vector<std::unique_ptr<ActivatedRasterOverlay>>
        activatedRasterOverlays_;
};

} // namespace earth_engine
