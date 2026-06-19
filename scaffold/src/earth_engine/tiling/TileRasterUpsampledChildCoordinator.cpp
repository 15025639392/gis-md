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
    TilesetTile& tile) {
    const bool changed =
        TileRasterUpsampledChildMaterializer::materialize(
            tile,
            cesiumTerrainGeometricError(tile.bounds),
            [this](const TileKey& key) {
                return contentAccess_.ensureTile(key);
            });
    if (changed) {
        markResourcesDirty();
    }
}

void TileRasterUpsampledChildCoordinator::markResourcesDirty() {
    resourceInvalidator_.markResourcesDirty();
}

} // namespace earth_engine
