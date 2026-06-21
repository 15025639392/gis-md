#include "QuantizedMeshContentLoader.h"

#include "GltfModel.h"
#include "../terrain/QuantizedMeshParser.h"

namespace earth_engine {
namespace {

std::unique_ptr<GltfModel> makeQuantizedMeshGltfModel(
    const SurfaceTileMesh& surfaceMesh) {
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails = surfaceMesh.rasterOverlayDetails;

    GltfPrimitive primitive;
    primitive.vertices = surfaceMesh.vertices;
    primitive.indices = surfaceMesh.indices;
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.doubleSided = false;
    primitive.metallicFactor = 0.0f;
    primitive.roughnessFactor = 1.0f;
    primitive.unlit = false;
    primitive.vertexTexCoords[0].reserve(surfaceMesh.vertices.size());
    for (const SurfaceVertex& vertex : surfaceMesh.vertices) {
        primitive.vertexTexCoords[0].push_back(vertex.uv);
    }
    primitive.runtime.baseVertices = primitive.vertices;
    primitive.runtime.hasNormals = true;

    model->primitives.push_back(std::move(primitive));
    return model;
}

} // namespace

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

    std::unique_ptr<GltfModel> gltfModel =
        makeQuantizedMeshGltfModel(*surfaceMesh);
    if (!gltfModel || gltfModel->primitives.empty()) {
        return result;
    }

    result.status = QuantizedMeshContentLoadStatus::Success;
    if (surfaceMesh->hasHeightRange) {
        result.metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
            tileRectangle,
            surfaceMesh->minimumHeight,
            surfaceMesh->maximumHeight);
    }
    result.metadata.rasterOverlayDetails = surfaceMesh->rasterOverlayDetails;
    result.gltfModel = std::move(gltfModel);
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
