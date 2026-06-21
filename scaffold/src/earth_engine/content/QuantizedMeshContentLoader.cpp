#include "QuantizedMeshContentLoader.h"

#include "../terrain/QuantizedMeshParser.h"

namespace earth_engine {

QuantizedMeshContentLoadResult QuantizedMeshContentLoader::load(
    const uint8_t* data,
    size_t size,
    const Rectangle& tileRectangle,
    bool enableWaterMask,
    const std::vector<QuantizedMeshMetadataContent>& metadata) {
    QuantizedMeshContentLoadResult result;

    std::unique_ptr<SurfaceTileMesh> surfaceMesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            data,
            size,
            tileRectangle,
            enableWaterMask);
    if (!surfaceMesh) {
        return result;
    }

    result.status = QuantizedMeshContentLoadStatus::Success;
    if (surfaceMesh->hasHeightRange) {
        result.updatedBoundingVolume = TileBoundingVolume::fromRegion(
            tileRectangle,
            surfaceMesh->minimumHeight,
            surfaceMesh->maximumHeight);
    }
    result.surfaceMesh = std::move(surfaceMesh);
    result.availabilityUpdates.reserve(metadata.size());
    for (const QuantizedMeshMetadataContent& item : metadata) {
        QuantizedMeshAvailabilityUpdate update;
        update.layerIndex = item.layerIndex;
        update.subtreeKey = item.subtreeKey;
        if (item.data && item.size > 0) {
            update.metadataAvailability =
                QuantizedMeshParser::parseMetadataAvailability(
                    item.data,
                    item.size);
        }
        result.availabilityUpdates.push_back(std::move(update));
    }

    return result;
}

} // namespace earth_engine
