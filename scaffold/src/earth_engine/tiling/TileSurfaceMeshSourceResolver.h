#pragma once

#include "DecodedHeightmapSampler.h"
#include "TileMeshLegacyHeightmapMode.h"
#include "TileGltfTerrainUpsampledChildMaterializer.h"
#include "TileSurface.h"
#include "TileSurfaceMeshResolutionPolicy.h"
#include "TilesetTile.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
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
        bool hasTerrainQuadtree,
        TileMeshLegacyHeightmapMode legacyHeightmapMode,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh) {
        const bool hasOwnTerrain = ownHeightmap != nullptr;
        const bool contentOwnedTerrainOnly =
            legacyHeightmapMode ==
            TileMeshLegacyHeightmapMode::ContentOwnedTerrainOnly;
        const bool useLegacyHeightmapSurface =
            !contentOwnedTerrainOnly;
        const bool allowEllipsoidFallbackWithoutTerrain =
            !hasTerrainQuadtree || useLegacyHeightmapSurface;
        TileSurfaceMeshResolution resolution =
            TileSurfaceMeshResolution::forContext(
                hasOwnTerrain,
                tile.content.derivesTerrainFromParent(),
                hasTerrainQuadtree);
        if (tile.content.derivesTerrainFromParent() &&
            !useLegacyHeightmapSurface) {
            resolution.markDone = false;
        }

        if (shouldDeferToGltfTerrainUpsample(tile, hasOwnTerrain)) {
            resolution.markDone = false;
            return resolution;
        }

        resolveAncestorUpsample(
            tile,
            hasOwnTerrain,
            useLegacyHeightmapSurface,
            findUpsampleSource,
            ensureAncestorMesh,
            resolution);
        resolveEllipsoidFallback(
            tile,
            ownHeightmap,
            allowEllipsoidFallbackWithoutTerrain,
            resolution);

        return resolution;
    }

private:
    static bool shouldDeferToGltfTerrainUpsample(const TilesetTile& tile,
                                                 bool hasOwnTerrain) {
        return !hasOwnTerrain &&
            tile.content.derivesTerrainFromParent() &&
            TileGltfTerrainUpsampledChildMaterializer::findGltfTerrainSource(
                tile) != nullptr;
    }

    template <typename FindUpsampleSourceFn,
              typename EnsureAncestorMeshFn>
    static void resolveAncestorUpsample(
        TilesetTile& tile,
        bool hasOwnTerrain,
        bool allowLegacySurfaceUpsample,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh,
        TileSurfaceMeshResolution& resolution) {
        if (tile.content.renderContent.hasSurfaceMesh() || hasOwnTerrain) {
            return;
        }
        if (!allowLegacySurfaceUpsample) {
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

    static void resolveEllipsoidFallback(
        TilesetTile& tile,
        DecodedHeightmap* ownHeightmap,
        bool allowEllipsoidFallbackWithoutTerrain,
        TileSurfaceMeshResolution& resolution) {
        if (tile.content.renderContent.hasSurfaceMesh()) {
            return;
        }
        if (!ownHeightmap && !allowEllipsoidFallbackWithoutTerrain) {
            return;
        }

        auto fallbackMesh = std::make_unique<SurfaceTileMesh>();
        *fallbackMesh = TileSurface::buildEllipsoidMesh(
            tile.bounds,
            ownHeightmap ? 64 : 16);
        resolution.source = ownHeightmap
            ? SurfaceDrawableSource::LegacyHeightmapTerrain
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
