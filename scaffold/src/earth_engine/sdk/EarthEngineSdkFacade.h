#pragma once

#include "EarthSceneConfig.h"

#include <cstdint>
#include <memory>
#include <string>
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

    /// 注册应用自建的影像 overlay(应用实现 ImageryProvider
    /// 冒充影像走地形合成)。**必须在 installScene 之前调用** —— overlay 是
    /// 在 Tileset 构造时一次性交进去的,事后追加需要重建整个 tileset。
    /// 注册的 overlay 排在配置 overlay **之后**,即叠在卫星影像之上。
    /// installScene 会消费本列表(每次 installScene 重新注册)。
    void addCustomImageryOverlay(std::unique_ptr<ImageryProvider> provider,
                                 std::unique_ptr<TileScheme> scheme,
                                 RasterOverlay::Options options);
    /// Advance deferred scene setup on the owning render thread.
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
    /// 应用自建 overlay 的待挂列表(installScene 消费,见
    /// addCustomImageryOverlay)。
    struct PendingCustomOverlay {
        std::unique_ptr<ImageryProvider> provider;
        std::unique_ptr<TileScheme> scheme;
        RasterOverlay::Options options;
    };
    std::vector<PendingCustomOverlay> pendingCustomOverlays_;
    std::vector<std::unique_ptr<RasterOverlay>> rasterOverlays_;
    std::vector<std::unique_ptr<ActivatedRasterOverlay>>
        activatedRasterOverlays_;
};

} // namespace earth_engine
