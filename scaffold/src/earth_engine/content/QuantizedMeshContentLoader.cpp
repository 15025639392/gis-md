#include "QuantizedMeshContentLoader.h"

#include "GltfModel.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/math/MathUtils.h"
#include "../terrain/QuantizedMeshParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace earth_engine {
namespace {

std::pair<Vec3, Vec3> splitHighLow(const Vec3& value) {
    constexpr double kSplit = 65536.0;
    const auto split = [](double v) {
        const double high = std::floor(v / kSplit) * kSplit;
        return std::pair<double, double>{high, v - high};
    };
    const auto sx = split(value.x());
    const auto sy = split(value.y());
    const auto sz = split(value.z());
    return {
        Vec3(sx.first, sy.first, sz.first),
        Vec3(sx.second, sy.second, sz.second)};
}

void setLocalPosition(SurfaceVertex& vertex, const Vec3& localPosition) {
    vertex.positionEcef = localPosition;
    const auto split = splitHighLow(localPosition);
    vertex.positionHighEcef = split.first;
    vertex.positionLowEcef = split.second;
}

std::unique_ptr<GltfModel> makeQuantizedMeshGltfModel(
    const QuantizedMeshParser::DecodedTile& decodedTile) {
    auto model = std::make_unique<GltfModel>();
    model->rasterOverlayDetails = decodedTile.rasterOverlayDetails;
    model->terrainWaterMask = decodedTile.waterMask;
    model->preferredLocalOriginEcef = decodedTile.localOriginEcef;

    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(decodedTile.localOriginEcef);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.baseTranslation = {
        decodedTile.localOriginEcef.x(),
        decodedTile.localOriginEcef.y(),
        decodedTile.localOriginEcef.z()};
    rootNode.translation = rootNode.baseTranslation;
    rootNode.mesh = 0;
    rootNode.hasMatrix = false;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);

    if (!decodedTile.waterMask.allLand &&
        !decodedTile.waterMask.allWater &&
        !decodedTile.waterMask.data.empty()) {
        constexpr size_t waterMaskPixelCount = 256u * 256u;
        GltfTexture waterMaskTexture;
        waterMaskTexture.image.width = 256;
        waterMaskTexture.image.height = 256;
        waterMaskTexture.image.channels = 1;
        if (decodedTile.waterMask.data.size() >= waterMaskPixelCount * 4u) {
            waterMaskTexture.image.pixels.reserve(waterMaskPixelCount);
            for (size_t i = 0; i < waterMaskPixelCount; ++i) {
                waterMaskTexture.image.pixels.push_back(
                    decodedTile.waterMask.data[i * 4u + 3u]);
            }
        } else if (decodedTile.waterMask.data.size() >= waterMaskPixelCount) {
            waterMaskTexture.image.pixels.assign(
                decodedTile.waterMask.data.begin(),
                decodedTile.waterMask.data.begin() + waterMaskPixelCount);
        }
        if (waterMaskTexture.image.pixels.size() != waterMaskPixelCount) {
            return nullptr;
        }
        waterMaskTexture.sampler.minFilter = GltfTextureFilter::Linear;
        waterMaskTexture.sampler.magFilter = GltfTextureFilter::Linear;
        waterMaskTexture.sampler.mipmap = true;
        waterMaskTexture.sampler.wrapS = GltfTextureWrap::ClampToEdge;
        waterMaskTexture.sampler.wrapT = GltfTextureWrap::ClampToEdge;
        model->terrainWaterMaskTextureIndex = model->textures.size();
        model->textures.push_back(std::move(waterMaskTexture));
    }

    GltfPrimitive primitive;
    primitive.vertices = decodedTile.vertices;
    primitive.indices = decodedTile.indices;
    primitive.skirtMetadata = decodedTile.skirtMetadata;
    primitive.hasTerrainWaterMaskMetadata = true;
    primitive.terrainOnlyWater = decodedTile.waterMask.allWater;
    primitive.terrainOnlyLand = decodedTile.waterMask.allLand;
    primitive.terrainWaterMaskTextureIndex =
        model->terrainWaterMaskTextureIndex;
    primitive.terrainWaterMaskTranslationX =
        decodedTile.waterMask.translationX;
    primitive.terrainWaterMaskTranslationY =
        decodedTile.waterMask.translationY;
    primitive.terrainWaterMaskScale = decodedTile.waterMask.scale;
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.doubleSided = false;
    primitive.metallicFactor = 0.0f;
    primitive.roughnessFactor = 1.0f;
    primitive.unlit = false;
    primitive.vertexTexCoords[0].reserve(decodedTile.vertices.size());
    for (const SurfaceVertex& vertex : decodedTile.vertices) {
        primitive.vertexTexCoords[0].push_back(vertex.uv);
    }
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    for (SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        setLocalPosition(
            vertex,
            vertex.positionEcef - decodedTile.localOriginEcef);
    }
    primitive.vertices = primitive.runtime.baseVertices;
    primitive.runtime.hasNormals = true;

    model->primitives.push_back(std::move(primitive));
    if (!model->rebuildRuntime()) {
        return nullptr;
    }
    return model;
}

RasterOverlayDetails makeRasterOverlayDetails(
    const Rectangle& geographicRectangle,
    double minimumHeight,
    double maximumHeight,
    RasterOverlayProjection terrainProjection) {
    RasterOverlayDetails details;
    details.rasterOverlayProjections = {terrainProjection};
    details.rasterOverlayInvertedVCoordinates = {false};
    if (terrainProjection == RasterOverlayProjection::WebMercator) {
        details.rasterOverlayRectangles = {
            projectRectangleSimple(
                WebMercatorProjection(Ellipsoid::WGS84()),
                geographicRectangle)};
    } else {
        details.rasterOverlayRectangles = {geographicRectangle};
    }
    details.boundingRegion = {
        geographicRectangle,
        minimumHeight,
        maximumHeight};
    return details;
}

void rewriteTerrainProjectionTexCoords(GltfModel& model,
                                        RasterOverlayProjection projection) {
    if (projection != RasterOverlayProjection::WebMercator) {
        return;
    }

    const Rectangle* projectedRectangle =
        model.rasterOverlayDetails.findRectangleForOverlayProjection(
            projection);
    if (!projectedRectangle || projectedRectangle->isEmpty()) {
        return;
    }

    const double width = projectedRectangle->width();
    const double height = projectedRectangle->height();
    if (std::abs(width) <= 0.0 || std::abs(height) <= 0.0) {
        return;
    }

    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const WebMercatorProjection webMercator(ellipsoid);
    for (GltfPrimitive& primitive : model.primitives) {
        const Mat4* nodeTransform = nullptr;
        if (primitive.runtime.nodeIndex >= 0 &&
            static_cast<size_t>(primitive.runtime.nodeIndex) <
                model.nodes.size()) {
            nodeTransform =
                &model.nodes[static_cast<size_t>(primitive.runtime.nodeIndex)]
                     .globalTransform;
        }
        std::vector<std::array<float, 2>>& texCoords =
            primitive.vertexTexCoords[0];
        if (texCoords.size() != primitive.vertices.size()) {
            texCoords.resize(primitive.vertices.size());
        }
        for (size_t i = 0; i < primitive.vertices.size(); ++i) {
            const Vec3 worldPosition = nodeTransform
                ? nodeTransform->transformPoint(
                      primitive.vertices[i].positionEcef)
                : primitive.vertices[i].positionEcef;
            const std::optional<Cartographic> cartographic =
                ellipsoid.tryCartesianToCartographic(
                    worldPosition);
            if (!cartographic) {
                texCoords[i] = primitive.vertices[i].uv;
                continue;
            }
            const Vec3 projected =
                projectPosition(webMercator, *cartographic);
            Vec3 adjustedProjected = projected;
            double longitude = cartographic->longitude();
            if (std::abs(std::abs(longitude) - MathUtils::OnePi) <
                    MathUtils::Epsilon5 &&
                (adjustedProjected.x() < projectedRectangle->west() ||
                 adjustedProjected.x() > projectedRectangle->east() ||
                 adjustedProjected.y() < projectedRectangle->south() ||
                 adjustedProjected.y() > projectedRectangle->north())) {
                const double testLongitude = longitude < 0.0
                    ? longitude + MathUtils::TwoPi
                    : longitude - MathUtils::TwoPi;
                const Vec3 alternateProjected = projectPosition(
                    webMercator,
                    Cartographic::fromRadians(
                        testLongitude,
                        cartographic->latitude(),
                        cartographic->height()));
                const double distance = projectedRectangle
                    ->computeSignedDistance(
                        adjustedProjected.x(),
                        adjustedProjected.y());
                const double alternateDistance = projectedRectangle
                    ->computeSignedDistance(
                        alternateProjected.x(),
                        alternateProjected.y());
                if (alternateDistance < distance) {
                    adjustedProjected = alternateProjected;
                }
            }
            texCoords[i] = {
                static_cast<float>(
                    std::clamp(
                        (adjustedProjected.x() -
                         projectedRectangle->west()) / width,
                        0.0,
                        1.0)),
                static_cast<float>(
                    std::clamp(
                        (adjustedProjected.y() -
                         projectedRectangle->south()) /
                            height,
                        0.0,
                        1.0))};
        }
    }
}

} // namespace

QuantizedMeshContentLoadResult QuantizedMeshContentLoader::load(
    const uint8_t* data,
    size_t size,
    const Rectangle& tileRectangle,
    bool enableWaterMask,
    const std::vector<QuantizedMeshMetadataContent>& metadata,
    RasterOverlayProjection terrainProjection,
    std::optional<QuantizedMeshAvailabilityUpdate>
        currentTileAvailabilityUpdate) {
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

    RasterOverlayDetails rasterOverlayDetails = makeRasterOverlayDetails(
        tileRectangle,
        decodedTile->minimumHeight,
        decodedTile->maximumHeight,
        terrainProjection);
    gltfModel->rasterOverlayDetails = rasterOverlayDetails;
    rewriteTerrainProjectionTexCoords(*gltfModel, terrainProjection);

    result.status = QuantizedMeshContentLoadStatus::Success;
    result.diagnostics = decodedTile->diagnostics;
    result.metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
        tileRectangle,
        decodedTile->minimumHeight,
        decodedTile->maximumHeight);
    result.metadata.terrainHeightRange = {
        decodedTile->minimumHeight,
        decodedTile->maximumHeight};
    result.metadata.horizonOcclusionPoint =
        decodedTile->horizonOcclusionPoint;
    result.metadata.rasterOverlayDetails = std::move(rasterOverlayDetails);
    result.gltfModel = std::move(gltfModel);
    result.availabilityUpdates.reserve(
        metadata.size() + (currentTileAvailabilityUpdate ? 1u : 0u));
    if (currentTileAvailabilityUpdate) {
        if (decodedTile->hasMetadataAvailability) {
            currentTileAvailabilityUpdate->metadataAvailability =
                decodedTile->metadataAvailability;
        }
        result.availabilityUpdates.push_back(
            std::move(*currentTileAvailabilityUpdate));
    }
    for (const QuantizedMeshMetadataContent& item : metadata) {
        QuantizedMeshAvailabilityUpdate update;
        update.layerIndex = item.layerIndex;
        update.subtreeKey = item.subtreeKey;
        if (item.data && item.size > 0) {
            QuantizedMeshParser::MetadataAvailabilityParseResult metadataResult =
                QuantizedMeshParser::parseMetadataAvailabilityWithDiagnostics(
                    item.data,
                    item.size);
            update.metadataAvailability = std::move(metadataResult.availability);
            result.diagnostics.insert(
                result.diagnostics.end(),
                metadataResult.diagnostics.begin(),
                metadataResult.diagnostics.end());
        }
        result.availabilityUpdates.push_back(std::move(update));
    }

    return result;
}

TileContentLoadResult QuantizedMeshContentLoader::toTileContentLoadResult(
    QuantizedMeshContentLoadResult&& result) {
    if (!result.success()) {
        return TileContentLoadResult::failed();
    }

    TileContentLoadResult contentResult =
        TileContentLoadResult::renderTerrain(
            std::move(result.gltfModel),
            std::move(result.metadata));
    contentResult.quantizedMeshAvailabilityUpdates =
        std::move(result.availabilityUpdates);
    return contentResult;
}

TileContentLoadResult QuantizedMeshContentLoader::loadTileContent(
    const uint8_t* data,
    size_t size,
    const Rectangle& tileRectangle,
    bool enableWaterMask,
    const std::vector<QuantizedMeshMetadataContent>& metadata,
    RasterOverlayProjection terrainProjection,
    std::optional<QuantizedMeshAvailabilityUpdate>
        currentTileAvailabilityUpdate) {
    return toTileContentLoadResult(
        load(data,
             size,
             tileRectangle,
             enableWaterMask,
             metadata,
             terrainProjection,
             std::move(currentTileAvailabilityUpdate)));
}

} // namespace earth_engine
