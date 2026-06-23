#pragma once

#include "../content/GltfTerrainUpsampler.h"
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
        const bool sourceStateReady =
            parent->content.loadState == TileLoadState::Done ||
            parent->content.loadState == TileLoadState::Unloading;
        return sourceStateReady &&
                parent->content.contentKind == TileContentKind::Render &&
                parent->content.renderContent.isTerrainRenderContent() &&
                parent->content.renderContent.hasGltfContent()
            ? parent
            : nullptr;
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
            chooseUpsampleTextureCoordinate(*parentModel, *source, tile);
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
        content.gltfModel = std::move(childModel);
        content.terrainRenderContent = true;
        content.metadata.rasterOverlayDetails =
            content.gltfModel->rasterOverlayDetails;
        return true;
    }

    static std::optional<TileLoadResult> createLoadResult(
        TilesetTile& tile) {
        TileLoadedContent content;
        if (!materialize(tile, content)) {
            return std::nullopt;
        }

        TileLoadResult result = TileLoadResult::createRenderableGltfTerrain(
            std::move(content.gltfModel),
            std::move(content.metadata),
            content.contentTransform);
        return result;
    }

private:
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
        for (const auto& mapping : source.rasterOverlayState.mappings()) {
            if (!mapping || !mapping->isMoreDetailAvailable()) {
                continue;
            }
            const int candidate = mapping->getTextureCoordinateID();
            if (modelHasTextureCoordinate(model, candidate)) {
                return candidate;
            }
        }

        if (tile.content.isTerrainAvailabilityUpsample()) {
            const int candidate =
                model.rasterOverlayDetails.textureCoordinateIDForProjection(
                    terrainProjectionForTileKey(tile.key));
            return modelHasTextureCoordinate(model, candidate) ? candidate
                                                               : -1;
        }

        if (tile.content.isRasterDetailUpsample()) {
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
