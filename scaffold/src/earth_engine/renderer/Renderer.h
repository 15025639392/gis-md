#pragma once

#include "RenderDevice.h"
#include "RenderCommand.h"
#include "IPrepareRendererResources.h"
#include <memory>
#include <array>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct GlobeMesh;
struct FrameState;
class RasterOverlayTile;
struct TileKey;

/// Stores a retained mapping from geometry tile → raster texture + UV transform.
/// Set by RasterMappedToTilesetTile during attach, read by Tileset during
/// render command construction.
struct RasterAttachment {
    const RasterOverlayTile* tile = nullptr;
    Texture* texture = nullptr;
    float offsetU = 0.0f;
    float offsetV = 0.0f;
    float scaleU = 1.0f;
    float scaleV = 1.0f;
};

/// 平台无关渲染器。
/// 管理共享 GPU 资源（shader、几何 buffer），供 Scene 和各 Layer 使用。
/// 实现 IPrepareRendererResources 以支持 retained raster attachment。
class Renderer : public IPrepareRendererResources {
public:
    /// @param device 平台渲染设备（生命周期由调用者管理，必须长于 Renderer）
    explicit Renderer(RenderDevice* device);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// 初始化所有共享渲染资源（shader 编译、mesh 上传）。
    /// 必须在 RenderDevice::onSurfaceCreated() 之后调用。
    bool initialize(const GlobeMesh& globeMesh);

    /// 提交渲染命令列表
    void submit(const RenderCommandList& commands);

    /// 释放所有 GPU 资源
    void dispose();

    // ---- 共享资源访问 ----

    /// Globe shader（地球椭球背景）
    ShaderProgram* globeShader() const;

    /// Globe 顶点/索引 buffer
    Buffer* globeVertexBuffer() const;
    Buffer* globeIndexBuffer() const;
    int globeIndexCount() const;

    /// 简单颜色 shader（矢量图层线/面渲染）
    ShaderProgram* colorShader() const;

    /// Tile 共享索引 buffer（64×64 grid，所有 surface tile 共用）
    Buffer* tileIndexBuffer() const;
    int tileIndexCount() const;
    Texture* surfaceFallbackTexture() const;

    /// glTF primitive shader.
    ShaderProgram* gltfShader() const;

    /// glTF primitive shader with EXT_mesh_gpu_instancing-style instance data.
    ShaderProgram* gltfInstancedShader() const;

    /// 地球模型矩阵（单位球 → ECEF meters）
    static std::array<float, 16> earthModelMatrix();

    /// 构建 globe 背景 RenderCommand
    RenderCommand makeGlobeCommand(const FrameState& frameState) const;

    /// Build surface tile command (unified, cesium-native glTF vertex layout).
    /// vertexStride=32: POSITION(12) + NORMAL(12) + TEXCOORD_0(8)
    RenderCommand makeSurfaceTileCommand(Texture* texture,
                                          Buffer* vertexBuffer,
                                          Buffer* indexBuffer = nullptr,
                                          int indexCount = 0) const;

    /// Build a glTF primitive command. The vertex layout matches
    /// POSITION(12) + NORMAL(12) + TEXCOORD_0..7 packed pairs (64)
    /// + COLOR_0(16) + TANGENT(16).
    RenderCommand makeGltfPrimitiveCommand(Buffer* vertexBuffer,
                                            Buffer* indexBuffer,
                                            int indexCount,
                                            int vertexCount) const;

    /// Build an instanced glTF primitive command. The per-instance layout is:
    /// mat4 relative model matrix (64 bytes) + mat3 normal matrix (36 bytes).
    RenderCommand makeGltfPrimitiveInstancedCommand(Buffer* vertexBuffer,
                                                    Buffer* indexBuffer,
                                                    Buffer* instanceBuffer,
                                                    int indexCount,
                                                    int vertexCount,
                                                    int instanceCount) const;

    // ── IPrepareRendererResources implementation ──

    /// cesium-native: attachRasterInMainThread.
    void attachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex,
        const RasterOverlayTile& rasterTile,
        Texture* texture,
        float translationU, float translationV,
        float scaleU, float scaleV) override;

    /// cesium-native: detachRasterInMainThread.
    void detachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex) noexcept override;

    /// Query the retained raster attachment for a geometry tile + overlay slot.
    /// Returns nullptr if no attachment exists.
    const RasterAttachment* getAttachedRaster(const TileKey& geometryKey,
                                               int32_t overlayIndex) const;

private:
    static std::string attachmentKey(const TileKey& key);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace earth_engine
