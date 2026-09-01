#pragma once

#include "TileFillGeometrySignature.h"
#include "TilesetTile.h"

#include <algorithm>
#include <array>
#include <optional>

namespace earth_engine {

struct TileSurfaceClip {
    static RasterOverlayProjection projectionFor(
        const TilesetTile& commandTile) {
        const TileFillGeometrySignature* fillSignature =
            commandTile.content.renderContent.fillGeometrySignature();
        if (commandTile.content.renderContent.drawsFill() && fillSignature) {
            return fillSignature->projection;
        }
        if (commandTile.content.renderContent
                .hasRasterOverlayDetailsContent()) {
            const RasterOverlayDetails& details =
                commandTile.content.renderContent.rasterOverlayDetails();
            if (!details.rasterOverlayProjections.empty() &&
                !details.rasterOverlayRectangles.empty() &&
                !details.rasterOverlayRectangles[0].isEmpty()) {
                return details.rasterOverlayProjections[0];
            }
        }
        return RasterOverlayProjection::Geographic;
    }

    static bool supportsTerrainHeightRemap(
        const TilesetTile& commandTile) {
        // Height textures are sampled in the retained DEM's geographic UV.
        // A WebMercator clip UV is valid for imagery/discard, but reusing it
        // for height lookup selects the wrong latitude interval.
        return projectionFor(commandTile) ==
               RasterOverlayProjection::Geographic;
    }

    static std::optional<std::array<float, 4>> forDescendantBounds(
        const TilesetTile& commandTile,
        const Rectangle& descendantBounds) {
        constexpr double kTwoPiForLongitudeWrap =
            3.14159265358979323846264338327950288 * 2.0;

        Rectangle texcoordRect = commandTile.bounds;
        const RasterOverlayProjection projection =
            projectionFor(commandTile);
        const TileFillGeometrySignature* fillSignature =
            commandTile.content.renderContent.fillGeometrySignature();
        if (commandTile.content.renderContent.drawsFill() &&
            fillSignature) {
            texcoordRect =
                projectWorldRectangleForRasterOverlay(
                    fillSignature->bounds.splitAtAntimeridian().first,
                    projection);
        } else if (
            commandTile.content.renderContent
                .hasRasterOverlayDetailsContent()) {
            const RasterOverlayDetails& details =
                commandTile.content.renderContent.rasterOverlayDetails();
            if (!details.rasterOverlayProjections.empty() &&
                !details.rasterOverlayRectangles.empty() &&
                !details.rasterOverlayRectangles[0].isEmpty()) {
                texcoordRect = details.rasterOverlayRectangles[0];
            }
        }

        const Rectangle descendantProjected =
            projectWorldRectangleForRasterOverlay(
                descendantBounds.splitAtAntimeridian().first,
                projection);

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

        // 裁剪封缝带:clip 是片元级 discard,切缝处没有裙墙可用 —— 与相邻
        // 面(fill 代理 / 不同层级祖先)高度不一致时,边界会张开一条透天细缝
        // (运动/缩放期可见,数据就绪 clip 退场后自愈)。把裁剪矩形按子瓦片
        // 自身跨度外扩一小圈,让 clip 面横向盖过邻居边界,起裙墙同等的封缝
        // 作用。同祖先兄弟 clip 互相重叠时几何逐位相同,深度一致无伪影。
        // 真机 A/B(冷缓存山地掠视暂态):0 → 边界天色细缝;0.03 → 同型边界
        // 无缝。矩形贴着祖先 texcoordRect 边缘时会被下方 clamp 截断,该处
        // 由祖先自身的外缘裙墙接管。
        constexpr double kClipSeamSealMarginFraction = 0.03;
        const double uSpan = uMax - uMin;
        const double vSpan = vMax - vMin;
        uMin -= uSpan * kClipSeamSealMarginFraction;
        uMax += uSpan * kClipSeamSealMarginFraction;
        vMin -= vSpan * kClipSeamSealMarginFraction;
        vMax += vSpan * kClipSeamSealMarginFraction;

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
