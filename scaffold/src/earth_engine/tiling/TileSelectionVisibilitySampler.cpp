#include "TileSelectionVisibilitySampler.h"

#include "RasterMappedToTilesetTile.h"
#include "TileBoundsMetrics.h"
#include "TileSelectionCullingPolicy.h"
#include "TilesetTile.h"

#include <optional>

namespace earth_engine {

bool TileSelectionVisibilitySampler::cameraInsideSelectionBounds(
    const TilesetTile& tile,
    const TileSelectionVisibilityContext& context) {
    const std::optional<Rectangle> cameraTestBounds =
        tile.boundingVolume
        ? tile.boundingVolume->estimateGlobeRectangle()
        : std::optional<Rectangle>(tile.bounds);
    return cameraTestBounds &&
           cameraTestBounds->contains(
               context.cameraLongitude,
               context.cameraLatitude);
}

bool TileSelectionVisibilitySampler::boundsVisible(
    const TilesetTile& tile,
    const std::vector<SelectorView>& views,
    const TileSelectionVisibilityContext& context) {
    if (context.renderTilesUnderCamera &&
        cameraInsideSelectionBounds(tile, context)) {
        return true;
    }

    for (const auto& view : views) {
        if (TileBoundsMetrics::tileIntersectsFrustum(tile, view.frustum)) {
            return true;
        }
    }
    return false;
}

TileSelectionVisibilitySample TileSelectionVisibilitySampler::sampleTileBounds(
    const TilesetTile& tile,
    const std::vector<SelectorView>& views,
    const TileSelectionVisibilityContext& context) {
    TileSelectionVisibilitySample sample;
    sample.visibleFromCamera = boundsVisible(tile, views, context);
    for (const auto& view : views) {
        sample.inFrustum =
            sample.inFrustum ||
            TileBoundsMetrics::tileIntersectsFrustum(tile, view.frustum);
    }
    return sample;
}

TileSelectionVisibilitySample TileSelectionVisibilitySampler::sampleChildBounds(
    const std::vector<TilesetTile*>& children,
    const std::vector<SelectorView>& views,
    const TileSelectionVisibilityContext& context) {
    TileSelectionVisibilitySample sample;
    for (const TilesetTile* child : children) {
        if (!child || !boundsVisible(*child, views, context)) {
            continue;
        }

        sample.visibleFromCamera = true;
        for (const auto& view : views) {
            sample.inFrustum =
                sample.inFrustum ||
                TileBoundsMetrics::tileIntersectsFrustum(*child, view.frustum);
        }
    }
    return sample;
}

TileSelectionVisibilitySample
TileSelectionVisibilitySampler::sampleForTileSelection(
    const TilesetTile& tile,
    const std::vector<SelectorView>& views,
    const TileSelectionVisibilityContext& context) {
    bool hasUnconditionallyRefinedChild = false;
    for (const TilesetTile* child : tile.children) {
        if (child && child->unconditionallyRefine) {
            hasUnconditionallyRefinedChild = true;
            break;
        }
    }

    const bool cullWithChildrenBounds =
        TileSelectionCullingPolicy::shouldUseChildrenBounds(
            tile.refine,
            !tile.children.empty(),
            hasUnconditionallyRefinedChild);

    return cullWithChildrenBounds
        ? sampleChildBounds(tile.children, views, context)
        : sampleTileBounds(tile, views, context);
}

} // namespace earth_engine
