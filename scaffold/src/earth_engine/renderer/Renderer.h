#pragma once

#include "RenderDevice.h"
#include "RenderCommand.h"
#include "IPrepareRendererResources.h"
#include <memory>
#include <array>

namespace earth_engine {

struct FrameState;
class RasterOverlayTile;
struct TileKey;
class TerrainPageStore;
class TerrainDisplacementTemplatePool;

/// 平台无关渲染器。
/// 管理共享 GPU 资源（shader、几何 buffer），供 Scene 和各 Layer 使用。
/// 实现 IPrepareRendererResources 仅作为资源生命周期通知入口。
/// Surface raster 可绘制性由核心层 RenderCommand / SurfaceRasterBinding 决定。
class Renderer : public IPrepareRendererResources {
public:
    /// @param device 平台渲染设备（生命周期由调用者管理，必须长于 Renderer）
    explicit Renderer(RenderDevice* device);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// 初始化所有共享渲染资源（shader 编译、mesh 上传）。
    /// 必须在 RenderDevice::onSurfaceCreated() 之后调用。
    bool initialize();

    /// 提交渲染命令列表
    void submit(const RenderCommandList& commands);

    /// 释放所有 GPU 资源
    void dispose();

    // ---- 共享资源访问 ----

    /// 简单颜色 shader（矢量图层线/面渲染）
    ShaderProgram* colorShader() const;

    /// Tile 共享索引 buffer（64×64 grid，所有 surface tile 共用）
    Buffer* tileIndexBuffer() const;
    int tileIndexCount() const;

    /// Neutral 1x1 texture used only while required base imagery is not ready.
    Texture* surfacePlaceholderTexture() const;

    /// glTF primitive shader.
    ShaderProgram* gltfShader() const;

    /// glTF primitive shader with EXT_mesh_gpu_instancing-style instance data.
    ShaderProgram* gltfInstancedShader() const;

    /// Terrain lightweight shader (28-byte compact vertex format, no PBR extensions).
    ShaderProgram* terrainShader() const;

    /// 地球模型矩阵（单位球 → ECEF meters）
    static std::array<float, 16> earthModelMatrix();

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

    /// Build a terrain primitive command using the lightweight terrain shader.
    /// Vertex layout: POSITION(12) + NORMAL(12) +
    /// packed TEXCOORD_0/1(16) = 40 bytes.
    /// kind stays GltfPrimitive; the 28-byte stride selects the terrain path.
    RenderCommand makeTerrainPrimitiveCommand(Buffer* vertexBuffer,
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
        std::shared_ptr<const RasterOverlayTile> rasterTile,
        Texture* texture,
        float translationU, float translationV,
        float scaleU, float scaleV) override;

    /// cesium-native: detachRasterInMainThread.
    void detachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex) noexcept override;

    /// 北极星合成方案页存储(门③ Step3):Engine 每帧设置(可空=未启用)。
    /// GltfDrawCommandBuilder 经此对目标 capped 瓦片挂 array 纹理并门控;
    /// Renderer 不持有其生命周期(Engine 拥有)。
    void setTerrainPageStore(TerrainPageStore* store) {
        terrainPageStore_ = store;
    }
    TerrainPageStore* terrainPageStore() const { return terrainPageStore_; }

    /// 北极星 Phase 2c 地形 GPU 位移:Engine 惰性设置(可空=未启用 flag)。
    /// GltfDrawCommandBuilder 经此非空即把地形命令改绑共享位移模板 + 刚体帧;
    /// Renderer 不持有其生命周期(Engine 拥有)。
    void setTerrainDisplacementPool(TerrainDisplacementTemplatePool* pool) {
        terrainDisplacementPool_ = pool;
    }
    TerrainDisplacementTemplatePool* terrainDisplacementPool() const {
        return terrainDisplacementPool_;
    }
    /// P5b:prepare 侧经 IPrepareRendererResources 查询共享模板几何是否活跃,
    /// 与 draw 侧模板 swap 的「pool 非空」门控同源。
    /// ⚠️ skip flag 默认关:P5b skip(跳过废弃 per-tile VBO)机制已真机验证
    /// (z=12 skip/coarse 保留/每瓦片省 ~538KB),但它把几何 Done 提速一个数量级,
    /// 触发影像启动链竞态死锁(mapping→fetch→attach 以「几何加载慢、多帧非
    /// reuse」为隐含前提;已修 strict-reuse 一环 presentationHeld,仍有残留竞态
    /// →偶发首屏黑屏 hold 不释放)。影像启动链健壮性专项修复后再置 true。
    void setTerrainBakedVboSkipEnabled(bool enabled) {
        terrainBakedVboSkipEnabled_ = enabled;
    }
    bool terrainSharedTemplateActive() const override {
        return terrainBakedVboSkipEnabled_ &&
               terrainDisplacementPool_ != nullptr;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TerrainPageStore* terrainPageStore_ = nullptr;
    TerrainDisplacementTemplatePool* terrainDisplacementPool_ = nullptr;
    // P5b skip flag(默认关,见 setTerrainBakedVboSkipEnabled 注释)。
    bool terrainBakedVboSkipEnabled_ = false;
};

} // namespace earth_engine
