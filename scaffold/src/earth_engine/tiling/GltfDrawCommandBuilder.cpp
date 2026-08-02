#include "GltfDrawCommandBuilder.h"

#include "RasterMappedToTilesetTile.h"
#include "SurfaceRasterBinding.h"
#include "TileCacheKey.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TilesetTile.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../renderer/Renderer.h"
#include "../renderer/TerrainPageStore.h"
#include "../content/TerrainDisplacementTemplate.h"
#include "../providers/TerrainProvider.h"  // DecodedHeightmap
#include "TerrainDisplacementTemplatePool.h"
#include "../debug/PerfTimer.h"
#include "../debug/PlatformLog.h"

#include <algorithm>
#include <string>
#include <utility>

namespace earth_engine {

namespace {

// 北极星 Phase 2c:地形位移随 LOD 连续衰减到平（GE 式「从太空看是光滑椭球，
// 靠近才长出起伏」）。粗/远瓦片（低 z，占屏大、facet 达上百 km）的真实 relief
// 在全球视角地平线极掠视下会被粗网格 faceting 成规则钻石尖刺 + 边缘 skirt 墙外露；
// 这些瓦片的 relief 本就不可分辨，应压平。fade→0 时 shader 位移 h=fade·h_orig=0
// → 停在椭球面（与 GPU:OFF 平椭球一致，chevron/skirt 墙全消）；fade→1 时全起伏。
// 仅缩放 minHeight/heightRange（h=minH+t·range 两端同乘 = h 整体乘 fade），零 shader
// 改动、零新 uniform、skirt 哨兵与 enabled 逻辑不受影响。maximumZoom=12，近景恒 z10-12
// → fade=1 无回归；全球视角 z1-5 → fade≈0 平椭球。
// terrainReliefFade 本体已挪 TerrainDisplacementTemplatePool.h(P5b:prepare 侧
// 跳过废弃 per-tile VBO 的判据与本文件模板 swap 判据必须同源)。

}  // namespace

// B2a 门② 页面 determination 也调用它(经 GltfDrawCommandBuilder.h 暴露),
// 故置于 earth_engine 具名作用域(非匿名 namespace)以获外部链接。
TerrainSurfaceCommandSource terrainSurfaceSourceForDraw(
    const TileRenderContentState& renderContent) {
    if (renderContent.drawsFill()) {
        return TerrainSurfaceCommandSource::FillProxy;
    }
    switch (renderContent.currentSurfaceSource()) {
        case SurfaceDrawableSource::EllipsoidFallback:
            return TerrainSurfaceCommandSource::EllipsoidFallback;
        case SurfaceDrawableSource::None:
            return TerrainSurfaceCommandSource::Unknown;
        case SurfaceDrawableSource::HeightmapTerrain:
        case SurfaceDrawableSource::AncestorUpsample:
        case SurfaceDrawableSource::GltfContent:
        default:
            return TerrainSurfaceCommandSource::RealTerrain;
    }
}

namespace {

float alphaModeUniform(GltfAlphaMode mode) {
    switch (mode) {
        case GltfAlphaMode::Mask:
            return 1.0f;
        case GltfAlphaMode::Blend:
            return 2.0f;
        case GltfAlphaMode::Opaque:
        default:
            return 0.0f;
    }
}

RenderCommand::PrimitiveType renderPrimitiveType(GltfPrimitiveMode mode) {
    switch (mode) {
        case GltfPrimitiveMode::Points:
            return RenderCommand::PrimitiveType::Points;
        case GltfPrimitiveMode::Lines:
            return RenderCommand::PrimitiveType::Lines;
        case GltfPrimitiveMode::LineStrip:
            return RenderCommand::PrimitiveType::LineStrip;
        case GltfPrimitiveMode::TriangleStrip:
            return RenderCommand::PrimitiveType::TriangleStrip;
        case GltfPrimitiveMode::Triangles:
        case GltfPrimitiveMode::TriangleFan:
        default:
            return RenderCommand::PrimitiveType::Triangles;
    }
}

/// 内容不变式重建:几何/材质/水面掩码/stableKey 一次性写进 tile 常驻缓存
/// (cesium per-tile DrawCommand 语义)。仅在缓存失效时执行——GPU 资源
/// (重)创建、动画改写、内容卸载都会经 TileRenderContentState 的失效点。
/// 每帧字段(frameId/opacity/blend 派生/clip/overlay 绑定)一律不写在常驻
/// 命令上,由 applyPerFrameCommandState 盖在帧列表副本上。
void rebuildCachedDrawCommands(Renderer& renderer, TilesetTile& tile,
                               uint64_t frameNumber) {
    RenderCommandList& cached =
        tile.content.renderContent.restartCachedDrawCommands();
    const std::string tileCacheKey = TileCacheKey::forTile(tile.key);
    size_t stableIndex = 0;
    // Draw-effective resources: real terrain if renderable, else the ellipsoid
    // fill proxy. All three per-primitive reads below (resources, terrain flag,
    // local origin) go through the draw-effective getters so the fill draws
    // through the identical command path and swaps to real geometry the frame
    // real terrain becomes ready (which invalidates the cache).
    bool sharedTemplateSwapFailed = false;
    for (const GltfPrimitiveRenderResources& primitive :
         tile.content.renderContent.drawPrimitiveResources()) {
        // P5b:共享模板几何 primitive 的 per-tile buffer 有意为空(nullptr),
        // 必须放行到下方模板 swap 换绑共享 VBO/IBO;swap 未成时由循环尾的
        // null-buffer 兜底丢弃。其余 primitive 保持原护栏。
        if (!primitive.sharedTemplateGeometry &&
            (!primitive.vertexBuffer || !primitive.indexBuffer ||
             primitive.indexCount <= 0)) {
            continue;
        }

        RenderCommand cmd;
        if (primitive.instanceCount > 0) {
            cmd = renderer.makeGltfPrimitiveInstancedCommand(
                primitive.vertexBuffer.get(),
                primitive.indexBuffer.get(),
                primitive.instanceBuffer.get(),
                primitive.indexCount,
                primitive.vertexCount,
                primitive.instanceCount);
        } else if (primitive.useTerrainVertexFormat) {
            // Terrain quantized-mesh primitive: 28-byte compact TerrainGpuVertex VBO
            // drawn with the dedicated lightweight terrain shader. The
            // per-command population below (water mask, material subset)
            // stays identical — the terrain shader consumes the subset of
            // uniforms it declares and ignores the rest.
            cmd = renderer.makeTerrainPrimitiveCommand(
                primitive.vertexBuffer.get(),
                primitive.indexBuffer.get(),
                primitive.indexCount,
                primitive.vertexCount);
        } else {
            cmd = renderer.makeGltfPrimitiveCommand(
                primitive.vertexBuffer.get(),
                primitive.indexBuffer.get(),
                primitive.indexCount,
                primitive.vertexCount);
        }
        cmd.indexType = primitive.indexByteSize ==
                static_cast<int>(sizeof(uint16_t))
            ? RenderCommand::IndexType::UInt16
            : RenderCommand::IndexType::UInt32;
        cmd.stableKey = tileCacheKey + "#" + std::to_string(stableIndex++);
        cmd.terrainRenderContent =
            tile.content.renderContent.drawIsTerrainContent();
        if (cmd.terrainRenderContent) {
            cmd.terrainSurfaceSource =
                terrainSurfaceSourceForDraw(tile.content.renderContent);
        }
        cmd.primitive = renderPrimitiveType(primitive.primitiveMode);
        const Vec3& localOrigin = tile.content.renderContent.drawLocalOrigin();
        GltfUniformBlock& u = cmd.gltfUniforms;
        u.modelOrigin = {
            static_cast<float>(localOrigin.x()),
            static_cast<float>(localOrigin.y()),
            static_cast<float>(localOrigin.z())};
        // geomorph:瓦片中心椭球法线(morph 方向)。地心 up 近似=normalize(ECEF 中心),
        // 与测地法线差 <0.2°,morph 视觉足够;a_position 在 ECEF 平移轴系故方向不变。
        // w(morphFactor)每帧由 applyPerFrameCommandState 盖章为 transitionOpacity。
        if (cmd.terrainRenderContent) {
            const double upLen = localOrigin.length();
            if (upLen > 1.0) {
                u.geomorphUpFactor = {
                    static_cast<float>(localOrigin.x() / upLen),
                    static_cast<float>(localOrigin.y() / upLen),
                    static_cast<float>(localOrigin.z() / upLen),
                    1.0f};
            }
        }
        cmd.hasWorldSortCenter = true;
        cmd.worldSortCenter = {
            primitive.sortCenterEcef.x(),
            primitive.sortCenterEcef.y(),
            primitive.sortCenterEcef.z()};
        u.baseColor = {
            primitive.baseColorFactor[0],
            primitive.baseColorFactor[1],
            primitive.baseColorFactor[2],
            primitive.baseColorFactor[3]};
        u.hasBaseColorTexture =
            primitive.baseColorTexture.texture ? 1.0f : 0.0f;
        u.materialFactors = {
            primitive.metallicFactor,
            primitive.roughnessFactor,
            primitive.normalTextureScale,
            primitive.occlusionTextureStrength};
        u.dielectricSpecularF0 = primitive.dielectricSpecularF0;
        u.specularFactor = primitive.specularFactor;
        u.specularColorFactor = {
            primitive.specularColorFactor[0],
            primitive.specularColorFactor[1],
            primitive.specularColorFactor[2]};
        u.specularGlossinessWorkflow =
            primitive.specularGlossinessWorkflow ? 1.0f : 0.0f;
        u.specularGlossinessFactor = {
            primitive.specularGlossinessSpecularFactor[0],
            primitive.specularGlossinessSpecularFactor[1],
            primitive.specularGlossinessSpecularFactor[2],
            primitive.specularGlossinessGlossinessFactor};
        u.transmissionFactor = primitive.transmissionFactor;
        u.anisotropyFactors = {
            primitive.anisotropyStrength,
            primitive.anisotropyRotation};
        u.clearcoatFactors = {
            primitive.clearcoatFactor,
            primitive.clearcoatRoughnessFactor,
            primitive.clearcoatNormalTextureScale};
        u.sheenColorFactor = {
            primitive.sheenColorFactor[0],
            primitive.sheenColorFactor[1],
            primitive.sheenColorFactor[2]};
        u.sheenRoughnessFactor = primitive.sheenRoughnessFactor;
        u.hasMaterialTextures = {
            primitive.metallicRoughnessTexture.texture ? 1.0f : 0.0f,
            primitive.normalTexture.texture ? 1.0f : 0.0f,
            primitive.occlusionTexture.texture ? 1.0f : 0.0f,
            primitive.emissiveTexture.texture ? 1.0f : 0.0f};
        u.hasAnisotropyTexture =
            primitive.anisotropyTexture.texture ? 1.0f : 0.0f;
        u.hasSpecularTextures = {
            primitive.specularTexture.texture ? 1.0f : 0.0f,
            primitive.specularColorTexture.texture ? 1.0f : 0.0f};
        u.hasSpecularGlossinessTexture =
            primitive.specularGlossinessTexture.texture ? 1.0f : 0.0f;
        u.hasTransmissionTexture =
            primitive.transmissionTexture.texture ? 1.0f : 0.0f;
        u.hasClearcoatTextures = {
            primitive.clearcoatTexture.texture ? 1.0f : 0.0f,
            primitive.clearcoatRoughnessTexture.texture ? 1.0f : 0.0f,
            primitive.clearcoatNormalTexture.texture ? 1.0f : 0.0f};
        u.hasSheenTextures = {
            primitive.sheenColorTexture.texture ? 1.0f : 0.0f,
            primitive.sheenRoughnessTexture.texture ? 1.0f : 0.0f};
        u.emissiveFactor = {
            primitive.emissiveFactor[0],
            primitive.emissiveFactor[1],
            primitive.emissiveFactor[2]};
        u.textureCoordSets = {
            static_cast<float>(primitive.baseColorTexture.texCoord),
            static_cast<float>(primitive.metallicRoughnessTexture.texCoord),
            static_cast<float>(primitive.normalTexture.texCoord),
            static_cast<float>(primitive.occlusionTexture.texCoord)};
        u.emissiveTexCoordSet =
            static_cast<float>(primitive.emissiveTexture.texCoord);
        u.anisotropyTexCoordSet =
            static_cast<float>(primitive.anisotropyTexture.texCoord);
        u.specularTexCoordSets = {
            static_cast<float>(primitive.specularTexture.texCoord),
            static_cast<float>(primitive.specularColorTexture.texCoord)};
        u.specularGlossinessTexCoordSet =
            static_cast<float>(primitive.specularGlossinessTexture.texCoord);
        u.transmissionTexCoordSet =
            static_cast<float>(primitive.transmissionTexture.texCoord);
        u.clearcoatTexCoordSets = {
            static_cast<float>(primitive.clearcoatTexture.texCoord),
            static_cast<float>(primitive.clearcoatRoughnessTexture.texCoord),
            static_cast<float>(primitive.clearcoatNormalTexture.texCoord)};
        u.sheenTexCoordSets = {
            static_cast<float>(primitive.sheenColorTexture.texCoord),
            static_cast<float>(primitive.sheenRoughnessTexture.texCoord)};
        u.alphaMode = alphaModeUniform(primitive.alphaMode);
        u.alphaCutoff = primitive.alphaCutoff;
        u.unlit = primitive.unlit ? 1.0f : 0.0f;
        // 实例化 blend/透射 primitive 用 alpha-to-coverage 保持单 draw(顺序无关),
        // 否则逐实例 alpha 排序会退化成每实例一个 draw call(N 爆炸+大 N OOM)。
        // 内容不变式:仅取决于材质 alphaMode/透射与是否实例化。
        cmd.alphaToCoverage =
            primitive.instanceCount > 0 &&
            (primitive.alphaMode == GltfAlphaMode::Blend ||
             primitive.transmissionFactor > 0.0f);

        auto setTransform = [](
            GltfUniformBlock::TextureTransform& transform,
            const GltfPrimitiveRenderResources::TextureBinding& binding) {
            transform.offsetScale = binding.offsetScale;
            transform.rotationSinCos = binding.rotationSinCos;
        };
        setTransform(u.baseColorTex, primitive.baseColorTexture);
        setTransform(u.metallicRoughnessTex, primitive.metallicRoughnessTexture);
        setTransform(u.anisotropyTex, primitive.anisotropyTexture);
        setTransform(u.specularTex, primitive.specularTexture);
        setTransform(u.specularColorTex, primitive.specularColorTexture);
        setTransform(
            u.specularGlossinessTex, primitive.specularGlossinessTexture);
        setTransform(u.transmissionTex, primitive.transmissionTexture);
        setTransform(u.clearcoatTex, primitive.clearcoatTexture);
        setTransform(
            u.clearcoatRoughnessTex, primitive.clearcoatRoughnessTexture);
        setTransform(u.clearcoatNormalTex, primitive.clearcoatNormalTexture);
        setTransform(u.sheenColorTex, primitive.sheenColorTexture);
        setTransform(u.sheenRoughnessTex, primitive.sheenRoughnessTexture);
        setTransform(u.normalTex, primitive.normalTexture);
        setTransform(u.occlusionTex, primitive.occlusionTexture);
        setTransform(u.emissiveTex, primitive.emissiveTexture);

        cmd.textures.resize(15, nullptr);
        cmd.textures[0] = primitive.baseColorTexture.texture;
        cmd.textures[1] = primitive.metallicRoughnessTexture.texture;
        cmd.textures[2] = primitive.normalTexture.texture;
        cmd.textures[3] = primitive.occlusionTexture.texture;
        cmd.textures[4] = primitive.emissiveTexture.texture;
        cmd.textures[5] = primitive.specularTexture.texture;
        cmd.textures[6] = primitive.specularColorTexture.texture;
        cmd.textures[7] = primitive.clearcoatTexture.texture;
        cmd.textures[8] = primitive.clearcoatRoughnessTexture.texture;
        cmd.textures[9] = primitive.clearcoatNormalTexture.texture;
        cmd.textures[10] = primitive.sheenColorTexture.texture;
        cmd.textures[11] = primitive.sheenRoughnessTexture.texture;
        cmd.textures[12] = primitive.anisotropyTexture.texture;
        cmd.textures[13] = primitive.specularGlossinessTexture.texture;
        cmd.textures[14] = primitive.transmissionTexture.texture;
        const bool primitiveHasWaterMask =
            primitive.hasTerrainWaterMaskMetadata &&
            !primitive.terrainOnlyLand &&
            (primitive.terrainOnlyWater ||
             primitive.terrainWaterMaskTexture != nullptr);
        if (primitiveHasWaterMask) {
            if (cmd.textures.size() <=
                static_cast<size_t>(kGltfWaterMaskTextureSlot)) {
                cmd.textures.resize(
                    static_cast<size_t>(kGltfWaterMaskTextureSlot) + 1u,
                    nullptr);
            }
            cmd.textures[kGltfWaterMaskTextureSlot] =
                primitive.terrainWaterMaskTexture;
            cmd.gltfHasWaterMask = 1.0f;
            cmd.gltfWaterMaskTranslationScale =
                primitive.terrainWaterMaskTranslationScale;
            cmd.gltfWaterMaskState = {
                primitive.terrainOnlyLand ? 1.0f : 0.0f,
                primitive.terrainOnlyWater ? 1.0f : 0.0f,
                primitive.terrainWaterMaskTexture ? 1.0f : 0.0f,
                0.0f};
        }
        u.hasWaterMask = cmd.gltfHasWaterMask;
        u.waterMaskTranslationScale = cmd.gltfWaterMaskTranslationScale;
        u.waterMaskState = cmd.gltfWaterMaskState;

        // 北极星 Phase 2c 地形 GPU 位移(flag-gated,仿页存储非空门控):把**真实
        // 地形**命令改绑共享位移模板 VBO/IBO(同 {LOD,row} 复用)+ per-tile 刚体
        // ENU→ECEF 帧(承载落位)。池空=未启用 → 保持现 per-tile baked VBO,零回归。
        // **只动 RealTerrain,不碰 FillProxy**:fill 本就是贴椭球代理(小 VBO),覆写它
        // 会扰乱 fill→real 生命周期(实测导致 fill 瓦片卡住不转 real + 每帧 fill 预备
        // 开销);且 §5 有界目标针对的是真实地形的大 per-tile VBO,非 fill。
        // **只对有自有高度图的瓦片走共享模板 GPU 位移**:原生 z≤12 瓦片有
        // retainedHeightmap → 建/取 per-tile 高度纹理,shader 沿法线位移+双分辨率
        // morph。上采样 z13+ 瓦片(父级裁剪来,无自有高度图)保持现 per-tile baked
        // VBO(其顶点已含父级真实高度)——绝不绑平模板(否则 enabled=0 → 深近景
        // 变平椭球回归)。先取高度纹理、成功才换模板 VBO;高度图缺失/纹理未就绪
        // 一律不碰 cmd(零回归回落 CPU baked VBO)。
        // **必须放在材质纹理块之后**:上面的 cmd.textures.resize(15) + water mask
        // 会重置纹理表,若在其前设置高度纹理槽(slot 22)会被 resize(15) 清空
        // → texelFetch 恒 0 → 地形平抬无起伏(真机踩过的排序 bug)。
        // 位移随 LOD 连续衰减到平(见文件顶 terrainReliefFade 注释)。fade≈0 的
        // 粗/远瓦片**完全跳过位移路径** → 保持原 CPU baked VBO + gltfShader(平、
        // 光滑,= GPU:OFF),既消除 chevron/skirt 墙,又避免平模板在地平线极掠视下
        // 的粗 facet 微纹(纯缩放 h→0 仍绑粗模板会留 limb faceting)。fade>0 才绑模板。
        const float reliefFade =
            (cmd.terrainRenderContent &&
             cmd.terrainSurfaceSource ==
                 TerrainSurfaceCommandSource::RealTerrain)
                ? terrainReliefFade(tile.key.z)
                : 0.0f;
        if (reliefFade > 0.001f &&
            cmd.terrainSurfaceSource ==
                TerrainSurfaceCommandSource::RealTerrain) {
            if (TerrainDisplacementTemplatePool* pool =
                    renderer.terrainDisplacementPool()) {
                const DecodedHeightmap* hm =
                    tile.content.renderContent.retainedHeightmap();
                // 自适应几何密度(单一事实源 terrainGridSizeForSse):几何 cap 在
                // DEM native max LOD 的近景瓦片 SSE 远超阈值 → 升 dense 档,拿回
                // 源数据里那 8 倍高程细节;正常细化的远瓦片留 coarse 档,故总三角
                // 数由屏幕面积封顶而非随可见瓦片数增长(见头文件推导)。
                int gridSize = terrainGridSizeForSse(
                    tile.selectionFrameState.screenSpaceError);
                const TerrainDisplacementTemplatePool::HeightTexture* ht =
                    hm ? pool->acquireHeightTexture(tile.key, *hm, gridSize,
                                                    frameNumber)
                       : nullptr;
                // dense 档层池容量小(近景瓦片才用),触顶时 acquire 返回 nullptr。
                // 此时降级回 coarse 档重试,而不是让本帧丢掉这块瓦片。
                if (!ht && hm && gridSize != kTerrainDisplacementGridSize) {
                    gridSize = kTerrainDisplacementGridSize;
                    ht = pool->acquireHeightTexture(tile.key, *hm, gridSize,
                                                    frameNumber);
                }
                const TerrainDisplacementTemplatePool::TemplateBuffers* tb =
                    (ht && ht->texture)
                        ? pool->acquire(tile.key, tile.bounds, gridSize)
                        : nullptr;
                if (tb && ht && ht->texture) {
                    cmd.vertexBuffer = tb->vertexBuffer;
                    cmd.indexBuffer = tb->indexBuffer;
                    cmd.indexCount = tb->indexCount;
                    cmd.vertexCount = tb->vertexCount;
                    cmd.indexType = RenderCommand::IndexType::UInt32;
                    cmd.vertexStride = 32;  // TerrainCompact32
                    cmd.hasTerrainDisplacementFrame = true;
                    // 关键:切到 terrainShader。本 demo 地形无水面掩码元数据 →
                    // useTerrainFormat=false → 原 cmd 用 gltfShader(120B 布局、**无
                    // 位移逻辑**),模板(32B,零高程)经它原样渲染=永远平。位移逻辑
                    // 只在 terrainShader(kTerrainVertexGLSL),且它读 32B TerrainGpuVertex
                    // 布局、fragment 处理影像叠加,与本模板天然匹配。不切 shader =
                    // Phase 2c 位移对本 demo 地形全程死代码。
                    cmd.shader = renderer.terrainShader();
                    const Mat4 frame = terrainTemplateTileFrame(tile.bounds);
                    const double* r = frame.data();  // 列主序 16 double
                    for (int m = 0; m < 16; ++m) {
                        cmd.terrainDisplacementModelMatrix[m] = r[m];
                    }
                    // 模板在 ENU 帧,morph up = 局部 +Z。w(morphFactor)每帧由
                    // applyPerFrameCommandState 盖章为 terrainMorphFactor。
                    u.geomorphUpFactor = {0.0f, 0.0f, 1.0f, 1.0f};
                    if (cmd.textures.size() <=
                        static_cast<size_t>(kGltfHeightTextureSlot)) {
                        cmd.textures.resize(
                            static_cast<size_t>(kGltfHeightTextureSlot) + 1);
                    }
                    cmd.textures[kGltfHeightTextureSlot] = ht->texture;
                    // 位移随 LOD 衰减到平:minHeight/heightRange 同乘 reliefFade →
                    // 顶点 shader h = minH·f + t·range·f = f·h_orig,过渡带瓦片压平。
                    u.heightDisplace = {
                        ht->minHeight * reliefFade, ht->heightRange * reliefFade,
                        1.0f, static_cast<float>(gridSize)};
                    // 合批 Step 1:高度数据在共享 array 的层号进 uniform;
                    // (layer, epoch) 记在命令上供 build 侧 staleness 校验。
                    u.terrainLayers[0] = static_cast<float>(ht->layer);
                    cmd.terrainHeightLayer = ht->layer;
                    cmd.terrainHeightLayerEpoch = ht->epoch;
                    // 档位记在命令上:staleness 校验要按档查对应 array,且
                    // 保活 touch 也必须落到同一档的 LRU(见 heightLayerCurrent)。
                    cmd.terrainHeightGridSize = gridSize;
                }
            }
        }
        // P5b 兜底:共享模板几何 primitive 的 per-tile buffer 有意为空,正常
        // 情况上方模板 swap 已换绑共享 VBO/IBO;若 swap 未发生(病理态:高度
        // 纹理 acquire 失败/池被运行时关闭而内容未重建),丢弃该命令——宁可
        // 该瓦片本帧不画,绝不携带 null buffer 下发 draw(UB)。带 buffer 的
        // 常规命令(legacy VBO/fill/实例化)不受影响。
        if (!cmd.vertexBuffer || !cmd.indexBuffer) {
            if (primitive.sharedTemplateGeometry) {
                sharedTemplateSwapFailed = true;
                platformLog(LogLevel::Warning, "GltfDrawCmd",
                    "shared-template swap missed, retry next frame: "
                    "key=%d/%d/%d relief=%.3f pool=%d hm=%d",
                    tile.key.z, tile.key.x, tile.key.y,
                    static_cast<double>(reliefFade),
                    renderer.terrainDisplacementPool() ? 1 : 0,
                    tile.content.renderContent.retainedHeightmap() ? 1 : 0);
            }
            continue;
        }
        cmd.cullFace = !primitive.doubleSided;
        cached.push_back(std::move(cmd));
    }
    // P5b:共享模板 swap 失败(瞬态:高度纹理未就绪/池分配失败)时不把缓存
    // 定格为 valid——否则空命令列表因内容 revision 不变永不重建,该瓦片永久
    // 不可见(启动期 = 无 surface command → presentation hold 永不释放的黑屏
    // 死锁)。置 invalid 让下一帧重试 rebuild,成功即自愈。
    if (sharedTemplateSwapFailed) {
        tile.content.renderContent.invalidateCachedDrawCommands();
    }
}

/// 每帧盖章:frameId/generation、过渡透明度及其 blend 派生态、clip 窗口、
/// raster overlay 绑定(纹理指针/UV 窗口/透明度随加载逐帧变化)。
/// 只写帧列表副本;常驻命令保持内容不变式,拷贝出来即处于默认每帧状态。
void applyPerFrameCommandState(
    Renderer& renderer,
    RenderCommand& cmd,
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays,
    const GltfDrawCommandBuildContext& context,
    GltfDrawCommandBuildTimings* timings) {
    cmd.frameId = context.frameNumber;
    cmd.generation = context.generation;
    GltfUniformBlock& u = cmd.gltfUniforms;
    cmd.surfaceTransitionOpacity = context.transitionOpacity;
    u.renderOpacity = context.transitionOpacity;
    // geomorph↔cross-fade 互斥:地形改用 geomorph 单层过渡(几何 morph),不走
    // cross-fade 的 alpha 双层混合→消除双影 ghosting。morphFactor(w)= 距离连续
    // 的 terrainMorphFactor(由 finalizer 从本瓦片 SSE 在有效 LOD 频带内的位置算出,
    // 随相机连续移动平滑推进,替代旧的定时 transitionOpacity),xyz(tileUp)常驻
    // 构建时已设。地形 renderOpacity 恒 1(不透明,子瓦片 morph=0 起点≈父面平滑态
    // 覆盖父瓦片,morph 到真实细节),cross-fade(alpha)保留给非地形内容(glTF)。
    if (cmd.terrainRenderContent) {
        u.geomorphUpFactor[3] = tile.selectionFrameState.terrainMorphFactor;
        u.renderOpacity = 1.0f;
    }
    if (cmd.terrainRenderContent && context.surfaceClipUv) {
        cmd.surfaceClipUv = *context.surfaceClipUv;
        cmd.surfaceClipEnabled = 1.0f;
        u.clipUv = *context.surfaceClipUv;
        u.clipEnabled = 1.0f;
    }

    const double rasterBindingStartMs =
        timings ? perf::nowMs() : 0.0;
    int rasterOverlayTextureCount = 0;
    cmd.surfaceBaseRasterState = 0;
    cmd.surfaceBaseIsMappedRasterTile = 0;
    for (size_t i = 0;
         i < overlays.size() && i < tile.rasterOverlayState.mappingCount();
         ++i) {
        if (rasterOverlayTextureCount >= kMaxGltfRasterOverlays) {
            break;
        }
        ActivatedRasterOverlay* activeOverlay = overlays[i];
        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        const SurfaceRasterBinding binding =
            chooseSurfaceRasterBinding(mapped);
        if (!rasterOverlayBindingAllowedByPolicy(
                activeOverlay,
                mapped,
                binding)) {
            continue;
        }
        const int32_t textureCoordinateID =
            mapped ? mapped->getTextureCoordinateID() : -1;
        if (textureCoordinateID < 0 ||
            textureCoordinateID >= static_cast<int32_t>(kGltfMaxTexCoordSets)) {
            continue;
        }
        Texture* texture = binding.tile->getTexture();
        if (!texture) {
            continue;
        }
        const size_t textureSlot =
            static_cast<size_t>(kGltfRasterOverlayTextureBase +
                                rasterOverlayTextureCount);
        if (cmd.textures.size() <= textureSlot) {
            cmd.textures.resize(textureSlot + 1u, nullptr);
        }
        cmd.textures[textureSlot] = texture;
        if (binding.tileHandle) {
            cmd.resourceKeepAlive.push_back(binding.tileHandle);
        }
        cmd.gltfRasterOverlayTileUvs[rasterOverlayTextureCount] = {
            binding.offsetU,
            binding.offsetV,
            binding.scaleU,
            binding.scaleV};
        cmd.gltfRasterOverlayOpacities[rasterOverlayTextureCount] =
            activeOverlay ? activeOverlay->opacity() : 1.0f;
        cmd.gltfRasterOverlayTexCoordSets[rasterOverlayTextureCount] =
            static_cast<float>(textureCoordinateID);
        if (activeOverlay &&
            activeOverlay->getOverlay().role() ==
                RasterOverlayRole::BaseImagery) {
            cmd.surfaceBaseRasterState = 1;
            cmd.surfaceBaseIsMappedRasterTile = 1;
            // 加载质量诊断:记下这片瓦片的底图到底贴的是第几级影像,以及
            // 距离"想要的那级"差多少 —— 差值就是屏幕上肉眼看到的糊的程度。
            // _pLoadingTile = 想要的目标层(还在加载),_pReadyTile = 实际渲染
            // 的那张(可能是祖先)。settled 时 loading 为空,二者相等 → delta 0。
            const RasterOverlayTile* wantedTile =
                mapped->getLoadingTile() ? mapped->getLoadingTile()
                                         : binding.tile;
            cmd.surfaceGeometryZoom = tile.key.z;
            cmd.surfaceTextureZoom = binding.tile->getTileID().z;
            cmd.imageryAncestorLevelDelta =
                std::max(0, wantedTile->getTileID().z -
                                binding.tile->getTileID().z);
        }
        ++rasterOverlayTextureCount;
    }
    cmd.gltfRasterOverlayTextureCount = rasterOverlayTextureCount;
    if (cmd.terrainRenderContent) {
        cmd.terrainSurfaceSource =
            terrainSurfaceSourceForDraw(tile.content.renderContent);
    }
    u.mappedRasterTextureCount =
        static_cast<float>(rasterOverlayTextureCount);
    for (int i = 0; i < kMaxGltfRasterOverlays; ++i) {
        u.mappedRasterTileUv[i] = cmd.gltfRasterOverlayTileUvs[i];
        u.mappedRasterOpacity[i] = cmd.gltfRasterOverlayOpacities[i];
        u.mappedRasterTexCoordSet[i] = cmd.gltfRasterOverlayTexCoordSets[i];
    }
    if (timings) {
        timings->rasterBindingMs +=
            perf::nowMs() - rasterBindingStartMs;
    }
    // alphaMode/transmission 是内容量(缓存里),透明度是每帧量:blend 状态
    // 必须每帧重新派生,否则 fade 结束后命令会卡在半透明状态。
    // A2C 命令(实例化 blend)材质半透明改由 alpha-to-coverage 承担,不开常规
    // alpha 混合(那需逐实例排序);但 LOD 淡入淡出期整块 alpha 叠加仍需真混合。
    // 地形走 geomorph(几何 morph)单层过渡,不 alpha 淡入→过渡期保持不透明,
    // 不进 blend(消除 cross-fade 双层 ghosting)。cross-fade alpha 仅用于非地形
    // 内容(glTF 模型)。地形若材质本身半透明(水面 transmission)仍按材质走。
    const bool fading =
        context.transitionOpacity < 0.999f && !cmd.terrainRenderContent;
    const bool materialTranslucent =
        u.alphaMode == 2.0f || u.transmissionFactor > 0.0f;
    if (fading || (materialTranslucent && !cmd.alphaToCoverage)) {
        cmd.blend = true;
        cmd.depthWrite = false;
        cmd.blendSrc = RenderCommand::BlendFactor::SrcAlpha;
        cmd.blendDst = RenderCommand::BlendFactorDst::OneMinusSrcAlpha;
    }

    // 北极星合成方案页存储(门③ Step3):最后一步,对目标 capped 真实地形瓦片
    // 挂 array 纹理 + 置 pageStoreParams.enabled=1(覆盖上采样 mappedRaster)。
    // 未启用(指针空)或非目标瓦片时 no-op → 逐字节走现状路径,零回归。
    if (TerrainPageStore* pageStore = renderer.terrainPageStore()) {
        pageStore->applyToTerrainCommand(cmd, tile, overlays);
    }
}

} // namespace

void GltfDrawCommandBuilder::build(
    Renderer& renderer,
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays,
    RenderCommandList& commands,
    const GltfDrawCommandBuildContext& context,
    GltfDrawCommandBuildTimings* timings) {
    const double eligibilityStartMs =
        timings ? perf::nowMs() : 0.0;
    TileRenderContentState& renderContent = tile.content.renderContent;
    if (!renderContent.hasDrawableResources()) {
        if (timings) {
            timings->eligibilityMs +=
                perf::nowMs() - eligibilityStartMs;
        }
        return;
    }
    if (!TileRasterOverlayReadinessPolicy::
            terrainSurfaceImageryDrawableReady(tile, overlays)) {
        if (timings) {
            timings->eligibilityMs +=
                perf::nowMs() - eligibilityStartMs;
        }
        return;
    }
    if (timings) {
        timings->eligibilityMs +=
            perf::nowMs() - eligibilityStartMs;
    }
    // 合批 Step 1:常驻命令引用的高度 array 层被 LRU 重分配(epoch 失配)时
    // invalidate → 下方立即 rebuild 重新 acquire(自愈,与 P5b 同模式)。
    // 可见瓦片每帧 touch 保活,失配只发生在离屏久驻后重新入视野的瓦片。
    if (renderContent.hasCachedDrawCommands()) {
        if (TerrainDisplacementTemplatePool* pool =
                renderer.terrainDisplacementPool()) {
            bool touched = false;
            for (const RenderCommand& cached :
                 renderContent.cachedDrawCommands()) {
                if (cached.terrainHeightLayer < 0) {
                    continue;
                }
                // 换档也要失效重建:常驻命令只在资源变化时重建,不查这一条的话
                // 瓦片会永远停在建命令那一帧的密度档(相机推近也不加密)。
                if (cached.terrainHeightGridSize !=
                    terrainGridSizeForSse(
                        tile.selectionFrameState.screenSpaceError,
                        cached.terrainHeightGridSize)) {
                    renderContent.invalidateCachedDrawCommands();
                    break;
                }
                if (!pool->heightLayerCurrent(
                        cached.terrainHeightGridSize,
                        cached.terrainHeightLayer,
                        cached.terrainHeightLayerEpoch)) {
                    renderContent.invalidateCachedDrawCommands();
                    break;
                }
                if (!touched) {
                    pool->touchHeightTexture(tile.key,
                                             cached.terrainHeightGridSize,
                                             context.frameNumber);
                    touched = true;
                }
            }
        }
    }
    if (!renderContent.hasCachedDrawCommands()) {
        const double cacheRebuildStartMs =
            timings ? perf::nowMs() : 0.0;
        rebuildCachedDrawCommands(renderer, tile, context.frameNumber);
        if (timings) {
            timings->cacheRebuildMs +=
                perf::nowMs() - cacheRebuildStartMs;
            ++timings->cacheRebuildCount;
        }
    }
    for (const RenderCommand& cachedCommand :
         renderContent.cachedDrawCommands()) {
        const double commandCopyStartMs =
            timings ? perf::nowMs() : 0.0;
        commands.push_back(cachedCommand);
        if (timings) {
            timings->commandCopyMs +=
                perf::nowMs() - commandCopyStartMs;
        }
        const double perFrameStateStartMs =
            timings ? perf::nowMs() : 0.0;
        applyPerFrameCommandState(
            renderer,
            commands.back(),
            tile,
            overlays,
            context,
            timings);
        if (timings) {
            timings->perFrameStateMs +=
                perf::nowMs() - perFrameStateStartMs;
            ++timings->commandCount;
        }
    }
}

} // namespace earth_engine
