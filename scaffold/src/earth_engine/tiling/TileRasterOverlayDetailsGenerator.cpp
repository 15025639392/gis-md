#include "TileRasterOverlayDetailsGenerator.h"

#include "RasterMappedToTilesetTile.h"
#include "TileBoundingVolume.h"
#include "TileRenderContentState.h"
#include "TilesetTile.h"

#include "../content/GltfModel.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace earth_engine {
namespace {

std::optional<size_t> findProjectionIndex(
    const RasterOverlayDetails& details,
    RasterOverlayProjection projection) {
    for (size_t i = 0; i < details.rasterOverlayProjections.size(); ++i) {
        if (details.rasterOverlayProjections[i] == projection) {
            return i;
        }
    }
    return std::nullopt;
}

bool hasProjectionRectangle(const RasterOverlayDetails& details,
                            size_t index) {
    return index < details.rasterOverlayRectangles.size() &&
           !details.rasterOverlayRectangles[index].isEmpty();
}

Vec3 projectPositionForOverlay(const Cartographic& cartographic,
                               RasterOverlayProjection projection) {
    switch (projection) {
        case RasterOverlayProjection::Geographic:
            return Vec3(
                cartographic.longitude(),
                cartographic.latitude(),
                cartographic.height());
        case RasterOverlayProjection::WebMercator:
            return projectPosition(
                WebMercatorProjection(Ellipsoid::WGS84()),
                cartographic);
    }
    return Vec3(
        cartographic.longitude(),
        cartographic.latitude(),
        cartographic.height());
}

bool writeGltfOverlayTexCoords(TileRenderContentState& renderContent,
                               RasterOverlayProjection projection,
                               const Rectangle& projectedRectangle,
                               size_t textureCoordinateIndex) {
    GltfModel* model = renderContent.gltfContent();
    if (!model ||
        textureCoordinateIndex >= kGltfMaxTexCoordSets ||
        projectedRectangle.isEmpty()) {
        return false;
    }

    const double width = projectedRectangle.width();
    const double height = projectedRectangle.height();
    if (std::abs(width) <= 0.0 || std::abs(height) <= 0.0) {
        return false;
    }

    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    for (GltfPrimitive& primitive : model->primitives) {
        const Mat4* nodeTransform = nullptr;
        if (primitive.runtime.nodeIndex >= 0 &&
            static_cast<size_t>(primitive.runtime.nodeIndex) <
                model->nodes.size()) {
            nodeTransform =
                &model->nodes[static_cast<size_t>(primitive.runtime.nodeIndex)]
                     .globalTransform;
        }
        std::vector<std::array<float, 2>>& texCoords =
            primitive.vertexTexCoords[textureCoordinateIndex];
        texCoords.clear();
        texCoords.reserve(primitive.vertices.size());
        for (const SurfaceVertex& vertex : primitive.vertices) {
            const Vec3 worldPosition = nodeTransform
                ? nodeTransform->transformPoint(vertex.positionEcef)
                : vertex.positionEcef;
            const std::optional<Cartographic> cartographic =
                ellipsoid.tryCartesianToCartographic(worldPosition);
            if (!cartographic) {
                texCoords.push_back({0.0f, 0.0f});
                continue;
            }
            const Vec3 projected =
                projectPositionForOverlay(*cartographic, projection);
            texCoords.push_back({
                static_cast<float>(
                    std::clamp(
                        (projected.x() - projectedRectangle.west()) / width,
                        0.0,
                        1.0)),
                static_cast<float>(
                    std::clamp(
                        (projected.y() - projectedRectangle.south()) /
                            height,
                        0.0,
                        1.0))});
        }
    }
    return true;
}

std::optional<BoundingRegionBuilder::BoundingRegion>
computeTightModelBoundingRegion(const TileRenderContentState& renderContent) {
    const GltfModel* model = renderContent.gltfModelForRead();
    if (!model) {
        return std::nullopt;
    }

    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    bool hasPosition = false;
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;
    double minimumHeight = 0.0;
    double maximumHeight = 0.0;
    for (const GltfPrimitive& primitive : model->primitives) {
        const Mat4* nodeTransform = nullptr;
        if (primitive.runtime.nodeIndex >= 0 &&
            static_cast<size_t>(primitive.runtime.nodeIndex) <
                model->nodes.size()) {
            nodeTransform =
                &model->nodes[static_cast<size_t>(primitive.runtime.nodeIndex)]
                     .globalTransform;
        }
        for (const SurfaceVertex& vertex : primitive.vertices) {
            const Vec3 worldPosition = nodeTransform
                ? nodeTransform->transformPoint(vertex.positionEcef)
                : vertex.positionEcef;
            const std::optional<Cartographic> cartographic =
                ellipsoid.tryCartesianToCartographic(worldPosition);
            if (!cartographic) {
                continue;
            }
            if (!hasPosition) {
                west = east = cartographic->longitude();
                south = north = cartographic->latitude();
                minimumHeight = maximumHeight = cartographic->height();
                hasPosition = true;
                continue;
            }
            west = std::min(west, cartographic->longitude());
            south = std::min(south, cartographic->latitude());
            east = std::max(east, cartographic->longitude());
            north = std::max(north, cartographic->latitude());
            minimumHeight = std::min(minimumHeight, cartographic->height());
            maximumHeight = std::max(maximumHeight, cartographic->height());
        }
    }

    if (!hasPosition) {
        return std::nullopt;
    }
    return BoundingRegionBuilder::BoundingRegion{
        Rectangle(west, south, east, north),
        minimumHeight,
        maximumHeight};
}

} // namespace

Rectangle TileRasterOverlayDetailsGenerator::projectRegionRectangle(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    switch (projection) {
        case RasterOverlayProjection::Geographic:
            return rectangle;
        case RasterOverlayProjection::WebMercator:
            return projectRectangleSimple(
                WebMercatorProjection(Ellipsoid::WGS84()),
                rectangle);
    }
    return rectangle;
}

std::optional<Rectangle>
TileRasterOverlayDetailsGenerator::projectEffectiveContentBoundingVolumeRectangle(
    const TilesetTile& tile,
    RasterOverlayProjection projection) {
    const TileBoundingVolume* effectiveContentBoundingVolume =
        tile.contentBoundingVolume
            ? &*tile.contentBoundingVolume
            : (tile.boundingVolume ? &*tile.boundingVolume : nullptr);
    if (!effectiveContentBoundingVolume ||
        effectiveContentBoundingVolume->kind != TileBoundingVolumeKind::Region) {
        return std::nullopt;
    }
    return projectRegionRectangle(
        effectiveContentBoundingVolume->region,
        projection);
}

bool TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromRegion(
    TileRenderContentState& renderContent,
    const TileBoundingVolume& boundingVolume,
    RasterOverlayProjection projection) {
    RasterOverlayDetails* details =
        renderContent.mutableRasterOverlayDetails();
    if (!details) {
        return false;
    }
    const size_t textureCoordinateIndex =
        details->rasterOverlayProjections.size();
    const std::optional<size_t> existingIndex =
        findProjectionIndex(*details, projection);
    if (existingIndex && hasProjectionRectangle(*details, *existingIndex)) {
        return false;
    }
    const size_t targetTextureCoordinateIndex =
        existingIndex.value_or(textureCoordinateIndex);
    if (boundingVolume.kind != TileBoundingVolumeKind::Region) {
        return false;
    }

    const Rectangle projectedRectangle =
        projectRegionRectangle(boundingVolume.region, projection);
    if (!writeGltfOverlayTexCoords(
            renderContent,
            projection,
            projectedRectangle,
            targetTextureCoordinateIndex)) {
        return false;
    }

    RasterOverlayDetails generated;
    generated.rasterOverlayProjections = {projection};
    generated.rasterOverlayRectangles = {projectedRectangle};
    generated.rasterOverlayInvertedVCoordinates = {false};
    generated.boundingRegion =
        computeTightModelBoundingRegion(renderContent)
            .value_or(BoundingRegionBuilder::BoundingRegion{
                boundingVolume.region,
                boundingVolume.minimumHeight,
                boundingVolume.maximumHeight});
    details->merge(generated);
    return true;
}

bool TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromModelBounds(
    TileRenderContentState& renderContent,
    RasterOverlayProjection projection) {
    RasterOverlayDetails* details =
        renderContent.mutableRasterOverlayDetails();
    if (!details) {
        return false;
    }
    const size_t textureCoordinateIndex =
        details->rasterOverlayProjections.size();
    const std::optional<size_t> existingIndex =
        findProjectionIndex(*details, projection);
    if (existingIndex && hasProjectionRectangle(*details, *existingIndex)) {
        return false;
    }

    const std::optional<BoundingRegionBuilder::BoundingRegion> tightRegion =
        computeTightModelBoundingRegion(renderContent);
    if (!tightRegion) {
        return false;
    }

    const size_t targetTextureCoordinateIndex =
        existingIndex.value_or(textureCoordinateIndex);
    const Rectangle projectedRectangle =
        projectRegionRectangle(tightRegion->rectangle, projection);
    if (!writeGltfOverlayTexCoords(
            renderContent,
            projection,
            projectedRectangle,
            targetTextureCoordinateIndex)) {
        return false;
    }

    RasterOverlayDetails generated;
    generated.rasterOverlayProjections = {projection};
    generated.rasterOverlayRectangles = {projectedRectangle};
    generated.rasterOverlayInvertedVCoordinates = {false};
    generated.boundingRegion = *tightRegion;
    details->merge(generated);
    return true;
}

int TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromActiveOverlays(
    TileRenderContentState& renderContent,
    const TileBoundingVolume* boundingVolume,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device) {
    int generated = 0;
    for (ActivatedRasterOverlay* activeOverlay : rasterOverlays) {
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        RasterOverlayTileProvider* provider =
            activeOverlay->ensureTileProvider(device);
        if (!provider || !provider->isReady()) {
            continue;
        }
        const RasterOverlayProjection projection = provider->getProjection();
        const bool generatedDetails =
            boundingVolume &&
                boundingVolume->kind == TileBoundingVolumeKind::Region
            ? ensureProjectionDetailsFromRegion(
                  renderContent,
                  *boundingVolume,
                  projection)
            : ensureProjectionDetailsFromModelBounds(
                  renderContent,
                  projection);
        if (generatedDetails) {
            ++generated;
        }
    }
    return generated;
}

int TileRasterOverlayDetailsGenerator::ensureProjectionDetailsFromActiveOverlays(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device) {
    const TileBoundingVolume* effectiveContentBoundingVolume =
        tile.contentBoundingVolume
            ? &*tile.contentBoundingVolume
            : (tile.boundingVolume ? &*tile.boundingVolume : nullptr);
    return ensureProjectionDetailsFromActiveOverlays(
        tile.content.renderContent,
        effectiveContentBoundingVolume,
        rasterOverlays,
        device);
}

} // namespace earth_engine
