#include "SurfaceTileDrawCommandBuilder.h"

#include "RasterMappedToTilesetTile.h"
#include "SurfaceRasterBinding.h"
#include "SurfaceTile.h"
#include "TilesetTile.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../providers/RasterOverlayTile.h"
#include "../renderer/Renderer.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace earth_engine {
namespace {

int rasterTextureSourceZoom(const RasterOverlayTile* tile) {
    if (!tile) return -1;
    return tile->isMappedRasterTile() ? tile->getMappedSourceZoom() : tile->getTileID().z;
}

bool overlayBindingAllowedByPolicy(
    const ActivatedRasterOverlay* activeOverlay,
    const RasterMappedToTilesetTile* mapped,
    const SurfaceRasterBinding& binding) {
    return rasterOverlayBindingAllowedByPolicy(
        activeOverlay,
        mapped,
        binding);
}

} // namespace

bool SurfaceTileDrawCommandBuilder::hasDrawableBaseRaster(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays) {
    for (size_t i = 0;
         i < overlays.size() && i < tile.rasterOverlayState.mappingCount();
         ++i) {
        auto* activeOverlay = overlays[i];
        if (!activeOverlay || !activeOverlay->visible() ||
            activeOverlay->getOverlay().role() != RasterOverlayRole::BaseImagery) {
            continue;
        }

        if (!tile.rasterOverlayState.hasDrawableReadyMapping(i)) {
            continue;
        }
        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        const SurfaceRasterBinding binding =
            chooseSurfaceRasterBinding(mapped);
        if (overlayBindingAllowedByPolicy(activeOverlay, mapped, binding)) {
            return true;
        }
    }
    return false;
}

void SurfaceTileDrawCommandBuilder::build(
    Renderer& renderer,
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays,
    RenderCommandList& commands,
    const SurfaceTileDrawCommandBuildContext& context) {
    Texture* baseTexture = nullptr;
    SurfaceRasterBinding baseBinding;
    std::optional<size_t> baseOverlayIndex;

    for (size_t i = 0;
         i < overlays.size() && i < tile.rasterOverlayState.mappingCount();
         ++i) {
        auto* activeOverlay = overlays[i];
        if (!activeOverlay || !activeOverlay->visible() ||
            activeOverlay->getOverlay().role() != RasterOverlayRole::BaseImagery) {
            continue;
        }

        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        const SurfaceRasterBinding binding =
            chooseSurfaceRasterBinding(mapped);
        if (overlayBindingAllowedByPolicy(activeOverlay, mapped, binding)) {
            baseTexture = binding.tile->getTexture();
            baseBinding = binding;
            baseOverlayIndex = i;
            break;
        }
    }

    const SurfaceTileMesh* mesh = tile.content.renderContent.surfaceMesh();
    if (!mesh) {
        return;
    }
    const bool explicitMesh = !mesh->vertices.empty();
    if (explicitMesh &&
        (mesh->indices.empty() ||
         !tile.content.renderContent.surfaceIndexBuffer())) {
        return;
    }
    if (!baseTexture) {
        baseTexture = renderer.surfacePlaceholderTexture();
    }
    if (!baseTexture) {
        return;
    }

    const int meshIndexCount = static_cast<int>(mesh->indices.size());
    int surfaceIndexOffset = 0;
    int surfaceIndexCount = meshIndexCount;
    const SkirtMetadata& skirt = mesh->skirtMeta;
    if (skirt.noSkirtIndicesCount > 0 &&
        skirt.noSkirtIndicesBegin < mesh->indices.size() &&
        skirt.noSkirtIndicesCount <=
            mesh->indices.size() - skirt.noSkirtIndicesBegin) {
        surfaceIndexOffset =
            static_cast<int>(skirt.noSkirtIndicesBegin * sizeof(uint32_t));
        surfaceIndexCount = static_cast<int>(skirt.noSkirtIndicesCount);
    }

    RenderCommand surfaceCommand = renderer.makeSurfaceTileCommand(
        baseTexture,
        tile.content.renderContent.surfaceVertexBuffer(),
        tile.content.renderContent.surfaceIndexBuffer(),
        surfaceIndexCount);
    surfaceCommand.indexOffset = surfaceIndexOffset;
    surfaceCommand.frameId = context.frameNumber;
    surfaceCommand.generation = context.generation;
    surfaceCommand.terrainRenderContent = true;
    surfaceCommand.surfaceMeshIndexCount = meshIndexCount;
    surfaceCommand.surfaceNoSkirtIndexCount = surfaceIndexCount;
    surfaceCommand.surfaceSkirtIndexCount =
        std::max(0, meshIndexCount - surfaceIndexCount);
    if (baseBinding.tile) {
        if (baseBinding.tileHandle) {
            surfaceCommand.resourceKeepAlive.push_back(
                baseBinding.tileHandle);
        }
        surfaceCommand.surfaceBaseRasterState =
            static_cast<int>(baseBinding.tile->getState());
        surfaceCommand.surfaceBaseIsCompositeTile =
            baseBinding.tile->isMappedRasterTile() ? 1 : 0;
    }
    surfaceCommand.surfaceTileUv = baseBinding.tile
        ? std::array<float, 4>{
              baseBinding.offsetU,
              baseBinding.offsetV,
              baseBinding.scaleU,
              baseBinding.scaleV}
        : std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};
    surfaceCommand.surfaceHasWaterMask =
        mesh->waterMask.valid() ? 1.0f : 0.0f;
    surfaceCommand.surfaceWaterMaskTranslationScale = {
        static_cast<float>(mesh->waterMask.translationX),
        static_cast<float>(mesh->waterMask.translationY),
        static_cast<float>(mesh->waterMask.scale),
        0.0f};
    surfaceCommand.surfaceWaterMaskState = {
        mesh->waterMask.allLand ? 1.0f : 0.0f,
        mesh->waterMask.allWater ? 1.0f : 0.0f,
        !mesh->waterMask.data.empty() ? 1.0f : 0.0f,
        0.0f};
    if (Texture* waterMaskTexture =
            tile.content.renderContent.surfaceWaterMaskTexture()) {
        while (surfaceCommand.textures.size() < 5) {
            surfaceCommand.textures.push_back(nullptr);
        }
        surfaceCommand.textures.push_back(waterMaskTexture);
    }
    if (context.surfaceClipUv) {
        surfaceCommand.surfaceClipUv = *context.surfaceClipUv;
        surfaceCommand.surfaceClipEnabled = 1.0f;
    }
    surfaceCommand.surfaceGeometryZoom = tile.key.z;
    surfaceCommand.surfaceTextureZoom = baseBinding.tile
        ? rasterTextureSourceZoom(baseBinding.tile)
        : -1;
    const Vec3& localOrigin = tile.content.renderContent.renderLocalOrigin();
    surfaceCommand.surfaceTileOrigin = {
        static_cast<float>(localOrigin.x()),
        static_cast<float>(localOrigin.y()),
        static_cast<float>(localOrigin.z())};
    surfaceCommand.surfaceTileOpacity = 1.0f;
    surfaceCommand.surfaceTransitionOpacity = context.transitionOpacity;
    if (context.transitionOpacity < 0.999f) {
        surfaceCommand.blend = true;
        surfaceCommand.blendSrc = RenderCommand::BlendFactor::SrcAlpha;
        surfaceCommand.blendDst =
            RenderCommand::BlendFactorDst::OneMinusSrcAlpha;
    }
    surfaceCommand.surfaceGeneration = static_cast<float>(context.generation);

    int overlayTextureCount = 0;
    for (size_t i = 0;
         i < overlays.size() && i < tile.rasterOverlayState.mappingCount();
         ++i) {
        auto* activeOverlay = overlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        if (baseOverlayIndex && *baseOverlayIndex == i) {
            continue;
        }
        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        const SurfaceRasterBinding binding =
            chooseSurfaceRasterBinding(mapped);
        if (!overlayBindingAllowedByPolicy(
                activeOverlay,
                mapped,
                binding)) {
            continue;
        }
        Texture* tex = binding.tile->getTexture();
        if (!tex) continue;

        if (overlayTextureCount >= kMaxSurfaceImageryOverlays) {
            continue;
        }

        surfaceCommand.textures.push_back(tex);
        if (binding.tileHandle) {
            surfaceCommand.resourceKeepAlive.push_back(binding.tileHandle);
        }
        surfaceCommand.surfaceOverlayTileUvs[overlayTextureCount] = {
            binding.offsetU,
            binding.offsetV,
            binding.scaleU,
            binding.scaleV};
        surfaceCommand.surfaceOverlayOpacities[overlayTextureCount] =
            activeOverlay->opacity();
        ++overlayTextureCount;
    }

    surfaceCommand.surfaceOverlayTextureCount = overlayTextureCount;
    commands.push_back(std::move(surfaceCommand));
}

} // namespace earth_engine
