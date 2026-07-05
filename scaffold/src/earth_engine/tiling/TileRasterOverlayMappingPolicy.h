#pragma once

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TilesetTile.h"

#include <optional>
#include <utility>

namespace earth_engine {

struct TileRasterOverlayMappingContext {
    bool hasRenderContentDetails = false;
    bool mapsLoadedRenderContent = false;
    bool waitForContentTerrainDetails = false;
    const RasterOverlayDetails* overlayDetails = nullptr;

    const RasterOverlayDetails& details() const {
        static const RasterOverlayDetails emptyDetails;
        return overlayDetails ? *overlayDetails : emptyDetails;
    }
};

struct TileRasterOverlayMappingPolicy {
    static TileRasterOverlayMappingContext contextFor(
        const TilesetTile& tile) {
        const bool hasRenderContentDetails =
            tile.hasCommittedRenderContent() &&
            tile.content.renderContent.hasRasterOverlayDetailsContent();
        const bool mapsLoadedRenderContent =
            tile.hasCommittedRenderContent() &&
            tile.content.renderContent.hasRenderableTerrainContent();

        return TileRasterOverlayMappingContext{
            hasRenderContentDetails,
            mapsLoadedRenderContent,
            tile.waitsForContentTerrainRasterDetails(),
            mapsLoadedRenderContent
                ? &tile.content.renderContent.rasterOverlayDetails()
                : nullptr};
    }

    static const Rectangle* geometryRectangle(
        const TileRasterOverlayMappingContext& context,
        RasterOverlayProjection projection) {
        return context.hasRenderContentDetails
            ? context.details().findRectangleForOverlayProjection(projection)
            : nullptr;
    }

    static std::optional<Rectangle> boundingVolumeRectangle(
        const TilesetTile& tile,
        const TileRasterOverlayMappingContext& context,
        RasterOverlayProjection projection) {
        return context.hasRenderContentDetails ||
               context.waitForContentTerrainDetails
            ? std::nullopt
            : TileRasterOverlayDetailsGenerator::
                  projectEffectiveContentBoundingVolumeRectangle(
                      tile,
                      projection);
    }

    static const Rectangle& targetRectangle(
        const TilesetTile& tile,
        const Rectangle* geometryRectangle,
        const std::optional<Rectangle>& boundingVolumeRectangle) {
        if (geometryRectangle) {
            return *geometryRectangle;
        }
        return boundingVolumeRectangle ? *boundingVolumeRectangle
                                       : tile.bounds;
    }

    struct ResolvedTargetGeometry {
        double screenPixelsX = 0.0;
        double screenPixelsY = 0.0;
        std::optional<Rectangle> boundingVolumeRectangle;
    };

    // 闸3:解析目标几何(computeDesiredScreenPixels + boundingVolumeRectangle),
    // 带映射槽缓存。cesium 里这套几何是 addTileOverlays 的「加载时一次」动作,
    // 不随帧/相机变。真渲染内容(details 非空)首次算一次后每帧复用,消除 Done
    // 瓦片每帧的三角开销。bounds 回退(details 空)不缓存,照旧每帧算。
    static ResolvedTargetGeometry resolveTargetGeometry(
        TilesetTile& tile,
        const TileRasterOverlayMappingContext& context,
        RasterOverlayProjection projection,
        RasterMappedToTilesetTile& mapped,
        double maximumScreenSpaceError) {
        const void* detailsPtr = context.overlayDetails;
        if (detailsPtr != nullptr &&
            mapped.targetGeometryCacheValid(detailsPtr, projection)) {
            return ResolvedTargetGeometry{
                mapped.cachedTargetScreenPixelsX(),
                mapped.cachedTargetScreenPixelsY(),
                mapped.cachedBoundingVolumeRectangle()};
        }
        const Rectangle* geomRect = geometryRectangle(context, projection);
        std::optional<Rectangle> bvr =
            boundingVolumeRectangle(tile, context, projection);
        const Rectangle& target = targetRectangle(tile, geomRect, bvr);
        const RasterTargetScreenPixels px =
            RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
                target,
                projection,
                tile.nonZeroGeometricError(),
                maximumScreenSpaceError);
        if (detailsPtr != nullptr) {
            mapped.cacheTargetGeometry(
                detailsPtr,
                projection,
                px.x,
                px.y,
                bvr);
        }
        return ResolvedTargetGeometry{px.x, px.y, std::move(bvr)};
    }
};

} // namespace earth_engine
