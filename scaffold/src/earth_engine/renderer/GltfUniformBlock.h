#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace earth_engine {

/// glTF / terrain 渲染命令的定长 uniform 块（P0-1 uniform 句柄化）。
///
/// 取代 RenderCommand::uniforms string-map 在 glTF/terrain 热路径上的使用：
/// 命令构建零堆分配、深拷贝退化为 memcpy、后端消费零字符串哈希。
///
/// 布局契约（三方必须一致，改字段时同步更新）：
///  1. 本结构体（C++ 侧生产者/GLES 消费）；
///  2. Renderer.cpp Metal MSL 源里的镜像 struct GltfUniforms —— 整块以一次
///     setFragmentBytes 绑到 fragment buffer(0)。MSL 侧全部使用
///     float / packed_float2/3/4 成员（4 字节对齐），与 C++ 自然布局逐字节
///     一致；唯一 16 字节对齐成员 float4x4 modelViewProjection 固定在
///     offset 0；
///  3. 下方 kGltfUniformTable 的 name→offset 描述表（GLES 按 program 一次性
///     解析 location 后直传）。
///
/// 默认值即 Renderer::makeGltfPrimitiveCommand / makeTerrainPrimitiveCommand
/// 原先逐键写入的默认值，构造即就绪。
struct alignas(16) GltfUniformBlock {
    /// 每纹理 KHR_texture_transform：offsetScale = {offsetU, offsetV,
    /// scaleU, scaleV}，rotationSinCos = {sin, cos}。
    struct TextureTransform {
        std::array<float, 4> offsetScale{0.0f, 0.0f, 1.0f, 1.0f};
        std::array<float, 2> rotationSinCos{0.0f, 1.0f};
    };

    // ---- vertex stage（Metal 侧单独以 buffer(1) 绑 modelViewProjection）----
    std::array<float, 16> modelViewProjection{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    // CPU-only：RTC 局部原点，仅 SceneRenderCommandUniformUpdater 用于每帧
    // 重算 MVP，shader 不消费（描述表中无对应条目）。
    std::array<float, 3> modelOrigin{0.0f, 0.0f, 0.0f};
    float _reservedOrigin = 0.0f;
    // geomorph(顶点阶段):xyz = 瓦片中心椭球法线(morph 方向,ECEF 单位向量;
    // a_position 在 ECEF 平移轴系故方向不变),w = morphFactor。顶点 shader 做
    // pos += xyz * a_heightDelta * (1 - w):w=1 无 morph(默认),w 从 0→1 时子瓦片
    // 顶点高度从粗起点长到真实值。w 复用 lodTransitionFadePercentage(刚 refine
    // 的子瓦片 0→1 over lodTransitionLength)。16 字节保持 alignas(16)。
    std::array<float, 4> geomorphUpFactor{0.0f, 0.0f, 1.0f, 1.0f};

    // ---- fragment stage ----
    std::array<float, 3> lightDir{0.0f, 0.0f, 0.0f};
    float useNormalMap = 0.0f;
    float debugNormalMap = 0.0f;
    // 天空环境填充光（rgb + 保留 a）；SkyGradient 求解，每帧由
    // SceneRenderCommandUniformUpdater 写入。16 字节保持 size % 16 == 0。
    std::array<float, 4> ambient{0.0f, 0.0f, 0.0f, 1.0f};
    // 相机世界坐标相对本瓦片 RTC 原点(modelOrigin)的偏移，每帧由
    // SceneRenderCommandUniformUpdater 写入。水面 sun-glint 在 shader 里用
    // V = normalize(eyePositionRTC - localPosition) 求视向量。float3 + pad 补齐
    // 16 字节。RTC 相减在 CPU 双精度下做，float 只承载小量级差值。
    std::array<float, 3> eyePositionRTC{0.0f, 0.0f, 0.0f};
    float _reservedEye = 0.0f;

    std::array<float, 4> baseColor{0.82f, 0.84f, 0.88f, 1.0f};
    float hasBaseColorTexture = 0.0f;
    // {metallicFactor, roughnessFactor, normalTextureScale, occlusionStrength}
    std::array<float, 4> materialFactors{1.0f, 1.0f, 1.0f, 1.0f};
    float dielectricSpecularF0 = 0.04f;
    // {metallicRoughness, normal, occlusion, emissive} texture flags
    std::array<float, 4> hasMaterialTextures{0.0f, 0.0f, 0.0f, 0.0f};

    std::array<float, 2> anisotropyFactors{0.0f, 0.0f};
    float hasAnisotropyTexture = 0.0f;
    std::array<float, 2> hasSpecularTextures{0.0f, 0.0f};
    float specularFactor = 1.0f;
    std::array<float, 3> specularColorFactor{1.0f, 1.0f, 1.0f};
    float specularGlossinessWorkflow = 0.0f;
    std::array<float, 4> specularGlossinessFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float hasSpecularGlossinessTexture = 0.0f;
    float transmissionFactor = 0.0f;
    float hasTransmissionTexture = 0.0f;
    // {clearcoatFactor, clearcoatRoughnessFactor, clearcoatNormalScale}
    std::array<float, 3> clearcoatFactors{0.0f, 0.0f, 1.0f};
    std::array<float, 3> hasClearcoatTextures{0.0f, 0.0f, 0.0f};
    std::array<float, 3> sheenColorFactor{0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.0f;
    std::array<float, 2> hasSheenTextures{0.0f, 0.0f};
    std::array<float, 3> emissiveFactor{0.0f, 0.0f, 0.0f};

    // {baseColor, metallicRoughness, normal, occlusion} texcoord set indices
    std::array<float, 4> textureCoordSets{0.0f, 0.0f, 0.0f, 0.0f};
    float emissiveTexCoordSet = 0.0f;
    float anisotropyTexCoordSet = 0.0f;
    std::array<float, 2> specularTexCoordSets{0.0f, 0.0f};
    float specularGlossinessTexCoordSet = 0.0f;
    float transmissionTexCoordSet = 0.0f;
    std::array<float, 3> clearcoatTexCoordSets{0.0f, 0.0f, 0.0f};
    std::array<float, 2> sheenTexCoordSets{0.0f, 0.0f};

    float alphaMode = 0.0f;   // 0=opaque 1=mask 2=blend
    float alphaCutoff = 0.5f;
    float renderOpacity = 1.0f;
    float unlit = 0.0f;

    TextureTransform baseColorTex{};
    TextureTransform metallicRoughnessTex{};
    TextureTransform anisotropyTex{};
    TextureTransform specularTex{};
    TextureTransform specularColorTex{};
    TextureTransform specularGlossinessTex{};
    TextureTransform transmissionTex{};
    TextureTransform clearcoatTex{};
    TextureTransform clearcoatRoughnessTex{};
    TextureTransform clearcoatNormalTex{};
    TextureTransform sheenColorTex{};
    TextureTransform sheenRoughnessTex{};
    TextureTransform normalTex{};
    TextureTransform occlusionTex{};
    TextureTransform emissiveTex{};

    float mappedRasterTextureCount = 0.0f;
    std::array<std::array<float, 4>, 4> mappedRasterTileUv{{
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f}}};
    std::array<float, 4> mappedRasterOpacity{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> mappedRasterTexCoordSet{0.0f, 0.0f, 0.0f, 0.0f};

    float hasWaterMask = 0.0f;
    std::array<float, 4> waterMaskTranslationScale{0.0f, 0.0f, 1.0f, 0.0f};
    std::array<float, 4> waterMaskState{1.0f, 0.0f, 0.0f, 0.0f};

    std::array<float, 4> clipUv{0.0f, 0.0f, 1.0f, 1.0f};
    float clipEnabled = 0.0f;
};

static_assert(alignof(GltfUniformBlock) == 16,
              "modelViewProjection maps to MSL float4x4 at offset 0");
static_assert(offsetof(GltfUniformBlock, modelViewProjection) == 0,
              "MSL float4x4 member must sit at offset 0");
static_assert(sizeof(GltfUniformBlock) % 16 == 0,
              "keep tail padding deterministic across compilers");
static_assert(sizeof(GltfUniformBlock) <= 4096,
              "Metal setBytes inline-constant limit is 4KB");

/// GLES 消费描述表条目：uniform 名 → 块内 float 偏移 + 分量数。
/// count==16 走 glUniformMatrix4fv，其余走 glUniform{1,2,3,4}fv。
struct GltfUniformTableEntry {
    const char* name;
    uint16_t floatOffset;
    uint16_t count;
};

namespace detail {
constexpr uint16_t gltfFloatOffset(size_t byteOffset) {
    return static_cast<uint16_t>(byteOffset / sizeof(float));
}
} // namespace detail

/// name→offset 描述表。GLES 按 program 首次遇到 glTF 块命令时把全部名字
/// 一次性解析成 GLint location 数组（不在表内的 shader 不声明 → -1 跳过），
/// 之后每 draw 零字符串操作直传。Metal 不使用本表（整块 setBytes）。
inline const auto& gltfUniformTable() {
#define EE_GLTF_ENTRY(uniformName, field, componentCount)                  \
    GltfUniformTableEntry {                                                \
        uniformName,                                                       \
        detail::gltfFloatOffset(offsetof(GltfUniformBlock, field)),        \
        componentCount                                                     \
    }
#define EE_GLTF_TRANSFORM(prefix, field)                                   \
    EE_GLTF_ENTRY(prefix "OffsetScale", field.offsetScale, 4),             \
    EE_GLTF_ENTRY(prefix "RotationSinCos", field.rotationSinCos, 2)
// std::array 成员不能在 offsetof 里带下标（非原生数组），索引条目用
// 基偏移 + index×分量数（std::array<float,N> 保证紧凑连续）。
#define EE_GLTF_ENTRY_AT(uniformName, field, index, componentCount)        \
    GltfUniformTableEntry {                                                \
        uniformName,                                                       \
        static_cast<uint16_t>(                                             \
            detail::gltfFloatOffset(offsetof(GltfUniformBlock, field)) +   \
            (index) * (componentCount)),                                   \
        componentCount                                                     \
    }
    static const std::array<GltfUniformTableEntry, 88> table = {{
        EE_GLTF_ENTRY("u_modelViewProjection", modelViewProjection, 16),
        EE_GLTF_ENTRY("u_geomorphUpFactor", geomorphUpFactor, 4),
        EE_GLTF_ENTRY("u_lightDir", lightDir, 3),
        EE_GLTF_ENTRY("u_ambient", ambient, 4),
        EE_GLTF_ENTRY("u_eyePositionRTC", eyePositionRTC, 3),
        EE_GLTF_ENTRY("u_useNormalMap", useNormalMap, 1),
        EE_GLTF_ENTRY("u_debugNormalMap", debugNormalMap, 1),
        EE_GLTF_ENTRY("u_baseColor", baseColor, 4),
        EE_GLTF_ENTRY("u_hasBaseColorTexture", hasBaseColorTexture, 1),
        EE_GLTF_ENTRY("u_materialFactors", materialFactors, 4),
        EE_GLTF_ENTRY("u_dielectricSpecularF0", dielectricSpecularF0, 1),
        EE_GLTF_ENTRY("u_hasMaterialTextures", hasMaterialTextures, 4),
        EE_GLTF_ENTRY("u_anisotropyFactors", anisotropyFactors, 2),
        EE_GLTF_ENTRY("u_hasAnisotropyTexture", hasAnisotropyTexture, 1),
        EE_GLTF_ENTRY("u_hasSpecularTextures", hasSpecularTextures, 2),
        EE_GLTF_ENTRY("u_specularFactor", specularFactor, 1),
        EE_GLTF_ENTRY("u_specularColorFactor", specularColorFactor, 3),
        EE_GLTF_ENTRY(
            "u_specularGlossinessWorkflow", specularGlossinessWorkflow, 1),
        EE_GLTF_ENTRY(
            "u_specularGlossinessFactor", specularGlossinessFactor, 4),
        EE_GLTF_ENTRY(
            "u_hasSpecularGlossinessTexture", hasSpecularGlossinessTexture, 1),
        EE_GLTF_ENTRY("u_transmissionFactor", transmissionFactor, 1),
        EE_GLTF_ENTRY("u_hasTransmissionTexture", hasTransmissionTexture, 1),
        EE_GLTF_ENTRY("u_clearcoatFactors", clearcoatFactors, 3),
        EE_GLTF_ENTRY("u_hasClearcoatTextures", hasClearcoatTextures, 3),
        EE_GLTF_ENTRY("u_sheenColorFactor", sheenColorFactor, 3),
        EE_GLTF_ENTRY("u_sheenRoughnessFactor", sheenRoughnessFactor, 1),
        EE_GLTF_ENTRY("u_hasSheenTextures", hasSheenTextures, 2),
        EE_GLTF_ENTRY("u_emissiveFactor", emissiveFactor, 3),
        EE_GLTF_ENTRY("u_textureCoordSets", textureCoordSets, 4),
        EE_GLTF_ENTRY("u_emissiveTexCoordSet", emissiveTexCoordSet, 1),
        EE_GLTF_ENTRY("u_anisotropyTexCoordSet", anisotropyTexCoordSet, 1),
        EE_GLTF_ENTRY("u_specularTexCoordSets", specularTexCoordSets, 2),
        EE_GLTF_ENTRY(
            "u_specularGlossinessTexCoordSet", specularGlossinessTexCoordSet, 1),
        EE_GLTF_ENTRY("u_transmissionTexCoordSet", transmissionTexCoordSet, 1),
        EE_GLTF_ENTRY("u_clearcoatTexCoordSets", clearcoatTexCoordSets, 3),
        EE_GLTF_ENTRY("u_sheenTexCoordSets", sheenTexCoordSets, 2),
        EE_GLTF_ENTRY("u_alphaMode", alphaMode, 1),
        EE_GLTF_ENTRY("u_alphaCutoff", alphaCutoff, 1),
        EE_GLTF_ENTRY("u_renderOpacity", renderOpacity, 1),
        EE_GLTF_ENTRY("u_unlit", unlit, 1),
        EE_GLTF_TRANSFORM("u_baseColorTex", baseColorTex),
        EE_GLTF_TRANSFORM("u_metallicRoughnessTex", metallicRoughnessTex),
        EE_GLTF_TRANSFORM("u_anisotropyTex", anisotropyTex),
        EE_GLTF_TRANSFORM("u_specularTex", specularTex),
        EE_GLTF_TRANSFORM("u_specularColorTex", specularColorTex),
        EE_GLTF_TRANSFORM("u_specularGlossinessTex", specularGlossinessTex),
        EE_GLTF_TRANSFORM("u_transmissionTex", transmissionTex),
        EE_GLTF_TRANSFORM("u_clearcoatTex", clearcoatTex),
        EE_GLTF_TRANSFORM("u_clearcoatRoughnessTex", clearcoatRoughnessTex),
        EE_GLTF_TRANSFORM("u_clearcoatNormalTex", clearcoatNormalTex),
        EE_GLTF_TRANSFORM("u_sheenColorTex", sheenColorTex),
        EE_GLTF_TRANSFORM("u_sheenRoughnessTex", sheenRoughnessTex),
        EE_GLTF_TRANSFORM("u_normalTex", normalTex),
        EE_GLTF_TRANSFORM("u_occlusionTex", occlusionTex),
        EE_GLTF_TRANSFORM("u_emissiveTex", emissiveTex),
        EE_GLTF_ENTRY(
            "u_mappedRasterTextureCount", mappedRasterTextureCount, 1),
        EE_GLTF_ENTRY_AT("u_mappedRasterTileUV0", mappedRasterTileUv, 0, 4),
        EE_GLTF_ENTRY_AT("u_mappedRasterTileUV1", mappedRasterTileUv, 1, 4),
        EE_GLTF_ENTRY_AT("u_mappedRasterTileUV2", mappedRasterTileUv, 2, 4),
        EE_GLTF_ENTRY_AT("u_mappedRasterTileUV3", mappedRasterTileUv, 3, 4),
        EE_GLTF_ENTRY_AT("u_mappedRasterOpacity0", mappedRasterOpacity, 0, 1),
        EE_GLTF_ENTRY_AT("u_mappedRasterOpacity1", mappedRasterOpacity, 1, 1),
        EE_GLTF_ENTRY_AT("u_mappedRasterOpacity2", mappedRasterOpacity, 2, 1),
        EE_GLTF_ENTRY_AT("u_mappedRasterOpacity3", mappedRasterOpacity, 3, 1),
        EE_GLTF_ENTRY_AT(
            "u_mappedRasterTexCoordSet0", mappedRasterTexCoordSet, 0, 1),
        EE_GLTF_ENTRY_AT(
            "u_mappedRasterTexCoordSet1", mappedRasterTexCoordSet, 1, 1),
        EE_GLTF_ENTRY_AT(
            "u_mappedRasterTexCoordSet2", mappedRasterTexCoordSet, 2, 1),
        EE_GLTF_ENTRY_AT(
            "u_mappedRasterTexCoordSet3", mappedRasterTexCoordSet, 3, 1),
        EE_GLTF_ENTRY("u_gltfHasWaterMask", hasWaterMask, 1),
        EE_GLTF_ENTRY(
            "u_gltfWaterMaskTranslationScale", waterMaskTranslationScale, 4),
        EE_GLTF_ENTRY("u_gltfWaterMaskState", waterMaskState, 4),
        EE_GLTF_ENTRY("u_clipUV", clipUv, 4),
        EE_GLTF_ENTRY("u_clipEnabled", clipEnabled, 1),
    }};
#undef EE_GLTF_ENTRY_AT
#undef EE_GLTF_TRANSFORM
#undef EE_GLTF_ENTRY
    return table;
}

} // namespace earth_engine
