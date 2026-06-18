#pragma once

#include "DecodedHeightmapSampler.h"
#include "SurfaceMeshResourcePreparer.h"
#include "TileSurface.h"
#include "TileTerrainHeightRangePolicy.h"
#include "TilesetTile.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../providers/TerrainProvider.h"
#include "../terrain/QuantizedMeshParser.h"
#include "../terrain/TerrainTile.h"

#include <memory>
#include <utility>

namespace earth_engine {

struct TileSurfaceMeshEnsureInput {
    TilesetTile& tile;
    DecodedHeightmap* ownHeightmap = nullptr;
    RenderDevice* device = nullptr;
    bool hasTerrainProvider = false;
};

struct TileSurfaceMeshEnsureResult {
    bool resourcesDirty = false;
};

class TileSurfaceMeshEnsurer {
public:
    template <typename IngestAvailabilityFn,
              typename FindUpsampleSourceFn,
              typename EnsureAncestorMeshFn,
              typename HasSurfaceDrawableFn,
              typename IsCompleteRenderableFn>
    static TileSurfaceMeshEnsureResult ensure(
        const TileSurfaceMeshEnsureInput& input,
        IngestAvailabilityFn&& ingestAvailability,
        FindUpsampleSourceFn&& findUpsampleSource,
        EnsureAncestorMeshFn&& ensureAncestorMesh,
        HasSurfaceDrawableFn&& hasSurfaceDrawable,
        IsCompleteRenderableFn&& isCompleteRenderable) {
        TilesetTile& tile = input.tile;
        DecodedHeightmap* ownHeightmap = input.ownHeightmap;
        const bool hasOwnTerrain = ownHeightmap != nullptr;

        if (tile.meshReady) {
            if (hasOwnTerrain &&
                tile.surfaceSource != SurfaceDrawableSource::OwnTerrain) {
                tile.meshReady = false;
                tile.surfaceDrawable = false;
                tile.surfaceSource = SurfaceDrawableSource::None;
                tile.mesh.reset();
                tile.gpuVertexBuffer.reset();
                tile.gpuIndexBuffer.reset();
            } else {
                tile.surfaceDrawable = hasSurfaceDrawable(tile);
                if (tile.contentKind == TileContentKind::Render &&
                    tile.loadState == TileLoadState::ContentLoaded) {
                    tile.loadState = TileLoadState::Done;
                }
                return TileSurfaceMeshEnsureResult{};
            }
        }

        if (ownHeightmap) {
            ingestAvailability(tile.key, *ownHeightmap);
        }

        SurfaceDrawableSource meshSource = SurfaceDrawableSource::None;

        if (!tile.mesh && !hasOwnTerrain) {
            if (!findUpsampleSource(tile, true) && tile.parent) {
                ensureAncestorMesh(*tile.parent);
            }
            if (const TilesetTile* source =
                    findUpsampleSource(tile, true)) {
                if (source->mesh) {
                    auto childMesh =
                        TileSurface::upsampleChildMeshFromParent(
                            *source->mesh,
                            source->bounds,
                            tile.bounds);
                    if (childMesh) {
                        tile.mesh = std::make_unique<SurfaceTileMesh>(
                            std::move(*childMesh));
                        meshSource =
                            SurfaceDrawableSource::AncestorUpsample;
                    }
                }
            }
        }

        if (!tile.mesh && hasOwnTerrain && ownHeightmap->surfaceMesh) {
            tile.mesh = std::move(ownHeightmap->surfaceMesh);
            meshSource = SurfaceDrawableSource::OwnTerrain;
        }

        if (!tile.mesh && hasOwnTerrain && !ownHeightmap->rawData.empty()) {
            tile.mesh = QuantizedMeshParser::parseToSurfaceTileMesh(
                ownHeightmap->rawData.data(),
                ownHeightmap->rawData.size(),
                tile.bounds);
            if (tile.mesh) {
                meshSource = SurfaceDrawableSource::OwnTerrain;
            }
        }

        if (!tile.mesh) {
            tile.mesh = std::make_unique<SurfaceTileMesh>();
            *tile.mesh = TileSurface::buildEllipsoidMesh(
                tile.bounds,
                ownHeightmap ? 64 : 16);
            meshSource = ownHeightmap
                ? SurfaceDrawableSource::OwnTerrain
                : SurfaceDrawableSource::EllipsoidFallback;
            if (ownHeightmap && ownHeightmap->valid()) {
                const auto& ellipsoid = Ellipsoid::WGS84();
                for (auto& vertex : tile.mesh->vertices) {
                    Cartographic cartographic =
                        ellipsoid.cartesianToCartographic(
                            vertex.positionEcef);
                    const double height = static_cast<double>(
                        DecodedHeightmapSampler::sampleHeight(
                            *ownHeightmap,
                            tile.bounds,
                            cartographic.longitude(),
                            cartographic.latitude()));
                    Cartographic terrainCartographic =
                        Cartographic::fromRadians(
                            cartographic.longitude(),
                            cartographic.latitude(),
                            height);
                    vertex.positionEcef =
                        ellipsoid.cartographicToCartesian(
                            terrainCartographic);
                }
            }
        }

        SurfaceMeshResourcePreparer::prepare(tile, input.device);

        TileTerrainHeightRangePolicy::applyMeshOrHeightmapRange(
            tile,
            tile.mesh.get(),
            ownHeightmap);
        TileTerrainHeightRangePolicy::inheritHeightRangeForUnreadyChildren(
            tile);

        tile.meshReady = true;
        tile.surfaceSource = meshSource == SurfaceDrawableSource::None
            ? SurfaceDrawableSource::EllipsoidFallback
            : meshSource;
        tile.contentKind = TileContentKind::Render;
        tile.surfaceDrawable = hasSurfaceDrawable(tile);
        if (hasOwnTerrain || tile.upsampledFromParent ||
            !input.hasTerrainProvider) {
            tile.loadState = TileLoadState::Done;
        }
        tile.completeRenderable = isCompleteRenderable(tile);
        tile.renderable = tile.completeRenderable;

        return TileSurfaceMeshEnsureResult{true};
    }
};

} // namespace earth_engine
