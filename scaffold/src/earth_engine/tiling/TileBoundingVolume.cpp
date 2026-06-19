#include "TileBoundingVolume.h"

#include "TileBoundsMetrics.h"

namespace earth_engine {

std::optional<OrientedBoundingBox>
TileBoundingVolume::toOrientedBoundingBox() const {
    switch (kind) {
        case TileBoundingVolumeKind::Sphere:
            return OrientedBoundingBox::fromSphere(sphere);
        case TileBoundingVolumeKind::Box:
            return box;
        case TileBoundingVolumeKind::Region:
            return TileBoundsMetrics::boundingRegionObb(
                region,
                minimumHeight,
                maximumHeight);
    }
    return std::nullopt;
}

} // namespace earth_engine
