#pragma once

#include "ImageryProvider.h"
#include "../core/math/Rectangle.h"
#include "../data/Feature.h"
#include "../data/AmapSurfaceMaskRasterizer.h"
#include "../platform/bridge/PlatformBridge.h"

#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

/// Shared zoom epoch between the Scene-owned official runtime and the
/// facade-owned mask provider. A discrete official zoom change invalidates
/// cached pages; sub-threshold camera motion does not churn imagery.
class AmapSurfaceMaskStyleState {
public:
    struct Snapshot {
        double displayZoom = 0.0;
        uint64_t revision = 0;
    };

    explicit AmapSurfaceMaskStyleState(double displayZoom = 0.0);

    void setDisplayZoom(double displayZoom);
    double displayZoom() const;
    uint64_t revision() const;
    Snapshot snapshot() const;

private:
    // Low 8 bits = discrete display zoom, high bits = style epoch.  Keeping
    // both in one atomic prevents a page from being tagged with the old epoch
    // while already painted with the new zoom.
    std::atomic<uint64_t> packedState_{0};
};

/// Build an RGBA8 official AMap surface page from already decoded features.
/// This helper performs no network request and owns no tile hierarchy.
std::unique_ptr<DecodedImage> makeAmapSurfaceMaskImage(
    const std::shared_ptr<const std::vector<Feature>>& featureSet,
    const Rectangle& tileBounds, double displayZoom,
    AmapSurfaceMaskRasterizerOptions::Projection projection =
        AmapSurfaceMaskRasterizerOptions::Projection::Geographic);

/// Official AMap ordinary surface-fill mask provider.
///
/// The provider intentionally uses the Geographic-TMS source scheme.  AMap
/// surface features are WGS84 geographic coordinates, and a geographic page
/// keeps the rasterizer's latitude mapping exact; the raster overlay runtime
/// projects the page to the terrain tile's sampling space later.
class AmapSurfaceMaskImageryProvider final : public ImageryProvider {
public:
    using SurfaceFeatures = std::vector<Feature>;
    using FeatureSet = std::shared_ptr<const SurfaceFeatures>;
    using FeatureFetchCallback = std::function<void(FeatureSet)>;
    using SurfaceFeatureFetch = std::function<void(
        const TileKey&, CancellationToken, FeatureFetchCallback,
        HttpRequestPriority)>;

    /// @param fetch asynchronously resolves all WGS84 ordinary surface
    ///        polygons needed for the requested page.  nullptr from fetch
    ///        means pending/failure; an empty vector means a successful
    ///        transparent page.
    /// @param displayZoom renderer display zoom used by the sealed AMap style
    ///        table to resolve each polygon's color window.
    explicit AmapSurfaceMaskImageryProvider(
        SurfaceFeatureFetch fetch, double displayZoom);
    AmapSurfaceMaskImageryProvider(
        SurfaceFeatureFetch fetch,
        std::shared_ptr<AmapSurfaceMaskStyleState> styleState);

    std::string id() const override { return "amap-surface-mask-256"; }
    std::string type() const override { return "amap-surface-mask-imagery"; }
    std::string schemeId() const override { return "Geographic-TMS"; }

    int minZoom() const override { return 0; }
    int maxZoom() const override { return 25; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    uint64_t contentRevision() const override {
        return styleState_ ? styleState_->revision() : 0;
    }

    std::string buildUrl(const TileKey& key) const override;
    bool supportsTile(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t* data, size_t len) override;

    double displayZoom() const {
        return styleState_ ? styleState_->displayZoom() : 0.0;
    }

private:
    SurfaceFeatureFetch fetch_;
    std::shared_ptr<AmapSurfaceMaskStyleState> styleState_;
};

} // namespace earth_engine
