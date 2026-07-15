#pragma once

#include "TileFillGeometrySignature.h"
#include "TilesetTile.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/geodesy/WebMercatorProjection.h"

#include <algorithm>
#include <array>
#include <optional>

namespace earth_engine {

struct TileSurfaceClip {
    static std::optional<std::array<float, 4>> forDescendantBounds(
        const TilesetTile& commandTile,
        const Rectangle& descendantBounds) {
        constexpr double kTwoPiForLongitudeWrap =
            3.14159265358979323846264338327950288 * 2.0;

        Rectangle texcoordRect = commandTile.bounds;
        RasterOverlayProjection projection =
            RasterOverlayProjection::Geographic;
        const TileFillGeometrySignature* fillSignature =
            commandTile.content.renderContent.fillGeometrySignature();
        if (commandTile.content.renderContent.drawsFill() &&
            fillSignature) {
            projection = fillSignature->projection;
            texcoordRect =
                projection == RasterOverlayProjection::WebMercator
                ? projectRectangleSimple(
                      WebMercatorProjection(Ellipsoid::WGS84()),
                      fillSignature->bounds.splitAtAntimeridian().first)
                : fillSignature->bounds;
        } else if (
            commandTile.content.renderContent
                .hasRasterOverlayDetailsContent()) {
            const RasterOverlayDetails& details =
                commandTile.content.renderContent.rasterOverlayDetails();
            if (!details.rasterOverlayProjections.empty() &&
                !details.rasterOverlayRectangles.empty() &&
                !details.rasterOverlayRectangles[0].isEmpty()) {
                projection = details.rasterOverlayProjections[0];
                texcoordRect = details.rasterOverlayRectangles[0];
            }
        }

        const Rectangle descendantProjected =
            projection == RasterOverlayProjection::WebMercator
                ? projectRectangleSimple(
                      WebMercatorProjection(Ellipsoid::WGS84()),
                      descendantBounds.splitAtAntimeridian().first)
                : descendantBounds;

        const double ancestorWidth = texcoordRect.width();
        const double ancestorHeight = texcoordRect.computeHeight();
        if (ancestorWidth <= 0.0 || ancestorHeight <= 0.0) {
            return std::nullopt;
        }

        auto horizontalOffset = [&](double x) {
            double offset = x - texcoordRect.west();
            if (texcoordRect.crossesAntimeridian() && offset < 0.0) {
                offset += kTwoPiForLongitudeWrap;
            }
            return offset;
        };

        double uMin =
            horizontalOffset(descendantProjected.west()) / ancestorWidth;
        double uMax =
            horizontalOffset(descendantProjected.east()) / ancestorWidth;
        if (texcoordRect.crossesAntimeridian() && uMax < uMin) {
            uMax += 1.0;
        }
        double vMin =
            (texcoordRect.north() - descendantProjected.north()) /
            ancestorHeight;
        double vMax =
            (texcoordRect.north() - descendantProjected.south()) /
            ancestorHeight;

        uMin = std::clamp(uMin, 0.0, 1.0);
        uMax = std::clamp(uMax, 0.0, 1.0);
        vMin = std::clamp(vMin, 0.0, 1.0);
        vMax = std::clamp(vMax, 0.0, 1.0);
        if (uMax <= uMin || vMax <= vMin) {
            return std::nullopt;
        }

        return std::array<float, 4>{
            static_cast<float>(uMin),
            static_cast<float>(vMin),
            static_cast<float>(uMax - uMin),
            static_cast<float>(vMax - vMin)};
    }
};

} // namespace earth_engine
