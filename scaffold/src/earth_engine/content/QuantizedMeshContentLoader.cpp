#include "QuantizedMeshContentLoader.h"

#include "GltfModel.h"
#include "../terrain/QuantizedMeshParser.h"

namespace earth_engine {
namespace {

std::unique_ptr<GltfModel> makeQuantizedMeshGltfModel(
    const QuantizedMeshParser::DecodedTile& decodedTile) {
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails = decodedTile.rasterOverlayDetails;
    model->terrainWaterMask = decodedTile.waterMask;
    model->preferredLocalOriginEcef = decodedTile.localOriginEcef;
    if (!decodedTile.waterMask.allLand &&
        !decodedTile.waterMask.allWater &&
        !decodedTile.waterMask.data.empty()) {
        GltfTexture waterMaskTexture;
        waterMaskTexture.image.width = 256;
        waterMaskTexture.image.height = 256;
        waterMaskTexture.image.channels = 1;
        waterMaskTexture.image.pixels.reserve(256u * 256u);
        for (size_t i = 0; i < 256u * 256u; ++i) {
            waterMaskTexture.image.pixels.push_back(
                decodedTile.waterMask.data[i * 4u + 3u]);
        }
        waterMaskTexture.sampler.minFilter = GltfTextureFilter::Linear;
        waterMaskTexture.sampler.magFilter = GltfTextureFilter::Linear;
        waterMaskTexture.sampler.mipmap = false;
        waterMaskTexture.sampler.wrapS = GltfTextureWrap::ClampToEdge;
        waterMaskTexture.sampler.wrapT = GltfTextureWrap::ClampToEdge;
        model->terrainWaterMaskTextureIndex = model->textures.size();
        model->textures.push_back(std::move(waterMaskTexture));
    }

    GltfPrimitive primitive;
    primitive.vertices = decodedTile.vertices;
    primitive.indices = decodedTile.indices;
    primitive.skirtMetadata = decodedTile.skirtMetadata;
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.doubleSided = false;
    primitive.metallicFactor = 0.0f;
    primitive.roughnessFactor = 1.0f;
    primitive.unlit = false;
    primitive.vertexTexCoords[0].reserve(decodedTile.vertices.size());
    for (const SurfaceVertex& vertex : decodedTile.vertices) {
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

    std::unique_ptr<QuantizedMeshParser::DecodedTile> decodedTile =
        QuantizedMeshParser::parseToDecodedTile(
            data,
            size,
            tileRectangle,
            enableWaterMask);
    if (!decodedTile) {
        return result;
    }

    std::unique_ptr<GltfModel> gltfModel =
        makeQuantizedMeshGltfModel(*decodedTile);
    if (!gltfModel || gltfModel->primitives.empty()) {
        return result;
    }

    result.status = QuantizedMeshContentLoadStatus::Success;
    result.metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
        tileRectangle,
        decodedTile->minimumHeight,
        decodedTile->maximumHeight);
    result.metadata.terrainHeightRange = {
        decodedTile->minimumHeight,
        decodedTile->maximumHeight};
    result.metadata.horizonOcclusionPoint =
        decodedTile->horizonOcclusionPoint;
    result.metadata.rasterOverlayDetails = decodedTile->rasterOverlayDetails;
    result.gltfModel = std::move(gltfModel);
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
