#pragma once

#include "../content/GltfModel.h"
#include "../core/math/Mat4.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderDevice.h"
#include "SurfaceTile.h"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {

enum class SurfaceDrawableSource {
    None,
    HeightmapTerrain,
    AncestorUpsample,
    EllipsoidFallback,
    GltfContent
};

struct TileSurfaceContentState {
    std::unique_ptr<DecodedHeightmap> heightmap;
    std::unique_ptr<SurfaceTileMesh> mesh;
    std::unique_ptr<Buffer> gpuVertexBuffer;
    std::unique_ptr<Buffer> gpuIndexBuffer;
    Vec3 localOrigin = Vec3::zero();
    bool hasTerrainHeightRange = false;
    double terrainMinimumHeight = 0.0;
    double terrainMaximumHeight = 0.0;
    std::optional<Vec3> horizonOcclusionPoint;
    bool meshReady = false;
    bool surfaceDrawable = false;
    SurfaceDrawableSource surfaceSource = SurfaceDrawableSource::None;
};

/// Renderer-side resources for one glTF mesh primitive.
/// This mirrors cesium-native TileRenderContent::getRenderResources without
/// mixing platform buffers into the parsed glTF model data.
struct GltfPrimitiveRenderResources {
    std::unique_ptr<Buffer> vertexBuffer;
    std::unique_ptr<Buffer> indexBuffer;
    std::unique_ptr<Buffer> instanceBuffer;
    struct TextureBinding {
        Texture* texture = nullptr;
        int texCoord = 0;
        std::array<float, 4> offsetScale = {0.0f, 0.0f, 1.0f, 1.0f};
        std::array<float, 2> rotationSinCos = {0.0f, 1.0f};
    };
    TextureBinding baseColorTexture;
    TextureBinding metallicRoughnessTexture;
    TextureBinding anisotropyTexture;
    TextureBinding specularTexture;
    TextureBinding specularColorTexture;
    TextureBinding specularGlossinessTexture;
    TextureBinding transmissionTexture;
    TextureBinding clearcoatTexture;
    TextureBinding clearcoatRoughnessTexture;
    TextureBinding clearcoatNormalTexture;
    TextureBinding sheenColorTexture;
    TextureBinding sheenRoughnessTexture;
    TextureBinding normalTexture;
    TextureBinding occlusionTexture;
    TextureBinding emissiveTexture;
    Texture* terrainWaterMaskTexture = nullptr;
    bool hasTerrainWaterMaskMetadata = false;
    bool terrainOnlyWater = false;
    bool terrainOnlyLand = true;
    std::array<float, 4> terrainWaterMaskTranslationScale{
        0.0f,
        0.0f,
        1.0f,
        0.0f};
    int vertexCount = 0;
    int indexCount = 0;
    int instanceCount = 0;
    GltfPrimitiveMode primitiveMode = GltfPrimitiveMode::Triangles;
    Vec3 sortCenterEcef = Vec3::zero();
    uint64_t animationRevision = 0;
    std::array<float, 4> baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float dielectricSpecularF0 = 0.04f;
    float specularFactor = 1.0f;
    std::array<float, 3> specularColorFactor = {1.0f, 1.0f, 1.0f};
    bool specularGlossinessWorkflow = false;
    std::array<float, 3> specularGlossinessSpecularFactor = {
        1.0f,
        1.0f,
        1.0f};
    float specularGlossinessGlossinessFactor = 1.0f;
    float transmissionFactor = 0.0f;
    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    float clearcoatNormalTextureScale = 1.0f;
    std::array<float, 3> sheenColorFactor = {0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.0f;
    float normalTextureScale = 1.0f;
    float occlusionTextureStrength = 1.0f;
    std::array<float, 3> emissiveFactor = {0.0f, 0.0f, 0.0f};
    GltfAlphaMode alphaMode = GltfAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
    bool dynamicVertices = false;
};

class TileRenderContentState {
public:
    bool hasTerrainMesh() const { return surface_.mesh != nullptr; }
    bool hasGltfContent() const { return gltfModel != nullptr; }
    bool hasGltfModel() const { return gltfModel != nullptr; }
    GltfModel* gltfContent() { return gltfModel.get(); }
    const GltfModel* gltfContent() const { return gltfModel.get(); }
    const GltfModel* gltfModelForRead() const { return gltfModel.get(); }
    const Mat4& gltfTransform() const { return gltfContentTransform; }
    bool isMeshReady() const { return surface_.meshReady; }
    bool isSurfaceMeshReady() const { return surface_.meshReady; }
    bool isSurfaceDrawable() const { return surface_.surfaceDrawable; }
    SurfaceDrawableSource currentSurfaceSource() const {
        return surface_.surfaceSource;
    }
    bool hasGpuSurfaceGeometry() const {
        return surface_.meshReady && surface_.gpuVertexBuffer != nullptr;
    }
    bool hasGltfResources() const {
        return gltfResourcesReady_ && !gltfPrimitiveResources.empty();
    }
    bool isGltfRenderReady() const {
        return gltfModel != nullptr && hasGltfResources();
    }
    bool isRenderContentReady() const {
        return gltfModel ? isGltfRenderReady() : surface_.meshReady;
    }
    bool hasRenderableTerrainContent() const {
        return hasSurfaceMesh() || hasGltfContent();
    }
    bool hasRasterOverlayDetailsContent() const {
        return hasSurfaceMesh() || hasGltfContent();
    }
    bool isTerrainRenderContent() const { return terrainRenderContent_; }
    const SurfaceTileMesh* surfaceMesh() const { return surface_.mesh.get(); }
    SurfaceTileMesh* surfaceMesh() { return surface_.mesh.get(); }
    bool hasSurfaceMesh() const { return surface_.mesh != nullptr; }
    const DecodedHeightmap* retainedHeightmap() const {
        return surface_.heightmap.get();
    }
    DecodedHeightmap* retainedHeightmap() { return surface_.heightmap.get(); }
    bool hasRetainedHeightmap() const { return surface_.heightmap != nullptr; }
    bool hasTerrainHeightRange() const {
        return surface_.hasTerrainHeightRange;
    }
    double terrainMinimumHeight() const { return surface_.terrainMinimumHeight; }
    double terrainMaximumHeight() const { return surface_.terrainMaximumHeight; }
    bool isSurfaceSource(SurfaceDrawableSource source) const {
        return surface_.surfaceSource == source;
    }
    bool needsHeightmapSurfaceReplacement(bool hasOwnTerrain) const {
        return surface_.meshReady &&
               hasOwnTerrain &&
               surface_.surfaceSource != SurfaceDrawableSource::HeightmapTerrain;
    }
    void setSurfaceMesh(std::unique_ptr<SurfaceTileMesh> surfaceMesh) {
        surface_.horizonOcclusionPoint.reset();
        surface_.mesh = std::move(surfaceMesh);
    }
    void setRetainedHeightmap(std::unique_ptr<DecodedHeightmap> decoded) {
        surface_.heightmap = std::move(decoded);
    }
    void clearRetainedHeightmap() {
        surface_.heightmap.reset();
    }
    void setMeshReady(bool ready) {
        surface_.meshReady = ready;
    }
    void setGltfResourcesReady(bool ready) {
        gltfResourcesReady_ = ready;
    }
    void setTerrainRenderContent(bool terrain) {
        terrainRenderContent_ = terrain;
    }
    void markRenderContentReady() {
        if (gltfModel) {
            gltfResourcesReady_ = true;
        } else {
            surface_.meshReady = true;
        }
    }
    void setSurfaceDrawable(bool drawable) {
        surface_.surfaceDrawable = drawable;
    }
    void setSurfaceSource(SurfaceDrawableSource source) {
        surface_.surfaceSource = source;
    }
    void setTerrainHeightRange(double minimumHeight, double maximumHeight) {
        surface_.hasTerrainHeightRange = true;
        surface_.terrainMinimumHeight = minimumHeight;
        surface_.terrainMaximumHeight = maximumHeight;
    }
    void setHorizonOcclusionPoint(const Vec3& point) {
        surface_.horizonOcclusionPoint = point;
    }
    SurfaceTileMesh* mutableSurfaceMesh() { return surface_.mesh.get(); }
    const Vec3* horizonOcclusionPoint() const {
        if (surface_.horizonOcclusionPoint) {
            return &*surface_.horizonOcclusionPoint;
        }
        return surface_.mesh && surface_.mesh->hasHorizonOcclusionPoint
            ? &surface_.mesh->horizonOcclusionPoint
            : nullptr;
    }
    const RasterOverlayDetails& rasterOverlayDetails() const {
        static const RasterOverlayDetails emptyDetails;
        if (gltfModel) {
            return gltfModel->rasterOverlayDetails;
        }
        if (surface_.mesh) {
            return surface_.mesh->rasterOverlayDetails;
        }
        return emptyDetails;
    }
    RasterOverlayDetails* mutableRasterOverlayDetails() {
        if (gltfModel) {
            return &gltfModel->rasterOverlayDetails;
        }
        if (surface_.mesh) {
            return &surface_.mesh->rasterOverlayDetails;
        }
        return nullptr;
    }
    const std::vector<std::string>& credits() const {
        static const std::vector<std::string> emptyCredits;
        return gltfModel ? gltfModel->credits : emptyCredits;
    }
    Buffer* surfaceVertexBuffer() const {
        return surface_.gpuVertexBuffer.get();
    }
    Buffer* surfaceIndexBuffer() const { return surface_.gpuIndexBuffer.get(); }
    Texture* surfaceWaterMaskTexture() const {
        return surfaceWaterMaskTexture_.get();
    }
    const Vec3& renderLocalOrigin() const { return surface_.localOrigin; }
    const std::vector<GltfPrimitiveRenderResources>&
    gltfPrimitiveResourcesForDraw() const {
        return gltfPrimitiveResources;
    }
    const std::vector<std::unique_ptr<Texture>>&
    gltfTextureResourcesForBinding() const {
        return gltfTextureResources;
    }
    size_t gltfPrimitiveResourceCount() const {
        return gltfPrimitiveResources.size();
    }
    bool hasGltfPrimitiveResources() const {
        return !gltfPrimitiveResources.empty();
    }
    const GltfPrimitiveRenderResources* gltfPrimitiveResourceForReadAt(
        size_t index) const {
        return index < gltfPrimitiveResources.size()
            ? &gltfPrimitiveResources[index]
            : nullptr;
    }
    GltfPrimitiveRenderResources* gltfPrimitiveResourceForBuildAt(
        size_t index) {
        return index < gltfPrimitiveResources.size()
            ? &gltfPrimitiveResources[index]
            : nullptr;
    }

    void setSurfaceLocalOrigin(const Vec3& origin) {
        surface_.localOrigin = origin;
    }

    void setSurfaceGpuBuffers(std::unique_ptr<Buffer> vertexBuffer,
                              std::unique_ptr<Buffer> indexBuffer) {
        surface_.gpuVertexBuffer = std::move(vertexBuffer);
        surface_.gpuIndexBuffer = std::move(indexBuffer);
    }

    void setSurfaceWaterMaskTexture(std::unique_ptr<Texture> texture) {
        surfaceWaterMaskTexture_ = std::move(texture);
    }

    void setGltfLocalOrigin(const Vec3& origin) {
        surface_.localOrigin = origin;
    }

    void setGltfContent(std::unique_ptr<GltfModel> model,
                        const Mat4& contentTransform = Mat4::identity()) {
        prepareGltfContent(std::move(model), contentTransform);
    }

    void addGltfTextureResource(std::unique_ptr<Texture> texture) {
        gltfTextureResources.push_back(std::move(texture));
    }

    void addGltfPrimitiveResource(GltfPrimitiveRenderResources resources) {
        gltfPrimitiveResources.push_back(std::move(resources));
    }

    static int64_t estimateHeightmapBytes(const DecodedHeightmap& heightmap) {
        int64_t bytes = 0;
        bytes += static_cast<int64_t>(
            heightmap.heights.size() * sizeof(float));
        bytes += static_cast<int64_t>(
            heightmap.noDataValues.size() * sizeof(float));
        bytes += static_cast<int64_t>(
            heightmap.metadataAvailability.size() *
            sizeof(QuantizedMeshAvailabilityRange));
        return bytes;
    }

    int64_t estimateRetainedBytes() const {
        int64_t bytes = 0;
        if (surface_.mesh) {
            bytes += static_cast<int64_t>(
                surface_.mesh->vertices.size() * sizeof(SurfaceVertex));
            bytes += static_cast<int64_t>(
                surface_.mesh->indices.size() * sizeof(uint32_t));
            bytes += static_cast<int64_t>(
                surface_.mesh->waterMask.data.size());
            bytes += static_cast<int64_t>(
                surface_.mesh->metadataAvailability.size() *
                sizeof(QuantizedMeshAvailabilityRange));
        }
        if (gltfModel) {
            bytes += gltfModel->byteSize();
        }
        if (surface_.gpuVertexBuffer) {
            bytes += static_cast<int64_t>(surface_.gpuVertexBuffer->size());
        }
        if (surface_.gpuIndexBuffer) {
            bytes += static_cast<int64_t>(surface_.gpuIndexBuffer->size());
        }
        if (surfaceWaterMaskTexture_) {
            bytes += static_cast<int64_t>(
                surfaceWaterMaskTexture_->width() *
                surfaceWaterMaskTexture_->height() * 4);
        }
        for (const std::unique_ptr<Texture>& texture : gltfTextureResources) {
            if (texture) {
                bytes += static_cast<int64_t>(
                    texture->width() * texture->height() * 4);
            }
        }
        for (const GltfPrimitiveRenderResources& primitive :
             gltfPrimitiveResources) {
            if (primitive.vertexBuffer) {
                bytes += static_cast<int64_t>(
                    primitive.vertexBuffer->size());
            }
            if (primitive.indexBuffer) {
                bytes += static_cast<int64_t>(
                    primitive.indexBuffer->size());
            }
        }
        if (surface_.heightmap) {
            bytes += estimateHeightmapBytes(*surface_.heightmap);
        }
        return bytes;
    }

    void clearGltfPrimitiveResources() {
        gltfPrimitiveResources.clear();
        gltfResourcesReady_ = false;
    }

    void clearGltfGpuResources() {
        gltfTextureResources.clear();
        clearGltfPrimitiveResources();
    }

    void beginGltfGpuResourceBuild(size_t textureCount,
                                   size_t primitiveResourceCount) {
        clearGltfGpuResources();
        gltfTextureResources.reserve(textureCount);
        gltfPrimitiveResources.reserve(primitiveResourceCount);
    }

    void releaseGpuResources() {
        surface_.gpuVertexBuffer.reset();
        surface_.gpuIndexBuffer.reset();
        gltfTextureResources.clear();
        gltfPrimitiveResources.clear();
        gltfResourcesReady_ = false;
        surface_.surfaceDrawable = false;
        surface_.surfaceSource = SurfaceDrawableSource::None;
    }

    void clearSurfaceMeshResources() {
        surface_.mesh.reset();
        surface_.gpuVertexBuffer.reset();
        surface_.gpuIndexBuffer.reset();
        surfaceWaterMaskTexture_.reset();
        surface_.horizonOcclusionPoint.reset();
        surface_.meshReady = false;
        surface_.surfaceDrawable = false;
        surface_.surfaceSource = SurfaceDrawableSource::None;
    }

    void clearRenderContent() {
        clearSurfaceMeshResources();
        surface_.heightmap.reset();
        gltfModel.reset();
        gltfContentTransform = Mat4::identity();
        gltfTextureResources.clear();
        gltfPrimitiveResources.clear();
        gltfResourcesReady_ = false;
        terrainRenderContent_ = false;
    }

    void prepareGltfContent(std::unique_ptr<GltfModel> model,
                            const Mat4& contentTransform) {
        surface_.heightmap.reset();
        surface_.mesh.reset();
        surface_.horizonOcclusionPoint.reset();
        surface_.meshReady = false;
        releaseGpuResources();
        terrainRenderContent_ = false;
        if (model && model->preferredLocalOriginEcef.has_value()) {
            surface_.localOrigin =
                contentTransform * *model->preferredLocalOriginEcef;
        }
        gltfModel = std::move(model);
        gltfContentTransform = contentTransform;
        gltfResourcesReady_ = false;
        surface_.surfaceSource = SurfaceDrawableSource::GltfContent;
    }

    void clearGltfContent() {
        const bool wasGltfOwnedContent =
            gltfModel != nullptr ||
            terrainRenderContent_ ||
            surface_.surfaceSource == SurfaceDrawableSource::GltfContent;
        gltfModel.reset();
        gltfContentTransform = Mat4::identity();
        clearGltfGpuResources();
        terrainRenderContent_ = false;
        if (wasGltfOwnedContent) {
            clearSurfaceMeshResources();
            surface_.heightmap.reset();
            surface_.surfaceDrawable = false;
            surface_.localOrigin = Vec3::zero();
            return;
        }
        if (surface_.surfaceSource == SurfaceDrawableSource::GltfContent) {
            surface_.surfaceSource = SurfaceDrawableSource::None;
        }
    }

private:
    TileSurfaceContentState surface_;
    std::unique_ptr<Texture> surfaceWaterMaskTexture_;
    std::unique_ptr<GltfModel> gltfModel;
    Mat4 gltfContentTransform = Mat4::identity();
    bool gltfResourcesReady_ = false;
    bool terrainRenderContent_ = false;
    std::vector<std::unique_ptr<Texture>> gltfTextureResources;
    std::vector<GltfPrimitiveRenderResources> gltfPrimitiveResources;
};

} // namespace earth_engine
