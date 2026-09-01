#pragma once

#include "Feature.h"
#include "../core/math/Rectangle.h"
#include "../tiling/TileKey.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

/// CPU coverage mask produced for an AMap surface-fill tile.
///
/// The mask is deliberately R8-only: it contains no color and no height.  A
/// terrain fragment shader can use the coverage to composite a style color on
/// top of the already displaced terrain surface.  Pixels are row-major and
/// y=0 is the north/top edge of the tile (the same convention as AMap's
/// geographic vector tiles).
struct AmapSurfaceMask {
    int size = 0;
    std::vector<uint8_t> coverage;

    bool empty() const { return size <= 0 || coverage.empty(); }

    uint8_t sample(int x, int y) const {
        if (x < 0 || y < 0 || x >= size || y >= size || coverage.empty()) {
            return 0;
        }
        return coverage[static_cast<size_t>(y) * static_cast<size_t>(size) +
                        static_cast<size_t>(x)];
    }

    const uint8_t* data() const { return coverage.data(); }
};

/// Options for the CPU rasterizer.  The production default is one 256x256
/// page with 2x supersampling (four binary samples per output pixel).
struct AmapSurfaceMaskRasterizerOptions {
    enum class Projection {
        Geographic,
        WebMercator
    };
    int size = 256;
    /// Values below 2 are promoted to 2; values above 4 are capped at 4.
    int supersample = 2;
    Projection projection = Projection::Geographic;
};

/// Rasterize polygon Features into a union coverage mask for `tileBounds`.
///
/// Coordinates in Feature rings are Cartographic radians.  `tileBounds` is
/// the geographic rectangle represented by the output page.  Each Feature is
/// evaluated using an even-odd rule across all of its rings, so holes,
/// independent fragments and nested islands are handled without winding
/// assumptions.  Distinct Features are unioned (maximum coverage), avoiding
/// darkening when two source records overlap.  Rings are implicitly closed;
/// no CDT or other triangulation is used.
AmapSurfaceMask rasterizeAmapSurfaceMask(
    const std::vector<const Feature*>& features, const Rectangle& tileBounds,
    const AmapSurfaceMaskRasterizerOptions& options = {});

/// Convenience overload for one Feature.
AmapSurfaceMask rasterizeAmapSurfaceMask(
    const Feature& feature, const Rectangle& tileBounds,
    const AmapSurfaceMaskRasterizerOptions& options = {});

/// AMap's geographic 2:1 tile rectangle (z=0 is the whole world, y=0 is
/// north).  This small helper keeps the TileKey entry point independent from
/// the source/renderer classes, while using exactly the same tile math as
/// AmapVectorSource::amapTileRectangle.
Rectangle amapSurfaceMaskTileRectangle(const TileKey& key);

/// Convenience TileKey entry point.  The key is interpreted in the AMap
/// geographic scheme (x/y at `key.z`).
AmapSurfaceMask rasterizeAmapSurfaceMask(
    const std::vector<const Feature*>& features, const TileKey& key,
    const AmapSurfaceMaskRasterizerOptions& options = {});

/// Value-owning convenience overload.  Rasterization itself remains
/// read-only; this overload only forms a transient pointer view of `features`.
AmapSurfaceMask rasterizeAmapSurfaceMask(
    const std::vector<Feature>& features, const Rectangle& tileBounds,
    const AmapSurfaceMaskRasterizerOptions& options = {});

AmapSurfaceMask rasterizeAmapSurfaceMask(
    const std::vector<Feature>& features, const TileKey& key,
    const AmapSurfaceMaskRasterizerOptions& options = {});

/// Named facade for callers that prefer an object-style API.  The methods are
/// stateless and simply forward to the free functions above.
class AmapSurfaceMaskRasterizer {
public:
    using Options = AmapSurfaceMaskRasterizerOptions;

    static AmapSurfaceMask rasterize(const Feature& feature,
                                     const Rectangle& tileBounds,
                                     const Options& options = {}) {
        return rasterizeAmapSurfaceMask(feature, tileBounds, options);
    }

    static AmapSurfaceMask rasterize(
        const std::vector<const Feature*>& features,
        const Rectangle& tileBounds, const Options& options = {}) {
        return rasterizeAmapSurfaceMask(features, tileBounds, options);
    }

    static AmapSurfaceMask rasterize(const std::vector<Feature>& features,
                                     const Rectangle& tileBounds,
                                     const Options& options = {}) {
        return rasterizeAmapSurfaceMask(features, tileBounds, options);
    }

    static AmapSurfaceMask rasterize(
        const std::vector<const Feature*>& features, const TileKey& key,
        const Options& options = {}) {
        return rasterizeAmapSurfaceMask(features, key, options);
    }

    static AmapSurfaceMask rasterize(const std::vector<Feature>& features,
                                     const TileKey& key,
                                     const Options& options = {}) {
        return rasterizeAmapSurfaceMask(features, key, options);
    }
};

}  // namespace earth_engine
