#include "TileSelectionVisibilitySampler.h"

#include "DirectRasterMapping.h"
#include "TileBoundsMetrics.h"
#include "TileSelectionCullingPolicy.h"
#include "TilesetTile.h"

#include <optional>

namespace earth_engine {
namespace {

TileSelectionVisibilitySample sampleSingleBounds(
    const TilesetTile& tile,
    const std::vector<SelectorView>& views,
    const TileSelectionVisibilityContext& context) {
    TileSelectionVisibilitySample sample;
    sample.cameraInside =
        TileSelectionVisibilitySampler::cameraInsideSelectionBounds(
            tile,
            context);
    for (const auto& view : views) {
        if (TileBoundsMetrics::tileIntersectsFrustum(tile, view.frustum)) {
            sample.inFrustum = true;
            break;
        }
    }
    sample.visibleFromCamera =
        sample.inFrustum ||
        (context.renderTilesUnderCamera && sample.cameraInside);
    return sample;
}

} // namespace

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
    return sampleSingleBounds(tile, views, context).visibleFromCamera;
}

TileSelectionVisibilitySample TileSelectionVisibilitySampler::sampleTileBounds(
    const TilesetTile& tile,
    const std::vector<SelectorView>& views,
    const TileSelectionVisibilityContext& context) {
    return sampleSingleBounds(tile, views, context);
}

TileSelectionVisibilitySample TileSelectionVisibilitySampler::sampleChildBounds(
    const std::vector<TilesetTile*>& children,
    const std::vector<SelectorView>& views,
    const TileSelectionVisibilityContext& context) {
    TileSelectionVisibilitySample sample;
    for (const TilesetTile* child : children) {
        if (!child) {
            continue;
        }
        const TileSelectionVisibilitySample childSample =
            sampleSingleBounds(*child, views, context);
        sample.visibleFromCamera =
            sample.visibleFromCamera || childSample.visibleFromCamera;
        sample.inFrustum = sample.inFrustum || childSample.inFrustum;
        sample.cameraInside =
            sample.cameraInside || childSample.cameraInside;
        if (sample.visibleFromCamera && sample.inFrustum) {
            break;
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

    if (!cullWithChildrenBounds) {
        return sampleTileBounds(tile, views, context);
    }

    TileSelectionVisibilitySample sample =
        sampleChildBounds(tile.children, views, context);
    // Child bounds may be tighter for culling, but frame summaries need the
    // camera-inside state of the tile itself.
    sample.cameraInside = cameraInsideSelectionBounds(tile, context);
    return sample;
}

} // namespace earth_engine
