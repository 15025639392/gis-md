#include "TileRasterOverlayDetailsGenerator.h"
#include "../debug/PlatformLog.h"

#include "RasterMappedToTilesetTile.h"
#include "TileBoundingVolume.h"
#include "TileRenderContentState.h"
#include "TilesetTile.h"

#include "../content/GltfModel.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/MathUtils.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

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

void mergeBoundingRegion(
    RasterOverlayDetails& details,
    const BoundingRegionBuilder::BoundingRegion& generatedRegion) {
    const bool hasRegion = details.boundingRegion.minimumHeight <=
                           details.boundingRegion.maximumHeight;
    const bool generatedHasRegion =
        generatedRegion.minimumHeight <= generatedRegion.maximumHeight;
    if (hasRegion && generatedHasRegion) {
        BoundingRegionBuilder builder;
        builder.expandToIncludeRegion(details.boundingRegion);
        builder.expandToIncludeRegion(generatedRegion);
        details.boundingRegion = builder.toRegion();
    } else if (generatedHasRegion) {
        details.boundingRegion = generatedRegion;
    }
}

void applyGeneratedProjectionDetails(
    RasterOverlayDetails& details,
    RasterOverlayProjection projection,
    const Rectangle& projectedRectangle,
    const BoundingRegionBuilder::BoundingRegion& generatedRegion,
    std::optional<size_t> existingIndex) {
    if (existingIndex) {
        while (details.rasterOverlayRectangles.size() <= *existingIndex) {
            details.rasterOverlayRectangles.push_back(Rectangle::EMPTY);
        }
        while (details.rasterOverlayInvertedVCoordinates.size() <=
               *existingIndex) {
            details.rasterOverlayInvertedVCoordinates.push_back(false);
        }
        details.rasterOverlayRectangles[*existingIndex] = projectedRectangle;
        details.rasterOverlayInvertedVCoordinates[*existingIndex] = false;
        mergeBoundingRegion(details, generatedRegion);
        return;
    }

    RasterOverlayDetails generated;
    generated.rasterOverlayProjections = {projection};
    generated.rasterOverlayRectangles = {projectedRectangle};
    generated.rasterOverlayInvertedVCoordinates = {false};
    generated.boundingRegion = generatedRegion;
    details.merge(generated);
}

bool contributesToComputedBounds(const GltfPrimitive& primitive,
                                 size_t vertexIndex) {
    if (!primitive.skirtMetadata) {
        return true;
    }
    const SkirtMetadata& skirt = *primitive.skirtMetadata;
    const size_t begin = skirt.noSkirtVerticesBegin;
    const size_t count = skirt.noSkirtVerticesCount;
    if (count == 0 ||
        begin >= primitive.vertices.size() ||
        count > primitive.vertices.size() - begin) {
        return true;
    }
    return vertexIndex >= begin && vertexIndex < begin + count;
}

Vec3 worldPositionForVertex(const TileRenderContentState& renderContent,
                            const GltfModel& model,
                            const GltfPrimitive& primitive,
                            const SurfaceVertex& vertex) {
    // Committed render-content models store primitive.vertices in WORLD ECEF:
    // GltfModel::rebuildRuntime bakes node.globalTransform into vertices and
    // keeps the local copy in runtime.baseVertices for GPU upload. Applying
    // the node transform here again double-counts the tile origin. Longitude/
    // latitude survive that (the radial direction is unchanged) but heights
    // become ~earth-radius, which poisons upsampled children's bounding
    // volumes and gets deep-zoom tiles frustum/fog-culled (z13+ black screen).
    (void)model;
    (void)primitive;
    return renderContent.gltfTransform().transformPoint(vertex.positionEcef);
}

Vec3 projectPositionForOverlay(const Cartographic& cartographic,
                               RasterOverlayProjection projection) {
    return projectWorldPositionForRasterOverlay(
        cartographic,
        projection);
}

Vec3 projectPositionForOverlayClosestToRectangle(
    const Cartographic& cartographic,
    RasterOverlayProjection projection,
    const Rectangle& rectangle) {
    Vec3 projected = projectPositionForOverlay(cartographic, projection);
    const double longitude = cartographic.longitude();
    if (std::abs(std::abs(longitude) - MathUtils::OnePi) >=
            MathUtils::Epsilon5 ||
        (projected.x() >= rectangle.west() &&
         projected.x() <= rectangle.east() &&
         projected.y() >= rectangle.south() &&
         projected.y() <= rectangle.north())) {
        return projected;
    }

    const double alternateLongitude = longitude < 0.0
        ? longitude + MathUtils::TwoPi
        : longitude - MathUtils::TwoPi;
    const Cartographic alternate = Cartographic::fromRadians(
        alternateLongitude,
        cartographic.latitude(),
        cartographic.height());
    const Vec3 alternateProjected =
        projectPositionForOverlay(alternate, projection);
    const double distance =
        rectangle.computeSignedDistance(projected.x(), projected.y());
    const double alternateDistance = rectangle.computeSignedDistance(
        alternateProjected.x(),
        alternateProjected.y());
    return alternateDistance < distance ? alternateProjected : projected;
}

bool writeGltfOverlayTexCoords(TileRenderContentState& renderContent,
                               RasterOverlayProjection projection,
                               const Rectangle& projectedRectangle,
                               size_t textureCoordinateIndex) {
    auto model = renderContent.editGltfContent();
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
        std::vector<std::array<float, 2>>& texCoords =
            primitive.vertexTexCoords[textureCoordinateIndex];
        texCoords.clear();
        texCoords.reserve(primitive.vertices.size());
        for (const SurfaceVertex& vertex : primitive.vertices) {
            const Vec3 worldPosition =
                worldPositionForVertex(
                    renderContent,
                    *model,
                    primitive,
                    vertex);
            const std::optional<Cartographic> cartographic =
                ellipsoid.tryCartesianToCartographic(worldPosition);
            if (!cartographic) {
                texCoords.push_back({0.0f, 0.0f});
                continue;
            }
            const Vec3 projected = projectPositionForOverlayClosestToRectangle(
                *cartographic,
                projection,
                projectedRectangle);
            texCoords.push_back({
                static_cast<float>(
                    std::clamp(
                        (projected.x() - projectedRectangle.west()) / width,
                        0.0,
                        1.0)),
                // NW 约定（v=0 在北）：与纹理行序（row0=北）及
                // textureWindowForNorthWestUv 的窗口换算一致
                static_cast<float>(
                    std::clamp(
                        (projectedRectangle.north() - projected.y()) /
                            height,
                        0.0,
                        1.0))});
        }
        // These bytes pack every terrain texcoord set. A dynamic overlay can
        // reach this fallback after the worker built them, so force the
        // lifecycle coordinator onto the rebuild path instead of uploading
        // stale UVs.
        primitive.terrainGpuVertexBytes.clear();
    }
    return true;
}

} // namespace

Rectangle TileRasterOverlayDetailsGenerator::projectRegionRectangle(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    const Rectangle splitRectangle =
        rectangle.splitAtAntimeridian().first;
    return projectWorldRectangleForRasterOverlay(
        splitRectangle,
        projection);
}

std::optional<BoundingRegionBuilder::BoundingRegion>
TileRasterOverlayDetailsGenerator::computeTightModelBoundingRegion(
    const TileRenderContentState& renderContent) {
    const GltfModel* model = renderContent.gltfModelForRead();
    if (!model) {
        return std::nullopt;
    }

    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    BoundingRegionBuilder builder;

    // Gather cartographic positions once, tracking the latitude span so the
    // pole tolerance can be set before expanding the bounds. This matches
    // cesium RasterOverlayUtilities.cpp:106-110, which ignores vertices within
    // 1/1000th of a tile height of the North/South pole because longitudes are
    // untrustworthy at extreme latitudes. cesium derives the height from the
    // tile's known rectangle; the model fills the tile, so its latitude span is
    // equivalent. Without this, the default poleTolerance (Epsilon10) never
    // triggers and near-pole tiles get a west/east bound polluted by noise.
    std::vector<Cartographic> positions;
    double minLatitude = std::numeric_limits<double>::max();
    double maxLatitude = std::numeric_limits<double>::lowest();
    for (const GltfPrimitive& primitive : model->primitives) {
        for (size_t i = 0; i < primitive.vertices.size(); ++i) {
            if (!contributesToComputedBounds(primitive, i)) {
                continue;
            }
            const SurfaceVertex& vertex = primitive.vertices[i];
            const Vec3 worldPosition =
                worldPositionForVertex(
                    renderContent,
                    *model,
                    primitive,
                    vertex);
            const std::optional<Cartographic> cartographic =
                ellipsoid.tryCartesianToCartographic(worldPosition);
            if (!cartographic) {
                continue;
            }
            minLatitude = std::min(minLatitude, cartographic->latitude());
            maxLatitude = std::max(maxLatitude, cartographic->latitude());
            positions.push_back(*cartographic);
        }
    }

    if (positions.empty()) {
        return std::nullopt;
    }

    builder.setPoleTolerance(0.001 * (maxLatitude - minLatitude));

    bool hasPosition = false;
    for (const Cartographic& cartographic : positions) {
        hasPosition =
            builder.expandToIncludePosition(cartographic) || hasPosition;
    }

    if (!hasPosition) {
        return std::nullopt;
    }
    return builder.toRegion();
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
    const std::optional<size_t> existingIndex =
        findProjectionIndex(*details, projection);
    if (existingIndex) {
        return false;
    }
    const size_t targetTextureCoordinateIndex =
        details->rasterOverlayProjections.size();
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

    applyGeneratedProjectionDetails(
        *details,
        projection,
        projectedRectangle,
        computeTightModelBoundingRegion(renderContent)
            .value_or(BoundingRegionBuilder::BoundingRegion{
                boundingVolume.region,
                boundingVolume.minimumHeight,
                boundingVolume.maximumHeight}),
        existingIndex);
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
    const std::optional<size_t> existingIndex =
        findProjectionIndex(*details, projection);
    if (existingIndex) {
        return false;
    }

    const std::optional<BoundingRegionBuilder::BoundingRegion> tightRegion =
        computeTightModelBoundingRegion(renderContent);
    if (!tightRegion) {
        return false;
    }

    const size_t targetTextureCoordinateIndex =
        details->rasterOverlayProjections.size();
    const Rectangle projectedRectangle =
        projectRegionRectangle(tightRegion->rectangle, projection);
    if (!writeGltfOverlayTexCoords(
            renderContent,
            projection,
            projectedRectangle,
            targetTextureCoordinateIndex)) {
        return false;
    }

    applyGeneratedProjectionDetails(
        *details,
        projection,
        projectedRectangle,
        *tightRegion,
        existingIndex);
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
        // Normalize overlay texcoords against the actual mesh's projected bounds
        // (cesium computes the overlay rectangle from the loaded vertices). The
        // declared tile bounding-volume region is only a reliable substitute
        // when it matches the mesh; for glTF-terrain-upsampled tiles the region
        // is the coarser ancestor rectangle (a full LOD larger and offset), so
        // using it collapses the texcoord V range and smears imagery into
        // vertical streaks. Prefer model bounds whenever geometry is present and
        // fall back to the declared region only when the model has no vertices.
        const bool generatedDetails =
            ensureProjectionDetailsFromModelBounds(renderContent, projection) ||
            (boundingVolume &&
             boundingVolume->kind == TileBoundingVolumeKind::Region &&
             ensureProjectionDetailsFromRegion(
                 renderContent,
                 *boundingVolume,
                 projection));
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
