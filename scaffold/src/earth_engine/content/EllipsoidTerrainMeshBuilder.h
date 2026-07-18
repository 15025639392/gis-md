#pragma once

#include "../tiling/SurfaceTile.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {

struct GltfModel;
class Rectangle;

// Per-vertex terrain height source (radians in, metres out). Return nullopt for
// "no loaded terrain covers this point" → the proxy vertex stays on the
// ellipsoid (height 0). Used to borrow real-terrain heights along edges shared
// with loaded neighbours so the proxy meets real terrain crack-free (the
// "rise" then only fills unknown interior). Because overlay UVs are a pure
// function of lon/lat, borrowing height does NOT move imagery.
using EllipsoidProxyHeightSampler =
    std::function<std::optional<float>(double longitudeRadians,
                                       double latitudeRadians)>;

// Generates an ellipsoid-surface proxy mesh (regular lon/lat grid over a
// geographic rectangle, positioned on the WGS84 ellipsoid at height 0) wrapped
// as a drape-ready GltfModel. Vertices carry absolute ECEF positions (with
// high/low split), geodetic surface normals, and overlay texture coordinates
// so raster imagery draping works identically to real terrain content.
//
// Ported out of EllipsoidTerrainContentProvider so both the permanent
// hole-fill provider and the temporal terrain-fill lifecycle (imagery draped
// on the ellipsoid before real terrain arrives, then swapped when it does) can
// share one generator. Because overlay UVs are a pure function of lon/lat (not
// height), a proxy vertex and the real-terrain vertex at the same lon/lat map
// imagery to the same texel — imagery stays put when terrain "rises".
class EllipsoidTerrainMeshBuilder {
public:
    /// Build a drape-ready ellipsoid proxy GltfModel over `geographicRectangle`
    /// with `gridSize` cells per side. Returns nullptr if runtime build fails.
    /// When `heightSampler` is set, each grid vertex is lifted to the sampled
    /// terrain height (borrowed edge heights → crack-free meeting with loaded
    /// neighbours); unset or nullopt samples keep the flat ellipsoid surface.
    // When `computeGridNormals` is true, per-vertex normals are derived from
    // neighbouring grid heights (central difference → correct slope shading for
    // real terrain). Default false keeps the flat geodetic surface normal used
    // by the ellipsoid proxy.
    static std::unique_ptr<GltfModel> makeModel(
        const Rectangle& geographicRectangle,
        RasterOverlayProjection projection,
        int gridSize,
        const EllipsoidProxyHeightSampler& heightSampler = {},
        bool computeGridNormals = false);
    static std::unique_ptr<GltfModel> makeModel(
        const Rectangle& geographicRectangle,
        const std::vector<RasterOverlayProjection>& projections,
        int gridSize,
        const EllipsoidProxyHeightSampler& heightSampler = {},
        bool computeGridNormals = false);

    /// The RasterOverlayDetails a proxy model of this rectangle/projection
    /// carries (projection list + projected rectangle + NW-V convention).
    static RasterOverlayDetails makeRasterOverlayDetails(
        const Rectangle& geographicRectangle,
        RasterOverlayProjection projection);
    static RasterOverlayDetails makeRasterOverlayDetails(
        const Rectangle& geographicRectangle,
        const std::vector<RasterOverlayProjection>& projections);
};

} // namespace earth_engine
