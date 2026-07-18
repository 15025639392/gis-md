#include "TileRasterUpsampledChildCoordinator.h"

#include "TileContentAccess.h"
#include "TileContentResourceInvalidator.h"
#include "RasterMappedToTilesetTile.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "TileRasterUpsampledChildMaterializer.h"
#include "TilesetTile.h"
#include "../core/geodesy/Ellipsoid.h"

namespace earth_engine {
namespace {

constexpr double kTerrainMapQuality = 0.25;
constexpr double kTerrainMapWidth = 65.0;

double cesiumTerrainGeometricError(const Rectangle& bounds) {
    const double maxGeometricErrorPerRadian =
        Ellipsoid::WGS84().semiMajorAxis() *
        kTerrainMapQuality /
        kTerrainMapWidth;
    return 8.0 * maxGeometricErrorPerRadian * bounds.width();
}

} // namespace

TileRasterUpsampledChildCoordinator::TileRasterUpsampledChildCoordinator(
    TileContentAccess& contentAccess,
    TileContentResourceInvalidator& resourceInvalidator)
    : contentAccess_(contentAccess),
      resourceInvalidator_(resourceInvalidator) {}

void TileRasterUpsampledChildCoordinator::createRasterOverlayUpsampledChildren(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer,
    bool decoupleImageryFromGeometry) {
    // 北极星 Phase 2a 断耦合:影像 isMoreDetailAvailable 不再驱动几何细分,
    // 几何 cap 在 DEM native max LOD(不捏造上采样地形子瓦片)。近景影像改走
    // scale-bias 祖先复用(暂糊),churn/瓦片数回落到 capped-z12。
    if (decoupleImageryFromGeometry) {
        return;
    }
    const bool changed =
        TileRasterUpsampledChildMaterializer::materialize(
            tile,
            cesiumTerrainGeometricError(tile.bounds),
            [this](const TileKey& key) {
                return contentAccess_.ensureTile(key);
            },
            pPrepRenderer);
    if (changed) {
        for (TilesetTile* child : tile.children) {
            if (child) {
                resourceInvalidator_.markTileResourcesChanged(*child);
            }
        }
    }
}

void TileRasterUpsampledChildCoordinator::markResourcesDirty() {
    resourceInvalidator_.markResourcesChanged();
}

} // namespace earth_engine
