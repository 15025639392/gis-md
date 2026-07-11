#include "TileFillProxyPreparer.h"

#include "GltfRenderGeometryBuilder.h"
#include "LoadedTerrainHeightSampler.h"
#include "RasterMappedToTilesetTile.h"
#include "TileSelectionRootPolicy.h"
#include "TilesetTile.h"
#include "../content/EllipsoidTerrainMeshBuilder.h"
#include "../content/GltfModel.h"
#include "../renderer/RenderDevice.h"

namespace earth_engine {
namespace {

RasterOverlayProjection projectionForTile(const TilesetTile& tile) {
    return tile.key.schemeId.str() == "XYZ-WebMercator"
        ? RasterOverlayProjection::WebMercator
        : RasterOverlayProjection::Geographic;
}

// Upload the fill model's single terrain-format primitive into the tile's fill
// GPU slots. Mirrors the terrain branch of GltfRenderResourcePreparer, but
// targets the fill slots and carries no water mask / base-color texture (the
// proxy's imagery is supplied by the raster-overlay bindings at draw time).
bool uploadFillPrimitive(TilesetTile& tile, RenderDevice* device) {
    TileRenderContentState& rc = tile.content.renderContent;
    const GltfModel* model = rc.fillContent();
    if (!model || model->primitives.empty()) {
        return false;
    }
    const GltfPrimitive& primitive = model->primitives.front();
    if (primitive.vertices.empty() || primitive.indices.empty()) {
        return false;
    }

    std::vector<TerrainGpuVertex> terrainVerts =
        GltfRenderGeometryBuilder::buildTerrainVertices(
            primitive,
            rc.fillTransform(),
            rc.fillLocalOrigin());
    if (terrainVerts.empty()) {
        return false;
    }

    GltfPrimitiveRenderResources resources;
    resources.useTerrainVertexFormat = true;

    BufferDesc vbDesc;
    vbDesc.size = terrainVerts.size() * sizeof(TerrainGpuVertex);
    vbDesc.data = terrainVerts.data();
    vbDesc.usage = BufferDesc::Usage::Static;
    vbDesc.type = BufferDesc::Type::Vertex;
    resources.vertexBuffer = device->createBuffer(vbDesc);

    BufferDesc ibDesc;
    ibDesc.size = primitive.indices.size() * sizeof(uint32_t);
    ibDesc.data = primitive.indices.data();
    ibDesc.usage = BufferDesc::Usage::Static;
    ibDesc.type = BufferDesc::Type::Index;
    resources.indexBuffer = device->createBuffer(ibDesc);

    if (!resources.vertexBuffer || !resources.indexBuffer) {
        return false;
    }

    resources.vertexCount = static_cast<int>(primitive.vertices.size());
    resources.indexCount = static_cast<int>(primitive.indices.size());
    resources.primitiveMode = primitive.primitiveMode;
    resources.sortCenterEcef =
        GltfRenderGeometryBuilder::primitiveSortCenterEcef(
            primitive,
            rc.fillTransform());
    resources.baseColorFactor = primitive.baseColorFactor;
    resources.metallicFactor = 0.0f;
    resources.roughnessFactor = 1.0f;
    resources.unlit = false;
    // No water mask on the flat ellipsoid proxy.
    resources.hasTerrainWaterMaskMetadata = false;
    resources.terrainOnlyWater = false;
    resources.terrainOnlyLand = true;

    rc.beginFillGpuResourceBuild(0, 1);
    rc.addFillPrimitiveResource(std::move(resources));
    rc.setFillResourcesReady(true);
    return true;
}

} // namespace

bool TileFillProxyPreparer::ensureFillProxy(
    TilesetTile& tile,
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    RenderDevice* device,
    int gridSize) {
    if (!device) {
        return false;
    }
    TileRenderContentState& rc = tile.content.renderContent;
    // Already showing (or about to show) real terrain, or a fill already
    // exists — nothing to do.
    if (rc.hasGltfResources() || rc.hasFillModel()) {
        return false;
    }
    // The virtual terrain root has world-spanning bounds and never renders.
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key)) {
        return false;
    }
    if (tile.bounds.isEmpty()) {
        return false;
    }

    // Borrow loaded-terrain heights along the proxy grid (nullopt where no
    // terrain is loaded → flat). Edges shared with loaded neighbours therefore
    // meet the real terrain crack-free. The area sampler gathers the terrain
    // tiles overlapping this tile's rectangle ONCE, so the ~289 grid-vertex
    // queries reuse that candidate set instead of rescanning the whole
    // registry per vertex.
    const LoadedTerrainAreaSampler areaSampler(tiles, tile.bounds);
    const EllipsoidProxyHeightSampler heightSampler =
        areaSampler.empty()
            ? EllipsoidProxyHeightSampler{}
            : [&areaSampler](double lonRad,
                             double latRad) -> std::optional<float> {
                  return areaSampler.sample(lonRad, latRad);
              };
    std::unique_ptr<GltfModel> proxy = EllipsoidTerrainMeshBuilder::makeModel(
        tile.bounds,
        projectionForTile(tile),
        gridSize,
        heightSampler);
    if (!proxy) {
        return false;
    }
    rc.setFillContent(std::move(proxy));
    if (!uploadFillPrimitive(tile, device)) {
        rc.clearFillContent();
        return false;
    }
    return true;
}

} // namespace earth_engine
