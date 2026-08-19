#pragma once

#include "GltfContentProvider.h"
#include "../providers/HeightmapTerrainProvider.h"
#include "../tiling/TileKey.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

struct DecodedHeightmap;

// Regular-grid raster-DEM terrain content provider (web-mercator XYZ). Composes
// a HeightmapTerrainProvider for async fetch+decode, then CPU-bakes each decoded
// tileSize×tileSize height grid into a regular-grid glTF terrain mesh via
// EllipsoidTerrainMeshBuilder (real heights + central-difference slope normals).
// Because it produces a real per-vertex GltfModel — identical in shape to the
// quantized-mesh path — the existing draw pipeline, overlay UV machinery, and
// CPU height-query systems all keep working unchanged.
//
// Structural metadata (root/children/bounds/quadtree topology) mirrors
// EllipsoidTerrainContentProvider; only requestTileContent differs (async fetch
// + mesh build instead of a synchronous flat-ellipsoid build).
class HeightmapTerrainContentProvider final : public TilesetContentProvider {
public:
    // Takes ownership of a configured HeightmapTerrainProvider (URL template,
    // encoding, zoom range, platform bridge already set by the caller).
    // `maximumLevel` is the quadtree depth exposed to selection (may exceed the
    // provider's native maxZoom once upsampling lands; P1 keeps them equal).
    explicit HeightmapTerrainContentProvider(
        std::unique_ptr<HeightmapTerrainProvider> provider,
        int maximumLevel = 14);

    std::string id() const override;
    bool supportsTile(const TileKey& key) const override;
    std::vector<TileKey> rootTiles() const override;
    std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey& key) const override;
    std::vector<TileKey> childTiles(const TileKey& key) const override;
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState availabilityState(
        const TileKey& key) const override;
    // 高度图地形四叉树拓扑由 scheme/zoom 静态决定(子瓦片集合不变),发布
    // 非零常量版本号让 TileChildFrameMaterializer 的 fast path 生效——否则
    // hasReliableTopologyVersion=false,物化检查每帧全量重跑(17 次调用/帧,
    // selTrav 7.6ms 里 refine 占 1-2.5ms 的浪费,2026-08-20 专项实测)。
    uint64_t childTopologyRevision() const override { return 1; }

    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority =
                                HttpRequestPriority::Normal) override;
    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority,
                            TileContentRequestOptions options) override;
    TileContentLoadResult decodeContent(const uint8_t* data,
                                        size_t size) override;

    ProviderRequestDiagnostics requestDiagnostics() const override;

private:
    // Build a renderable terrain GltfModel from a decoded height grid for `key`.
    TileContentLoadResult buildContent(
        const TileKey& key,
        const DecodedHeightmap& heightmap,
        const TileContentRequestOptions& options) const;

    std::unique_ptr<HeightmapTerrainProvider> provider_;
    std::string schemeId_;
    int maximumLevel_ = 14;
};

} // namespace earth_engine
