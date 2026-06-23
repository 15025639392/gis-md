#pragma once

#include "GltfContentProvider.h"
#include "../tiling/TileKey.h"

#include <string>
#include <vector>

namespace earth_engine {

class EllipsoidTerrainContentProvider final : public TilesetContentProvider {
public:
    explicit EllipsoidTerrainContentProvider(
        std::string schemeId = "XYZ-WebMercator",
        int maximumLevel = 14,
        int gridSize = 16);

    std::string id() const override;
    bool supportsTile(const TileKey& key) const override;
    std::vector<TileKey> rootTiles() const override;
    std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey& key) const override;
    std::vector<TileKey> childTiles(const TileKey& key) const override;
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState terrainAvailabilityState(
        const TileKey& key) const override;

    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority =
                                HttpRequestPriority::Normal) override;
    TileContentLoadResult decodeContent(const uint8_t* data,
                                        size_t size) override;

private:
    std::string schemeId_;
    int maximumLevel_ = 14;
    int gridSize_ = 16;
};

} // namespace earth_engine
