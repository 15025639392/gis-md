#pragma once

#include "../content/GltfModel.h"
#include "../core/math/Mat4.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderCommand.h"
#include "../renderer/RenderDevice.h"
#include "SurfaceTile.h"
#include "TileFillGeometrySignature.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
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
    // Byte width of the uploaded index buffer: 2 (uint16) or 4 (uint32).
    // Drives RenderCommand::indexType at draw command build time.
    int indexByteSize = 4;
    // 北极星 Phase 2c P5b:本 primitive 的几何由共享位移模板承担(draw 时
    // GltfDrawCommandBuilder 把命令换绑模板 VBO/IBO),per-tile vertex/index
    // buffer 被有意跳过(nullptr)——省去主线程顶点重建(terrainUpload 尖刺)与
    // 每瓦片 ~507KB 废弃显存。true 时 null buffer 视为「已就绪」,不得再触发
    // 重 prepare;draw 侧若模板 swap 失败则丢弃该命令(不画,绝不解引用 null)。
    bool sharedTemplateGeometry = false;
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
    bool useTerrainVertexFormat = false;  // true = 28-byte TerrainGpuVertex, false = 120-byte GltfGpuVertex
};

class TileRenderContentState {
public:
    // ── 地形高度数据全局代次 ────────────────────────────────────────────────
    // 进程级原子计数,任一瓦片的 retained heightmap 出现/消亡/替换时 +1。
    // 消费方(TerrainHeightService 索引重建、相机探针失效、矢量贴地重钳节流)
    // 用"读数变了 = 地形高度世界变了"做失效判断;方向上只会过度失效(多个
    // tileset 共享一个计数),不会漏失效——替代 contentBytesUsed() 弱代理
    // (字节数恰好不变时会漏)。稳态无瓦片变化时读数必须不动(诊断判据)。
    //
    // 收口契约:surface_.heightmap 的**每一个**赋值/reset 点都必须经
    // bumpHeightmapGeneration() 记账(仅在真的有变化时 bump)。变异点全部
    // 在本类内——新增变异点时必须同步加 bump,守卫见
    // test_terrain_heightmap_generation.cpp。
    static std::uint64_t heightmapGeneration() {
        return heightmapGenerationCounter().load(std::memory_order_relaxed);
    }

    bool asyncGpuUploadPending = false;  // true = CPU work dispatched to worker, GPU upload pending next frame
    class GltfContentEdit {
    public:
        GltfContentEdit(
            TileRenderContentState& owner,
            GltfModel* model)
            : owner_(&owner),
              model_(model) {}
        ~GltfContentEdit() {
            if (model_) {
                owner_->markRetainedResourcesChanged();
            }
        }

        GltfContentEdit(const GltfContentEdit&) = delete;
        GltfContentEdit& operator=(const GltfContentEdit&) = delete;

        explicit operator bool() const { return model_ != nullptr; }
        bool operator==(std::nullptr_t) const { return model_ == nullptr; }
        bool operator!=(std::nullptr_t) const { return model_ != nullptr; }
        friend bool operator==(
            std::nullptr_t,
            const GltfContentEdit& edit) {
            return edit.model_ == nullptr;
        }
        friend bool operator!=(
            std::nullptr_t,
            const GltfContentEdit& edit) {
            return edit.model_ != nullptr;
        }
        GltfModel* operator->() { return model_; }
        GltfModel& operator*() { return *model_; }

    private:
        TileRenderContentState* owner_ = nullptr;
        GltfModel* model_ = nullptr;
    };

public:
    // ── Shadow readiness mirror (async selection) ──────────────────────────
    // A shadow TilesetTile carries no real glTF model / GPU resources (kept
    // lightweight + thread-safe), yet the selection traversal it runs must see
    // the SAME renderability classification the live tile would. When
    // shadowReadinessMirror_ is set (shadow tiles ONLY — never live), the
    // readiness PREDICATES below report the mirrored live values instead of
    // deriving them from the absent gltfModel/resources. Pointer accessors
    // (hasGltfModel/gltfContent/...) stay strictly model-backed, so selection —
    // which only reads the booleans, never dereferences — is safe, while any
    // accidental model access on a shadow tile still yields null.
    void setShadowReadinessMirror(bool hasGltfContent,
                                  bool renderContentReady,
                                  bool terrainRenderContent) {
        shadowReadinessMirror_ = true;
        shadowHasGltfContent_ = hasGltfContent;
        shadowRenderContentReady_ = renderContentReady;
        terrainRenderContent_ = terrainRenderContent;
    }

    bool hasGltfContent() const {
        return shadowReadinessMirror_ ? shadowHasGltfContent_
                                      : gltfModel != nullptr;
    }
    bool hasGltfModel() const { return gltfModel != nullptr; }
    const GltfModel* gltfContent() const { return gltfModel.get(); }
    const GltfModel* gltfModelForRead() const { return gltfModel.get(); }
    GltfContentEdit editGltfContent() {
        return GltfContentEdit(*this, gltfModel.get());
    }
    bool updateGltfAnimation(double timeSeconds) {
        return gltfModel && gltfModel->updateAnimation(timeSeconds);
    }
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

    // ── Terrain fill (ellipsoid proxy) ─────────────────────────────────────
    // A drape-ready ellipsoid proxy model shown while the real terrain is still
    // loading. It lives in a SEPARATE slot from gltfModel so the many code
    // paths that read gltfModel (upsample parent geometry, raster-detail
    // generation, canPrepareRasterOverlays, ...) keep treating gltfModel as the
    // committed REAL content only. The draw path consults the draw-effective
    // getters below, which prefer real content and fall back to the fill.
    // Because a proxy vertex and the real-terrain vertex at the same lon/lat
    // map imagery to the same texel (overlay UVs are height-independent), the
    // tile's raster mappings bind identically to fill and real geometry — no
    // re-map on swap; imagery stays put while terrain "rises".
    bool hasFillModel() const { return fillModel_ != nullptr; }
    bool hasFillResources() const { return !fillPrimitiveResources_.empty(); }
    bool hasMatchingFillGeometry(
        const TileFillGeometrySignature& signature) const {
        return fillGeometrySignature_.has_value() &&
               *fillGeometrySignature_ == signature;
    }
    const TileFillGeometrySignature* fillGeometrySignature() const {
        return fillGeometrySignature_
            ? &*fillGeometrySignature_
            : nullptr;
    }
    bool isFillReady() const {
        return fillResourcesReady_ && !fillPrimitiveResources_.empty();
    }
    /// True when the draw path should render the fill proxy: the real terrain
    /// mesh is not yet renderable but a fill proxy is ready.
    bool drawsFill() const {
        return !isRenderContentReady() && isFillReady();
    }
    /// The draw path can emit commands (real OR fill geometry ready).
    bool hasDrawableResources() const {
        return hasGltfResources() || isFillReady();
    }
    bool drawsTransientFallbackSurface() const {
        return drawsFill() ||
               surface_.surfaceSource ==
                   SurfaceDrawableSource::EllipsoidFallback;
    }
    const std::vector<GltfPrimitiveRenderResources>& drawPrimitiveResources()
        const {
        return drawsFill() ? fillPrimitiveResources_ : gltfPrimitiveResources;
    }
    const Vec3& drawLocalOrigin() const {
        return drawsFill() ? fillLocalOrigin_ : surface_.localOrigin;
    }
    bool drawIsTerrainContent() const {
        return drawsFill() ? true : terrainRenderContent_;
    }
    bool isGltfRenderReady() const {
        if (shadowReadinessMirror_) {
            return shadowHasGltfContent_ && shadowRenderContentReady_;
        }
        return gltfModel != nullptr && hasGltfResources();
    }
    bool isRenderContentReady() const {
        if (shadowReadinessMirror_) {
            return shadowRenderContentReady_;
        }
        return gltfModel ? isGltfRenderReady() : surface_.meshReady;
    }
    bool hasRenderableTerrainContent() const {
        return hasGltfContent();
    }
    bool hasRasterOverlayDetailsContent() const {
        return !rasterOverlayDetails().empty();
    }
    bool isTerrainRenderContent() const { return terrainRenderContent_; }
    const DecodedHeightmap* retainedHeightmap() const {
        return surface_.heightmap.get();
    }
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
    void setRetainedHeightmap(std::unique_ptr<DecodedHeightmap> decoded) {
        // Phase 2c:heightmap 地形虽当 glTF 交付(prepareGltfContent 设 gltfModel
        // → isGltfOwnedContentState() 为 true),但 GPU 位移需保留其原始高度图建
        // per-tile 高度纹理。故只要**显式传入非空高度图**(仅 heightmap 地形的
        // 上传路径这么做,真 glTF 内容从不传)就保留;仅当 decoded 为空(意在清理)
        // 且已是 gltf-owned 态时,才丢弃可能残留的 stale 高度图。
        if (!decoded && isGltfOwnedContentState()) {
            if (surface_.heightmap) {
                surface_.heightmap.reset();
                markRetainedResourcesChanged();
                bumpHeightmapGeneration();
            }
            return;
        }
        if (surface_.heightmap || decoded) {
            markRetainedResourcesChanged();
            bumpHeightmapGeneration();
        }
        surface_.heightmap = std::move(decoded);
    }
    void clearRetainedHeightmap() {
        const bool hadHeightmap = surface_.heightmap != nullptr;
        surface_.heightmap.reset();
        if (hadHeightmap) {
            markRetainedResourcesChanged();
            bumpHeightmapGeneration();
        }
        if (!gltfModel) {
            clearTerrainHeightRange();
        }
    }
    void setMeshReady(bool ready) {
        if (isGltfOwnedContentState()) {
            surface_.meshReady = false;
            return;
        }
        surface_.meshReady = ready;
        if (ready) {
            clearFillContent();
        }
    }
    void setGltfResourcesReady(bool ready) {
        if (gltfResourcesReady_ == ready) {
            return;
        }
        gltfResourcesReady_ = ready;
        invalidateCachedDrawCommands();
    }
    void setTerrainRenderContent(bool terrain) {
        if (terrainRenderContent_ == terrain) {
            return;
        }
        terrainRenderContent_ = terrain;
        invalidateCachedDrawCommands();
    }
    void markRenderContentReady() {
        if (gltfModel) {
            const bool readinessChanged = !gltfResourcesReady_;
            gltfResourcesReady_ = true;
            // Real terrain is now renderable — the proxy has served its purpose;
            // drop it so the tile draws real geometry (the "rise") and frees the
            // proxy GPU buffers. Imagery UVs are unchanged (lon/lat-based).
            if (hasAnyFillState()) {
                clearFillContent();
            }
            if (readinessChanged) {
                invalidateCachedDrawCommands();
            }
        } else {
            const bool readinessChanged = !surface_.meshReady;
            surface_.meshReady = true;
            if (hasAnyFillState()) {
                clearFillContent();
            } else if (!readinessChanged) {
                return;
            }
        }
    }
    void setSurfaceDrawable(bool drawable) {
        surface_.surfaceDrawable = drawable;
    }
    void setSurfaceSource(SurfaceDrawableSource source) {
        if (gltfModel && source != SurfaceDrawableSource::GltfContent &&
            source != SurfaceDrawableSource::EllipsoidFallback) {
            surface_.surfaceSource = SurfaceDrawableSource::GltfContent;
            return;
        }
        surface_.surfaceSource = source;
    }
    void setTerrainHeightRange(double minimumHeight, double maximumHeight) {
        surface_.hasTerrainHeightRange = true;
        surface_.terrainMinimumHeight = minimumHeight;
        surface_.terrainMaximumHeight = maximumHeight;
    }
    void clearTerrainHeightRange() {
        surface_.hasTerrainHeightRange = false;
        surface_.terrainMinimumHeight = 0.0;
        surface_.terrainMaximumHeight = 0.0;
    }
    void setHorizonOcclusionPoint(const Vec3& point) {
        surface_.horizonOcclusionPoint = point;
    }
    const Vec3* horizonOcclusionPoint() const {
        if (surface_.horizonOcclusionPoint) {
            return &*surface_.horizonOcclusionPoint;
        }
        return nullptr;
    }
    const RasterOverlayDetails& rasterOverlayDetails() const {
        static const RasterOverlayDetails emptyDetails;
        if (gltfModel) {
            return gltfModel->rasterOverlayDetails;
        }
        return emptyDetails;
    }
    RasterOverlayDetails* mutableRasterOverlayDetails() {
        if (gltfModel) {
            return &gltfModel->rasterOverlayDetails;
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
    GltfPrimitiveRenderResources* gltfPrimitiveResourceForAnimationUpdateAt(
        size_t index) {
        // The animation path only updates existing buffer contents and
        // size-stable draw metadata.
        invalidateCachedDrawCommands();
        return index < gltfPrimitiveResources.size()
            ? &gltfPrimitiveResources[index]
            : nullptr;
    }

    // ── per-tile 常驻 draw command 缓存(cesium per-tile DrawCommand 语义)──
    // GltfDrawCommandBuilder 把内容不变式命令(几何/材质/水面掩码/stableKey)
    // 一次性构建进这里;后续帧仅把缓存命令拷入帧列表并盖章每帧字段(frameId/
    // opacity/clip/overlay 绑定,MVP 由 Scene 统一重算)。
    //
    // 失效是**代次校验、机器可查**的:缓存命令持有本状态内 GPU 资源的裸指针,
    // 有效性 = 构建时记录的 drawCommandReadSetRevision_ 与当前值一致。凡走
    // markRetainedResourcesChanged()(所有 GPU 资源生命周期 mutator 为了字节
    // 记账本来就必须调)或 invalidateCachedDrawCommands() 的路径都会 bump 该
    // 代次 → 新增资源 mutator 即使忘调 invalidate,缓存也会在下次消费时自动
    // 判stale 重建,不再依赖"人工记得失效"。仅改读取集标志(readiness/
    // localOrigin 等)不动资源的 mutator 仍显式调 invalidate(同样 bump 代次)。
    bool hasCachedDrawCommands() const {
        return cachedDrawCommandsValid_ &&
               cachedDrawCommandsBuiltRevision_ == drawCommandReadSetRevision_;
    }
    const RenderCommandList& cachedDrawCommands() const {
        return cachedDrawCommands_;
    }
    RenderCommandList& restartCachedDrawCommands() {
        cachedDrawCommands_.clear();
        cachedDrawCommandsValid_ = true;
        cachedDrawCommandsBuiltRevision_ = drawCommandReadSetRevision_;
        return cachedDrawCommands_;
    }
    void invalidateCachedDrawCommands() {
        cachedDrawCommands_.clear();
        cachedDrawCommandsValid_ = false;
        ++drawCommandReadSetRevision_;
    }

    void setSurfaceLocalOrigin(const Vec3& origin) {
        if (surface_.localOrigin == origin) {
            return;
        }
        surface_.localOrigin = origin;
        invalidateCachedDrawCommands();
    }

    void setSurfaceGpuBuffers(std::unique_ptr<Buffer> vertexBuffer,
                              std::unique_ptr<Buffer> indexBuffer) {
        if (isGltfOwnedContentState()) {
            const bool hadBuffers =
                surface_.gpuVertexBuffer || surface_.gpuIndexBuffer;
            surface_.gpuVertexBuffer.reset();
            surface_.gpuIndexBuffer.reset();
            if (hadBuffers) {
                markRetainedResourcesChanged();
            }
            return;
        }
        if (surface_.gpuVertexBuffer || surface_.gpuIndexBuffer ||
            vertexBuffer || indexBuffer) {
            markRetainedResourcesChanged();
        }
        surface_.gpuVertexBuffer = std::move(vertexBuffer);
        surface_.gpuIndexBuffer = std::move(indexBuffer);
    }

    void setSurfaceWaterMaskTexture(std::unique_ptr<Texture> texture) {
        if (isGltfOwnedContentState()) {
            const bool hadTexture = surfaceWaterMaskTexture_ != nullptr;
            surfaceWaterMaskTexture_.reset();
            if (hadTexture) {
                markRetainedResourcesChanged();
            }
            return;
        }
        if (surfaceWaterMaskTexture_ || texture) {
            markRetainedResourcesChanged();
        }
        surfaceWaterMaskTexture_ = std::move(texture);
    }

    void setGltfLocalOrigin(const Vec3& origin) {
        if (surface_.localOrigin == origin) {
            return;
        }
        surface_.localOrigin = origin;
        invalidateCachedDrawCommands();
    }

    void setGltfContent(std::unique_ptr<GltfModel> model,
                        const Mat4& contentTransform = Mat4::identity()) {
        prepareGltfContent(std::move(model), contentTransform);
    }

    void addGltfTextureResource(std::unique_ptr<Texture> texture) {
        markRetainedResourcesChanged();
        gltfTextureResources.push_back(std::move(texture));
    }

    void addGltfPrimitiveResource(GltfPrimitiveRenderResources resources) {
        markRetainedResourcesChanged();
        gltfPrimitiveResources.push_back(std::move(resources));
        invalidateCachedDrawCommands();
    }

    // ── Fill (ellipsoid proxy) lifecycle ───────────────────────────────────
    void setFillContent(
        std::unique_ptr<GltfModel> model,
        const Mat4& contentTransform = Mat4::identity()) {
        markRetainedResourcesChanged();
        clearFillGpuResources();
        if (model && model->preferredLocalOriginEcef.has_value()) {
            fillLocalOrigin_ =
                contentTransform * *model->preferredLocalOriginEcef;
        } else {
            fillLocalOrigin_ = Vec3::zero();
        }
        fillModel_ = std::move(model);
        fillContentTransform_ = contentTransform;
        fillResourcesReady_ = false;
        invalidateCachedDrawCommands();
    }
    const GltfModel* fillContent() const { return fillModel_.get(); }
    const Mat4& fillTransform() const { return fillContentTransform_; }
    const Vec3& fillLocalOrigin() const { return fillLocalOrigin_; }
    void beginFillGpuResourceBuild(size_t textureCount,
                                   size_t primitiveResourceCount) {
        clearFillGpuResources();
        fillTextureResources_.reserve(textureCount);
        fillPrimitiveResources_.reserve(primitiveResourceCount);
    }
    void addFillTextureResource(std::unique_ptr<Texture> texture) {
        markRetainedResourcesChanged();
        fillTextureResources_.push_back(std::move(texture));
    }
    void addFillPrimitiveResource(GltfPrimitiveRenderResources resources) {
        markRetainedResourcesChanged();
        fillPrimitiveResources_.push_back(std::move(resources));
        invalidateCachedDrawCommands();
    }
    void commitFillResourcesReady(
        const TileFillGeometrySignature& geometrySignature) {
        fillGeometrySignature_ = geometrySignature;
        fillResourcesReady_ = true;
        invalidateCachedDrawCommands();
    }
    void clearFillCpuModelAfterUpload() {
        if (fillModel_) {
            markRetainedResourcesChanged();
        }
        fillModel_.reset();
    }
    void clearFillGpuResources() {
        const bool changed =
            !fillTextureResources_.empty() ||
            !fillPrimitiveResources_.empty() ||
            fillGeometrySignature_.has_value() ||
            fillResourcesReady_;
        if (!changed) {
            return;
        }
        if (!fillTextureResources_.empty() ||
            !fillPrimitiveResources_.empty()) {
            markRetainedResourcesChanged();
        }
        fillTextureResources_.clear();
        fillPrimitiveResources_.clear();
        fillGeometrySignature_.reset();
        fillResourcesReady_ = false;
        invalidateCachedDrawCommands();
    }
    /// Drop the proxy entirely (real terrain took over, or the tile unloaded).
    void clearFillContent() {
        if (!hasAnyFillState()) {
            return;
        }
        if (fillModel_) {
            markRetainedResourcesChanged();
        }
        fillModel_.reset();
        fillContentTransform_ = Mat4::identity();
        fillLocalOrigin_ = Vec3::zero();
        clearFillGpuResources();
    }

    uint64_t retainedResourcesRevision() const {
        return retainedResourcesRevision_;
    }

    static int64_t estimateHeightmapBytes(const DecodedHeightmap& heightmap) {
        int64_t bytes = 0;
        bytes += static_cast<int64_t>(
            heightmap.quantizedHeights.size() * sizeof(uint16_t));
        bytes += static_cast<int64_t>(
            heightmap.noDataValues.size() * sizeof(float));
        return bytes;
    }

    int64_t estimateRetainedBytes() const {
        int64_t bytes = 0;
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
                surfaceWaterMaskTexture_->sizeBytes());
        }
        for (const std::unique_ptr<Texture>& texture : gltfTextureResources) {
            if (texture) {
                bytes += static_cast<int64_t>(texture->sizeBytes());
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
            if (primitive.instanceBuffer) {
                bytes += static_cast<int64_t>(
                    primitive.instanceBuffer->size());
            }
        }
        if (surface_.heightmap) {
            bytes += estimateHeightmapBytes(*surface_.heightmap);
        }
        if (fillModel_) {
            bytes += fillModel_->byteSize();
        }
        for (const std::unique_ptr<Texture>& texture : fillTextureResources_) {
            if (texture) {
                bytes += static_cast<int64_t>(texture->sizeBytes());
            }
        }
        for (const GltfPrimitiveRenderResources& primitive :
             fillPrimitiveResources_) {
            if (primitive.vertexBuffer) {
                bytes += static_cast<int64_t>(primitive.vertexBuffer->size());
            }
            if (primitive.indexBuffer) {
                bytes += static_cast<int64_t>(primitive.indexBuffer->size());
            }
            if (primitive.instanceBuffer) {
                bytes += static_cast<int64_t>(
                    primitive.instanceBuffer->size());
            }
        }
        return bytes;
    }

    void clearGltfPrimitiveResources() {
        if (!gltfPrimitiveResources.empty()) {
            markRetainedResourcesChanged();
        }
        gltfPrimitiveResources.clear();
        gltfResourcesReady_ = false;
        invalidateCachedDrawCommands();
    }

    /// 幽灵网格摘除:共享位移模板路下,地形瓦片的 CPU 网格(世界顶点阵 +
    /// 索引 + runtime.baseVertices 那份重复拷贝)从不被绘制 —— draw 侧必换
    /// 模板 VBO(GltfDrawCommandBuilder),prepare 侧也已跳过其 GPU 上传
    /// (skipBakedTerrainGeometry)。实测近景 90 个地形瓦片为此常驻 95MB、
    /// 占内容缓存预算近一半(见 CpuAcct 走账)。
    ///
    /// **摘除前提**(调用方负责,见 GltfRenderResourcePreparer 的判据):
    ///   ① 该瓦片所有 primitive 都走了共享模板(sharedTemplateGeometry);
    ///   ② rasterOverlayDetails 已在 commit 期由顶点算好
    ///      (TileContentUploadCommitter → ensureProjectionDetailsFromActiveOverlays
    ///       是该函数**唯一**调用点,晚于它就不会再从顶点重算);
    ///   ③ decoupleImageryFromGeometry=true —— 关掉时 GltfTerrainUpsampler
    ///      要读**父瓦片**的顶点造子瓦片,父被摘了会产出空网格。
    /// 顶点数/索引数**不清**(metadata 已在 GpuReadyPrimitive 里留档),清的
    /// 只是字节。运行时关位移池要靠 ghostGeometryReleased() 把这些瓦片整个
    /// 重载 —— 没有顶点就没法回落 legacy VBO(见 Engine::setTerrainGpuDisplacementEnabled)。
    void releaseGhostTerrainGeometry() {
        if (!gltfModel || ghostGeometryReleased_) {
            return;
        }
        bool changed = false;
        for (GltfPrimitive& primitive : gltfModel->primitives) {
            changed |= !primitive.vertices.empty() ||
                       !primitive.indices.empty() ||
                       !primitive.runtime.baseVertices.empty();
            primitive.vertices.clear();
            primitive.vertices.shrink_to_fit();
            primitive.indices.clear();
            primitive.indices.shrink_to_fit();
            primitive.runtime.baseVertices.clear();
            primitive.runtime.baseVertices.shrink_to_fit();
            for (auto& texCoords : primitive.vertexTexCoords) {
                texCoords.clear();
                texCoords.shrink_to_fit();
            }
        }
        ghostGeometryReleased_ = true;
        if (changed) {
            markRetainedResourcesChanged();
        }
    }

    /// 该瓦片的 CPU 网格是否已被摘除(= 不可能再回落 legacy VBO 路径)。
    bool ghostGeometryReleased() const { return ghostGeometryReleased_; }

    /// 已提交的 glTF primitive 是否**全部**走共享模板几何。摘除判据用:
    /// 只要有一个 primitive 还靠自己的 per-tile VBO 绘制(水面掩码格式/
    /// 实例化/上采样),它的顶点就还在被 draw 消费,整瓦片不可摘。
    bool allGltfPrimitivesUseSharedTemplate() const {
        if (gltfPrimitiveResources.empty()) {
            return false;
        }
        for (const GltfPrimitiveRenderResources& primitive :
             gltfPrimitiveResources) {
            if (!primitive.sharedTemplateGeometry) {
                return false;
            }
        }
        return true;
    }

    void clearTerrainGpuVertexBytes() {
        if (!gltfModel) {
            return;
        }
        bool changed = false;
        for (GltfPrimitive& primitive : gltfModel->primitives) {
            changed |= !primitive.terrainGpuVertexBytes.empty();
            primitive.terrainGpuVertexBytes.clear();
            primitive.terrainGpuVertexBytes.shrink_to_fit();
        }
        if (changed) {
            markRetainedResourcesChanged();
        }
    }

    std::vector<std::vector<uint8_t>>
    takeTerrainGpuVertexBytesForDeferredRelease() {
        std::vector<std::vector<uint8_t>> retired;
        if (!gltfModel) {
            return retired;
        }
        retired.reserve(gltfModel->primitives.size());
        for (GltfPrimitive& primitive : gltfModel->primitives) {
            if (primitive.terrainGpuVertexBytes.empty()) {
                continue;
            }
            retired.push_back(
                std::move(primitive.terrainGpuVertexBytes));
        }
        if (!retired.empty()) {
            markRetainedResourcesChanged();
        }
        return retired;
    }

    void clearGltfGpuResources() {
        if (!gltfTextureResources.empty()) {
            markRetainedResourcesChanged();
        }
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
        // NOTE: does NOT touch the fill proxy. prepareGltfContent() calls this
        // when committing real content that is not yet GPU-ready; the fill must
        // keep drawing across that window and is dropped only once the real
        // mesh is renderable (markRenderContentReady) or the tile unloads
        // (clearRenderContent / clearGltfContent).
        const bool changed =
            surface_.gpuVertexBuffer ||
            surface_.gpuIndexBuffer ||
            !gltfTextureResources.empty() ||
            !gltfPrimitiveResources.empty();
        surface_.gpuVertexBuffer.reset();
        surface_.gpuIndexBuffer.reset();
        gltfTextureResources.clear();
        gltfPrimitiveResources.clear();
        if (changed) {
            markRetainedResourcesChanged();
        }
        gltfResourcesReady_ = false;
        surface_.surfaceDrawable = false;
        surface_.surfaceSource = SurfaceDrawableSource::None;
        invalidateCachedDrawCommands();
    }

    void clearSurfaceMeshResources() {
        const bool changed =
            surface_.gpuVertexBuffer ||
            surface_.gpuIndexBuffer ||
            surfaceWaterMaskTexture_;
        surface_.gpuVertexBuffer.reset();
        surface_.gpuIndexBuffer.reset();
        surfaceWaterMaskTexture_.reset();
        surface_.horizonOcclusionPoint.reset();
        surface_.meshReady = false;
        surface_.surfaceDrawable = false;
        surface_.surfaceSource = SurfaceDrawableSource::None;
        clearTerrainHeightRange();
        if (changed) {
            markRetainedResourcesChanged();
        }
    }

    void clearSurfaceResiduePreservingContentMetadata() {
        const bool hadGltfContent = gltfModel != nullptr;
        const bool hadTerrainHeightRange = surface_.hasTerrainHeightRange;
        const double terrainMinimumHeight = surface_.terrainMinimumHeight;
        const double terrainMaximumHeight = surface_.terrainMaximumHeight;
        clearSurfaceMeshResources();
        if (hadGltfContent) {
            surface_.surfaceSource = SurfaceDrawableSource::GltfContent;
            if (hadTerrainHeightRange) {
                setTerrainHeightRange(
                    terrainMinimumHeight,
                    terrainMaximumHeight);
            }
        }
    }

    void clearRenderContent() {
        const bool hadHeightmap = surface_.heightmap != nullptr;
        const bool hadModel = gltfModel != nullptr;
        const bool hadTextures = !gltfTextureResources.empty();
        const bool hadPrimitives = !gltfPrimitiveResources.empty();
        clearSurfaceMeshResources();
        surface_.heightmap.reset();
        gltfModel.reset();
        gltfContentTransform = Mat4::identity();
        gltfTextureResources.clear();
        gltfPrimitiveResources.clear();
        gltfResourcesReady_ = false;
        terrainRenderContent_ = false;
        ghostGeometryReleased_ = false;
        clearFillContent();
        if (hadHeightmap || hadModel || hadTextures || hadPrimitives) {
            markRetainedResourcesChanged();
        }
        if (hadHeightmap) {
            bumpHeightmapGeneration();
        }
        invalidateCachedDrawCommands();
    }

    void prepareGltfContent(std::unique_ptr<GltfModel> model,
                            const Mat4& contentTransform) {
        markRetainedResourcesChanged();
        if (surface_.heightmap) {
            bumpHeightmapGeneration();
        }
        surface_.heightmap.reset();
        surfaceWaterMaskTexture_.reset();
        surface_.horizonOcclusionPoint.reset();
        surface_.meshReady = false;
        clearTerrainHeightRange();
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
        clearGltfContentState(true);
    }
    void clearGltfContentPreservingFill() {
        clearGltfContentState(false);
    }

private:
    // 见 heightmapGeneration() 注释。仅类内变异点调用;guard 由调用处负责
    // (只有 heightmap 真的从有到无/从无到有/被替换时才 bump,稳态零噪声)。
    static void bumpHeightmapGeneration() {
        heightmapGenerationCounter().fetch_add(1, std::memory_order_relaxed);
    }
    static std::atomic<std::uint64_t>& heightmapGenerationCounter() {
        static std::atomic<std::uint64_t> counter{1};
        return counter;
    }

    void clearGltfContentState(bool clearFill) {
        const bool hadModel = gltfModel != nullptr;
        if (clearFill) {
            clearFillContent();
        }
        const bool wasGltfOwnedContent =
            gltfModel != nullptr ||
            terrainRenderContent_ ||
            surface_.surfaceSource == SurfaceDrawableSource::GltfContent ||
            surface_.surfaceSource == SurfaceDrawableSource::EllipsoidFallback;
        gltfModel.reset();
        if (hadModel) {
            markRetainedResourcesChanged();
        }
        gltfContentTransform = Mat4::identity();
        clearGltfGpuResources();
        terrainRenderContent_ = false;
        if (wasGltfOwnedContent) {
            clearSurfaceMeshResources();
            if (surface_.heightmap) {
                bumpHeightmapGeneration();
            }
            surface_.heightmap.reset();
            surface_.surfaceDrawable = false;
            surface_.localOrigin = Vec3::zero();
            clearTerrainHeightRange();
            return;
        }
        if (surface_.surfaceSource == SurfaceDrawableSource::GltfContent) {
            surface_.surfaceSource = SurfaceDrawableSource::None;
        }
    }

    bool hasAnyFillState() const {
        return fillModel_ != nullptr ||
               fillGeometrySignature_.has_value() ||
               fillResourcesReady_ ||
               !fillTextureResources_.empty() ||
               !fillPrimitiveResources_.empty();
    }

    void markRetainedResourcesChanged() {
        ++retainedResourcesRevision_;
        // 资源生命周期变化必然可能动到缓存命令引用的裸指针:同步 bump 命令
        // 读取集代次,让缓存自动判 stale(见 hasCachedDrawCommands 注释)。
        // 保守失效(如仅 fill 资源变化)代价只是一次命令重建,换来指针安全。
        ++drawCommandReadSetRevision_;
    }

    bool isGltfOwnedContentState() const {
        return gltfModel != nullptr ||
               surface_.surfaceSource == SurfaceDrawableSource::GltfContent ||
               surface_.surfaceSource == SurfaceDrawableSource::EllipsoidFallback;
    }

    TileSurfaceContentState surface_;
    RenderCommandList cachedDrawCommands_;
    bool cachedDrawCommandsValid_ = false;
    // 命令读取集代次:资源/读取集 mutator bump,缓存构建时记录快照,消费时
    // 比对——代次失配 = 缓存 stale(见 hasCachedDrawCommands)。
    uint64_t drawCommandReadSetRevision_ = 1;
    uint64_t cachedDrawCommandsBuiltRevision_ = 0;
    std::unique_ptr<Texture> surfaceWaterMaskTexture_;
    std::unique_ptr<GltfModel> gltfModel;
    Mat4 gltfContentTransform = Mat4::identity();
    bool gltfResourcesReady_ = false;
    bool terrainRenderContent_ = false;
    // 幽灵网格已摘(见 releaseGhostTerrainGeometry)。clearRenderContent 会连同
    // 模型一起归零 —— 重载后的新内容必须重新走一遍摘除判据,不能继承旧标记。
    bool ghostGeometryReleased_ = false;
    // Shadow readiness mirror (see setShadowReadinessMirror). Never set on live
    // tiles, so the readiness predicates keep their model-backed behavior there.
    bool shadowReadinessMirror_ = false;
    bool shadowHasGltfContent_ = false;
    bool shadowRenderContentReady_ = false;
    std::vector<std::unique_ptr<Texture>> gltfTextureResources;
    std::vector<GltfPrimitiveRenderResources> gltfPrimitiveResources;

    // Terrain fill (ellipsoid proxy) — separate from real gltf* content above.
    std::unique_ptr<GltfModel> fillModel_;
    std::optional<TileFillGeometrySignature> fillGeometrySignature_;
    Mat4 fillContentTransform_ = Mat4::identity();
    Vec3 fillLocalOrigin_ = Vec3::zero();
    bool fillResourcesReady_ = false;
    std::vector<std::unique_ptr<Texture>> fillTextureResources_;
    std::vector<GltfPrimitiveRenderResources> fillPrimitiveResources_;
    uint64_t retainedResourcesRevision_ = 1;
};

} // namespace earth_engine
