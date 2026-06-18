#pragma once

namespace earth_engine {

struct TilesetTile;

struct TileSelectionHistory {
    static bool wasRenderedLastFrame(const TilesetTile& tile);

    static bool childWasRefinedLastFrame(const TilesetTile& tile);

    static bool anyDescendantWasRenderedLastFrame(const TilesetTile& tile);
};

} // namespace earth_engine
