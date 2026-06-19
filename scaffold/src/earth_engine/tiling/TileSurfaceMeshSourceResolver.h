#pragma once

#include "DecodedHeightmapSampler.h"
#include "TileSurface.h"
#include "TileSurfaceMeshResolutionPolicy.h"
#include "TilesetTile.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../terrain/QuantizedMeshParser.h"
#include "../terrain/TerrainTile.h"

#include <memory>
#include <utility>

namespace earth_engine {

class TileSurfaceMeshSourceResolver {
public:
    template <typename FindUpsampleSourceFn,
              typename EnsureAncestorMeshFn>
    static TileSurfaceMeshResolution resolve(
        TilesetTile& tile,
        DecodedHeightmap* ownHeightmap,
        bool hasTerrainProvider,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh) {
        const bool hasOwnTerrain = ownHeightmap != nullptr;
        TileSurfaceMeshResolution resolution =
            TileSurfaceMeshResolution::forContext(
                hasOwnTerrain,
                tile.content.upsampledFromParent,
                hasTerrainProvider);

        resolveAncestorUpsample(
            tile,
            hasOwnTerrain,
            findUpsampleSource,
            ensureAncestorMesh,
            resolution);
        resolveOwnSurfaceMesh(tile, ownHeightmap, resolution);
        resolveQuantizedMesh(tile, ownHeightmap, resolution);
        resolveEllipsoidFallback(tile, ownHeightmap, resolution);

        return resolution;
    }

private:
    template <typename FindUpsampleSourceFn,
              typename EnsureAncestorMeshFn>
    static void resolveAncestorUpsample(
        TilesetTile& tile,
        bool hasOwnTerrain,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh,
        TileSurfaceMeshResolution& resolution) {
        if (tile.content.renderContent.hasSurfaceMesh() || hasOwnTerrain) {
            return;
        }

        if (!findUpsampleSource(tile, true) && tile.parent) {
            ensureAncestorMesh(*tile.parent);
        }

        const TilesetTile* source = findUpsampleSource(tile, true);
        if (!source) {
            return;
        }

        const SurfaceTileMesh* sourceMesh =
            source->content.renderContent.surfaceMesh();
        if (!sourceMesh) {
            return;
        }

        auto childMesh = TileSurface::upsampleChildMeshFromParent(
            *sourceMesh,
            source->bounds,
            tile.bounds);
        if (!childMesh) {
            return;
        }

        tile.content.renderContent.setSurfaceMesh(
            std::make_unique<SurfaceTileMesh>(std::move(*childMesh)));
        resolution.source = SurfaceDrawableSource::AncestorUpsample;
    }

    static void resolveOwnSurfaceMesh(
        TilesetTile& tile,
        DecodedHeightmap* ownHeightmap,
        TileSurfaceMeshResolution& resolution) {
        if (tile.content.renderContent.hasSurfaceMesh() || !ownHeightmap ||
            !ownHeightmap->surfaceMesh) {
            return;
        }

        tile.content.renderContent.setSurfaceMesh(
            std::move(ownHeightmap->surfaceMesh));
        resolution.source = SurfaceDrawableSource::OwnTerrain;
    }

    static void resolveQuantizedMesh(
        TilesetTile& tile,
        DecodedHeightmap* ownHeightmap,
        TileSurfaceMeshResolution& resolution) {
        if (tile.content.renderContent.hasSurfaceMesh() || !ownHeightmap ||
            ownHeightmap->rawData.empty()) {
            return;
        }

        std::unique_ptr<SurfaceTileMesh> parsedMesh =
            QuantizedMeshParser::parseToSurfaceTileMesh(
                ownHeightmap->rawData.data(),
                ownHeightmap->rawData.size(),
                tile.bounds);
        if (!parsedMesh) {
            return;
        }

        tile.content.renderContent.setSurfaceMesh(std::move(parsedMesh));
        resolution.source = SurfaceDrawableSource::OwnTerrain;
    }

    static void resolveEllipsoidFallback(
        TilesetTile& tile,
        DecodedHeightmap* ownHeightmap,
        TileSurfaceMeshResolution& resolution) {
        if (tile.content.renderContent.hasSurfaceMesh()) {
            return;
        }

        auto fallbackMesh = std::make_unique<SurfaceTileMesh>();
        *fallbackMesh = TileSurface::buildEllipsoidMesh(
            tile.bounds,
            ownHeightmap ? 64 : 16);
        resolution.source = ownHeightmap
            ? SurfaceDrawableSource::OwnTerrain
            : SurfaceDrawableSource::EllipsoidFallback;

        if (ownHeightmap && ownHeightmap->valid()) {
            applyHeightmapHeights(tile, *ownHeightmap, *fallbackMesh);
        }

        tile.content.renderContent.setSurfaceMesh(std::move(fallbackMesh));
    }

    static void applyHeightmapHeights(TilesetTile& tile,
                                      const DecodedHeightmap& heightmap,
                                      SurfaceTileMesh& mesh) {
        const auto& ellipsoid = Ellipsoid::WGS84();
        for (auto& vertex : mesh.vertices) {
            Cartographic cartographic =
                ellipsoid.cartesianToCartographic(vertex.positionEcef);
            const double height = static_cast<double>(
                DecodedHeightmapSampler::sampleHeight(
                    heightmap,
                    tile.bounds,
                    cartographic.longitude(),
                    cartographic.latitude()));
            Cartographic terrainCartographic = Cartographic::fromRadians(
                cartographic.longitude(),
                cartographic.latitude(),
                height);
            vertex.positionEcef =
                ellipsoid.cartographicToCartesian(terrainCartographic);
        }
    }
};

} // namespace earth_engine
