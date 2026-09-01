#include "AmapClassicRuntime.h"
#include "../Engine.h"
#include "../layers/FeatureRenderLayer.h"
#include "../providers/AmapSurfaceMaskImageryProvider.h"
#include "../renderer/AmapTerrainFillMaskStore.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

AmapClassicRuntime::AmapClassicRuntime(
    Engine& engine, RenderDevice& renderDevice,
    PlatformBridge& platformBridge,
    std::shared_ptr<ThreadPool> type1DecodePool,
    std::shared_ptr<ThreadPool> poiDecodePool,
    std::shared_ptr<ThreadPool> tessellationPool,
    Options options)
    : assets_(engine, platformBridge, options.credentials),
      transport_(std::make_unique<Transport>(
          platformBridge,
          Transport::Credentials{options.credentials.webKey},
          [this](std::string version, std::string path, std::string type) {
              assets_.installManifest(std::move(version), std::move(path),
                                      std::move(type));
          })),
      sources_(engine, renderDevice,
               [this](const TileKey& key,
                      AmapClassicSourceBundle::FetchCallback callback) {
                   transport_->fetchType1(key, std::move(callback));
               },
               [this](const TileKey& key,
                      AmapClassicSourceBundle::FetchCallback callback) {
                   transport_->fetchPoi(key, std::move(callback));
               },
               std::move(type1DecodePool),
               std::move(poiDecodePool), std::move(tessellationPool),
               std::move(options.sources)),
      surfaceMaskStyleState_(
          std::make_shared<AmapSurfaceMaskStyleState>()),
      terrainFillMaskStore_(std::make_unique<AmapTerrainFillMaskStore>(
          [this](const TileKey& key, CancellationToken token,
                 AmapTerrainFillMaskStore::FeatureFetchCallback callback) {
              requestSurfaceFeatures(
                  key, std::move(token), std::move(callback));
          },
          surfaceMaskStyleState_)) {
    if (auto* poiLayer = sources_.poiLayer_) {
        poiLayer->setOfficialIconAtlasDemand(
            [this](int atlas) { assets_.requireAtlas(atlas); });
    }
    engine.activateAmapClassicOfficialGlyphProvider(
        [this](uint32_t codepoint) { assets_.requireGlyph(codepoint); });
    assets_.start();
}

AmapClassicRuntime::~AmapClassicRuntime() = default;

void AmapClassicRuntime::setOfficialSurfaceFillBaked(bool enabled) {
    sources_.setOfficialSurfaceFillBaked(enabled);
}

void AmapClassicRuntime::setSurfaceMaskStyleState(
    std::shared_ptr<AmapSurfaceMaskStyleState> state) {
    if (!state) {
        state = std::make_shared<AmapSurfaceMaskStyleState>();
    }
    surfaceMaskStyleState_ = std::move(state);
    if (terrainFillMaskStore_) {
        terrainFillMaskStore_->setStyleState(surfaceMaskStyleState_);
    }
}

AmapTerrainFillMaskStore::Probe AmapClassicRuntime::maskProbe() const {
    if (!terrainFillMaskStore_) return AmapTerrainFillMaskStore::Probe{};
    // takeProbe() only resets diagnostic counters; the const_cast is
    // diagnostic-only and never alters render state.
    return const_cast<AmapTerrainFillMaskStore*>(terrainFillMaskStore_.get())
        ->takeProbe();
}

void AmapClassicRuntime::update(const Rectangle& viewRectangle,
                                double cameraHeightMeters,
                                SceneFrameResourceArbiter& resourceArbiter) {
    if (surfaceMaskStyleState_) {
        const double displayZoom = std::min(
            24.0, std::max(0.0, std::log2(
                4.0e7 / std::max(1.0, cameraHeightMeters))));
        surfaceMaskStyleState_->setDisplayZoom(displayZoom);
    }
    assets_.update(resourceArbiter);
    transport_->update();
    sources_.update(viewRectangle, cameraHeightMeters, resourceArbiter);
}

} // namespace earth_engine
