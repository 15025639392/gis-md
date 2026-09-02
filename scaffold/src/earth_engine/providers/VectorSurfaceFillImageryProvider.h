#pragma once

#include "earth_engine/providers/ImageryProvider.h"
#include "earth_engine/providers/VectorSurfaceFillRasterizer.h"
#include "earth_engine/tiling/TileScheme.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

class CancellationToken;

/// Generic vector-surface-fill imagery provider.  It turns ordinary surface
/// polygons (from any vector tile source / projection) into RGBA pages that the
/// raster-overlay runtime samples on the terrain at the display zoom - exactly
/// like a raster imagery overlay, but generated locally from decoded vector
/// features instead of a network fetch.  This is the mechanism that lets the
/// surface fill follow the camera zoom (fine near the camera) independent of
/// the coarse terrain mesh zoom.
class VectorSurfaceFillImageryProvider final : public ImageryProvider {
public:
    using FeatureSet = std::shared_ptr<const std::vector<Feature>>;
    using FeatureFetch = std::function<void(
        const TileKey&, CancellationToken,
        std::function<void(FeatureSet)>, HttpRequestPriority)>;

    /// @param scheme tile scheme of the vector source (e.g. Geographic-TMS or
    ///        WebMercator); the rasterizer projects each page into the terrain
    ///        sampling space through the raster-overlay runtime.
    /// @param fetch asynchronously resolves the surface polygons for a page.
    ///        nullptr result = pending/failure; empty vector = transparent page.
    /// @param resolver source schema/style: identity + color for each polygon.
    VectorSurfaceFillImageryProvider(
        std::unique_ptr<TileScheme> scheme,
        FeatureFetch fetch,
        SurfaceFillResolver resolver,
        double displayZoom = 0.0);

    std::string id() const override { return "vector-surface-fill-256"; }
    std::string type() const override { return "vector-surface-fill-imagery"; }
    std::string schemeId() const override;
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 25; }
    /// Source zoom follows the display zoom: the mask is CPU-generated, so the
    /// page is finely subdivided near the camera (fine pages) independent of the
    /// coarse terrain mesh zoom.
    int targetSourceZoom() const override {
        return static_cast<int>(packedState_.load(
            std::memory_order_acquire) & 0xFFu);
    }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }
    uint64_t contentRevision() const override;
    std::string buildUrl(const TileKey& key) const override;
    bool supportsTile(const TileKey& key) const override;
    void requestTile(const TileKey& key, CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;
    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t* data, size_t len) override;

    /// Update the renderer display zoom used by the style resolver.  A discrete
    /// zoom change bumps the content revision so cached pages are invalidated.
    void setDisplayZoom(double displayZoom);

private:
    std::unique_ptr<TileScheme> scheme_;
    FeatureFetch fetch_;
    SurfaceFillResolver resolver_;
    // Low 8 bits = discrete display zoom, high bits = style epoch.
    std::atomic<uint64_t> packedState_{0};
};

} // namespace earth_engine
