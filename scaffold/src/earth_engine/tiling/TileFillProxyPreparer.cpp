#include "TileFillProxyPreparer.h"

#include "GltfRenderGeometryBuilder.h"
#include "RasterMappedToTilesetTile.h"
#include "TerrainRasterOverlayProjectionResolver.h"
#include "LoadedTerrainHeightSampler.h"
#include "TileFillGeometrySignature.h"
#include "TileSelectionRootPolicy.h"
#include "TilesetTile.h"
#include "../content/EllipsoidTerrainMeshBuilder.h"
#include "../content/GltfModel.h"
#include "../renderer/RenderDevice.h"

namespace earth_engine {
namespace {

// Upload the fill model's single terrain-format primitive into the tile's fill
// GPU slots. Mirrors the terrain branch of GltfRenderResourcePreparer, but
// targets the fill slots and carries no water mask / base-color texture (the
// proxy's imagery is supplied by the raster-overlay bindings at draw time).
bool uploadFillPrimitive(
    TilesetTile& tile,
    RenderDevice* device,
    const TileFillGeometrySignature& geometrySignature) {
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
    rc.commitFillResourcesReady(geometrySignature);
    return true;
}

} // namespace

TileFillProxyPrepareResult TileFillProxyPreparer::ensureFillProxy(
    TilesetTile& tile,
    RenderDevice* device,
    int gridSize,
    IPrepareRendererResources* pPrepRenderer) {
    TileRenderContentState& rc = tile.content.renderContent;
    auto clearFill = [&]() {
        if (!rc.hasFillModel() && !rc.hasFillResources()) {
            return false;
        }
        rc.clearFillContent();
        return true;
    };
    auto clearFillAndMappings = [&]() {
        if (!rc.hasFillModel() && !rc.hasFillResources()) {
            return false;
        }
        tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
        rc.clearFillContent();
        return true;
    };

    // Real geometry owns the draw path once ready.
    if (rc.isRenderContentReady()) {
        return TileFillProxyPrepareResult{
            false,
            clearFill()};
    }
    // The virtual terrain root has world-spanning bounds and never renders.
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key)) {
        return TileFillProxyPrepareResult{
            false,
            clearFillAndMappings()};
    }
    const RasterOverlayProjection projection =
        TerrainRasterOverlayProjectionResolver::forTileKey(tile.key);
    // 借最近已加载祖先的高度,代理才能停在粗版真实地形面上而不是海平面。
    const TilesetTile* heightSource =
        TerrainAncestorHeightSource::find(tile);
    const std::optional<TileFillGeometrySignature> signature =
        TileFillGeometrySignature::tryCreate(
            tile.bounds,
            projection,
            gridSize,
            heightSource
                ? std::optional<TileKey>(heightSource->key)
                : std::nullopt);
    if (!signature) {
        return TileFillProxyPrepareResult{
            false,
            clearFillAndMappings()};
    }
    if (rc.isFillReady() && rc.hasMatchingFillGeometry(*signature)) {
        return {};
    }
    if (!device) {
        return {};
    }

    bool resourcesChanged = false;
    if (rc.hasFillModel() || rc.hasFillResources()) {
        const TileFillGeometrySignature* existingSignature =
            rc.fillGeometrySignature();
        const bool mappingDomainChanged =
            !existingSignature ||
            existingSignature->bounds != signature->bounds ||
            existingSignature->projection != signature->projection;
        if (mappingDomainChanged) {
            tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
        }
        rc.clearFillContent();
        resourcesChanged = true;
    }

    // Fill is a temporary visual bridge while real terrain is loading. It used
    // to stay flat on the ellipsoid because sampling per grid vertex meant
    // scanning terrain triangles — that cost is gone (P3 moved height queries
    // onto the retained heightmap), so the proxy now borrows the nearest
    // ancestor's heights: 17² O(1) bilinear samples against one heightmap.
    //
    // 平代理是「海平面凹坑」的来源:代理停在 h=0,而旁边已加载的真地形在几百到
    // 几千米高,交界处只能靠真瓦片的裙墙拉出一堵拉伸纹理的断崖。借了祖先高度后
    // 代理与真地形处在同一量级的面上,自身也带裙墙兜住残余落差。
    // 影像不受影响:overlay UV 是 lon/lat 的纯函数,顶点升高不移动纹理。
    EllipsoidProxyHeightSampler heightSampler;
    if (heightSource) {
        heightSampler = [heightSource](double longitudeRadians,
                                       double latitudeRadians) {
            return TerrainAncestorHeightSource::sample(
                *heightSource,
                longitudeRadians,
                latitudeRadians);
        };
    }
    std::unique_ptr<GltfModel> proxy = EllipsoidTerrainMeshBuilder::makeModel(
        tile.bounds,
        projection,
        signature->gridSize,
        heightSampler,
        /*computeGridNormals=*/heightSource != nullptr,
        /*computeGeomorphDelta=*/false,
        /*buildSkirt=*/true);
    if (!proxy) {
        return TileFillProxyPrepareResult{false, resourcesChanged};
    }
    rc.setFillContent(std::move(proxy));
    if (!uploadFillPrimitive(tile, device, *signature)) {
        rc.clearFillContent();
        return TileFillProxyPrepareResult{false, resourcesChanged};
    }
    rc.clearFillCpuModelAfterUpload();
    return TileFillProxyPrepareResult{true, true};
}

} // namespace earth_engine
