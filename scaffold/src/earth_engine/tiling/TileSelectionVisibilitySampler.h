#pragma once

#include "../scene/FrameState.h"

#include <vector>

namespace earth_engine {

struct TilesetTile;

struct TileSelectionVisibilityContext {
    bool renderTilesUnderCamera = true;
    double cameraLongitude = 0.0;
    double cameraLatitude = 0.0;
};

struct TileSelectionVisibilitySample {
    bool visibleFromCamera = false;
    bool inFrustum = false;
};

struct TileSelectionVisibilitySampler {
    static bool boundsVisible(
        const TilesetTile& tile,
        const std::vector<SelectorView>& views,
        const TileSelectionVisibilityContext& context);

    static TileSelectionVisibilitySample sampleTileBounds(
        const TilesetTile& tile,
        const std::vector<SelectorView>& views,
        const TileSelectionVisibilityContext& context);

    static TileSelectionVisibilitySample sampleChildBounds(
        const std::vector<TilesetTile*>& children,
        const std::vector<SelectorView>& views,
        const TileSelectionVisibilityContext& context);

    static TileSelectionVisibilitySample sampleForTileSelection(
        const TilesetTile& tile,
        const std::vector<SelectorView>& views,
        const TileSelectionVisibilityContext& context);
};

} // namespace earth_engine
