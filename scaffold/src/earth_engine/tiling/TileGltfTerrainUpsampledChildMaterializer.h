#pragma once

#include "../content/GltfTerrainUpsampler.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "TileLoadTypes.h"
#include "TilesetTile.h"

#include <algorithm>
#include <cstddef>

namespace earth_engine {

class TileGltfTerrainUpsampledChildMaterializer {
public:
    static const TilesetTile* findGltfTerrainSource(const TilesetTile& tile) {
        const TilesetTile* ancestor = tile.parent;
        while (ancestor) {
            const bool sourceStateReady =
                ancestor->content.loadState == TileLoadState::Done ||
                ancestor->content.loadState == TileLoadState::Unloading;
            if (sourceStateReady &&
                ancestor->content.contentKind == TileContentKind::Render &&
                ancestor->content.renderContent.isTerrainRenderContent() &&
                ancestor->content.renderContent.hasGltfContent()) {
                return ancestor;
            }
            ancestor = ancestor->parent;
        }
        return nullptr;
    }

    static bool materialize(TilesetTile& tile,
                            TileLoadedContent& content) {
        if (!tile.content.derivesTerrainFromParent() ||
            content.hasTerrainPayload()) {
            return false;
        }

        const TilesetTile* source = findGltfTerrainSource(tile);
        if (!source) {
            return false;
        }

        const GltfModel* parentModel =
            source->content.renderContent.gltfModelForRead();
        if (!parentModel) {
            return false;
        }

        const int textureCoordinateIndex =
            chooseUpsampleTextureCoordinate(*parentModel);
        if (textureCoordinateIndex < 0) {
            return false;
        }

        std::unique_ptr<GltfModel> childModel =
            GltfTerrainUpsampler::upsampleForRasterOverlay(
                *parentModel,
                UpsampledQuadtreeNode{tile.key},
                textureCoordinateIndex,
                false);
        if (!childModel) {
            return false;
        }

        childModel->rasterOverlayDetails = deriveChildRasterOverlayDetails(
            parentModel->rasterOverlayDetails,
            tile.bounds);
        content.gltfModel = std::move(childModel);
        content.terrainPayloadKind = TerrainTilePayloadKind::GltfModel;
        content.metadata.rasterOverlayDetails =
            content.gltfModel->rasterOverlayDetails;
        return true;
    }

private:
    static RasterOverlayDetails deriveChildRasterOverlayDetails(
        const RasterOverlayDetails& parentDetails,
        const Rectangle& childBounds) {
        RasterOverlayDetails childDetails;
        if (parentDetails.empty()) {
            childDetails.setGeographicRectangle(childBounds);
            return childDetails;
        }

        childDetails.rasterOverlayProjections =
            parentDetails.rasterOverlayProjections;
        childDetails.rasterOverlayRectangles.reserve(
            parentDetails.rasterOverlayProjections.size());

        for (size_t i = 0;
             i < parentDetails.rasterOverlayProjections.size();
             ++i) {
            if (i >= parentDetails.rasterOverlayRectangles.size() ||
                parentDetails.rasterOverlayRectangles[i].isEmpty()) {
                childDetails.rasterOverlayRectangles.push_back(
                    Rectangle::EMPTY);
                continue;
            }
            childDetails.rasterOverlayRectangles.push_back(
                projectChildRectangle(
                    parentDetails.rasterOverlayProjections[i],
                    childBounds));
        }
        childDetails.boundingRegion = {childBounds, 0.0, 0.0};
        return childDetails;
    }

    static Rectangle projectChildRectangle(RasterOverlayProjection projection,
                                           const Rectangle& childBounds) {
        switch (projection) {
            case RasterOverlayProjection::Geographic:
                return childBounds;
            case RasterOverlayProjection::WebMercator:
                return projectRectangleSimple(
                    WebMercatorProjection(Ellipsoid::WGS84()),
                    childBounds);
        }
        return childBounds;
    }

    static bool modelHasTextureCoordinate(const GltfModel& model,
                                          int textureCoordinateIndex) {
        if (textureCoordinateIndex < 0 ||
            textureCoordinateIndex >= static_cast<int>(kGltfMaxTexCoordSets)) {
            return false;
        }
        const size_t index = static_cast<size_t>(textureCoordinateIndex);
        for (const GltfPrimitive& primitive : model.primitives) {
            if (!primitive.vertices.empty() &&
                primitive.vertexTexCoords[index].size() ==
                    primitive.vertices.size()) {
                return true;
            }
        }
        return false;
    }

    static int chooseUpsampleTextureCoordinate(const GltfModel& model) {
        const int geographic =
            model.rasterOverlayDetails.textureCoordinateIDForProjection(
                RasterOverlayProjection::Geographic);
        if (modelHasTextureCoordinate(model, geographic)) {
            return geographic;
        }

        for (RasterOverlayProjection projection :
             model.rasterOverlayDetails.rasterOverlayProjections) {
            const int candidate =
                model.rasterOverlayDetails.textureCoordinateIDForProjection(
                    projection);
            if (modelHasTextureCoordinate(model, candidate)) {
                return candidate;
            }
        }

        return modelHasTextureCoordinate(model, 0) ? 0 : -1;
    }
};

} // namespace earth_engine
