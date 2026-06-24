#pragma once

#include "../content/GltfTerrainUpsampler.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "RasterMappedToTilesetTile.h"
#include "TileRasterOverlayDetailsDeriver.h"
#include "TileLoadTypes.h"
#include "TilesetTile.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

namespace earth_engine {

class TileGltfTerrainUpsampledChildMaterializer {
public:
    static const TilesetTile* findGltfTerrainSource(const TilesetTile& tile) {
        const TilesetTile* parent = tile.parent;
        if (!parent) {
            return nullptr;
        }
        return isCommittedGltfTerrainSource(tile, *parent)
            ? parent
            : nullptr;
    }

    static bool isCommittedGltfTerrainSource(const TilesetTile& tile) {
        return tile.hasCommittedRenderContent() &&
               tile.content.renderContent.isTerrainRenderContent() &&
               tile.content.renderContent.hasGltfContent();
    }

    static std::optional<TileLoadResult> createLoadResult(
        TilesetTile& tile) {
        std::unique_ptr<GltfModel> childModel = createUpsampledModel(tile);
        if (!childModel) {
            return std::nullopt;
        }

        TileLoadResultMetadata metadata;
        const auto& region = childModel->rasterOverlayDetails.boundingRegion;
        metadata.rasterOverlayDetails = childModel->rasterOverlayDetails;
        metadata.terrainHeightRange = {
            region.minimumHeight,
            region.maximumHeight};
        return TileLoadResult::createRenderableGltfTerrain(
            std::move(childModel),
            std::move(metadata));
    }

    static bool canCreateLoadResult(const TilesetTile& tile) {
        if (!tile.content.derivesTerrainFromParent()) {
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

        return chooseUpsampleTextureCoordinate(*parentModel, *source, tile) >=
               0;
    }

private:
    static bool isCommittedGltfTerrainSource(
        const TilesetTile& child,
        const TilesetTile& source) {
        const bool sourceStateReady =
            child.content.isRasterDetailUpsample()
                ? source.hasCommittedRenderContent()
                : source.content.loadState == TileLoadState::Done;
        return sourceStateReady &&
               source.content.renderContent.isTerrainRenderContent() &&
               source.content.renderContent.hasGltfContent();
    }

    static std::unique_ptr<GltfModel> createUpsampledModel(
        TilesetTile& tile) {
        if (!tile.content.derivesTerrainFromParent()) {
            return nullptr;
        }

        const TilesetTile* source = findGltfTerrainSource(tile);
        if (!source) {
            return nullptr;
        }

        const GltfModel* parentModel =
            source->content.renderContent.gltfModelForRead();
        if (!parentModel) {
            return nullptr;
        }

        const int textureCoordinateIndex =
            chooseUpsampleTextureCoordinate(*parentModel, *source, tile);
        if (textureCoordinateIndex < 0) {
            return nullptr;
        }
        const bool hasInvertedVCoordinate =
            parentModel->rasterOverlayDetails
                .rasterOverlayInvertedVCoordinates.size() >
                    static_cast<size_t>(textureCoordinateIndex) &&
            parentModel->rasterOverlayDetails
                .rasterOverlayInvertedVCoordinates[
                    static_cast<size_t>(textureCoordinateIndex)];

        std::unique_ptr<GltfModel> childModel =
            GltfTerrainUpsampler::upsampleForRasterOverlay(
                *parentModel,
                UpsampledQuadtreeNode{tile.key},
                textureCoordinateIndex,
                hasInvertedVCoordinate);
        if (!childModel) {
            return nullptr;
        }
        const double minimumHeight =
            parentModel->rasterOverlayDetails.boundingRegion.minimumHeight;
        const double maximumHeight =
            parentModel->rasterOverlayDetails.boundingRegion.maximumHeight;
        const Rectangle parentRasterBounds =
            parentModel->rasterOverlayDetails.boundingRegion.rectangle.isEmpty()
            ? source->bounds
            : parentModel->rasterOverlayDetails.boundingRegion.rectangle;
        childModel->rasterOverlayDetails =
            TileRasterOverlayDetailsDeriver::deriveChildFromParent(
            parentModel->rasterOverlayDetails,
            parentRasterBounds,
            tile.bounds,
            minimumHeight,
            maximumHeight);
        return childModel;
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

    static int chooseUpsampleTextureCoordinate(const GltfModel& model,
                                               const TilesetTile& source,
                                               const TilesetTile& tile) {
        if (tile.content.isTerrainAvailabilityUpsample()) {
            const int candidate =
                model.rasterOverlayDetails.textureCoordinateIDForProjection(
                    terrainProjectionForTileKey(tile.key));
            return modelHasTextureCoordinate(model, candidate) ? candidate
                                                               : -1;
        }

        if (tile.content.isRasterDetailUpsample()) {
            if (tile.content.rasterDetailSourceProjection) {
                const int candidate =
                    model.rasterOverlayDetails.textureCoordinateIDForProjection(
                        *tile.content.rasterDetailSourceProjection);
                return modelHasTextureCoordinate(model, candidate)
                    ? candidate
                    : -1;
            }

            for (const auto& mapping : source.rasterOverlayState.mappings()) {
                if (!mapping || !mapping->isMoreDetailAvailable()) {
                    continue;
                }
                const RasterOverlayTile* readyTile = mapping->getReadyTile();
                if (!readyTile) {
                    continue;
                }
                const RasterOverlayProjection projection =
                    readyTile->getTileProvider().getProjection();
                const int detailsTextureCoordinate =
                    model.rasterOverlayDetails.textureCoordinateIDForProjection(
                        projection);
                if (modelHasTextureCoordinate(
                        model,
                        detailsTextureCoordinate)) {
                    return detailsTextureCoordinate;
                }
            }
            return -1;
        }

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

        return -1;
    }

    static RasterOverlayProjection terrainProjectionForTileKey(
        const TileKey& key) {
        return key.schemeId.find("WebMercator") != std::string::npos
            ? RasterOverlayProjection::WebMercator
            : RasterOverlayProjection::Geographic;
    }
};

} // namespace earth_engine
