#pragma once

#include "RenderDevice.h"
#include "RenderCommand.h"
#include "IPrepareRendererResources.h"
#include <memory>
#include <array>

namespace earth_engine {

struct FrameState;
class GlyphAtlas;
class IconAtlas;
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

    /// 矢量线 ribbon shader（矢量 P1,§6.2 屏幕空间线宽,P6b 顶点色）
    ShaderProgram* vectorLineShader() const;
    /// P6d stencil 贴地线 shader(墙带体 28B pos+extrude+lengthSoFar;
    /// VS 按眼深挤出世界半宽;dash 空隙输出 alpha=0 不 discard,保
    /// stencil 清零;ClassifyVolume/ClassifyColor 两 pass 共用)
    ShaderProgram* vectorLineStencilShader() const;
    /// 矢量 fill shader(P6b:pos+顶点色 16B;colorShader 保持 pos-only
    /// 服务 stencil 分类等 uniform 色路径)
    ShaderProgram* vectorFillShader() const;

    /// 矢量点符号/图标 billboard shader（矢量 P5a 解析 SDF 形状 + P6c 位图
    /// 图集通道;编辑手柄/Point 要素/marker 共用）
    ShaderProgram* vectorPointShader() const;

    /// 位图图标图集（矢量 P6c;图标位图由应用层经 Engine 注入）
    IconAtlas* iconAtlas() const;

    /// 矢量文字标注 shader（矢量 P5b,SDF 字形 + halo）
    ShaderProgram* vectorLabelShader() const;

    /// SDF 字形图集（矢量 P5b;字体字节由应用层经 Engine 注入）
    GlyphAtlas* glyphAtlas() const;

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

    /// 地形实例化合批命令(方案 A/Step 3)。共享 32B 位移模板 VBO/IBO +
    /// 96B per-instance 流(kTerrainInstanceStride:rel 帧 + dispMorph +
    /// clipUv + layers)。kind=GltfPrimitiveInstanced,shader=terrainInstanced,
    /// instanceStride 与 glTF 实例区分 → 后端分派 Terrain32Instanced 布局。
    RenderCommand makeTerrainInstancedCommand(Buffer* vertexBuffer,
                                              Buffer* indexBuffer,
                                              Buffer* instanceBuffer,
                                              int indexCount,
                                              int vertexCount,
                                              int instanceCount) const;
    ShaderProgram* terrainInstancedShader() const;

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
    /// 默认开(2026-07-23 收口):历史「影像启动链竞态死锁」已定位并根治——
    /// 真因不在影像链(mapping→fetch→attach 每帧健康推进),而是
    /// rebuildCachedDrawCommands 循环入口护栏把 sharedTemplateGeometry 的
    /// 有意空 buffer primitive 无条件 continue,模板 swap 永远执行不到→常驻
    /// 命令缓存定格为空→无 surface command→presentation hold 永不释放(黑屏
    /// 100% 确定性复现,非偶发竞态)。修 = 入口护栏放行 + swap 失败不定格空
    /// 缓存(下一帧重试自愈)。flag 保留作紧急回退开关(false = 恢复 per-tile
    /// baked VBO 全量构建/上传,行为与 P5b 前逐字节一致)。
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
    // P5b skip flag(默认开,见 setTerrainBakedVboSkipEnabled 注释)。
    bool terrainBakedVboSkipEnabled_ = true;
};

} // namespace earth_engine
