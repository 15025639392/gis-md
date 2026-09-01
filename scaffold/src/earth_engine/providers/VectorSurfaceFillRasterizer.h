#pragma once

#include "earth_engine/data/AmapSurfaceMaskRasterizer.h"
#include "earth_engine/data/Feature.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {

struct DecodedImage;

/// One resolved surface-fill record for a polygon Feature.  The resolver is
/// source-specific (it parses the vector schema and style to decide whether a
/// feature is an ordinary surface fill and what color it gets); the rasterizer
/// below is generic across any vector tile source / projection.
struct SurfaceFillRecord {
    std::array<float, 4> color{};
    int drawOrder = 0;
    /// Groups features that rasterize as an even-odd union (avoid darkening
    /// when the same footprint is covered by several source records).  For a
    /// single style the identity can be 0; overlapping records of the same
    /// identity union instead of alpha-over.
    uint64_t identity = 0;
};

using SurfaceFillResolver = std::function<std::optional<SurfaceFillRecord>(
    const Feature& feature, double displayZoom)>;

/// Rasterize ordinary surface-fill polygons into a 256x256 RGBA page for
/// `tileBounds`.  Generic across projections and vector schemas: the
/// source-specific parts (schema keys, style color windows, identity) are all
/// delegated to `resolver`.  Non-polygon features and features the resolver
/// returns nullopt for (extrusions, labels, unknown identity) are excluded.
std::unique_ptr<DecodedImage> rasterizeSurfaceFill(
    const std::shared_ptr<const std::vector<Feature>>& featureSet,
    const Rectangle& tileBounds, double displayZoom,
    const SurfaceFillResolver& resolver,
    AmapSurfaceMaskRasterizerOptions::Projection projection);

} // namespace earth_engine
