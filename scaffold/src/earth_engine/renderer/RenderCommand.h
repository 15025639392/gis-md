#pragma once

#include <initializer_list>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include "GltfUniformBlock.h"
#include "RenderDevice.h"  // for Texture/Buffer/ShaderProgram/Framebuffer forward decls

namespace earth_engine {

static constexpr int kMaxSurfaceImageryOverlays = 4;
static constexpr int kGltfRasterOverlayTextureBase = 15;
static constexpr int kMaxGltfRasterOverlays = 4;
static constexpr int kGltfWaterMaskTextureSlot =
    kGltfRasterOverlayTextureBase + kMaxGltfRasterOverlays;
// 北极星合成方案页存储(Step 3):sampler2DArray 页存储占 water mask 之后的下一槽。
// 仅目标 capped 瓦片的 terrain 命令绑定此槽(见 GltfDrawCommandBuilder 门控)。
static constexpr int kGltfPageStoreArrayTextureSlot =
    kGltfWaterMaskTextureSlot + 1;
// 稀疏虚拟纹理(Step B1):per-tile 间接纹理(RGBA8 编 layer 索引),片元经它
// 单次 NEAREST fetch 定位 array 层,替代闭式公式。紧挨页存储 array 槽(20→21)。
static constexpr int kGltfPageStoreIndirTextureSlot =
    kGltfPageStoreArrayTextureSlot + 1;
// 北极星 Phase 2c Stage B:per-tile 高度纹理槽(**顶点级** sampler,首个;地形
// 顶点 shader texelFetch 取回归一化高度反量化位移)。紧接页存储 indir 槽(21→22)。
static constexpr int kGltfHeightTextureSlot =
    kGltfPageStoreIndirTextureSlot + 1;
// 刀2 路网 SDF 场"第二平面":R8 sampler2DArray,与页存储影像 array 同层号
// 驻留,FS 在同一次间接查找下再采一次做贴地线解算。紧接高度纹理槽(22→23)。
// ⚠️ 新增最高槽的孪生同步点:RenderCommandTextureList::kCapacity(本文件,
// 随本常量自动)、RenderDeviceGLES 的 currentTextures 容量与 setSampler 登记、
// Metal 绑定循环上限 —— 漏任何一处该纹理永不绑定,texelFetch 恒 0。
static constexpr int kGltfRoadFieldTextureSlot = kGltfHeightTextureSlot + 1;
static constexpr int kGltfInstanceMatrixStride = 100;
// 地形合批(Step 3)实例流步长:6× vec4 = 96B。rel 帧 3 行(相对批参考帧
// frame0)+ dispMorph(minH·fade,range·fade,morphFactor,gridN)+ clipUv +
// layers(heightLayer,indirLayer,clipEnabled,_)。与 kGltfInstanceMatrixStride
// 不同值 → 后端据此分派 Terrain32Instanced 布局(非 Gltf/Surface 实例布局)。
static constexpr int kTerrainInstanceStride = 128;

/// 固定容量纹理槽表（inline 存储，无堆分配）。
/// 命令拷贝是每帧热路径——常驻缓存命令逐可见瓦片拷进帧列表，vector 版本
/// 每次拷贝付一次堆分配；槽位全集有硬上限（material 0-14、raster overlay
/// 15-18、water mask 19 = kGltfWaterMaskTextureSlot），定容数组即可承载。
/// API 与既有 vector 用法兼容（size/operator[]/front/push_back/resize）。
class RenderCommandTextureList {
public:
    static constexpr size_t kCapacity =
        static_cast<size_t>(kGltfRoadFieldTextureSlot) + 1u;

    RenderCommandTextureList() = default;
    RenderCommandTextureList(std::initializer_list<Texture*> init) {
        for (Texture* texture : init) {
            push_back(texture);
        }
    }

    Texture** begin() { return items_.data(); }
    Texture** end() { return items_.data() + size_; }
    Texture* const* begin() const { return items_.data(); }
    Texture* const* end() const { return items_.data() + size_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    Texture*& operator[](size_t index) { return items_[index]; }
    Texture* operator[](size_t index) const { return items_[index]; }
    Texture*& front() { return items_[0]; }
    Texture* front() const { return items_[0]; }
    void clear() {
        items_.fill(nullptr);
        size_ = 0;
    }
    // 超容量的 push_back 丢弃（槽位全集恒 <= kCapacity；防御式而非契约）。
    void push_back(Texture* texture) {
        if (size_ < kCapacity) {
            items_[size_++] = texture;
        }
    }
    void resize(size_t newSize, Texture* fill = nullptr) {
        newSize = newSize < kCapacity ? newSize : kCapacity;
        for (size_t i = size_; i < newSize; ++i) {
            items_[i] = fill;
        }
        for (size_t i = newSize; i < size_; ++i) {
            items_[i] = nullptr;
        }
        size_ = newSize;
    }

private:
    std::array<Texture*, kCapacity> items_{};
    size_t size_ = 0;
};

enum class RenderCommandKind {
    Unknown,
    SkyBackground,        // order 0: skybox / starfield
    AtmosphereBackground,  // order 5: atmospheric scattering
    GltfPrimitive,         // order 15
    GltfPrimitiveInstanced, // order 15
    VectorOverlay,         // order 30
    // 矢量数据系统 P1/P5(设计 §6.1):FeatureStore 桶几何的三类命令。
    // 与 VectorOverlay(旧 GeoJSON 标注路径)平行,同 order 30。
    VectorFill,            // order 30: 多边形 fill(CDT 三角化,pos-only 顶点)
    VectorLine,            // order 30: 线 ribbon(44B 顶点,shader 屏幕挤出)
    VectorPoint,           // order 30: 点符号 billboard(20B 顶点,SDF 圆)
    VectorLabel,           // order 31: SDF 文字标注(28B 顶点,压最后画)
    // P6 stencil 终态贴地(设计 §7.2 方案 B):面 fill 挤成水密体做阴影体
    // 分类,像素级贴合已渲染地形深度。同一体积几何两条相邻命令:
    // ClassifyVolume(z-fail 计数)→ ClassifyColor(stencil≠0 着色并清零)。
    VectorStencil          // order 29: 分类 fill 压在其它矢量之下
};

/// VectorStencil 命令的阶段(后端据此设置 stencil/colorMask 组合)。
enum class StencilPhase : uint8_t {
    None,            ///< 非 stencil 命令(默认)
    ClassifyVolume,  ///< 体 pass:两侧 z-fail INCR/DECR_WRAP,关颜色写,深度测开/写关
    ClassifyColor    ///< 色 pass:NOTEQUAL 0 着色,op ZERO 顺手清零,关深度测,开混合
};

enum class TerrainSurfaceCommandSource {
    Unknown,
    RealTerrain,
    FillProxy,
    EllipsoidFallback
};

/// 单条渲染命令。
/// 由 Layer::buildRenderCommands() 生成，Renderer 收集并提交给 RenderDevice。
struct RenderCommand {
    RenderCommandKind kind = RenderCommandKind::Unknown;
    std::string owner;     // layer id（调试用）
    std::string pass;      // "depth" | "color" | "picking" | "shadow" | "postprocess"
    // Stable identity for long-lived renderables. Transient commands leave this
    // empty and are rebuilt directly into the frame list.
    std::string stableKey;
    uint64_t frameId = 0;
    uint64_t generation = 0;
    bool terrainRenderContent = false;
    TerrainSurfaceCommandSource terrainSurfaceSource =
        TerrainSurfaceCommandSource::Unknown;

    // GPU 资源引用（裸指针，生命周期由 RenderDevice 管理）
    ShaderProgram* shader = nullptr;
    Buffer* vertexBuffer = nullptr;
    Buffer* indexBuffer = nullptr;
    Buffer* instanceBuffer = nullptr;
    RenderCommandTextureList textures;
    // Optional short-lived resource owners for raw pointers above. Commands can
    // keep raster/content resources alive through submit without taking over
    // renderer ownership.
    std::vector<std::shared_ptr<const void>> resourceKeepAlive;

    // 绘制参数
    int vertexCount = 0;       // glDrawArrays 的顶点数（indexBuffer 为 null 时使用）
    int indexCount = 0;        // glDrawElements 的索引数
    int indexOffset = 0;       // 起始索引偏移，单位为“索引个数”（非字节）；各后端按 indexType 大小自行换算字节偏移
    int vertexStride = 0;      // bytes per vertex (0=auto, 32=surface, 40=terrain, 120=glTF)
    int instanceCount = 0;
    int instanceStride = 0;
    enum class PrimitiveType { Triangles, TriangleStrip, Lines, LineStrip, Points } primitive = PrimitiveType::Triangles;
    enum class IndexType { UInt16, UInt32 } indexType = IndexType::UInt16;

    // 渲染状态
    bool depthTest = true;
    bool depthWrite = true;
    bool blend = false;
    // P6 stencil 分类阶段(仅 VectorStencil kind 非 None;后端翻译成
    // glStencil*/colorMask 状态,None = 关 stencil 测试)。
    StencilPhase stencilPhase = StencilPhase::None;
    // 实例化 blend/透射 primitive 走 alpha-to-coverage(MSAA 覆盖抖动)而非逐实例
    // alpha 排序:顺序无关、单 draw 画完所有实例,避免 blend 实例化退化成每实例
    // 一个 draw call(见 GltfDrawCommandBuilder + RenderDeviceGLES)。true 时后端
    // 启用 GL_SAMPLE_ALPHA_TO_COVERAGE 且不开常规 alpha 混合。
    bool alphaToCoverage = false;
    bool cullFace = true;
    /// cullFace=true 时剔除哪一面。默认 Back(常规实体)。
    ///
    /// Front 目前只有一个用途:stencil 分类的**色 pass**。那一 pass 的几何是
    /// 挤出体、着色的却是 stencil 选中的地形像素,所以覆盖面只要「每个选中像素
    /// 至少被盖一次」即可 —— 水密体的背面单独就满足,画双面等于把光栅化量翻倍
    /// 换零收益。取背面而非正面是因为**相机进体内时正面被近平面切掉**,背面
    /// 永远在。
    enum class CullMode { Back, Front };
    CullMode cullMode = CullMode::Back;
    enum class BlendFactor { SrcAlpha, OneMinusSrcAlpha } blendSrc = BlendFactor::SrcAlpha;
    enum class BlendFactorDst { OneMinusSrcAlpha, One } blendDst = BlendFactorDst::OneMinusSrcAlpha;

    // World-space center used by camera-aware transparent sorting.
    // glTF BLEND and fade commands are only valid once Scene has converted this
    // to a camera-space translucentSortDepth for the current frame.
    bool hasWorldSortCenter = false;
    std::array<double, 3> worldSortCenter{0.0, 0.0, 0.0};
    bool hasTranslucentSortDepth = false;
    double translucentSortDepth = 0.0;

    // Uniform 数据（name → float 数组）。
    // 仅供冷路径命令使用（SkyBackground / AtmosphereBackground /
    // VectorOverlay 等，每帧个位数条目）。glTF/terrain 热路径命令必须走
    // 下方 gltfUniforms 定长块并保持本 map 为空——空 map 拷贝零分配。
    std::unordered_map<std::string, std::vector<float>> uniforms;

    // glTF / terrain 命令的定长 uniform 块（热路径，见 GltfUniformBlock.h）。
    // hasGltfUniforms=true 时后端整块消费：Metal 一次 setBytes 绑
    // fragment buffer(0)，GLES 经 program 级 location 表直传。
    bool hasGltfUniforms = false;
    GltfUniformBlock gltfUniforms;

    // 北极星 Phase 2c（地形 GPU 位移）：per-tile 全刚体模型帧（ENU→ECEF），
    // 承载共享位移模板的经纬度落位。CPU-only、不上 GPU（不进 GltfUniformBlock/
    // MSL 镜像/描述表）——SceneRenderCommandUniformUpdater 每帧双精度 compose
    // mvp = viewProj·frame（替 translate(modelOrigin) 平移路径）并据其反算
    // eyePositionRTC。列主序 16 double（与 Mat4::raw()/glm 一致）。默认 false
    // → 所有现有 glTF/terrain 命令逐字节走原 translation 路径，零回归。
    bool hasTerrainDisplacementFrame = false;
    std::array<double, 16> terrainDisplacementModelMatrix{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
    // 合批 Step 1(CPU-only):本命令高度纹理占用的共享 array (layer, epoch)。
    // 层被 LRU 重分配后 epoch 失配 → build 侧 invalidate 命令缓存自愈重建。
    int terrainHeightLayer = -1;
    uint32_t terrainHeightLayerEpoch = 0;
    // 该命令高度纹理所在的密度档(层边长-1)。自适应密度下 coarse/dense 各有独立
    // array + LRU + epoch,故校验/保活都必须带上档位。
    int terrainHeightGridSize = 0;
    // 合批 Step 3(CPU-only):pageStore 本瓦片全 cell 高清页驻留 → mappedRaster
    // fallback 必不被采样 → 该命令可进实例化批(批 shader 丢 mappedRaster)。
    // 由 TerrainPageStore::applyToTerrainCommand 设,TerrainInstanceBatcher 读。
    bool terrainPageStoreFullyResident = false;
    // [瓦界对齐] instanced 管线专用:几何 UV(共享模板边到边 0..1)→ 源格的逐瓦
    // 仿射 {c0.x,c0.y,dU.x,dU.y,dV.x,dV.y}(见 TerrainPageStore::TileIndir::
    // geomAffine)。逐瓦 shader 不消费(其 psUv 是 details 逐顶点 texcoord,走
    // gltfUniforms.pageStoreUv 的 origin/span);TerrainInstanceBatcher 打包进
    // 实例记录 pageUv + pageAux.zw。
    std::array<float, 6> terrainPageGeomAffine{0.0f, 0.0f, 1.0f,
                                               0.0f, 0.0f, 1.0f};

    // 地表(地形)命令的固定存储 uniform。放在定长字段里而不是 uniforms 字符串
    // 表,是为了避免每瓦片一次 unordered_map/string/vector 分配。
    // ⚠️ `surface*` 这个前缀是历史遗留:它们原属已删除的 SurfaceTile 绘制路径
    // (2026-08-07 整链删除),现在由 GltfPrimitive 的地形命令继续使用。
    std::array<float, 4> surfaceClipUv{0.0f, 0.0f, 1.0f, 1.0f};
    float surfaceTransitionOpacity = 1.0f;
    float surfaceClipEnabled = 0.0f;
    // 加载质量诊断(CPU-only,不进 GPU)。旧绘制路径删除后这两个字段一度
    // 无人赋值恒 -1(诊断层的 z/texZ 因此恒 0),现由 GltfDrawCommandBuilder 在
    // 绑定 BaseImagery 时填回真值。
    int surfaceGeometryZoom = -1;   // 本瓦片几何层级
    int surfaceTextureZoom = -1;    // 实际贴上的底图影像层级
    // 「糊几级」:0 = 用的是本级影像(清晰);N>0 = 想要的影像还没到,退回用了
    // N 级之上的祖先纹理上采样(屏幕上就是糊块)。-1 = 没有底图影像。
    // 影像层级与几何层级无法互推(纹理/几何解耦后几何 cap 在 z12、影像可到
    // z18),所以必须单独记,不能用上面两者相减。
    int imageryAncestorLevelDelta = -1;
    int surfaceMeshIndexCount = 0;
    int surfaceNoSkirtIndexCount = 0;
    int surfaceSkirtIndexCount = 0;
    int surfaceBaseRasterState = 0;
    int surfaceBaseIsMappedRasterTile = 0;

    // glTF raster overlays use _CESIUMOVERLAY_n attributes and separate
    // material texture slots, so they cannot reuse surface overlay samplers.
    std::array<std::array<float, 4>, kMaxGltfRasterOverlays>
        gltfRasterOverlayTileUvs{};
    std::array<float, kMaxGltfRasterOverlays> gltfRasterOverlayOpacities{};
    std::array<float, kMaxGltfRasterOverlays> gltfRasterOverlayTexCoordSets{};
    int gltfRasterOverlayTextureCount = 0;
    float gltfHasWaterMask = 0.0f;
    std::array<float, 4> gltfWaterMaskTranslationScale{
        0.0f,
        0.0f,
        1.0f,
        0.0f};
    std::array<float, 4> gltfWaterMaskState{1.0f, 0.0f, 0.0f, 0.0f};
};

/// 渲染命令列表（每帧一帧）
using RenderCommandList = std::vector<RenderCommand>;

struct RenderCommandValidationError {
    size_t commandIndex = 0;
    std::string owner;
    std::string message;
};

/// MVP 3D globe 主链路固定顺序：surface -> vector。
int mvpRenderOrder(RenderCommandKind kind);

/// 对 MVP 主链路的 pass/depth/cull/blend 状态做硬校验。
/// 非 MVP 命令必须标为 Unknown 或新增独立 kind 后再定义规则。
std::optional<RenderCommandValidationError>
validateMvpRenderCommands(const RenderCommandList& commands,
                          uint64_t expectedFrameId = 0);

void sortMvpRenderCommands(RenderCommandList& commands);

} // namespace earth_engine
