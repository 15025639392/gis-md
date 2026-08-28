#include "Renderer.h"
#include "GlyphAtlas.h"
#include "IconAtlas.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../core/math/Vec3.h"
#include "../core/math/Mat4.h"
#include "../tiling/TileKey.h"
#include "PageStoreSamplingGLSL.h"
#include "TerrainSurfaceLightGLSL.h"
#include "PipelineConfig.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace earth_engine {

// ============================================================
// Terrain / glTF shaders — cesium-native glTF vertex layout
// POSITION(vec3) + NORMAL(vec3) + TEXCOORD_0(vec2) = 32 bytes
//
// Render-chain steps after core binding:
// 11. Renderer backend translates RenderCommand into texture slots, sampler
//     uniforms, fixed uniforms (u_tileUV / overlay UVs), buffers, and draw calls.
// 12. This shader consumes those uniforms and samples u_tileTexture /
//     u_overlayTextureN.
// 13. GPU depth/cull/blend tests resolve fragments into framebuffer pixels.
// 14. Android/Metal surface presentation swaps that framebuffer to the screen.
//
// Core unit tests can prove which texture and UV window should be drawn; they
// cannot prove texture-slot binding, uniform upload, shader sampling, depth
// rejection, or final device pixels. Use backend spy tests or offscreen
// framebuffer readback with small color fixtures for those failures.
// ============================================================

// ============================================================
// glTF primitive shader — TileRenderContent render resources
// POSITION(vec3) + NORMAL(vec3) + TEXCOORD_0..7(packed vec4 pairs)
// + COLOR_0(vec4) + TANGENT(vec4) = 120 bytes
// ============================================================

static const char* kGltfVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_texcoord01;
layout(location = 10) in vec4 a_color;
layout(location = 11) in vec4 a_tangent;
layout(location = 12) in vec4 a_texcoord23;
layout(location = 13) in vec4 a_texcoord45;
layout(location = 14) in vec4 a_texcoord67;

uniform mat4 u_modelViewProjection;

out vec3 v_normal;
out vec3 v_position;
out vec4 v_texcoord01;
out vec4 v_color;
out vec4 v_tangent;
out vec4 v_texcoord23;
out vec4 v_texcoord45;
out vec4 v_texcoord67;

void main() {
    v_normal = normalize(a_normal);
    v_position = a_position;
    v_texcoord01 = a_texcoord01;
    v_color = a_color;
    v_tangent = a_tangent;
    v_texcoord23 = a_texcoord23;
    v_texcoord45 = a_texcoord45;
    v_texcoord67 = a_texcoord67;
    gl_PointSize = 1.0;
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kGltfFragmentGLSL = R"glsl(#version 300 es
precision highp float;

in vec3 v_normal;
in vec3 v_position;
in vec4 v_texcoord01;
in vec4 v_color;
in vec4 v_tangent;
in vec4 v_texcoord23;
in vec4 v_texcoord45;
in vec4 v_texcoord67;

uniform vec3 u_lightDir;
uniform vec4 u_ambient;
uniform vec3 u_eyePositionRTC;
uniform vec4 u_baseColor;
uniform sampler2D u_baseColorTexture;
uniform sampler2D u_metallicRoughnessTexture;
uniform sampler2D u_normalTexture;
uniform sampler2D u_occlusionTexture;
uniform sampler2D u_emissiveTexture;
// GLES ≤16-texture-unit compatibility (e.g. Adreno reports
// GL_MAX_TEXTURE_IMAGE_UNITS==16, the GLES spec floor). The full PBR material
// otherwise declares 20 sampler2D, overflowing the fragment texture-unit limit
// so the program fails to link ("Sampler location or component exceeds max
// allowed") and, with no fallback globe, the screen stays black. The advanced
// KHR-extension textures below are never populated by this engine's content —
// QM terrain draws through the terrain shader, and 3D-Tiles glTF uses only
// baseColor / normal / metallicRoughness — so their u_has*Texture flags stay
// 0.0 and these sampler reads never execute. Aliasing them to the base-color
// sampler drops the active-sampler count to 10 while leaving every factor-based
// extension path (u_specularFactor, u_clearcoatFactors, u_sheen*, …) intact.
// Metal (kGltfFragmentMSL) keeps the full independent sampler set.
#define u_anisotropyTexture u_baseColorTexture
#define u_specularTexture u_baseColorTexture
#define u_specularColorTexture u_baseColorTexture
#define u_specularGlossinessTexture u_baseColorTexture
#define u_transmissionTexture u_baseColorTexture
#define u_clearcoatTexture u_baseColorTexture
#define u_clearcoatRoughnessTexture u_baseColorTexture
#define u_clearcoatNormalTexture u_baseColorTexture
#define u_sheenColorTexture u_baseColorTexture
#define u_sheenRoughnessTexture u_baseColorTexture
uniform sampler2D u_directRasterTexture0;
uniform sampler2D u_directRasterTexture1;
uniform sampler2D u_directRasterTexture2;
uniform sampler2D u_directRasterTexture3;
uniform sampler2D u_gltfWaterMaskTexture;
uniform float u_hasBaseColorTexture;
uniform vec4 u_materialFactors;       // metallic, roughness, normal scale, occlusion strength
uniform float u_dielectricSpecularF0;
uniform vec4 u_hasMaterialTextures;   // metallicRoughness, normal, occlusion, emissive
uniform float u_hasAnisotropyTexture;
uniform vec2 u_hasSpecularTextures;   // specular, specularColor
uniform vec3 u_hasClearcoatTextures;  // clearcoat, roughness, normal
uniform vec2 u_hasSheenTextures;      // color, roughness
uniform float u_specularFactor;
uniform vec3 u_specularColorFactor;
uniform float u_specularGlossinessWorkflow;
uniform vec4 u_specularGlossinessFactor; // specular rgb, glossiness
uniform float u_hasSpecularGlossinessTexture;
uniform float u_transmissionFactor;
uniform float u_hasTransmissionTexture;
uniform vec2 u_anisotropyFactors;     // strength, rotation
uniform vec3 u_clearcoatFactors;      // factor, roughness, normal scale
uniform vec3 u_sheenColorFactor;
uniform float u_sheenRoughnessFactor;
uniform vec3 u_emissiveFactor;
uniform float u_alphaMode;
uniform float u_alphaCutoff;
uniform float u_renderOpacity;
uniform float u_unlit;
uniform vec4 u_baseColorTexOffsetScale;
uniform vec2 u_baseColorTexRotationSinCos;
uniform vec4 u_metallicRoughnessTexOffsetScale;
uniform vec2 u_metallicRoughnessTexRotationSinCos;
uniform vec4 u_anisotropyTexOffsetScale;
uniform vec2 u_anisotropyTexRotationSinCos;
uniform vec4 u_specularTexOffsetScale;
uniform vec2 u_specularTexRotationSinCos;
uniform vec4 u_specularColorTexOffsetScale;
uniform vec2 u_specularColorTexRotationSinCos;
uniform vec4 u_specularGlossinessTexOffsetScale;
uniform vec2 u_specularGlossinessTexRotationSinCos;
uniform vec4 u_transmissionTexOffsetScale;
uniform vec2 u_transmissionTexRotationSinCos;
uniform vec4 u_clearcoatTexOffsetScale;
uniform vec2 u_clearcoatTexRotationSinCos;
uniform vec4 u_clearcoatRoughnessTexOffsetScale;
uniform vec2 u_clearcoatRoughnessTexRotationSinCos;
uniform vec4 u_clearcoatNormalTexOffsetScale;
uniform vec2 u_clearcoatNormalTexRotationSinCos;
uniform vec4 u_sheenColorTexOffsetScale;
uniform vec2 u_sheenColorTexRotationSinCos;
uniform vec4 u_sheenRoughnessTexOffsetScale;
uniform vec2 u_sheenRoughnessTexRotationSinCos;
uniform vec4 u_normalTexOffsetScale;
uniform vec2 u_normalTexRotationSinCos;
uniform vec4 u_occlusionTexOffsetScale;
uniform vec2 u_occlusionTexRotationSinCos;
uniform vec4 u_emissiveTexOffsetScale;
uniform vec2 u_emissiveTexRotationSinCos;
uniform vec4 u_textureCoordSets;      // baseColor, metallicRoughness, normal, occlusion
uniform float u_emissiveTexCoordSet;
uniform float u_anisotropyTexCoordSet;
uniform vec2 u_specularTexCoordSets;  // specular, specularColor
uniform float u_specularGlossinessTexCoordSet;
uniform float u_transmissionTexCoordSet;
uniform vec3 u_clearcoatTexCoordSets; // clearcoat, roughness, normal
uniform vec2 u_sheenTexCoordSets;     // color, roughness
uniform float u_directRasterTextureCount;
uniform vec4 u_directRasterTileUV0;
uniform vec4 u_directRasterTileUV1;
uniform vec4 u_directRasterTileUV2;
uniform vec4 u_directRasterTileUV3;
uniform float u_directRasterOpacity0;
uniform float u_directRasterOpacity1;
uniform float u_directRasterOpacity2;
uniform float u_directRasterOpacity3;
uniform float u_directRasterTexCoordSet0;
uniform float u_directRasterTexCoordSet1;
uniform float u_directRasterTexCoordSet2;
uniform float u_directRasterTexCoordSet3;
uniform float u_gltfHasWaterMask;
uniform vec4 u_gltfWaterMaskTranslationScale;
uniform vec4 u_gltfWaterMaskState;
uniform vec4 u_clipUV;
uniform float u_clipEnabled;
// 稀疏虚拟纹理(Step B2b):真实 heightmap DEM 表面走**此** glTF shader(无 water-mask
// 元数据 → useTerrainFormat=false),故 SVT 页存储采样必须在这里(非 kTerrainFragmentGLSL)。
// x=enabled y=gridN z/w=保留;layer 由间接纹理 RG 承载、resident 标志由 A 承载。
uniform highp sampler2DArray u_pageStore;
// 合批 Step 2:per-tile 间接纹理搬共享 texture2DArray(固定 64² 每层,texel 写
// 左上 gridN² 区),层号由 u_terrainLayers.y 给出,texelFetch 整数寻址。
uniform highp sampler2DArray u_pageStoreIndir;
uniform vec4 u_pageStoreParams;
uniform vec4 u_pageStoreUv;
uniform vec4 u_terrainLayers;  // x=高度纹理层(顶点) y=间接纹理层(片元)
// 刀2 路网 SDF 场"第二平面"(R8,与页存储影像页同层号驻留):
// 反向编码 0=远/1=线内/0.5=边缘(0 是失败安全值:未绑定采样恒 0 = 无线)。
uniform highp sampler2DArray u_roadField;
uniform highp sampler2DArray u_roadFieldIndir;  // 步3 场间接纹理
uniform vec4 u_roadFieldParams;  // x=enable y=cellZoom z=场纹素边长 w=D2 偏移编码范围
uniform vec4 u_roadFieldColor;   // 线色(RGBA 非预乘)
uniform vec4 u_roadFieldWidth;   // 宽度 ramp (z0,halfPx0,z1,halfPx1)
uniform vec4 u_pageGeomA;        // [瓦界对齐] 几何仿射 c0.xy, dU.xy
uniform vec4 u_pageGeomB;        // [瓦界对齐] 几何仿射 dV.xy

out vec4 fragColor;

vec2 uvFromSet(float texCoordSet) {
    int setIndex = int(floor(texCoordSet + 0.5));
    if (setIndex == 1) return v_texcoord01.zw;
    if (setIndex == 2) return v_texcoord23.xy;
    if (setIndex == 3) return v_texcoord23.zw;
    if (setIndex == 4) return v_texcoord45.xy;
    if (setIndex == 5) return v_texcoord45.zw;
    if (setIndex == 6) return v_texcoord67.xy;
    if (setIndex == 7) return v_texcoord67.zw;
    return v_texcoord01.xy;
}

vec2 transformUv(vec2 uv, vec4 offsetScale, vec2 sinCos) {
    vec2 scaled = uv * offsetScale.zw;
    return vec2(
        scaled.x * sinCos.y + scaled.y * sinCos.x,
        scaled.y * sinCos.y - scaled.x * sinCos.x) + offsetScale.xy;
}

vec4 alphaOver(vec4 base, vec4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

vec4 applyDirectRaster(
    vec4 base,
    sampler2D rasterTexture,
    float texCoordSet,
    vec4 tileUV,
    float opacity) {
    vec2 overlayUv = tileUV.xy + uvFromSet(texCoordSet) * tileUV.zw;
    return alphaOver(base, texture(rasterTexture, overlayUv), opacity);
}

vec4 applyGltfWaterMask(vec4 base, vec3 N, vec3 L, vec3 V) {
    if (u_gltfHasWaterMask < 0.5 || u_gltfWaterMaskState.x > 0.5) {
        return base;
    }
    float water = u_gltfWaterMaskState.y;
    if (u_gltfWaterMaskState.z > 0.5) {
        vec2 waterUv = u_gltfWaterMaskTranslationScale.xy +
            uvFromSet(0.0) * u_gltfWaterMaskTranslationScale.z;
        water = texture(u_gltfWaterMaskTexture, waterUv).r;
    }
    // 海洋像素轻度压暗+冷偏，使其与陆地有辨识度。
    vec3 waterRgb = base.rgb * 0.8 + vec3(0.01, 0.04, 0.07);
    // sun-glint：太阳在水面的镜面高光(Blinn-Phong)。H=半程向量，指数越高高光
    // 越紧。glint 只加进 waterRgb → 下方 mix 用 water 权重门控，陆地像素为 0；
    // facing 让背光侧(NdotL<0)淡出，避免夜面出现假高光。系数是可调旋钮。
    vec3 H = normalize(L + V);
    float facing = smoothstep(0.0, 0.05, dot(N, L));
    float glint = pow(max(dot(N, H), 0.0), 400.0) * facing;
    waterRgb += vec3(1.0, 0.95, 0.85) * glint * 0.5;
    return vec4(mix(base.rgb, waterRgb, clamp(water, 0.0, 1.0)), base.a);
}

vec3 applyTbn(vec3 tangent, vec3 bitangent, vec3 n, vec3 mapNormal) {
    vec3 perturbed = mat3(tangent, bitangent, n) * mapNormal;
    float perturbedLenSq = dot(perturbed, perturbed);
    return perturbedLenSq > 1e-8 ? normalize(perturbed) : n;
}

vec3 perturbNormalFromMap(vec3 n, vec2 uv, vec4 tangentInput, vec3 mapNormal) {
    float mapNormalLenSq = dot(mapNormal, mapNormal);
    if (mapNormalLenSq < 1e-8) {
        return n;
    }
    mapNormal = normalize(mapNormal);

    if (dot(tangentInput.xyz, tangentInput.xyz) > 0.0) {
        vec3 tangent = tangentInput.xyz - n * dot(n, tangentInput.xyz);
        if (dot(tangent, tangent) > 1e-8) {
            tangent = normalize(tangent);
            vec3 bitangent = cross(n, tangent);
            float bitangentLenSq = dot(bitangent, bitangent);
            if (bitangentLenSq > 1e-8) {
                bitangent = normalize(bitangent) *
                    (tangentInput.w < 0.0 ? -1.0 : 1.0);
                return applyTbn(tangent, bitangent, n, mapNormal);
            }
        }
    }

    vec3 dp1 = dFdx(v_position);
    vec3 dp2 = dFdy(v_position);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (abs(det) < 1e-8) {
        return n;
    }
    vec3 tangent = (dp1 * duv2.y - dp2 * duv1.y) / det;
    vec3 bitangent = (-dp1 * duv2.x + dp2 * duv1.x) / det;
    float tangentLenSq = dot(tangent, tangent);
    float bitangentLenSq = dot(bitangent, bitangent);
    if (tangentLenSq < 1e-8 || bitangentLenSq < 1e-8) {
        return n;
    }
    tangent = normalize(tangent);
    bitangent = normalize(bitangent);
    return applyTbn(tangent, bitangent, n, mapNormal);
}

vec3 perturbNormal(vec3 n, vec2 uv, vec4 tangentInput, float normalScale) {
    vec3 mapNormal = texture(u_normalTexture, uv).rgb * 2.0 - 1.0;
    mapNormal.xy *= normalScale;
    return perturbNormalFromMap(n, uv, tangentInput, mapNormal);
}

vec3 perturbClearcoatNormal(
    vec3 n,
    vec2 uv,
    vec4 tangentInput,
    float normalScale) {
    vec3 mapNormal = texture(u_clearcoatNormalTexture, uv).rgb * 2.0 - 1.0;
    mapNormal.xy *= normalScale;
    return perturbNormalFromMap(n, uv, tangentInput, mapNormal);
}

bool tangentSpaceBasis(
    vec3 n,
    vec2 uv,
    vec4 tangentInput,
    out vec3 tangent,
    out vec3 bitangent) {
    if (dot(tangentInput.xyz, tangentInput.xyz) > 0.0) {
        tangent = tangentInput.xyz - n * dot(n, tangentInput.xyz);
        if (dot(tangent, tangent) > 1e-8) {
            tangent = normalize(tangent);
            bitangent = cross(n, tangent);
            float bitangentLenSq = dot(bitangent, bitangent);
            if (bitangentLenSq > 1e-8) {
                bitangent = normalize(bitangent) *
                    (tangentInput.w < 0.0 ? -1.0 : 1.0);
                return true;
            }
        }
    }

    vec3 dp1 = dFdx(v_position);
    vec3 dp2 = dFdy(v_position);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (abs(det) < 1e-8) {
        return false;
    }
    tangent = (dp1 * duv2.y - dp2 * duv1.y) / det;
    bitangent = (-dp1 * duv2.x + dp2 * duv1.x) / det;
    if (dot(tangent, tangent) < 1e-8 ||
        dot(bitangent, bitangent) < 1e-8) {
        return false;
    }
    tangent = normalize(tangent);
    bitangent = normalize(bitangent);
    return true;
}

vec2 rotateDirection(vec2 direction, float rotation) {
    float s = sin(rotation);
    float c = cos(rotation);
    return vec2(
        direction.x * c - direction.y * s,
        direction.x * s + direction.y * c);
}

float anisotropicSpecular(
    float isotropicSpecular,
    float roughness,
    float strength,
    vec3 n,
    vec3 l,
    vec3 tangent,
    vec3 bitangent) {
    if (strength <= 0.0) {
        return isotropicSpecular;
    }
    vec3 planarL = l - n * dot(n, l);
    float planarLenSq = dot(planarL, planarL);
    if (planarLenSq < 1e-8) {
        return isotropicSpecular;
    }
    planarL = normalize(planarL);
    float along = abs(dot(planarL, tangent));
    float directionalRoughness =
        clamp(mix(roughness, 1.0, strength * along), 0.04, 1.0);
    float directionalPower = mix(96.0, 8.0, directionalRoughness);
    float directionalSpecular =
        pow(max(dot(n, l), 0.0), directionalPower) *
        (1.0 - directionalRoughness);
    float perpendicularWeight = abs(dot(planarL, bitangent));
    float blendWeight = clamp(strength * max(along, perpendicularWeight), 0.0, 1.0);
    return mix(isotropicSpecular, directionalSpecular, blendWeight);
}

void main() {
    vec2 terrainUv = uvFromSet(0.0);
    if (u_clipEnabled > 0.5 && u_clipEnabled < 1.5 &&
        (terrainUv.x < u_clipUV.x ||
         terrainUv.x > u_clipUV.x + u_clipUV.z ||
         terrainUv.y < u_clipUV.y ||
         terrainUv.y > u_clipUV.y + u_clipUV.w)) {
        discard;
    }
    float faceSign = gl_FrontFacing ? 1.0 : -1.0;
    vec3 N = normalize(v_normal) * faceSign;
    vec3 geometryN = N;
    vec3 L = normalize(u_lightDir);
    vec2 baseColorUv = transformUv(
        uvFromSet(u_textureCoordSets.x),
        u_baseColorTexOffsetScale,
        u_baseColorTexRotationSinCos);
    float NdotL = max(dot(N, L), 0.0);
    vec4 base = u_baseColor * v_color;
    if (u_hasBaseColorTexture > 0.5) {
        base *= texture(u_baseColorTexture, baseColorUv);
    }
    if (u_directRasterTextureCount > 0.5) {
        base = applyDirectRaster(
            base,
            u_directRasterTexture0,
            u_directRasterTexCoordSet0,
            u_directRasterTileUV0,
            u_directRasterOpacity0);
    }
    if (u_directRasterTextureCount > 1.5) {
        base = applyDirectRaster(
            base,
            u_directRasterTexture1,
            u_directRasterTexCoordSet1,
            u_directRasterTileUV1,
            u_directRasterOpacity1);
    }
    if (u_directRasterTextureCount > 2.5) {
        base = applyDirectRaster(
            base,
            u_directRasterTexture2,
            u_directRasterTexCoordSet2,
            u_directRasterTileUV2,
            u_directRasterOpacity2);
    }
    if (u_directRasterTextureCount > 3.5) {
        base = applyDirectRaster(
            base,
            u_directRasterTexture3,
            u_directRasterTexCoordSet3,
            u_directRasterTileUV3,
            u_directRasterOpacity3);
    }
    // 稀疏虚拟纹理(Step B2b):capped 真实地形瓦片经 per-tile 间接纹理单次 NEAREST
    // fetch 定位共享 array 层 → 覆盖 directComposite 显更细影像。cell resident(A=1)才
    // 覆盖,miss(A=0)保留 directComposite(决策② 共存优雅降级)。UV 复用 set 0 mercator。
    if (u_pageStoreParams.x > 0.5) {
        // cell 网格 = **影像源瓦片网格**(单位:源瓦片),不是几何瓦片等分。
        // t = origin + uv*span → floor 即 cell 下标。标准 overlay 恒为
        // origin=0/span=gridN,整段退化成改造前的 uv*gridN(零回归判据)。
        // UV 取 u_pageStoreParams.w 指定的 texcoord 集:地形 set 0 是地形 scheme
        // 的投影,GCJ 的 UV 烘在另一套里,硬编码 0 等于这个特性没生效。
        // params.w 打包 texCoordSet(低 3 位)+ 祖先寻址相位(x/y 各 6 位)。
        // 相位把局部 cell 下标还原成全局源瓦片下标的低位 —— 祖先子区原点必须在
        // 全局下标上算,否则 d>0 的 cell 采错子区(块状棋盘格)。
        float psPack = u_pageStoreParams.w;
        vec2 psPhase = vec2(mod(floor(psPack / 8.0), 64.0),
                            floor(psPack / 512.0));
        vec2 psUv = uvFromSet(mod(psPack, 8.0));
        vec2 cells = max(u_pageStoreParams.yz, vec2(1.0));
        // 采样链收进单一治理点 eePageStoreCompose(PageStoreSamplingGLSL.h,
        // withPageStoreSampling() 注入)。本变体 UV = details 逐顶点 texcoord,
        // 轴对齐 origin/span 以退化仿射传入(g = origin + uv·span 逐位同旧式)。
        base = eePageStoreCompose(
            base, psUv, vec4(u_pageStoreUv.xy, u_pageStoreUv.z, 0.0),
            vec2(0.0, u_pageStoreUv.w), psPhase, cells,
            int(u_terrainLayers.y + 0.5), u_roadFieldParams.y,
            u_roadFieldParams, u_roadFieldWidth, u_roadFieldColor);
    }
    base = applyGltfWaterMask(base, N, L, normalize(u_eyePositionRTC - v_position));
    // B2 刀2:HDR 下把 sRGB 反照率解到线性(glTF PBR 本为线性设计),BRDF 随之在
    // 线性域算、结果直接输出即线性 HDR(末端 tonemap encode);LDR 恒等=零回归。
    // 覆盖 unlit(下方直出 base)与 lit(base 驱动 diffuse/specularColor)两路。
    base.rgb = hdrAlbedo(base.rgb);
    if (u_alphaMode > 0.5 && u_alphaMode < 1.5 && base.a < u_alphaCutoff) {
        discard;
    }
    float alpha = u_alphaMode > 1.5 ? base.a : 1.0;
    if (u_unlit > 0.5) {
        fragColor = vec4(base.rgb, alpha * clamp(u_renderOpacity, 0.0, 1.0));
        return;
    }

    float metallic = clamp(u_materialFactors.x, 0.0, 1.0);
    float roughness = clamp(u_materialFactors.y, 0.04, 1.0);
    if (u_hasMaterialTextures.x > 0.5) {
        vec2 mrUv = transformUv(
            uvFromSet(u_textureCoordSets.y),
            u_metallicRoughnessTexOffsetScale,
            u_metallicRoughnessTexRotationSinCos);
        vec4 mr = texture(u_metallicRoughnessTexture, mrUv);
        roughness = clamp(roughness * mr.g, 0.04, 1.0);
        metallic = clamp(metallic * mr.b, 0.0, 1.0);
    }

    if (u_hasMaterialTextures.y > 0.5) {
        vec2 normalUv = transformUv(
            uvFromSet(u_textureCoordSets.z),
            u_normalTexOffsetScale,
            u_normalTexRotationSinCos);
        N = perturbNormal(N, normalUv, v_tangent, u_materialFactors.z);
        NdotL = max(dot(N, L), 0.0);
    }

    float occlusion = 1.0;
    if (u_hasMaterialTextures.z > 0.5) {
        vec2 occlusionUv = transformUv(
            uvFromSet(u_textureCoordSets.w),
            u_occlusionTexOffsetScale,
            u_occlusionTexRotationSinCos);
        float ao = texture(u_occlusionTexture, occlusionUv).r;
        occlusion = clamp(1.0 + u_materialFactors.w * (ao - 1.0), 0.0, 1.0);
    }

    vec3 emissive = u_emissiveFactor;
    if (u_hasMaterialTextures.w > 0.5) {
        vec2 emissiveUv = transformUv(
            uvFromSet(u_emissiveTexCoordSet),
            u_emissiveTexOffsetScale,
            u_emissiveTexRotationSinCos);
        emissive *= texture(u_emissiveTexture, emissiveUv).rgb;
    }
    // B2 刀2:emissive 同为 sRGB,HDR 下解到线性(见 base 处注释)。
    emissive = hdrAlbedo(emissive);

    vec3 sheenColor = max(u_sheenColorFactor, vec3(0.0));
    float sheenRoughness = clamp(u_sheenRoughnessFactor, 0.0, 1.0);
    if (u_hasSheenTextures.x > 0.5) {
        vec2 sheenColorUv = transformUv(
            uvFromSet(u_sheenTexCoordSets.x),
            u_sheenColorTexOffsetScale,
            u_sheenColorTexRotationSinCos);
        sheenColor *= texture(u_sheenColorTexture, sheenColorUv).rgb;
    }
    if (u_hasSheenTextures.y > 0.5) {
        vec2 sheenRoughnessUv = transformUv(
            uvFromSet(u_sheenTexCoordSets.y),
            u_sheenRoughnessTexOffsetScale,
            u_sheenRoughnessTexRotationSinCos);
        sheenRoughness = clamp(
            sheenRoughness *
                texture(u_sheenRoughnessTexture, sheenRoughnessUv).a,
            0.0,
            1.0);
    }

    float clearcoat = clamp(u_clearcoatFactors.x, 0.0, 1.0);
    float clearcoatRoughness = clamp(u_clearcoatFactors.y, 0.0, 1.0);
    if (u_hasClearcoatTextures.x > 0.5) {
        vec2 clearcoatUv = transformUv(
            uvFromSet(u_clearcoatTexCoordSets.x),
            u_clearcoatTexOffsetScale,
            u_clearcoatTexRotationSinCos);
        clearcoat *= texture(u_clearcoatTexture, clearcoatUv).r;
    }
    if (u_hasClearcoatTextures.y > 0.5) {
        vec2 clearcoatRoughnessUv = transformUv(
            uvFromSet(u_clearcoatTexCoordSets.y),
            u_clearcoatRoughnessTexOffsetScale,
            u_clearcoatRoughnessTexRotationSinCos);
        clearcoatRoughness = clamp(
            clearcoatRoughness *
                texture(u_clearcoatRoughnessTexture, clearcoatRoughnessUv).g,
            0.0,
            1.0);
    }
    vec3 clearcoatNormal = geometryN;
    if (u_hasClearcoatTextures.z > 0.5) {
        vec2 clearcoatNormalUv = transformUv(
            uvFromSet(u_clearcoatTexCoordSets.z),
            u_clearcoatNormalTexOffsetScale,
            u_clearcoatNormalTexRotationSinCos);
        clearcoatNormal = perturbClearcoatNormal(
            geometryN,
            clearcoatNormalUv,
            v_tangent,
            u_clearcoatFactors.z);
    }

    vec3 specGlossSpecularColor = vec3(0.0);
    float specGlossMaxSpecular = 0.0;
    if (u_specularGlossinessWorkflow > 0.5) {
        vec4 specGloss = vec4(
            clamp(u_specularGlossinessFactor.rgb, 0.0, 1.0),
            clamp(u_specularGlossinessFactor.a, 0.0, 1.0));
        if (u_hasSpecularGlossinessTexture > 0.5) {
            vec2 sgUv = transformUv(
                uvFromSet(u_specularGlossinessTexCoordSet),
                u_specularGlossinessTexOffsetScale,
                u_specularGlossinessTexRotationSinCos);
            specGloss *= texture(u_specularGlossinessTexture, sgUv);
        }
        specGloss = clamp(specGloss, 0.0, 1.0);
        roughness = clamp(1.0 - specGloss.a, 0.04, 1.0);
        metallic = 0.0;
        specGlossSpecularColor = specGloss.rgb;
        specGlossMaxSpecular = max(
            max(specGlossSpecularColor.r, specGlossSpecularColor.g),
            specGlossSpecularColor.b);
    }

    float transmission = clamp(u_transmissionFactor, 0.0, 1.0);
    if (transmission > 0.0 && u_hasTransmissionTexture > 0.5) {
        vec2 transmissionUv = transformUv(
            uvFromSet(u_transmissionTexCoordSet),
            u_transmissionTexOffsetScale,
            u_transmissionTexRotationSinCos);
        transmission *= texture(u_transmissionTexture, transmissionUv).r;
    }
    transmission = clamp(transmission, 0.0, 1.0);

    float anisotropyStrength = clamp(u_anisotropyFactors.x, 0.0, 1.0);
    vec2 anisotropyDirection = vec2(1.0, 0.0);
    vec3 anisotropyTangent;
    vec3 anisotropyBitangent;
    bool hasAnisotropyBasis = false;
    if (anisotropyStrength > 0.0) {
        vec2 anisotropyUv = transformUv(
            uvFromSet(u_anisotropyTexCoordSet),
            u_anisotropyTexOffsetScale,
            u_anisotropyTexRotationSinCos);
        if (u_hasAnisotropyTexture > 0.5) {
            vec3 anisotropySample =
                texture(u_anisotropyTexture, anisotropyUv).rgb;
            anisotropyDirection = anisotropySample.rg * 2.0 - 1.0;
            anisotropyStrength *= anisotropySample.b;
        }
        anisotropyStrength = clamp(anisotropyStrength, 0.0, 1.0);
        if (anisotropyStrength > 0.0) {
            if (dot(anisotropyDirection, anisotropyDirection) < 1e-8) {
                anisotropyDirection = vec2(1.0, 0.0);
            }
            anisotropyDirection = normalize(
                rotateDirection(anisotropyDirection, u_anisotropyFactors.y));
            hasAnisotropyBasis = tangentSpaceBasis(
                N,
                anisotropyUv,
                v_tangent,
                anisotropyTangent,
                anisotropyBitangent);
            if (hasAnisotropyBasis) {
                vec3 baseTangent = anisotropyTangent;
                vec3 baseBitangent = anisotropyBitangent;
                anisotropyTangent = normalize(
                    baseTangent * anisotropyDirection.x +
                    baseBitangent * anisotropyDirection.y);
                anisotropyBitangent = normalize(
                    -baseTangent * anisotropyDirection.y +
                    baseBitangent * anisotropyDirection.x);
            }
        }
    }

    float diffuse = smoothstep(0.0, 1.0, NdotL);
    float specPower = mix(96.0, 8.0, roughness);
    float specular = pow(max(NdotL, 0.0), specPower) * (1.0 - roughness);
    if (hasAnisotropyBasis) {
        specular = anisotropicSpecular(
            specular,
            roughness,
            anisotropyStrength,
            N,
            L,
            anisotropyTangent,
            anisotropyBitangent);
    }
    float specularStrength = clamp(u_specularFactor, 0.0, 1.0);
    if (u_hasSpecularTextures.x > 0.5) {
        vec2 specularUv = transformUv(
            uvFromSet(u_specularTexCoordSets.x),
            u_specularTexOffsetScale,
            u_specularTexRotationSinCos);
        specularStrength *= texture(u_specularTexture, specularUv).a;
    }
    vec3 specularColor;
    vec3 diffuseColor;
    if (u_specularGlossinessWorkflow > 0.5) {
        specularColor = specGlossSpecularColor;
        diffuseColor = base.rgb * (1.0 - specGlossMaxSpecular);
    } else {
        vec3 dielectricSpecular =
            vec3(clamp(u_dielectricSpecularF0, 0.0, 1.0)) *
            max(u_specularColorFactor, vec3(0.0));
        if (u_hasSpecularTextures.y > 0.5) {
            vec2 specularColorUv = transformUv(
                uvFromSet(u_specularTexCoordSets.y),
                u_specularColorTexOffsetScale,
                u_specularColorTexRotationSinCos);
            dielectricSpecular *=
                texture(u_specularColorTexture, specularColorUv).rgb;
        }
        dielectricSpecular = clamp(dielectricSpecular, 0.0, 1.0) *
            specularStrength;
        specularColor = mix(dielectricSpecular, base.rgb, metallic);
        diffuseColor = base.rgb * (1.0 - metallic);
    }
    diffuseColor *= 1.0 - transmission;
    vec3 color = diffuseColor * (0.38 * occlusion + 0.62 * diffuse) +
                 diffuseColor * u_ambient.rgb * occlusion +
                 specularColor * specular +
                 emissive;
    if (dot(sheenColor, sheenColor) > 0.0) {
        float sheenNdotL = max(dot(N, L), 0.0);
        float sheenPower = mix(2.0, 0.5, sheenRoughness);
        float sheen = pow(1.0 - sheenNdotL, sheenPower) * sheenNdotL;
        color += sheenColor * sheen * (1.0 - metallic);
    }
    if (clearcoat > 0.0) {
        float clearcoatNdotL = max(dot(clearcoatNormal, L), 0.0);
        float clearcoatPower = mix(160.0, 8.0, clearcoatRoughness);
        float clearcoatSpecular =
            pow(clearcoatNdotL, clearcoatPower) * (1.0 - clearcoatRoughness);
        float clearcoatFresnel =
            0.04 + 0.96 * pow(1.0 - clearcoatNdotL, 5.0);
        float coatWeight = clamp(clearcoat * clearcoatFresnel, 0.0, 1.0);
        color = color * (1.0 - coatWeight) +
                vec3(clearcoatSpecular) * coatWeight;
    }
    alpha *= 1.0 - transmission;
    fragColor = vec4(color, alpha * clamp(u_renderOpacity, 0.0, 1.0));
}
)glsl";

static const char* kGltfInstancedVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_texcoord01;
layout(location = 3) in vec4 a_instanceCol0;
layout(location = 4) in vec4 a_instanceCol1;
layout(location = 5) in vec4 a_instanceCol2;
layout(location = 6) in vec4 a_instanceCol3;
layout(location = 7) in vec3 a_normalCol0;
layout(location = 8) in vec3 a_normalCol1;
layout(location = 9) in vec3 a_normalCol2;
layout(location = 10) in vec4 a_color;
layout(location = 11) in vec4 a_tangent;
layout(location = 12) in vec4 a_texcoord23;
layout(location = 13) in vec4 a_texcoord45;
layout(location = 14) in vec4 a_texcoord67;

uniform mat4 u_modelViewProjection;

out vec3 v_normal;
out vec3 v_position;
out vec4 v_texcoord01;
out vec4 v_color;
out vec4 v_tangent;
out vec4 v_texcoord23;
out vec4 v_texcoord45;
out vec4 v_texcoord67;

void main() {
    mat4 instanceModel = mat4(
        a_instanceCol0,
        a_instanceCol1,
        a_instanceCol2,
        a_instanceCol3);
    mat3 instanceNormal = mat3(
        a_normalCol0,
        a_normalCol1,
        a_normalCol2);
    mat3 instanceTangent = mat3(instanceModel);
    vec4 localPosition = instanceModel * vec4(a_position, 1.0);
    v_normal = normalize(instanceNormal * a_normal);
    v_position = localPosition.xyz;
    v_texcoord01 = a_texcoord01;
    v_color = a_color;
    vec3 tangent = a_tangent.xyz;
    if (dot(tangent, tangent) > 0.0) {
        tangent = normalize(instanceTangent * tangent);
    }
    v_tangent = vec4(tangent, a_tangent.w);
    v_texcoord23 = a_texcoord23;
    v_texcoord45 = a_texcoord45;
    v_texcoord67 = a_texcoord67;
    gl_PointSize = 1.0;
    gl_Position = u_modelViewProjection * localPosition;
}
)glsl";

// ============================================================
// Terrain lightweight shader — 32-byte compact TerrainGpuVertex layout
// POSITION(f32x3@0) + NORMAL(snorm16x3+pad@12) + TEXCOORD_0/1(unorm16x4@20)
// + geomorph heightDelta(f32@28) = 32 bytes。Quantized attributes arrive in
// the shader as normalized floats.
// This is the glTF shader MINUS all PBR-extension uniforms: it keeps only
// base color, raster-overlay compositing (slots 15-18), water mask (slot 19),
// terrain clip, directional lighting and render opacity. RTC origin stays
// baked into u_modelViewProjection (double precision on the CPU side).
// ============================================================

static const char* kTerrainVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_texcoord01;
layout(location = 3) in float a_heightDelta;  // geomorph:粗起点−真实高度(米)

uniform mat4 u_modelViewProjection;
uniform vec4 u_geomorphUpFactor;  // xyz=瓦片中心椭球法线, w=morphFactor
// Phase 2c Stage B 地形 GPU 位移:x=minHeight y=heightRange z=enabled w=gridSize。
uniform vec4 u_heightDisplace;
// 合批 Step 1:per-tile 高度纹理搬共享 texture2DArray(gridN+1 方每层,RG 打包
// 16bit 归一化高度),层号由 u_terrainLayers.x 给出。
uniform highp sampler2DArray u_heightTexture;
uniform vec4 u_terrainLayers;  // x=高度纹理层号
// 祖先高度重映射(无缝北极星机制 A):clipEnabled 语义分档 —— 0=关;1=旧
// 片元 discard 裁剪(遗留路径);2=**顶点级 UV 重映射**:本瓦片模板几何采样
// **祖先**的高度纹理子矩形(clipUv=子瓦片在祖先 UV 里的 scale-bias)。几何是
// 子瓦片自己的边+裙墙(真几何,无 discard 切缝),这是替掉"裁剪祖先"缝源的
// 核心。v_texcoord01 输出重映射后的祖先 UV → 片元所有以瓦片 UV 为基的消费
// 者(影像 scale-bias/法线场/页表)原样工作。
uniform vec4 u_clipUV;
uniform float u_clipEnabled;

out vec3 v_normal;
out vec3 v_position;
out vec4 v_texcoord01;
// 裙墙标志(1=裙顶点)。片元据此跳过法线场——裙墙是竖直墙面,其朝向不由高度场
// 决定,套用高度场法线会把墙照成"平地"。
out float v_skirt;

// Phase 2c P2 morph:反量化 RG 16bit 高度纹素→米(mr = (minHeight, heightRange))。
float eeSampleTerrainHeight(
    highp sampler2DArray tex, ivec2 texel, int layer, vec2 mr) {
    vec4 p = texelFetch(tex, ivec3(texel, layer), 0);
    float t = (p.r * 255.0 * 256.0 + p.g * 255.0) / 65535.0;
    return mr.x + t * mr.y;
}

// 无缝北极星 ①-1:边吸附的邻居高度**差值**表。存在每层 (gridN+1) 方高度数据
// 之下的 4 行里(每边一行,行内第 j 个纹素 = 该边第 j 个吸附节点的差值)。
// 量程固定 ±2048m(与 TerrainDisplacementTemplatePool::kEdgeLutDeltaRangeMeters
// 同源,改一处必须改另一处),故不需要每瓦片的 (min,range),零传输改动。
// ⚠️ 差值 0 落在量程中点,不是纹素 0。
float eeEdgeLutDelta(highp sampler2DArray tex, int node, int edge, int gridN,
                     int layer) {
    vec4 p = texelFetch(tex, ivec3(node, gridN + 1 + edge, layer), 0);
    float t = (p.r * 255.0 * 256.0 + p.g * 255.0) / 65535.0;
    return t * 4096.0 - 2048.0;
}

void main() {
    // cesium-native RTC: tile origin is baked into the MVP matrix (computed in
    // CPU double precision). a_position is relative to the tile center.
    // geomorph:沿瓦片中心椭球法线把顶点从粗起点(morph=0)长到真实高度(morph=1)。
    // heightDelta=0(如上采样子瓦片)或 w=1(无 morph)时 offset 为 0,退化为原样。
    // Phase 2c 裙墙自适应:裙顶点以 heightDelta=-1 哨兵标记(仅位移路径下有效)。
    // 对裙顶点 geomorph delta 归零(哨兵非真实 delta),位移 h 也归零(下方)→ 裙底
    // 停在椭球面,与位移后的边顶点撑成逐瓦片自适应墙(墙高=边缘真实地形高度)。
    float heightDelta = a_heightDelta;
    float skirt = (u_heightDisplace.z > 0.5 && heightDelta < -0.5) ? 1.0 : 0.0;
    heightDelta = mix(heightDelta, 0.0, skirt);
    vec3 morphPos = a_position +
        u_geomorphUpFactor.xyz * heightDelta * (1.0 - u_geomorphUpFactor.w);
    // Phase 2c P2:共享模板零高程面点沿法线采高度纹理位移。morph 连续生长——本纹理
    // 双分辨率采样:fine=本顶点栅格纹素(texelFetch NEAREST),coarse=四个偶数格点
    // 双线性(osgEarth 邻居平均:偶点=self→delta0,奇点=相邻偶点均值)。按 SSE 驱动
    // morphFactor(=u_geomorphUpFactor.w:0=粗起点≈父面,1=细真实)mix→跨 LOD 无 pop、
    // 相邻瓦片共享偶点高度一致→无接缝。enabled=0 的瓦片跳过(零回归)。
    // remap 模式:模板 UV → 祖先 UV(高度采样与下游 varying 都用它)。
    vec2 tileUv = a_texcoord01.xy;
    if (u_clipEnabled > 1.5) {
        tileUv = u_clipUV.xy + tileUv * u_clipUV.zw;
    }
    if (u_heightDisplace.z > 0.5) {
        float gridN = u_heightDisplace.w;
        int hLayer = int(u_terrainLayers.x + 0.5);
        vec2 gf = tileUv * gridN;                          // 栅格坐标 [0,gridN]
        vec2 mr = u_heightDisplace.xy;                      // (minHeight, heightRange)
        // fine = 1× 手工双线性。常规模式下模板节点与纹素重合(gf 整),权重
        // 退化为 0 → 逐位等于原 texelFetch 最近邻;remap 模式下 gf 落在祖先
        // 纹素之间,最近邻会出祖先纹素尺寸的台阶,双线性给出与祖先自身网格
        // 一致的线性插值面。
        vec2 fb = floor(gf);
        vec2 ff = gf - fb;
        float f00 = eeSampleTerrainHeight(u_heightTexture, ivec2(fb), hLayer, mr);
        float f10 = eeSampleTerrainHeight(
            u_heightTexture, ivec2(min(fb.x + 1.0, gridN), fb.y), hLayer, mr);
        float f01 = eeSampleTerrainHeight(
            u_heightTexture, ivec2(fb.x, min(fb.y + 1.0, gridN)), hLayer, mr);
        float f11 = eeSampleTerrainHeight(
            u_heightTexture,
            ivec2(min(fb.x + 1.0, gridN), min(fb.y + 1.0, gridN)), hLayer, mr);
        float hFine = mix(mix(f00, f10, ff.x), mix(f01, f11, ff.x), ff.y);
        vec2 g0 = floor(gf * 0.5) * 2.0;                    // 左下偶数格点
        vec2 fr = (gf - g0) * 0.5;                          // 2× 格内插值系数 [0,1]
        float e00 = eeSampleTerrainHeight(u_heightTexture, ivec2(g0), hLayer, mr);
        float e10 = eeSampleTerrainHeight(
            u_heightTexture, ivec2(min(g0.x + 2.0, gridN), g0.y), hLayer, mr);
        float e01 = eeSampleTerrainHeight(
            u_heightTexture, ivec2(g0.x, min(g0.y + 2.0, gridN)), hLayer, mr);
        float e11 = eeSampleTerrainHeight(
            u_heightTexture,
            ivec2(min(g0.x + 2.0, gridN), min(g0.y + 2.0, gridN)), hLayer, mr);
        float hCoarse = mix(mix(e00, e10, fr.x), mix(e01, e11, fr.x), fr.y);
        // 机制 B 边吸附(TileEdgeSnapResolver 供数,打包序 W+8E+64N+512S,
        // N=v0 边):邻居更粗 k 八度时,该边顶点高度改为自纹理 2^k 间距的
        // 线性插值 → 与邻居边几何共线,T-junction 在几何上不存在(残余=
        // 金字塔层间重采样差,裙墙覆盖)。仅常规模式参与(remap 由 CPU 清零)。
        float hOut = mix(hCoarse, hFine, u_geomorphUpFactor.w);
        // ①-1:高位是「本帧邻居差值表可用」标志(CPU 上传成功才置)。
        float snapPacked = u_terrainLayers.z;
        float lutValid = floor(snapPacked / 4096.0);
        snapPacked = snapPacked - lutValid * 4096.0;
        if (snapPacked > 0.5) {
            float eps = 0.5 / gridN;
            float sel = -1.0;
            if (tileUv.x < eps) sel = 0.0;
            else if (tileUv.x > 1.0 - eps) sel = 1.0;
            else if (tileUv.y < eps) sel = 2.0;
            else if (tileUv.y > 1.0 - eps) sel = 3.0;
            if (sel >= 0.0) {
                float lg = floor(mod(snapPacked / exp2(sel * 3.0), 8.0));
                if (lg > 0.5) {
                    float snapStep = exp2(min(lg, 6.0));
                    bool vertEdge = sel < 1.5;      // W/E:边沿 y 方向延伸
                    float a = vertEdge ? gf.y : gf.x;
                    float c = vertEdge ? gf.x : gf.y;
                    float a0 = floor(a / snapStep) * snapStep;
                    float a1 = min(a0 + snapStep, gridN);
                    float t = clamp((a - a0) / max(a1 - a0, 1e-6), 0.0, 1.0);
                    float hA = eeSampleTerrainHeight(u_heightTexture,
                        ivec2(vertEdge ? vec2(c, a0) : vec2(a0, c)),
                        hLayer, mr);
                    float hB = eeSampleTerrainHeight(u_heightTexture,
                        ivec2(vertEdge ? vec2(c, a1) : vec2(a1, c)),
                        hLayer, mr);
                    // ①-1:加上「粗邻居实际渲染高度 − 本纹素」的差 → 两侧在
                    // 共享边上求值同一个函数(恒等,而非逼近)。表不可用时
                    // lutValid=0,退回自纹理吸附 = 改前行为。
                    if (lutValid > 0.5) {
                        int e = int(sel);
                        int gN = int(gridN);
                        hA += eeEdgeLutDelta(u_heightTexture,
                            int(a0 / snapStep), e, gN, hLayer);
                        hB += eeEdgeLutDelta(u_heightTexture,
                            int(a1 / snapStep), e, gN, hLayer);
                    }
                    hOut = mix(hA, hB, t);          // 吸附:覆盖 morph 混合
                }
            }
        }
        // 裙顶点(skirt=1)h 归零 → 停在椭球面,撑起自适应裙墙。
        float h = hOut * (1.0 - skirt);
        morphPos += normalize(a_normal) * h;
    }
    v_normal = normalize(a_normal);
    v_position = morphPos;
    v_texcoord01 = vec4(tileUv, a_texcoord01.zw);
    v_skirt = skirt;
    gl_PointSize = 1.0;
    gl_Position = u_modelViewProjection * vec4(morphPos, 1.0);
}
)glsl";

static const char* kTerrainFragmentGLSL = R"glsl(#version 300 es
precision highp float;

in vec3 v_normal;
in vec3 v_position;
in vec4 v_texcoord01;
// 裙墙标志:裙墙是竖直墙面,其朝向不由高度场决定,必须跳过法线贴图。
in float v_skirt;

// 高度纹理在片元里也要读:B/A 通道存切空间法线(见 acquireHeightTexture)。
// 与顶点级同一 uniform、同一纹理单元(GL 按 program 绑定,不占新槽)。
uniform highp sampler2DArray u_heightTexture;
// z=位移是否启用(=法线场是否可用) w=栅格边长 gridN。与顶点级同一 uniform。
uniform vec4 u_heightDisplace;

uniform vec3 u_lightDir;
uniform vec4 u_ambient;
uniform vec3 u_sunTint;      // 太阳色温(CPU 按太阳高度角:白天微暖白→日落暖橙)
uniform vec3 u_eyePositionRTC;
uniform vec4 u_baseColor;
uniform float u_hasBaseColorTexture;
uniform sampler2D u_baseColorTexture;
uniform float u_alphaMode;
uniform float u_alphaCutoff;
uniform float u_renderOpacity;
uniform sampler2D u_directRasterTexture0;
uniform sampler2D u_directRasterTexture1;
uniform sampler2D u_directRasterTexture2;
uniform sampler2D u_directRasterTexture3;
uniform sampler2D u_gltfWaterMaskTexture;
uniform float u_directRasterTextureCount;
uniform vec4 u_directRasterTileUV0;
uniform vec4 u_directRasterTileUV1;
uniform vec4 u_directRasterTileUV2;
uniform vec4 u_directRasterTileUV3;
uniform float u_directRasterOpacity0;
uniform float u_directRasterOpacity1;
uniform float u_directRasterOpacity2;
uniform float u_directRasterOpacity3;
uniform float u_directRasterTexCoordSet0;
uniform float u_directRasterTexCoordSet1;
uniform float u_directRasterTexCoordSet2;
uniform float u_directRasterTexCoordSet3;
uniform float u_gltfHasWaterMask;
uniform vec4 u_gltfWaterMaskTranslationScale;
uniform vec4 u_gltfWaterMaskState;
uniform vec4 u_clipUV;
uniform float u_clipEnabled;
// 北极星合成方案页存储(Step 3):x=enabled y=gridN z=layerBase(B1 起由间接
// 纹理承载,shader 不再用)w=保留。
uniform highp sampler2DArray u_pageStore;
uniform vec4 u_pageStoreParams;
uniform vec4 u_pageStoreUv;
// 稀疏虚拟纹理(Step B1):间接纹理(RGBA8 编 layer 索引)。合批 Step 2:搬共享
// texture2DArray(固定 64² 每层,texel 写左上 gridN² 区),层号 u_terrainLayers.y。
uniform highp sampler2DArray u_pageStoreIndir;
uniform vec4 u_terrainLayers;  // x=高度纹理层(顶点) y=间接纹理层(片元)
// 刀2 路网 SDF 场"第二平面"(R8,与页存储影像页同层号驻留):
// 反向编码 0=远/1=线内/0.5=边缘(0 是失败安全值:未绑定采样恒 0 = 无线)。
uniform highp sampler2DArray u_roadField;
uniform highp sampler2DArray u_roadFieldIndir;  // 步3 场间接纹理
uniform vec4 u_roadFieldParams;  // x=enable y=cellZoom z=场纹素边长 w=D2 偏移编码范围
uniform vec4 u_roadFieldColor;   // 线色(RGBA 非预乘)
uniform vec4 u_roadFieldWidth;   // 宽度 ramp (z0,halfPx0,z1,halfPx1)
uniform vec4 u_pageGeomA;        // [瓦界对齐] 几何仿射 c0.xy, dU.xy
uniform vec4 u_pageGeomB;        // [瓦界对齐] 几何仿射 dV.xy

out vec4 fragColor;

// ============================================================
// 连续法线场:替代逐三角面 dFdx 叉积(那是"竖条"刻面的来源)
// ============================================================
// 位移面 S(u,v) = B(u,v) + up·h(u,v)(瓦片内 up≈常量,即位移方向)。取瓦片切
// 平面正交基 (e_u,e_v),则 ∂S/∂u × ∂S/∂v ∝ up − (h_u/L_u)·e_u − (h_v/L_v)·e_v,
// 括号内即高度纹理 B/A 存的切空间坡度 → N = nx·e_u + ny·e_v + nz·up。
//
// 切基不用新 uniform 传:由屏幕导数 2×2 反解 ∂P/∂u、∂P/∂v 再投掉 up 分量即得。
// uv 的地理朝向约定(v 朝北还是朝南、瓦片东西向米数)因此全不需要在 shader 里
// 假设——反解出的 e_u 天然就是"u 增大的世界方向",与烘焙时的 uv 同源。
//
// B/A 必须**手动**双线性:高度纹理是 NEAREST(R/G 打包 16bit,硬件插值会破坏
// 打包),直接 texelFetch 取法线会退化成逐纹素常量 = 方块刻面,不比三角刻面好。
bool eeTerrainNormalFromHeightTex(
    highp sampler2DArray tex, int layer, float gridN,
    vec2 uv, vec3 position, vec3 up, out vec3 outNormal) {
    vec2 duvdx = dFdx(uv);
    vec2 duvdy = dFdy(uv);
    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(det) < 1e-12) return false;
    vec3 dpdx = dFdx(position);
    vec3 dpdy = dFdy(position);
    float invDet = 1.0 / det;
    vec3 su = ( duvdy.y * dpdx - duvdx.y * dpdy) * invDet;
    vec3 sv = (-duvdy.x * dpdx + duvdx.x * dpdy) * invDet;
    vec3 tu = su - up * dot(up, su);
    vec3 tv = sv - up * dot(up, sv);
    float lu = length(tu);
    float lv = length(tv);
    if (lu < 1e-6 || lv < 1e-6) return false;

    // [T1 texop 消冗余] B/A 法线通道改**硬件双线性**(纹理滤波已改 LINEAR):
    // 4 texelFetch + 手工 mix → 1 textureLod(真机实测 terrain pass 86→77ms)。
    // 数学与手工双线性同构,差异仅硬件 8bit 子纹素权重量化(法线 xy ≤1 LSB)。
    // R/G 打包高度**不受影响**——其全部消费点是 texelFetch,规范定义 texelFetch
    // 无视滤波状态。textureLod(lod=0) 显式 LOD:免非一致控制流下隐式导数未定义。
    // 底部 edge-LUT 行守卫:g 钳到 gridN−ε,双线性脚永不跨进 LUT 首行
    // (v 权重在 g.y=gridN 处本就为 0,ε 只是把浮点尾差挡在构造安全侧)。
    vec2 g = min(clamp(uv, 0.0, 1.0) * gridN, vec2(gridN - 1.0e-4));
    vec2 ts = vec2(textureSize(tex, 0).xy);
    vec2 nxy =
        textureLod(tex, vec3((g + 0.5) / ts, float(layer)), 0.0).ba * 2.0 - 1.0;
    float nz = sqrt(max(0.0, 1.0 - dot(nxy, nxy)));
    outNormal = normalize(nxy.x * (tu / lu) + nxy.y * (tv / lv) + nz * up);
    return true;
}


vec2 uvFromSet(float texCoordSet) {
    int setIndex = int(floor(texCoordSet + 0.5));
    return setIndex == 1 ? v_texcoord01.zw : v_texcoord01.xy;
}

vec4 alphaOver(vec4 base, vec4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

vec4 applyDirectRaster(
    vec4 base,
    sampler2D rasterTexture,
    float texCoordSet,
    vec4 tileUV,
    float opacity) {
    vec2 uv = uvFromSet(texCoordSet);
    // set 0 在 VS 里已被祖先模板的 scale-bias 重映射(clipMode>1.5 那支),
    // 其余 set 是原始子瓦局部 UV —— 必须补同一 scale-bias(镜像下方 pageStore
    // psUv 的处理)。aa99a4ac5 只修了 pageStore 路,directComposite 这条**回落路**
    // 漏了:过渡期页未驻留时画面正是它,GCJ(set 1)错整一个 LOD 窗口,页到齐
    // 被 alphaOver 盖掉 = "瞬间异常,稳态自愈"(imagery.md V11 真机复现)。
    // 标准底图 texCoordSet=0 不进此分支,行为逐位不变。glTF 变体不加:mode 2
    // 只发给位移模板路径,真实网格两套 texcoord 都是烘焙祖先 UV,无需补。
    if (texCoordSet > 0.5 && u_clipEnabled > 1.5) {
        uv = u_clipUV.xy + uv * u_clipUV.zw;
    }
    vec2 overlayUv = tileUV.xy + uv * tileUV.zw;
    return alphaOver(base, texture(rasterTexture, overlayUv), opacity);
}

vec4 applyGltfWaterMask(vec4 base, vec3 N, vec3 L, vec3 V) {
    if (u_gltfHasWaterMask < 0.5 || u_gltfWaterMaskState.x > 0.5) {
        return base;
    }
    float water = u_gltfWaterMaskState.y;
    if (u_gltfWaterMaskState.z > 0.5) {
        vec2 waterUv = u_gltfWaterMaskTranslationScale.xy +
            uvFromSet(0.0) * u_gltfWaterMaskTranslationScale.z;
        water = texture(u_gltfWaterMaskTexture, waterUv).r;
    }
    // 海洋像素轻度压暗+冷偏，使其与陆地有辨识度。
    vec3 waterRgb = base.rgb * 0.8 + vec3(0.01, 0.04, 0.07);
    // sun-glint：太阳在水面的镜面高光(Blinn-Phong)。H=半程向量，指数越高高光
    // 越紧。glint 只加进 waterRgb → 下方 mix 用 water 权重门控，陆地像素为 0；
    // facing 让背光侧(NdotL<0)淡出，避免夜面出现假高光。系数是可调旋钮。
    vec3 H = normalize(L + V);
    float facing = smoothstep(0.0, 0.05, dot(N, L));
    float glint = pow(max(dot(N, H), 0.0), 400.0) * facing;
    waterRgb += vec3(1.0, 0.95, 0.85) * glint * 0.5;
    return vec4(mix(base.rgb, waterRgb, clamp(water, 0.0, 1.0)), base.a);
}

void main() {
    vec2 terrainUv = uvFromSet(0.0);
    if (u_clipEnabled > 0.5 && u_clipEnabled < 1.5 &&
        (terrainUv.x < u_clipUV.x ||
         terrainUv.x > u_clipUV.x + u_clipUV.z ||
         terrainUv.y < u_clipUV.y ||
         terrainUv.y > u_clipUV.y + u_clipUV.w)) {
        discard;
    }
    float faceSign = gl_FrontFacing ? 1.0 : -1.0;
    vec3 up = normalize(v_normal);
    vec3 geomN;
    // 首选连续法线场(高度纹理 B/A);它逐像素平滑,消除逐三角面刻面("竖条")。
    if (!(u_heightDisplace.z > 0.5 && v_skirt < 0.5 &&
          eeTerrainNormalFromHeightTex(
              u_heightTexture, int(u_terrainLayers.x + 0.5),
              u_heightDisplace.w, terrainUv, v_position, up, geomN))) {
        // 回落:位移面的逐三角面几何法线(局部位置屏幕导数叉积)。这是法线场不
        // 可用时的原路径(无位移的椭球回落/fill 代理/裙墙/反解退化)——它逐三角面
        // 恒定,在规则网格上表现为对角刻面。平坦时导数≈椭球 up,自然退化。叉积
        // 符号随屏幕朝向/winding 不定,用 v_normal 定向保证朝外。
        vec3 dpx = dFdx(v_position);
        vec3 dpy = dFdy(v_position);
        vec3 reliefN = cross(dpx, dpy);
        geomN = up;
        if (dot(reliefN, reliefN) > 1e-12) {
            geomN = normalize(reliefN);
            if (dot(geomN, v_normal) < 0.0) { geomN = -geomN; }
        }
    }
    vec3 N = geomN * faceSign;
    vec3 L = normalize(u_lightDir);
    float NdotL = max(dot(N, L), 0.0);

    vec4 base = u_baseColor;
    if (u_hasBaseColorTexture > 0.5) {
        base *= texture(u_baseColorTexture, terrainUv);
    }
    // C-1:全部 directComposite 层按序合成在页存储之前。页存储此时承载的是**同一个
    // 有序源列表**的合成结果(不再只有底图),故它按 cell 粒度整体覆盖是对的 ——
    // 覆盖的是同一批源的上采样祖先版本,换成高清版本。
    // (E4-3 曾按 role 把 annotation 层挪到页存储之后绕开覆盖;C-1 之后那么做会
    // 让同一个源在页内和页外各合成一次 = 清晰版上再糊一层。已连同 uniform 撤掉。)
    if (u_directRasterTextureCount > 0.5) {
        base = applyDirectRaster(
            base, u_directRasterTexture0, u_directRasterTexCoordSet0,
            u_directRasterTileUV0, u_directRasterOpacity0);
    }
    if (u_directRasterTextureCount > 1.5) {
        base = applyDirectRaster(
            base, u_directRasterTexture1, u_directRasterTexCoordSet1,
            u_directRasterTileUV1, u_directRasterOpacity1);
    }
    if (u_directRasterTextureCount > 2.5) {
        base = applyDirectRaster(
            base, u_directRasterTexture2, u_directRasterTexCoordSet2,
            u_directRasterTileUV2, u_directRasterOpacity2);
    }
    if (u_directRasterTextureCount > 3.5) {
        base = applyDirectRaster(
            base, u_directRasterTexture3, u_directRasterTexCoordSet3,
            u_directRasterTileUV3, u_directRasterOpacity3);
    }
    // 合成方案页存储(Step 3):目标 capped 瓦片改采 sampler2DArray 页存储
    // (enabled=1),覆盖上采样 directComposite → 显示真实高清影像。瓦片规则切
    // gridN×gridN 页,mesh UV 落格算 layer + 层内局部 UV,单次索引 + 单次采样;
    // 层间不插值 + 每层 CLAMP_TO_EDGE 天然无页缝(§13.1)。enabled=0 恒不进。
    if (u_pageStoreParams.x > 0.5) {
        // 见 glTF 变体注释。terrainUv 是 set 0,页存储可能要另一套 → 单独取。
        float psPack = u_pageStoreParams.w;
        vec2 psPhase = vec2(mod(floor(psPack / 8.0), 64.0),
                            floor(psPack / 512.0));
        vec2 psUv = uvFromSet(mod(psPack, 8.0));
        // set 0 在 VS 里已被祖先模板的 scale-bias 重映射(clipMode>1.5 那支),
        // 其余 set 是原始 ancestor UV —— 不补同一个 scale-bias 就会整片错位一
        // 个 LOD 窗口(一阶误差,远大于 GCJ 本身)。GCJ 空间的精确 scale-bias 与
        // mercator 的略有差异,但 warp 在单瓦片内近似仿射,二阶量可忽略。
        if (mod(psPack, 8.0) > 0.5 && u_clipEnabled > 1.5) {
            psUv = u_clipUV.xy + psUv * u_clipUV.zw;
        }
        vec2 cells = max(u_pageStoreParams.yz, vec2(1.0));
        // 采样链收进单一治理点 eePageStoreCompose(PageStoreSamplingGLSL.h)。
        // [瓦界对齐] 位移路径 UV = 共享模板几何 UV,传逐瓦几何仿射
        // u_pageGeomA/B(与 instanced 同源),不能走 details-UV 标定的
        // u_pageStoreUv——GCJ 下差出瓦包围矩形翘曲,瓦界错缝 ~30m。
        base = eePageStoreCompose(
            base, psUv, u_pageGeomA, u_pageGeomB.xy, psPhase, cells,
            int(u_terrainLayers.y + 0.5), u_roadFieldParams.y,
            u_roadFieldParams, u_roadFieldWidth, u_roadFieldColor);
    }
    base = applyGltfWaterMask(base, N, L, normalize(u_eyePositionRTC - v_position));
    if (u_alphaMode > 0.5 && u_alphaMode < 1.5 && base.a < u_alphaCutoff) {
        discard;
    }
    float alpha = u_alphaMode > 1.5 ? base.a : 1.0;

    // GE 式半球光照(单一治理点见 TerrainSurfaceLightGLSL.h:系数来源 / 不用
    // smoothstep 的理由 / 暖阳冷阴影分配)。函数由 withTerrainLight() 注入。
    vec3 color = terrainSurfaceLight(base.rgb, NdotL, u_ambient.rgb, u_sunTint);
    fragColor = vec4(color, alpha * clamp(u_renderOpacity, 0.0, 1.0));
}
)glsl";

// ============================================================
// Terrain instanced shader (合批 Step 3)
// ============================================================
// 32B 位移模板逐顶点(loc 0-3,同 terrainShader)+ 96B per-instance 流
// (loc 4-9)。同 {schemeId,z,row} 可见瓦片一次 glDrawElementsInstanced 画完。
// per-instance:rel 帧 3 行(相对批参考帧 frame0 的刚体变换)+ dispMorph
// (minH·fade,range·fade,morphFactor,pageStore gridN)+ clipUv + layers
// (heightLayer,indirLayer,clipEnabled,_)。批级 u_modelViewProjection=
// viewProj·frame0,u_lightDir/u_eyePositionRTC 已变到 frame0 帧(updater)。
static const char* kTerrainInstancedVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_texcoord01;
layout(location = 3) in float a_heightDelta;
layout(location = 4) in vec4 i_relRow0;
layout(location = 5) in vec4 i_relRow1;
layout(location = 6) in vec4 i_relRow2;
layout(location = 7) in vec4 i_dispMorph;  // minH·fade, range·fade, morph, gridN
layout(location = 8) in vec4 i_clipUv;
layout(location = 9) in vec4 i_layers;     // heightLayer, indirLayer, clipEn, gridN
layout(location = 10) in vec4 i_pageUv;    // 几何仿射:c0.xy, dU.xy(瓦界对齐)
layout(location = 11) in vec4 i_pageAux;   // 相位 phase.xy + 几何仿射 dV.zw

uniform mat4 u_modelViewProjection;  // = viewProj · frame0
uniform highp sampler2DArray u_heightTexture;

out vec3 v_normal;
out vec3 v_position;
out vec4 v_texcoord01;
out float v_skirt;            // 1=裙顶点(片元据此跳过法线场,见 terrainShader 注释)
flat out float v_heightLayer; // 高度纹理层号(片元读 B/A 法线要用)
flat out vec4 v_pageParams;   // x=pageCellDesc y=indirLayer z=clipEnabled w=模板 gridN
flat out vec4 v_pageUv;       // 页 cell 定位(单位=源瓦片):origin.xy, span.zw
flat out vec4 v_pageAux;      // 祖先寻址相位:phase.xy
flat out vec4 v_clipUv;

// 模板栅格边长逐实例给(i_layers.w):自适应密度后不再是常量,coarse=64、
// dense=256(见 TerrainDisplacementTemplatePool.h terrainGridSizeForSse)。

float eeSampleTerrainHeight(
    highp sampler2DArray tex, ivec2 texel, int layer, vec2 mr) {
    vec4 p = texelFetch(tex, ivec3(texel, layer), 0);
    float t = (p.r * 255.0 * 256.0 + p.g * 255.0) / 65535.0;
    return mr.x + t * mr.y;
}

// ①-1 边吸附邻居差值表(语义同 kTerrainVertexGLSL,量程 ±2048m 固定)。
float eeEdgeLutDelta(highp sampler2DArray tex, int node, int edge, int gridN,
                     int layer) {
    vec4 p = texelFetch(tex, ivec3(node, gridN + 1 + edge, layer), 0);
    float t = (p.r * 255.0 * 256.0 + p.g * 255.0) / 65535.0;
    return t * 4096.0 - 2048.0;
}

void main() {
    float morph = i_dispMorph.z;
    int hLayer = int(i_layers.x + 0.5);
    vec2 mr = i_dispMorph.xy;
    // 裙墙自适应:裙顶点(heightDelta=-1 哨兵)geomorph delta 与位移 h 均归零。
    float heightDelta = a_heightDelta;
    float skirt = (heightDelta < -0.5) ? 1.0 : 0.0;
    heightDelta = mix(heightDelta, 0.0, skirt);
    // geomorph 方向 = 模板局部 +Z(共享模板恒在 ENU 帧,与 terrainShader 一致)。
    vec3 morphPos = a_position + vec3(0.0, 0.0, 1.0) * heightDelta * (1.0 - morph);
    // i_layers.z 打包:clipMode(0/1/2) + 4·snapPacked(见 batcher 注释)。
    float clipMode = mod(i_layers.z, 4.0);
    float snapPacked = floor(i_layers.z * 0.25);
    // ①-1:高位是「本帧邻居差值表可用」标志(组合后 ≤32767,float 仍精确)。
    float lutValid = floor(snapPacked / 4096.0);
    snapPacked = snapPacked - lutValid * 4096.0;
    // remap(clipMode==2,机制 A):模板 UV → 祖先 UV。语义见 kTerrainVertexGLSL。
    vec2 tileUv = a_texcoord01.xy;
    if (clipMode > 1.5) {
        tileUv = i_clipUv.xy + tileUv * i_clipUv.zw;
    }
    // 双分辨率高度采样(fine=1× 手工双线性:常规模式 gf 整、权重退化逐位同
    // 最近邻;remap 模式 gf 落纹素间,双线性免祖先纹素台阶。coarse=偶数格点
    // 双线性),morph 混合。注意 i_layers.w 是**高度纹理**的 gridN(remap 时=
    // 祖先的),与本模板顶点栅格密度解耦。
    float kGridSize = max(i_layers.w, 1.0);
    vec2 gf = tileUv * kGridSize;
    vec2 fb = floor(gf);
    vec2 ff = gf - fb;
    float f00 = eeSampleTerrainHeight(u_heightTexture, ivec2(fb), hLayer, mr);
    float f10 = eeSampleTerrainHeight(
        u_heightTexture, ivec2(min(fb.x + 1.0, kGridSize), fb.y), hLayer, mr);
    float f01 = eeSampleTerrainHeight(
        u_heightTexture, ivec2(fb.x, min(fb.y + 1.0, kGridSize)), hLayer, mr);
    float f11 = eeSampleTerrainHeight(
        u_heightTexture,
        ivec2(min(fb.x + 1.0, kGridSize), min(fb.y + 1.0, kGridSize)), hLayer, mr);
    float hFine = mix(mix(f00, f10, ff.x), mix(f01, f11, ff.x), ff.y);
    vec2 g0 = floor(gf * 0.5) * 2.0;
    vec2 fr = (gf - g0) * 0.5;
    float e00 = eeSampleTerrainHeight(u_heightTexture, ivec2(g0), hLayer, mr);
    float e10 = eeSampleTerrainHeight(
        u_heightTexture, ivec2(min(g0.x + 2.0, kGridSize), g0.y), hLayer, mr);
    float e01 = eeSampleTerrainHeight(
        u_heightTexture, ivec2(g0.x, min(g0.y + 2.0, kGridSize)), hLayer, mr);
    float e11 = eeSampleTerrainHeight(
        u_heightTexture,
        ivec2(min(g0.x + 2.0, kGridSize), min(g0.y + 2.0, kGridSize)), hLayer, mr);
    float hCoarse = mix(mix(e00, e10, fr.x), mix(e01, e11, fr.x), fr.y);
    // 机制 B 边吸附(同 kTerrainVertexGLSL,变量名对齐实例化路径)。
    float hOut = mix(hCoarse, hFine, morph);
    if (snapPacked > 0.5) {
        float eps = 0.5 / kGridSize;
        float sel = -1.0;
        if (tileUv.x < eps) sel = 0.0;
        else if (tileUv.x > 1.0 - eps) sel = 1.0;
        else if (tileUv.y < eps) sel = 2.0;
        else if (tileUv.y > 1.0 - eps) sel = 3.0;
        if (sel >= 0.0) {
            float lg = floor(mod(snapPacked / exp2(sel * 3.0), 8.0));
            if (lg > 0.5) {
                float snapStep = exp2(min(lg, 6.0));
                bool vertEdge = sel < 1.5;
                float a = vertEdge ? gf.y : gf.x;
                float c = vertEdge ? gf.x : gf.y;
                float a0 = floor(a / snapStep) * snapStep;
                float a1 = min(a0 + snapStep, kGridSize);
                float t = clamp((a - a0) / max(a1 - a0, 1e-6), 0.0, 1.0);
                float hA = eeSampleTerrainHeight(u_heightTexture,
                    ivec2(vertEdge ? vec2(c, a0) : vec2(a0, c)), hLayer, mr);
                float hB = eeSampleTerrainHeight(u_heightTexture,
                    ivec2(vertEdge ? vec2(c, a1) : vec2(a1, c)), hLayer, mr);
                // ①-1:加邻居差值(同 kTerrainVertexGLSL)。表不可用时退回
                // 自纹理吸附 = 改前行为。
                if (lutValid > 0.5) {
                    int e = int(sel);
                    int gN = int(kGridSize);
                    hA += eeEdgeLutDelta(u_heightTexture,
                        int(a0 / snapStep), e, gN, hLayer);
                    hB += eeEdgeLutDelta(u_heightTexture,
                        int(a1 / snapStep), e, gN, hLayer);
                }
                hOut = mix(hA, hB, t);
            }
        }
    }
    float h = hOut * (1.0 - skirt);
    morphPos += normalize(a_normal) * h;
    // 相对批参考帧的刚体变换(rel 三行,行主序;第 4 行恒 0001)。
    vec4 mp = vec4(morphPos, 1.0);
    vec3 world = vec3(dot(i_relRow0, mp), dot(i_relRow1, mp), dot(i_relRow2, mp));
    vec3 nrm = normalize(a_normal);
    vec3 worldN = normalize(vec3(
        dot(i_relRow0.xyz, nrm), dot(i_relRow1.xyz, nrm), dot(i_relRow2.xyz, nrm)));
    v_normal = worldN;
    v_position = world;
    v_texcoord01 = vec4(tileUv, a_texcoord01.zw);
    v_skirt = skirt;
    v_heightLayer = i_layers.x;
    v_pageParams = vec4(i_dispMorph.w, i_layers.y, clipMode, kGridSize);
    v_pageUv = i_pageUv;
    v_pageAux = i_pageAux;
    v_clipUv = i_clipUv;
    gl_Position = u_modelViewProjection * vec4(world, 1.0);
}
)glsl";

// T2 地形深度 prepass 的片元着色器:只写深度,不产颜色。
//
// **顶点段不另写一份**——prepass 命令直接复用 kTerrainVertexGLSL /
// kTerrainInstancedVertexGLSL(见 TerrainDepthPrepass::extractTerrainCommands
// 只换 shader 不换顶点布局)。位移/geomorph/边界吸附是一大段会持续演进的
// 逻辑,复制一份必然漂移,而漂移的表现是「符号莫名被遮挡」——极难归因。
//
// 写 vec4(0) 而非 discard:discard 会关掉早期深度测试。这里唯一的产出就是
// 深度,必须让它走快路径。
static const char* kTerrainDepthOnlyFragmentGLSL = R"glsl(#version 300 es
precision highp float;
out vec4 fragColor;
void main() { fragColor = vec4(0.0); }
)glsl";

static const char* kTerrainInstancedFragmentGLSL = R"glsl(#version 300 es
precision highp float;

in vec3 v_normal;
in vec3 v_position;
in vec4 v_texcoord01;
in float v_skirt;
flat in float v_heightLayer;
flat in vec4 v_pageParams;  // x=pageCellDesc y=indirLayer z=clipEnabled w=模板 gridN
flat in vec4 v_pageUv;      // 页 cell 定位(单位=源瓦片):origin.xy, span.zw
flat in vec4 v_pageAux;     // 祖先寻址相位:phase.xy
flat in vec4 v_clipUv;

uniform vec3 u_lightDir;
uniform vec4 u_ambient;
uniform vec3 u_sunTint;      // 太阳色温(CPU 按太阳高度角:白天微暖白→日落暖橙)
uniform vec4 u_baseColor;
uniform highp sampler2DArray u_pageStore;
uniform highp sampler2DArray u_pageStoreIndir;
uniform highp sampler2DArray u_heightTexture;
// 刀2 路网 SDF 场"第二平面"(R8,与页存储影像页同层号驻留):
// 反向编码 0=远/1=线内/0.5=边缘(0 是失败安全值:未绑定采样恒 0 = 无线)。
uniform highp sampler2DArray u_roadField;
uniform highp sampler2DArray u_roadFieldIndir;  // 步3 场间接纹理
uniform vec4 u_roadFieldParams;  // x=enable y=cellZoom z=场纹素边长 w=D2 偏移编码范围
uniform vec4 u_roadFieldColor;   // 线色(RGBA 非预乘)
uniform vec4 u_roadFieldWidth;   // 宽度 ramp (z0,halfPx0,z1,halfPx1)
uniform vec4 u_pageGeomA;        // [瓦界对齐] 几何仿射 c0.xy, dU.xy
uniform vec4 u_pageGeomB;        // [瓦界对齐] 几何仿射 dV.xy

out vec4 fragColor;

// 推导、回落条件、以及 B/A 为何必须手动双线性,见 kTerrainFragmentGLSL 同名
// 函数的注释(两处 shader 源独立,故各存一份;改一处必须同步另一处)。
bool eeTerrainNormalFromHeightTex(
    highp sampler2DArray tex, int layer, float gridN,
    vec2 uv, vec3 position, vec3 up, out vec3 outNormal) {
    vec2 duvdx = dFdx(uv);
    vec2 duvdy = dFdy(uv);
    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(det) < 1e-12) return false;
    vec3 dpdx = dFdx(position);
    vec3 dpdy = dFdy(position);
    float invDet = 1.0 / det;
    vec3 su = ( duvdy.y * dpdx - duvdx.y * dpdy) * invDet;
    vec3 sv = (-duvdy.x * dpdx + duvdx.x * dpdy) * invDet;
    vec3 tu = su - up * dot(up, su);
    vec3 tv = sv - up * dot(up, sv);
    float lu = length(tu);
    float lv = length(tv);
    if (lu < 1e-6 || lv < 1e-6) return false;
    // [T1 texop 消冗余] 同 kTerrainFragmentGLSL 那份(两份必须同步):B/A 走
    // 硬件双线性 1 textureLod;R/G texelFetch 消费点不受滤波状态影响。
    vec2 g = min(clamp(uv, 0.0, 1.0) * gridN, vec2(gridN - 1.0e-4));
    vec2 ts = vec2(textureSize(tex, 0).xy);
    vec2 nxy =
        textureLod(tex, vec3((g + 0.5) / ts, float(layer)), 0.0).ba * 2.0 - 1.0;
    float nz = sqrt(max(0.0, 1.0 - dot(nxy, nxy)));
    outNormal = normalize(nxy.x * (tu / lu) + nxy.y * (tv / lv) + nz * up);
    return true;
}

vec4 alphaOver(vec4 base, vec4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

void main() {
    vec2 terrainUv = v_texcoord01.xy;
    // mode==1 才 discard;mode==2(remap)几何即子瓦片自身,无需裁剪。
    if (v_pageParams.z > 0.5 && v_pageParams.z < 1.5 &&
        (terrainUv.x < v_clipUv.x || terrainUv.x > v_clipUv.x + v_clipUv.z ||
         terrainUv.y < v_clipUv.y || terrainUv.y > v_clipUv.y + v_clipUv.w)) {
        discard;
    }
    // 连续法线场优先,失败回落逐三角面几何法线(与 terrainShader 一致)。
    vec3 up = normalize(v_normal);
    vec3 geomN;
    if (!(v_pageParams.w > 0.5 && v_skirt < 0.5 &&
          eeTerrainNormalFromHeightTex(
              u_heightTexture, int(v_heightLayer + 0.5), v_pageParams.w,
              terrainUv, v_position, up, geomN))) {
        vec3 dpx = dFdx(v_position);
        vec3 dpy = dFdy(v_position);
        vec3 reliefN = cross(dpx, dpy);
        geomN = up;
        if (dot(reliefN, reliefN) > 1e-12) {
            geomN = normalize(reliefN);
            if (dot(geomN, v_normal) < 0.0) { geomN = -geomN; }
        }
    }
    vec3 N = gl_FrontFacing ? geomN : -geomN;
    vec3 L = normalize(u_lightDir);
    float NdotL = max(dot(N, L), 0.0);

    vec4 base = u_baseColor;
    // 页存储:资格闸保证全 cell 驻留 → 直接覆盖(无 directComposite fallback)。
    // cell 网格 = 影像源瓦片网格。cellsX/cellsY/texCoordSet 打包在 v_pageParams.x
    // (逐实例只剩一个槽,见 TerrainInstanceBatcher::packPageCellDescriptor)。
    float packed = v_pageParams.x;
    vec2 cells = max(vec2(mod(packed, 128.0),
                          mod(floor(packed / 128.0), 128.0)), vec2(1.0));
    float psSet = mod(floor(packed / 16384.0), 8.0);
    float psCellZoom = floor(packed / 131072.0);
    int indirLayer = int(v_pageParams.y + 0.5);
    // 实例化地形顶点只带 texcoord01 两套;set 0 是地形 scheme 的投影,GCJ 的
    // UV 烘在 set 1。硬编码 set 0 = 该特性静默失效。
    vec2 psUv = psSet > 0.5 ? v_texcoord01.zw : v_texcoord01.xy;
    // set 0 在 VS 里已被祖先模板的 scale-bias 重映射(clipMode>1.5 那支),
    // 其余 set 是原始 ancestor UV —— 不补同一个 scale-bias 就会整片错位一
    // 个 LOD 窗口(一阶误差,远大于 GCJ 本身)。GCJ 空间的精确 scale-bias 与
    // mercator 的略有差异,但 warp 在单瓦片内近似仿射,二阶量可忽略。
    if (psSet > 0.5 && v_pageParams.z > 1.5) {
        psUv = v_clipUv.xy + psUv * v_clipUv.zw;
    }
    // 采样链收进单一治理点 eePageStoreCompose(PageStoreSamplingGLSL.h)。
    // [瓦界对齐] UV = 共享模板几何 UV,仿射经实例流传入(c0=pageUv.xy,
    // dU=pageUv.zw,dV=pageAux.zw;相位=pageAux.xy)。
    base = eePageStoreCompose(
        base, psUv, v_pageUv, v_pageAux.zw, v_pageAux.xy, cells,
        int(v_pageParams.y + 0.5), psCellZoom,
        u_roadFieldParams, u_roadFieldWidth, u_roadFieldColor);

    // GE 式半球光照(与 terrainShader 共用 TerrainSurfaceLightGLSL.h 的单一
    // 函数;由 withTerrainLight() 注入)。
    vec3 color = terrainSurfaceLight(base.rgb, NdotL, u_ambient.rgb, u_sunTint);
    fragColor = vec4(color, 1.0);
}
)glsl";

// ============================================================
// Metal Shading Language 源码
// ============================================================

// ============================================================
// Color Shader (Vector Layers) — GLSL ES 3.0
// ============================================================

static const char* kColorVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;

uniform mat4 u_modelViewProjection;

void main() {
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kColorFragmentGLSL = R"glsl(#version 300 es
precision mediump float;

uniform vec4 u_color;
out vec4 fragColor;

void main() {
    fragColor = vec4(encodeSceneOutput(u_color.rgb), u_color.a);
}
)glsl";

// ============================================================
// Color Shader (Vector Layers) — MSL
// ============================================================

static const char* kColorVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexOut {
    float4 position [[position]];
};

vertex VertexOut colorVertex(VertexIn in [[stage_in]],
                              constant float4x4& u_modelViewProjection [[buffer(1)]]) {
    VertexOut out;
    out.position = u_modelViewProjection * float4(in.position, 1.0);
    return out;
}
)msl";

static const char* kColorFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

fragment float4 colorFragment(constant float4& u_color [[buffer(0)]]) {
    return u_color;
}
)msl";

// ============================================================
// Vector Line Shader (矢量数据系统 P1,设计 §6.2)
// 顶点带 prev/next,屏幕垂向 + miter join 在顶点着色器现算(球面上
// 屏幕垂向视角相关,不能预烘焙)。44B 顶点:pos(12)+prev(12)+next(12)
// +side(4)+lengthSoFar(4),对应 GLES VertexLayoutKind::VectorLine44。
// ============================================================

// ============================================================
// Vector Fill Shader (矢量 P6b 数据驱动顶点色 fill)
// 顶点 16B:pos(12)+color(4,RGBA8),对应 GLES VectorFill16。
// colorShader 保持 pos-only + u_color(stencil 分类/旧路径用)。
// ============================================================

static const char* kVectorFillVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;   // RGBA8 归一化

uniform mat4 u_modelViewProjection;

out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kVectorFillFragmentGLSL = R"glsl(#version 300 es
precision mediump float;

in vec4 v_color;
out vec4 fragColor;

void main() {
    fragColor = vec4(encodeSceneOutput(v_color.rgb), v_color.a);
}
)glsl";

static const char* kVectorFillVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorFillVertexIn {
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VectorFillVertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VectorFillVertexOut vectorFillVertex(
        VectorFillVertexIn in [[stage_in]],
        constant float4x4& u_modelViewProjection [[buffer(1)]]) {
    VectorFillVertexOut out;
    out.color = in.color;
    out.position = u_modelViewProjection * float4(in.position, 1.0);
    return out;
}
)msl";

static const char* kVectorFillFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorFillFragmentIn {
    float4 color;
};

fragment float4 vectorFillFragment(VectorFillFragmentIn in [[stage_in]]) {
    return in.color;
}
)msl";

// ============================================================
// Vector Extrusion Shader (V6 建筑挤出)
// 顶点 28B:pos(12)+normal(12)+color(4,RGBA8)。lambert 顶光 + 环境光。
// ============================================================

static const char* kVectorExtrusionVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_color;
uniform mat4 u_modelViewProjection;
uniform vec3 u_lightDir;
out vec4 v_color;
out float v_ndl;
void main() {
    vec3 n = normalize(a_normal);
    v_ndl = max(dot(n, -u_lightDir), 0.0);
    v_color = a_color;
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kVectorExtrusionFragmentGLSL = R"glsl(#version 300 es
precision mediump float;
in vec4 v_color;
in float v_ndl;
uniform float u_ambient;
out vec4 fragColor;
void main() {
    float l = u_ambient + (1.0 - u_ambient) * v_ndl;
    fragColor = vec4(encodeSceneOutput(v_color.rgb * l), v_color.a);
}
)glsl";

static const char* kVectorExtrusionVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;
struct VectorExtrusionVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float4 color [[attribute(2)]];
};
struct VectorExtrusionVertexOut {
    float4 position [[position]];
    float4 color;
    float ndl;
};
vertex VectorExtrusionVertexOut vectorExtrusionVertex(
        VectorExtrusionVertexIn in [[stage_in]],
        constant float4x4& u_modelViewProjection [[buffer(1)]],
        constant float3& u_lightDir [[buffer(2)]]) {
    VectorExtrusionVertexOut out;
    float3 n = normalize(in.normal);
    out.ndl = max(dot(n, -u_lightDir), 0.0);
    out.color = in.color;
    out.position = u_modelViewProjection * float4(in.position, 1.0);
    return out;
}
)msl";

static const char* kVectorExtrusionFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;
struct VectorExtrusionFragmentIn {
    float4 color;
    float ndl;
};
fragment float4 vectorExtrusionFragment(
        VectorExtrusionFragmentIn in [[stage_in]],
        constant float& u_ambient [[buffer(0)]]) {
    float l = u_ambient + (1.0 - u_ambient) * in.ndl;
    return float4(in.color.rgb * l, in.color.a);
}
)msl";

// ============================================================
// Vector Page Mesh Shader (C-2c:矢量画进页存储 array 层)
// 顶点 20B:pos(2×f32,瓦片本地归一化)+ extrude(2×f32,单位法线×半线宽/页像素)
// + color(4,RGBA8),对应 GLES VectorPageMesh20。
//
// **线宽在这里展开而不是在网格里**:a_extrude 的单位是页像素,u_extrudeScale 是
// 「一个页像素等于多少瓦片归一化单位」= 页覆盖的瓦片跨度 / 页边长。同一份网格
// 因此能画进任意 zoom 的页 —— 这是 C-2 干掉 8 倍放大糊的整个机制。
//
// **后端 y 方向差异全部烘在 u_modelViewProjection 里**(CPU 侧算),shader 两边
// 逐字符相同:GL 的 FBO 原点在左下、Metal 的 render target 原点在左上,把这个差异
// 塞进矩阵比在 shader 里分叉安全 —— 分叉过的地方后来都出过「只改一半」的事故。
// ============================================================

static const char* kVectorPageMeshVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_extrude;
layout(location = 2) in vec4 a_color;   // RGBA8 归一化

uniform mat4 u_modelViewProjection;
uniform vec2 u_extrudeScale;            // 页像素 → 瓦片归一化

out vec4 v_color;

void main() {
    v_color = a_color;
    vec2 p = a_position + a_extrude * u_extrudeScale;
    gl_Position = u_modelViewProjection * vec4(p, 0.0, 1.0);
}
)glsl";

static const char* kVectorPageMeshFragmentGLSL = R"glsl(#version 300 es
precision mediump float;

in vec4 v_color;
out vec4 fragColor;

void main() {
    fragColor = vec4(encodeSceneOutput(v_color.rgb), v_color.a);
}
)glsl";

static const char* kVectorPageMeshVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorPageMeshVertexIn {
    float2 position [[attribute(0)]];
    float2 extrude [[attribute(1)]];
    float4 color [[attribute(2)]];
};

struct VectorPageMeshVertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VectorPageMeshVertexOut vectorPageMeshVertex(
        VectorPageMeshVertexIn in [[stage_in]],
        constant float4x4& u_modelViewProjection [[buffer(1)]],
        constant float2& u_extrudeScale [[buffer(2)]]) {
    VectorPageMeshVertexOut out;
    out.color = in.color;
    float2 p = in.position + in.extrude * u_extrudeScale;
    out.position = u_modelViewProjection * float4(p, 0.0, 1.0);
    return out;
}
)msl";

static const char* kVectorPageMeshFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorPageMeshFragmentIn {
    float4 color;
};

fragment float4 vectorPageMeshFragment(VectorPageMeshFragmentIn in [[stage_in]]) {
    return in.color;
}
)msl";

static const char* kVectorLineVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_prev;
layout(location = 2) in vec3 a_next;
layout(location = 3) in float a_side;
layout(location = 4) in float a_lengthSoFar;
layout(location = 5) in vec4 a_color;   // P6b 数据驱动色(RGBA8 归一化)

uniform mat4 u_modelViewProjection;
uniform vec2 u_viewport;      // 视口像素 (w, h)
uniform float u_lineWidthPx;  // 全线宽(px),半宽 = /2

out float v_lengthSoFar;
out vec4 v_color;

// miter 长度下限对应 miter-limit(尖角防爆);挤出上限防近地平线
// w→0 时发散(设计 §6.2 锁定)。
const float kMiterMin = 0.25;         // miter-limit = 4
const float kMaxExtrudeNdc = 0.25;    // 单侧挤出 ≤ 1/4 半屏

void main() {
    vec4 cp = u_modelViewProjection * vec4(a_position, 1.0);
    vec4 cpr = u_modelViewProjection * vec4(a_prev, 1.0);
    vec4 cnx = u_modelViewProjection * vec4(a_next, 1.0);
    v_lengthSoFar = a_lengthSoFar;
    v_color = a_color;

    // 相机后方顶点不挤出(除法翻向会把 ribbon 拉花;段的可见部分由
    // 另一端撑开 + clip 收尾)。
    if (cp.w <= 0.0) {
        gl_Position = cp;
        return;
    }

    float aspect = u_viewport.x / max(u_viewport.y, 1.0);
    // NDC → 各向同性空间(x 乘 aspect),屏幕角度才是真角度
    vec2 s  = cp.xy  / cp.w;  s.x  *= aspect;
    vec2 sp = cpr.xy / cpr.w; sp.x *= aspect;
    vec2 sn = cnx.xy / cnx.w; sn.x *= aspect;

    vec2 dirA = s - sp;   // 前段方向(端点哨兵 prev==pos → 零向量)
    vec2 dirB = sn - s;   // 后段方向
    float lenA = length(dirA);
    float lenB = length(dirB);
    bool hasA = lenA > 1e-7 && cpr.w > 0.0;
    bool hasB = lenB > 1e-7 && cnx.w > 0.0;

    vec2 dir;
    float scale = 1.0;
    if (hasA && hasB) {
        vec2 na = normalize(dirA);
        vec2 nb = normalize(dirB);
        dir = normalize(na + nb);            // 角平分方向
        // miter 长度 = 1/cos(半夹角) = 1/dot(miter法向, 段法向),带下限
        vec2 normalB = vec2(-nb.y, nb.x);
        vec2 miterNormal = vec2(-dir.y, dir.x);
        scale = 1.0 / max(dot(miterNormal, normalB), kMiterMin);
    } else if (hasA) {
        dir = dirA / lenA;                   // 尾端点:用前段方向(butt cap)
    } else if (hasB) {
        dir = dirB / lenB;                   // 首端点:用后段方向
    } else {
        gl_Position = cp;                    // 完全退化(单点/共点)
        return;
    }

    vec2 normal = vec2(-dir.y, dir.x);
    // 半宽(px) → NDC:1px = 2/vpH NDC → halfWidth*2/vpH = lineWidth/vpH
    float halfWidthNdc = u_lineWidthPx / max(u_viewport.y, 1.0);
    vec2 offset = normal * a_side * halfWidthNdc * scale;
    float offLen = length(offset);
    if (offLen > kMaxExtrudeNdc) {
        offset *= kMaxExtrudeNdc / offLen;
    }
    offset.x /= aspect;                      // 回到 NDC 各向异性
    gl_Position = cp + vec4(offset * cp.w, 0.0, 0.0);
}
)glsl";

static const char* kVectorLineFragmentGLSL = R"glsl(#version 300 es
precision mediump float;

in float v_lengthSoFar;   // 沿线累计弧长(m)
in vec4 v_color;          // P6b 顶点色(逐要素,镶嵌期表达式求值烘入)
uniform float u_dashPeriodMeters;  // P6d dash:<=0 = 实线(与 stencil 路径同语义)
uniform float u_dashOnFraction;
out vec4 fragColor;

void main() {
    float a = v_color.a;
    if (u_dashPeriodMeters > 0.0) {
        float phase = fract(v_lengthSoFar / u_dashPeriodMeters);
        if (phase >= u_dashOnFraction) a = 0.0;
    }
    fragColor = vec4(encodeSceneOutput(v_color.rgb), a);
}
)glsl";

// MSL 双份约定(设计 §6.1)。Metal 端矢量路径当前不出货(Metal 合批
// Step4 同期挂起),源码保持契约、未经真机验证。
static const char* kVectorLineVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorLineVertexIn {
    float3 position [[attribute(0)]];
    float3 prev [[attribute(1)]];
    float3 next [[attribute(2)]];
    float side [[attribute(3)]];
    float lengthSoFar [[attribute(4)]];
    float4 color [[attribute(5)]];   // P6b 数据驱动色
};

struct VectorLineVertexOut {
    float4 position [[position]];
    float lengthSoFar;
    float4 color;
};

constant float kMiterMin = 0.25;
constant float kMaxExtrudeNdc = 0.25;

vertex VectorLineVertexOut vectorLineVertex(
        VectorLineVertexIn in [[stage_in]],
        constant float4x4& u_modelViewProjection [[buffer(1)]],
        constant float2& u_viewport [[buffer(2)]],
        constant float& u_lineWidthPx [[buffer(3)]]) {
    VectorLineVertexOut out;
    float4 cp = u_modelViewProjection * float4(in.position, 1.0);
    float4 cpr = u_modelViewProjection * float4(in.prev, 1.0);
    float4 cnx = u_modelViewProjection * float4(in.next, 1.0);
    out.lengthSoFar = in.lengthSoFar;
    out.color = in.color;
    out.position = cp;
    if (cp.w <= 0.0) return out;

    float aspect = u_viewport.x / max(u_viewport.y, 1.0f);
    float2 s = cp.xy / cp.w;   s.x *= aspect;
    float2 sp = cpr.xy / cpr.w; sp.x *= aspect;
    float2 sn = cnx.xy / cnx.w; sn.x *= aspect;
    float2 dirA = s - sp;
    float2 dirB = sn - s;
    float lenA = length(dirA);
    float lenB = length(dirB);
    bool hasA = lenA > 1e-7f && cpr.w > 0.0;
    bool hasB = lenB > 1e-7f && cnx.w > 0.0;

    float2 dir;
    float scale = 1.0;
    if (hasA && hasB) {
        float2 na = normalize(dirA);
        float2 nb = normalize(dirB);
        dir = normalize(na + nb);
        float2 normalB = float2(-nb.y, nb.x);
        float2 miterNormal = float2(-dir.y, dir.x);
        scale = 1.0 / max(dot(miterNormal, normalB), kMiterMin);
    } else if (hasA) {
        dir = dirA / lenA;
    } else if (hasB) {
        dir = dirB / lenB;
    } else {
        return out;
    }

    float2 normal = float2(-dir.y, dir.x);
    float halfWidthNdc = u_lineWidthPx / max(u_viewport.y, 1.0f);
    float2 offset = normal * in.side * halfWidthNdc * scale;
    float offLen = length(offset);
    if (offLen > kMaxExtrudeNdc) {
        offset *= kMaxExtrudeNdc / offLen;
    }
    offset.x /= aspect;
    out.position = cp + float4(offset * cp.w, 0.0, 0.0);
    return out;
}
)msl";

static const char* kVectorLineFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorLineFragmentIn {
    float lengthSoFar;
    float4 color;
};

fragment float4 vectorLineFragment(
        VectorLineFragmentIn in [[stage_in]],
        constant float& u_dashPeriodMeters [[buffer(0)]],
        constant float& u_dashOnFraction [[buffer(1)]]) {
    float a = in.color.a;
    if (u_dashPeriodMeters > 0.0) {
        float phase = fract(in.lengthSoFar / u_dashPeriodMeters);
        if (phase >= u_dashOnFraction) a = 0.0;
    }
    return float4(in.color.rgb, a);
}
)msl";

// ============================================================
// Vector Line Stencil Shader(P6d stencil 贴地线)
// 墙带体 24B:pos(12)+extrude(12,miter 方向×缩放×左右符号,CPU 烘入)。
// VS 按眼深把像素线宽换算世界米挤出:halfW = u_halfWidthPerEyeZ*|ec.z|
// (u_halfWidthPerEyeZ = lineWidthPx*tan(fovy/2)/vpH)。世界空间挤出无
// 除 w,不需要屏幕线 shader 的 NDC 挤出上限防御。两个 stencil pass 共用
// (体 pass 颜色写被后端关闭,u_color 无效)。
// ============================================================

static const char* kVectorLineStencilVertexGLSL = R"glsl(#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_extrude;  // miter 方向×缩放×侧符号

uniform mat4 u_modelViewProjection;
uniform mat4 u_modelView;
uniform float u_halfWidthPerEyeZ;  // 每米眼深对应的半宽(m)

void main() {
    vec4 ec = u_modelView * vec4(a_position, 1.0);
    float halfW = u_halfWidthPerEyeZ * abs(ec.z);
    vec3 world = a_position + a_extrude * halfW;
    gl_Position = u_modelViewProjection * vec4(world, 1.0);
}
)glsl";

// dash 不在这里判:虚线已在镶嵌期切成一段段独立划体(几何边界),
// FS 只出纯色 —— 从体面插值里程在侧视下有视差会撕裂花纹(见
// FeatureRenderLayer::appendLineVolume 的 dash 切分注释)。
static const char* kVectorLineStencilFragmentGLSL = R"glsl(#version 300 es
precision mediump float;

uniform vec4 u_color;
out vec4 fragColor;

void main() {
    fragColor = vec4(encodeSceneOutput(u_color.rgb), u_color.a);
}
)glsl";

// MSL 双份约定。Metal 端 stencil 分类未接线(supportsStencilClassification
// 恒 false),源码保持契约、未经真机验证。
static const char* kVectorLineStencilVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorLineStencilVertexIn {
    float3 position [[attribute(0)]];
    float3 extrude [[attribute(1)]];
};

struct VectorLineStencilVertexOut {
    float4 position [[position]];
};

vertex VectorLineStencilVertexOut vectorLineStencilVertex(
        VectorLineStencilVertexIn in [[stage_in]],
        constant float4x4& u_modelViewProjection [[buffer(1)]],
        constant float4x4& u_modelView [[buffer(2)]],
        constant float& u_halfWidthPerEyeZ [[buffer(3)]]) {
    VectorLineStencilVertexOut out;
    float4 ec = u_modelView * float4(in.position, 1.0);
    float halfW = u_halfWidthPerEyeZ * abs(ec.z);
    float3 world = in.position + in.extrude * halfW;
    out.position = u_modelViewProjection * float4(world, 1.0);
    return out;
}
)msl";

static const char* kVectorLineStencilFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

fragment float4 vectorLineStencilFragment(
        constant float4& u_color [[buffer(0)]]) {
    return u_color;
}
)msl";

// ============================================================
// Vector Point/Symbol Shader
//   (矢量 P5a 点符号/编辑手柄 + P6b 顶点色 + P6c 图标)
// billboard quad 屏幕空间展开;fragment 双通道:
//   shape >= 0 → 内置解析 SDF 形状(任意尺寸锐利,零外部资源)
//   shape <  0 → IconAtlas 位图采样 × 顶点色 tint(美术图标)
// 顶点 36B:anchor(12)+offsetUnit(8)+uv(8)+color(4,RGBA8)+shape(4),
// GLES VectorPoint36。offsetUnit = 相对锚点的「符号尺寸倍数」偏移
// (中心锚定 ±0.5;pin/图集底尖锚定由 CPU 烘进 offsetUnit,shader 不分支),
// uv = 解析形状的局部坐标 [-1,1]² 或图集 uv。
// **契约**:下面 fragment 的 shape 分支值必须与 SymbolShape.h 枚举一致。
// ============================================================

// T2:符号 × 地形遮挡判定(GLSL/MSL 共用一份文本,同 kSymbolSdfBody 的约定)。
//
// **为什么在顶点级、按锚点判、整符号一次**:符号 quad 是屏幕空间展开的,而
// 深度只有锚点一个值。交给硬件逐像素深度测试的话,山坡会把 quad 横切成半个
// (真机上就是"billboard 斜视被削半")。maplibre 同样是拿锚点采地形深度纹理
// 决定整个符号的可见性,而不是逐像素测。
//
// **为什么比线性视距而不是比 NDC 深度**:NDC 深度是非线性的,一个能同时适配
// 近景与远景的 NDC bias 不存在。换成米制 bias 后语义是直白的"地形比锚点近这
// 么多米才算挡住",贴地符号不会自遮挡。反算式与 OffscreenPostProcess 的
// aerial fog 同一条(reverse-Z),两处必须一致。
//
// 淡出而非硬切:硬切会让符号在相机微动时闪烁。淡出带宽 = bias,共用一个量。
static const char* kSymbolTerrainOcclusionBody = R"glsl(
uniform sampler2D u_terrainDepth;
// x: 1=深度纹理可用(否则整条判定跳过);y: near(米);z: far(米);
// w: 容差**角比**(乘锚点距离 = 米制容差),CPU 按 fov/视口高从像素数换算
uniform vec4 u_terrainOcclusion;
// x: 容差下限(米);y: 遮挡到底后的最低可见度(仅图标消费,文字侧不读)
uniform vec4 u_symbolOcclusion;

// 符号遮挡判定。三条视觉约定,改这段前先读懂,否则很容易改回错的形态:
//  ① **判定只看锚点**。符号是"位置的注记",quad 其余像素没有 3D 位置
//     语义 —— 拿它们比深度是无中生有,按像素切 quad 更是传达了一条并不
//     存在的形状边界。故这里只采锚点一处深度,输出整符号可见度。
//     (硬件逐像素深度测试同理必须关,见 FeatureRenderLayer 的命令装配。)
//  ② **输出连续量,不是布尔**。二值判定会在临界点抖:相机一动、山脊扫过
//     锚点一个像素,整个符号闪掉。
//  ③ **容差是屏幕空间常量,不是世界空间常量**。观察者感知的是像素;固定
//     米数在近景过松、远景过紧,同一个数两端语义完全不同。
float eeSymbolTerrainVisibility(vec4 clipPos) {
    if (u_terrainOcclusion.x < 0.5 || clipPos.w <= 0.0) return 1.0;
    vec3 ndc = clipPos.xyz / clipPos.w;
    // 屏幕外的锚点无深度可采(采到会 clamp 到边缘像素 = 错的地形)。
    if (any(lessThan(ndc.xy, vec2(-1.0))) ||
        any(greaterThan(ndc.xy, vec2(1.0)))) return 1.0;
    float terrainZWin = texture(u_terrainDepth, ndc.xy * 0.5 + 0.5).r;
    // reverse-Z 清除值为 0 = 该像素没画到地形(天空/地平线外)→ 不遮挡。
    if (terrainZWin <= 0.0) return 1.0;
    float near = u_terrainOcclusion.y;
    float far = u_terrainOcclusion.z;
    float terrainDist =
        near * far / ((2.0 * terrainZWin - 1.0) * (far - near) + near);
    float anchorDist = near * far / (ndc.z * (far - near) + near);
    // 容差 = max(下限, 角比×距离)。下限吸收与距离无关的噪声:锚点高度由
    // CPU 采样、地表由 GPU 位移,两者差米级;掠射角下这点差换算成深度差
    // 会放大若干倍 —— 没有它锚点会**自己遮挡自己**(近景伪遮挡的真因)。
    float tol = max(u_symbolOcclusion.x, u_terrainOcclusion.w * anchorDist);
    // inFront > 0 = 地形挡在锚点前面。[0,tol) 判完全可见(容差死区),
    // [tol, 2·tol) 线性淡出 —— 死区与淡出带同宽,两者都随距离缩放,
    // 于是"埋多少像素开始淡、埋多少像素淡完"在近远景一致。
    float inFront = anchorDist - terrainDist;
    return clamp(1.0 - (inFront - tol) / tol, 0.0, 1.0);
}
)glsl";

static const std::string kVectorPointVertexGLSL =
    std::string(R"glsl(#version 300 es
layout(location = 0) in vec3 a_anchor;
layout(location = 1) in vec2 a_offsetUnit;  // 相对锚点偏移(符号尺寸倍数)
layout(location = 2) in vec2 a_uv;          // 局部坐标 或 图集 uv
layout(location = 3) in vec4 a_color;       // P6b 数据驱动色(RGBA8 归一化)
layout(location = 4) in float a_shape;      // >=0 内置形状;<0 图集

uniform mat4 u_modelViewProjection;
uniform vec2 u_viewport;       // 视口像素
uniform float u_pointSizePx;   // 符号基准尺寸(px:圆直径/方边长/图标高)
// >0 = 高空模式:深度顶到近平面(z/w = 该值,reverse-Z 近=1)。billboard
// 是锚点常数深度,高空下 quad 覆盖数百 km 地面,地形逐像素深度会把符号
// 斜切/整吞;该高度下地形起伏不足一像素,遮挡语义已无意义。背面点不会
// 误显:视野外/背面桶已被层级的地平线圆裁剪掉。
uniform float u_depthPushNdc;

out vec2 v_uv;
out vec4 v_color;
out float v_shape;
)glsl") + kSymbolTerrainOcclusionBody + R"glsl(
void main() {
    vec4 cp = u_modelViewProjection * vec4(a_anchor, 1.0);
    v_uv = a_uv;
    v_color = a_color;
    v_shape = a_shape;
    if (cp.w <= 0.0) {
        gl_Position = cp;      // 相机后方:不展开
        return;
    }
    // T2:整符号遮挡淡出。折进顶点色 alpha —— 片元侧无需改动,SDF 软边、
    // 图集 tint 都自然跟着淡。遮挡到底不清零而是落到最低可见度:完全消失
    // 会 popping,且丢掉"那边有个东西、在山后面"这条信息(文字侧不留底,
    // 半透明文字只是噪声,见 label VS)。
    v_color.a *= mix(u_symbolOcclusion.y, 1.0, eeSymbolTerrainVisibility(cp));
    // 像素偏移 → NDC:视口跨 2 个 NDC 单位。
    vec2 offsetNdc = a_offsetUnit * u_pointSizePx * 2.0 / u_viewport;
    gl_Position = cp + vec4(offsetNdc * cp.w, 0.0, 0.0);
    if (u_depthPushNdc > 0.0) {
        gl_Position.z = gl_Position.w * u_depthPushNdc;
    }
}
)glsl";

// 内置形状的解析 SDF(GLSL/MSL **同一份文本**,MSL 侧靠 vec2/vec4 别名
// 兼容 —— 形状数学是纯计算,双份维护是错位风险,不套用本文件的双份约定)。
// 约定:d < 0 = 形状内部;局部坐标 quad 半宽 = 1.0;形状留 ~0.05 边距防
// fwidth 软边被 quad 裁掉。
static const char* kSymbolSdfBody = R"glsl(
float sdCircle(vec2 p) { return length(p) - 0.9; }

float sdSquare(vec2 p) {
    vec2 d = abs(p) - vec2(0.82);
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

// 菱形:|x|+|y|-r 是 L1 距离,乘 1/√2 换成欧氏尺度(让 fwidth 软边宽度
// 与其它形状一致)。
float sdDiamond(vec2 p) { return (abs(p.x) + abs(p.y) - 0.95) * 0.70710678; }

// 正三角(尖朝上):y 翻转成尖朝下的标准式后镜像折叠。
float sdTriangle(vec2 p) {
    float k = 1.7320508;
    float r = 0.95;
    p.y = -p.y;
    p.x = abs(p.x) - r;
    p.y = p.y + r / k;
    if (p.x + k * p.y > 0.0) {
        p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
    }
    p.x -= clamp(p.x, -2.0 * r, 0.0);
    return -length(p) * sign(p.y);
}

// 五角星:两次镜像折叠把 5 个扇区收敛到一条边,再算点到边段距离。
float sdStar(vec2 p) {
    float r = 0.95;
    float rf = 0.45;   // 内外半径比
    vec2 k1 = vec2(0.809016994, -0.587785252);
    vec2 k2 = vec2(-k1.x, k1.y);
    p.y = -p.y;
    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;
    vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}

// 水滴图钉:头部圆 ∪ 从底尖线性收窄的尾锥(局部 y≈-1 处即锚点位置)。
float sdPin(vec2 p) {
    float head = length(p - vec2(0.0, 0.42)) - 0.55;
    vec2 a = vec2(0.0, -0.98);
    vec2 b = vec2(0.0, 0.42);
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    float tail = length(pa - ba * h) - h * 0.55;
    return min(head, tail);
}

// 分支值 = SymbolShape.h 枚举,改一处必须改另一处。
float symbolSdf(float shape, vec2 p) {
    if (shape < 0.5) return sdCircle(p);
    if (shape < 1.5) return sdSquare(p);
    if (shape < 2.5) return sdTriangle(p);
    if (shape < 3.5) return sdDiamond(p);
    if (shape < 4.5) return sdStar(p);
    return sdPin(p);
}
)glsl";

static const std::string kVectorPointFragmentGLSL =
    std::string(R"glsl(#version 300 es
precision mediump float;

uniform sampler2D u_iconAtlas;

in vec2 v_uv;
in vec4 v_color;
in float v_shape;
out vec4 fragColor;
)glsl") + kSymbolSdfBody + R"glsl(
void main() {
    if (v_shape < 0.0) {
        // 图集通道:位图 × 顶点色 tint(tint 全白 = 原图)。
        vec4 tex = texture(u_iconAtlas, v_uv);
        float alpha = tex.a * v_color.a;
        if (alpha <= 0.004) discard;
        fragColor = vec4(encodeSceneOutput(tex.rgb * v_color.rgb), alpha);
        return;
    }
    // 解析 SDF:fwidth 自适应软边(尺寸无关,任意缩放都是 ~1px)。
    float d = symbolSdf(v_shape, v_uv);
    float w = max(fwidth(d), 1e-5);
    float alpha = (1.0 - smoothstep(-w, w, d)) * v_color.a;
    if (alpha <= 0.004) discard;
    fragColor = vec4(encodeSceneOutput(v_color.rgb), alpha);
}
)glsl";

// MSL 双份约定;Metal 端矢量路径当前不出货,未经真机验证。
static const char* kVectorPointVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorPointVertexIn {
    float3 anchor [[attribute(0)]];
    float2 offsetUnit [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float4 color [[attribute(3)]];   // P6b 数据驱动色
    float shape [[attribute(4)]];    // P6c:>=0 内置形状;<0 图集
};

struct VectorPointVertexOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
    float shape;
};

vertex VectorPointVertexOut vectorPointVertex(
        VectorPointVertexIn in [[stage_in]],
        constant float4x4& u_modelViewProjection [[buffer(1)]],
        constant float2& u_viewport [[buffer(2)]],
        constant float& u_pointSizePx [[buffer(3)]],
        constant float& u_depthPushNdc [[buffer(4)]]) {
    VectorPointVertexOut out;
    float4 cp = u_modelViewProjection * float4(in.anchor, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    out.shape = in.shape;
    out.position = cp;
    if (cp.w <= 0.0) return out;
    float2 offsetNdc = in.offsetUnit * u_pointSizePx * 2.0 / u_viewport;
    out.position = cp + float4(offsetNdc * cp.w, 0.0, 0.0);
    // 语义见 GLSL 版注释:高空深度顶近平面(reverse-Z 近=1)。
    if (u_depthPushNdc > 0.0) {
        out.position.z = out.position.w * u_depthPushNdc;
    }
    return out;
}
)msl";

static const std::string kVectorPointFragmentMSL =
    std::string(R"msl(#include <metal_stdlib>
using namespace metal;
// 形状 SDF 与 GLSL 共用同一份文本(见 kSymbolSdfBody),别名补齐类型名。
#define vec2 float2
#define vec4 float4

struct VectorPointFragmentIn {
    float2 uv;
    float4 color;
    float shape;
};
)msl") + kSymbolSdfBody + R"msl(
fragment float4 vectorPointFragment(
        VectorPointFragmentIn in [[stage_in]],
        texture2d<float> u_iconAtlas [[texture(0)]],
        sampler u_sampler [[sampler(0)]]) {
    if (in.shape < 0.0) {
        float4 tex = u_iconAtlas.sample(u_sampler, in.uv);
        float alpha = tex.a * in.color.a;
        if (alpha <= 0.004) discard_fragment();
        return float4(tex.rgb * in.color.rgb, alpha);
    }
    float d = symbolSdf(in.shape, in.uv);
    float w = max(fwidth(d), 1e-5);
    float alpha = (1.0 - smoothstep(-w, w, d)) * in.color.a;
    if (alpha <= 0.004) discard_fragment();
    return float4(in.color.rgb, alpha);
}
)msl";

// ============================================================
// Vector Label Shader (矢量 P5b SDF 文字标注 + P5c placement opacity)
// 顶点 32B:anchor(12)+offsetPx(8)+uv(8)+opacity(4),对应 GLES
// VectorLabel32。锚点投影后按像素偏移屏幕展开(billboard);opacity 由
// CPU placement fade 回写(0 = 避让隐藏,顶点直接折叠裁掉不进光栅);
// fragment 采 SDF 图集 smoothstep 出字 + halo 描边(单 pass 双阈值)。
// ============================================================

static const std::string kVectorLabelVertexGLSL =
    std::string(R"glsl(#version 300 es
layout(location = 0) in vec3 a_anchor;
// xy = 相对锚点屏幕像素偏移(y 向上);z = placement fade opacity(0 = 隐藏)。
// opacity 并进 offset 而非独立 attribute:三属性布局(0/1/2)。
layout(location = 1) in vec3 a_offsetPx;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_modelViewProjection;
uniform vec2 u_viewport;
// 语义同 VectorPoint 的 u_depthPushNdc(高空深度顶近平面)。
uniform float u_depthPushNdc;

out vec2 v_uv;
out float v_opacity;
)glsl") + kSymbolTerrainOcclusionBody + R"glsl(
void main() {
    vec4 cp = u_modelViewProjection * vec4(a_anchor, 1.0);
    v_uv = a_uv;
    // T2:遮挡淡出乘进 placement fade —— 两者都是"这个标注该显示多少",
    // 相乘即可,不需要新的 varying。
    v_opacity = a_offsetPx.z * eeSymbolTerrainVisibility(cp);
    if (cp.w <= 0.0 || a_offsetPx.z <= 0.0) {
        // 折叠到裁剪空间外,整字形不进光栅。
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        return;
    }
    vec2 offsetNdc = a_offsetPx.xy * 2.0 / u_viewport;
    gl_Position = cp + vec4(offsetNdc * cp.w, 0.0, 0.0);
    if (u_depthPushNdc > 0.0) {
        gl_Position.z = gl_Position.w * u_depthPushNdc;
    }
}
)glsl";

static const char* kVectorLabelFragmentGLSL = R"glsl(#version 300 es
precision mediump float;

uniform highp sampler2DArray u_glyphAtlas;
uniform vec4 u_color;
uniform vec4 u_haloColor;
uniform float u_sdfEdge;       // 轮廓阈值(kSdfOnEdge/255)
uniform float u_sdfHaloDelta;  // halo 宽换算的 SDF 值差

in vec2 v_uv;
in float v_opacity;
out vec4 fragColor;

void main() {
    float d = texture(u_glyphAtlas, vec3(v_uv, 0.0)).r;
    float w = fwidth(d);
    float fill = smoothstep(u_sdfEdge - w, u_sdfEdge + w, d);
    float halo = smoothstep(u_sdfEdge - u_sdfHaloDelta - w,
                            u_sdfEdge - u_sdfHaloDelta + w, d);
    float alpha = max(fill * u_color.a, halo * u_haloColor.a) * v_opacity;
    if (alpha <= 0.004) discard;
    vec3 rgb = mix(u_haloColor.rgb, u_color.rgb, fill);
    fragColor = vec4(encodeSceneOutput(rgb), alpha);
}
)glsl";

// MSL 双份约定;Metal 端矢量路径当前不出货,未经真机验证。
static const char* kVectorLabelVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorLabelVertexIn {
    float3 anchor [[attribute(0)]];
    float3 offsetPx [[attribute(1)]];  // z = placement opacity
    float2 uv [[attribute(2)]];
};

struct VectorLabelVertexOut {
    float4 position [[position]];
    float2 uv;
    float opacity;
};

vertex VectorLabelVertexOut vectorLabelVertex(
        VectorLabelVertexIn in [[stage_in]],
        constant float4x4& u_modelViewProjection [[buffer(1)]],
        constant float2& u_viewport [[buffer(2)]],
        constant float& u_depthPushNdc [[buffer(3)]]) {
    VectorLabelVertexOut out;
    float4 cp = u_modelViewProjection * float4(in.anchor, 1.0);
    out.uv = in.uv;
    out.opacity = in.offsetPx.z;
    if (cp.w <= 0.0 || in.offsetPx.z <= 0.0) {
        out.position = float4(0.0, 0.0, 2.0, 1.0);
        return out;
    }
    float2 offsetNdc = in.offsetPx.xy * 2.0 / u_viewport;
    out.position = cp + float4(offsetNdc * cp.w, 0.0, 0.0);
    // 语义见 GLSL 版注释:高空深度顶近平面(reverse-Z 近=1)。
    if (u_depthPushNdc > 0.0) {
        out.position.z = out.position.w * u_depthPushNdc;
    }
    return out;
}
)msl";

static const char* kVectorLabelFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VectorLabelFragmentIn {
    float2 uv;
    float opacity;
};

fragment float4 vectorLabelFragment(
        VectorLabelFragmentIn in [[stage_in]],
        texture2d_array<float> u_glyphAtlas [[texture(0)]],
        sampler u_sampler [[sampler(0)]],
        constant float4& u_color [[buffer(0)]],
        constant float4& u_haloColor [[buffer(1)]],
        constant float& u_sdfEdge [[buffer(2)]],
        constant float& u_sdfHaloDelta [[buffer(3)]]) {
    float d = u_glyphAtlas.sample(u_sampler, in.uv, 0).r;
    float w = fwidth(d);
    float fill = smoothstep(u_sdfEdge - w, u_sdfEdge + w, d);
    float halo = smoothstep(u_sdfEdge - u_sdfHaloDelta - w,
                            u_sdfEdge - u_sdfHaloDelta + w, d);
    float alpha = max(fill * u_color.a, halo * u_haloColor.a) * in.opacity;
    if (alpha <= 0.004) discard_fragment();
    float3 rgb = mix(u_haloColor.rgb, u_color.rgb, fill);
    return float4(rgb, alpha);
}
)msl";

static const char* kGltfVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct GltfVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float4 texcoord01 [[attribute(2)]];
    float4 color    [[attribute(10)]];
    float4 tangent  [[attribute(11)]];
    float4 texcoord23 [[attribute(12)]];
    float4 texcoord45 [[attribute(13)]];
    float4 texcoord67 [[attribute(14)]];
};

struct GltfVertexOut {
    float4 position [[position]];
    float3 normal;
    float3 localPosition;
    float4 texcoord01;
    float4 color;
    float4 tangent;
    float4 texcoord23;
    float4 texcoord45;
    float4 texcoord67;
};

vertex GltfVertexOut gltfVertex(GltfVertexIn in [[stage_in]],
                                 constant float4x4& u_modelViewProjection [[buffer(1)]]) {
    GltfVertexOut out;
    out.position = u_modelViewProjection * float4(in.position, 1.0);
    out.normal = normalize(in.normal.xyz);
    out.localPosition = in.position;
    out.texcoord01 = in.texcoord01;
    out.color = in.color;
    out.tangent = in.tangent;
    out.texcoord23 = in.texcoord23;
    out.texcoord45 = in.texcoord45;
    out.texcoord67 = in.texcoord67;
    return out;
}
)msl";

static const char* kGltfFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

// 逐字节镜像 C++ GltfUniformBlock（GltfUniformBlock.h）。所有成员 4 字节对
// 齐（float / packed_*），唯一 16 字节对齐成员 float4x4 固定在 offset 0，
// 两侧自然布局逐字节一致。改字段时三方同步（C++ 块 / 本 struct / GLES
// name→offset 描述表）。
struct GltfTextureTransform {
    packed_float4 offsetScale;
    packed_float2 rotationSinCos;
};
struct GltfUniforms {
    float4x4 modelViewProjection;   // 仅 vertex 语义占位，fragment 不读
    packed_float3 modelOrigin;      // CPU-only，shader 不读
    float _reservedOrigin;
    packed_float4 geomorphUpFactor; // xyz=瓦片中心椭球法线, w=morphFactor
    packed_float3 lightDir;
    float useNormalMap;
    float debugNormalMap;
    packed_float4 ambient;
    packed_float3 eyePositionRTC;
    float _reservedEye;
    packed_float4 baseColor;
    float hasBaseColorTexture;
    packed_float4 materialFactors;
    float dielectricSpecularF0;
    packed_float4 hasMaterialTextures;
    packed_float2 anisotropyFactors;
    float hasAnisotropyTexture;
    packed_float2 hasSpecularTextures;
    float specularFactor;
    packed_float3 specularColorFactor;
    float specularGlossinessWorkflow;
    packed_float4 specularGlossinessFactor;
    float hasSpecularGlossinessTexture;
    float transmissionFactor;
    float hasTransmissionTexture;
    packed_float3 clearcoatFactors;
    packed_float3 hasClearcoatTextures;
    packed_float3 sheenColorFactor;
    float sheenRoughnessFactor;
    packed_float2 hasSheenTextures;
    packed_float3 emissiveFactor;
    packed_float4 textureCoordSets;
    float emissiveTexCoordSet;
    float anisotropyTexCoordSet;
    packed_float2 specularTexCoordSets;
    float specularGlossinessTexCoordSet;
    float transmissionTexCoordSet;
    packed_float3 clearcoatTexCoordSets;
    packed_float2 sheenTexCoordSets;
    float alphaMode;
    float alphaCutoff;
    float renderOpacity;
    float unlit;
    GltfTextureTransform baseColorTex;
    GltfTextureTransform metallicRoughnessTex;
    GltfTextureTransform anisotropyTex;
    GltfTextureTransform specularTex;
    GltfTextureTransform specularColorTex;
    GltfTextureTransform specularGlossinessTex;
    GltfTextureTransform transmissionTex;
    GltfTextureTransform clearcoatTex;
    GltfTextureTransform clearcoatRoughnessTex;
    GltfTextureTransform clearcoatNormalTex;
    GltfTextureTransform sheenColorTex;
    GltfTextureTransform sheenRoughnessTex;
    GltfTextureTransform normalTex;
    GltfTextureTransform occlusionTex;
    GltfTextureTransform emissiveTex;
    float directRasterTextureCount;
    packed_float4 directRasterTileUv[4];
    float directRasterOpacity[4];
    float directRasterTexCoordSet[4];
    float hasWaterMask;
    packed_float4 waterMaskTranslationScale;
    packed_float4 waterMaskState;
    packed_float4 clipUV;
    float clipEnabled;
    packed_float4 pageStoreParams;
    packed_float4 pageStoreUv;
    packed_float4 heightDisplace;  // Phase 2c Stage B(顶点消费,fragment 仅占位对齐)
    packed_float4 terrainLayers;   // 合批 Step 1:x=高度纹理 array 层号(顶点消费)
    packed_float4 sunTint;         // 日落太阳色温(rgb;MSL 地形暂用内部常量,此处为字节对齐镜像)
    packed_float4 roadFieldParams; // 刀2 场解算:x=enable y=cellZoom z=边长 w=偏移范围
    packed_float4 roadFieldColor;  // 线色(RGBA 非预乘)
    packed_float4 roadFieldWidth;  // 宽度 ramp (z0,halfPx0,z1,halfPx1)
    packed_float4 pageGeomA;       // [瓦界对齐] 几何仿射 c0.xy, dU.xy(位移路径)
    packed_float4 pageGeomB;       // [瓦界对齐] 几何仿射 dV.xy + 保留
};

float2 gltfTransformUv(float2 uv, float4 offsetScale, float2 sinCos) {
    float2 scaled = uv * offsetScale.zw;
    return float2(
        scaled.x * sinCos.y + scaled.y * sinCos.x,
        scaled.y * sinCos.y - scaled.x * sinCos.x) + offsetScale.xy;
}

float4 gltfAlphaOver(float4 base, float4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

float2 gltfUvFromSet(GltfVertexOut in, float texCoordSet);

float4 gltfApplyDirectRaster(float4 base,
                             GltfVertexOut in,
                             texture2d<float> rasterTexture,
                             sampler rasterSampler,
                             float texCoordSet,
                             float4 tileUV,
                             float opacity) {
    float2 overlayUv =
        tileUV.xy + gltfUvFromSet(in, texCoordSet) * tileUV.zw;
    return gltfAlphaOver(
        base,
        rasterTexture.sample(rasterSampler, overlayUv),
        opacity);
}

float2 gltfUvFromSet(GltfVertexOut in, float texCoordSet) {
    int setIndex = int(floor(texCoordSet + 0.5));
    if (setIndex == 1) return in.texcoord01.zw;
    if (setIndex == 2) return in.texcoord23.xy;
    if (setIndex == 3) return in.texcoord23.zw;
    if (setIndex == 4) return in.texcoord45.xy;
    if (setIndex == 5) return in.texcoord45.zw;
    if (setIndex == 6) return in.texcoord67.xy;
    if (setIndex == 7) return in.texcoord67.zw;
    return in.texcoord01.xy;
}

float4 gltfApplyWaterMask(float4 base,
                          GltfVertexOut in,
                          texture2d<float> waterMaskTexture,
                          sampler waterMaskSampler,
                          float hasWaterMask,
                          float4 translationScale,
                          float4 state,
                          float3 N,
                          float3 L,
                          float3 V) {
    if (hasWaterMask < 0.5 || state.x > 0.5) {
        return base;
    }
    float water = state.y;
    if (state.z > 0.5) {
        float2 waterUv =
            translationScale.xy + gltfUvFromSet(in, 0.0) * translationScale.z;
        water = waterMaskTexture.sample(waterMaskSampler, waterUv).r;
    }
    // 见 GLSL 侧注释；tint + sun-glint 系数镜像保持一致。
    float3 waterRgb = base.rgb * 0.8 + float3(0.01, 0.04, 0.07);
    float3 H = normalize(L + V);
    float facing = smoothstep(0.0, 0.05, dot(N, L));
    float glint = pow(max(dot(N, H), 0.0), 400.0) * facing;
    waterRgb += float3(1.0, 0.95, 0.85) * glint * 0.5;
    return float4(mix(base.rgb, waterRgb, clamp(water, 0.0, 1.0)), base.a);
}

float3 gltfApplyTbn(float3 tangent,
                    float3 bitangent,
                    float3 n,
                    float3 mapNormal) {
    float3 perturbed = float3x3(tangent, bitangent, n) * mapNormal;
    float perturbedLenSq = dot(perturbed, perturbed);
    return perturbedLenSq > 1e-8 ? normalize(perturbed) : n;
}

float3 gltfPerturbNormal(float3 n,
                         float2 uv,
                         float3 localPosition,
                         float4 tangentInput,
                         float normalScale,
                         texture2d<float> normalTexture,
                         sampler normalSampler) {
    float3 mapNormal = normalTexture.sample(normalSampler, uv).rgb * 2.0 - 1.0;
    mapNormal.xy *= normalScale;
    float mapNormalLenSq = dot(mapNormal, mapNormal);
    if (mapNormalLenSq < 1e-8) {
        return n;
    }
    mapNormal = normalize(mapNormal);
    if (dot(tangentInput.xyz, tangentInput.xyz) > 0.0) {
        float3 tangent = tangentInput.xyz - n * dot(n, tangentInput.xyz);
        if (dot(tangent, tangent) > 1e-8) {
            tangent = normalize(tangent);
            float3 bitangent = cross(n, tangent);
            float bitangentLenSq = dot(bitangent, bitangent);
            if (bitangentLenSq > 1e-8) {
                bitangent = normalize(bitangent) *
                    (tangentInput.w < 0.0 ? -1.0 : 1.0);
                return gltfApplyTbn(tangent, bitangent, n, mapNormal);
            }
        }
    }
    float3 dp1 = dfdx(localPosition);
    float3 dp2 = dfdy(localPosition);
    float2 duv1 = dfdx(uv);
    float2 duv2 = dfdy(uv);
    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (fabs(det) < 1e-8) {
        return n;
    }
    float3 tangent = (dp1 * duv2.y - dp2 * duv1.y) / det;
    float3 bitangent = (-dp1 * duv2.x + dp2 * duv1.x) / det;
    float tangentLenSq = dot(tangent, tangent);
    float bitangentLenSq = dot(bitangent, bitangent);
    if (tangentLenSq < 1e-8 || bitangentLenSq < 1e-8) {
        return n;
    }
    tangent = normalize(tangent);
    bitangent = normalize(bitangent);
    return gltfApplyTbn(tangent, bitangent, n, mapNormal);
}

struct GltfAnisotropyBasis {
    float3 tangent;
    float3 bitangent;
    bool valid;
};

GltfAnisotropyBasis gltfTangentSpaceBasis(float3 n,
                                          float2 uv,
                                          float3 localPosition,
                                          float4 tangentInput) {
    GltfAnisotropyBasis basis;
    basis.tangent = float3(1.0, 0.0, 0.0);
    basis.bitangent = float3(0.0, 1.0, 0.0);
    basis.valid = false;

    if (dot(tangentInput.xyz, tangentInput.xyz) > 0.0) {
        float3 tangent = tangentInput.xyz - n * dot(n, tangentInput.xyz);
        if (dot(tangent, tangent) > 1e-8) {
            tangent = normalize(tangent);
            float3 bitangent = cross(n, tangent);
            float bitangentLenSq = dot(bitangent, bitangent);
            if (bitangentLenSq > 1e-8) {
                basis.tangent = tangent;
                basis.bitangent = normalize(bitangent) *
                    (tangentInput.w < 0.0 ? -1.0 : 1.0);
                basis.valid = true;
                return basis;
            }
        }
    }

    float3 dp1 = dfdx(localPosition);
    float3 dp2 = dfdy(localPosition);
    float2 duv1 = dfdx(uv);
    float2 duv2 = dfdy(uv);
    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (fabs(det) < 1e-8) {
        return basis;
    }
    float3 tangent = (dp1 * duv2.y - dp2 * duv1.y) / det;
    float3 bitangent = (-dp1 * duv2.x + dp2 * duv1.x) / det;
    if (dot(tangent, tangent) < 1e-8 ||
        dot(bitangent, bitangent) < 1e-8) {
        return basis;
    }
    basis.tangent = normalize(tangent);
    basis.bitangent = normalize(bitangent);
    basis.valid = true;
    return basis;
}

float2 gltfRotateDirection(float2 direction, float rotation) {
    float s = sin(rotation);
    float c = cos(rotation);
    return float2(
        direction.x * c - direction.y * s,
        direction.x * s + direction.y * c);
}

float gltfAnisotropicSpecular(float isotropicSpecular,
                              float roughness,
                              float strength,
                              float3 n,
                              float3 light,
                              float3 tangent,
                              float3 bitangent) {
    if (strength <= 0.0) {
        return isotropicSpecular;
    }
    float3 planarLight = light - n * dot(n, light);
    float planarLenSq = dot(planarLight, planarLight);
    if (planarLenSq < 1e-8) {
        return isotropicSpecular;
    }
    planarLight = normalize(planarLight);
    float along = fabs(dot(planarLight, tangent));
    float directionalRoughness =
        clamp(mix(roughness, 1.0, strength * along), 0.04, 1.0);
    float directionalPower = mix(96.0, 8.0, directionalRoughness);
    float directionalSpecular =
        pow(max(dot(n, light), 0.0), directionalPower) *
        (1.0 - directionalRoughness);
    float perpendicularWeight = fabs(dot(planarLight, bitangent));
    float blendWeight =
        clamp(strength * max(along, perpendicularWeight), 0.0, 1.0);
    return mix(isotropicSpecular, directionalSpecular, blendWeight);
}

fragment float4 gltfFragment(GltfVertexOut in [[stage_in]],
                             bool frontFacing [[front_facing]],
                             constant GltfUniforms& u [[buffer(0)]],
                             texture2d<float> u_baseColorTexture [[texture(0)]],
                             texture2d<float> u_metallicRoughnessTexture [[texture(1)]],
                             texture2d<float> u_normalTexture [[texture(2)]],
                             texture2d<float> u_occlusionTexture [[texture(3)]],
                             texture2d<float> u_emissiveTexture [[texture(4)]],
                             texture2d<float> u_specularTexture [[texture(5)]],
                             texture2d<float> u_specularColorTexture [[texture(6)]],
                             texture2d<float> u_clearcoatTexture [[texture(7)]],
                             texture2d<float> u_clearcoatRoughnessTexture [[texture(8)]],
                             texture2d<float> u_clearcoatNormalTexture [[texture(9)]],
                             texture2d<float> u_sheenColorTexture [[texture(10)]],
                             texture2d<float> u_sheenRoughnessTexture [[texture(11)]],
                             texture2d<float> u_anisotropyTexture [[texture(12)]],
                             texture2d<float> u_specularGlossinessTexture [[texture(13)]],
                             texture2d<float> u_transmissionTexture [[texture(14)]],
                             texture2d<float> u_directRasterTexture0 [[texture(15)]],
                             texture2d<float> u_directRasterTexture1 [[texture(16)]],
                             texture2d<float> u_directRasterTexture2 [[texture(17)]],
                             texture2d<float> u_directRasterTexture3 [[texture(18)]],
                             texture2d<float> u_gltfWaterMaskTexture [[texture(19)]],
                             // SVT(Step B2b):真实 DEM 表面走此 glTF shader,页存储在此。
                             // 合批 Step 2:间接纹理搬 array(64² 每层,层号 u.terrainLayers.y)。
                             texture2d_array<float> u_pageStore [[texture(20)]],
                             texture2d_array<float> u_pageStoreIndir [[texture(21)]],
                             texture2d_array<float> u_roadField [[texture(23)]],
                             texture2d_array<float> u_roadFieldIndir [[texture(24)]],
                             sampler u_baseColorSampler [[sampler(0)]],
                             sampler u_metallicRoughnessSampler [[sampler(1)]],
                             sampler u_normalSampler [[sampler(2)]],
                             sampler u_occlusionSampler [[sampler(3)]],
                             sampler u_emissiveSampler [[sampler(4)]],
                             sampler u_specularSampler [[sampler(5)]],
                             sampler u_specularColorSampler [[sampler(6)]],
                             sampler u_clearcoatSampler [[sampler(7)]],
                             sampler u_clearcoatRoughnessSampler [[sampler(8)]],
                             sampler u_clearcoatNormalSampler [[sampler(9)]],
                             sampler u_sheenColorSampler [[sampler(10)]],
                             sampler u_sheenRoughnessSampler [[sampler(11)]],
                             sampler u_anisotropySampler [[sampler(12)]],
                             sampler u_specularGlossinessSampler [[sampler(13)]],
                             sampler u_transmissionSampler [[sampler(14)]],
                             // raster overlay (texture 15-18) 与 water mask (texture 19)
                             // 共用一个 sampler，encoder 无条件绑到 slot 15。
                             sampler u_tileSharedSampler [[sampler(15)]]) {
    float2 terrainUv = gltfUvFromSet(in, 0.0);
    if (u.clipEnabled > 0.5 &&
        (terrainUv.x < u.clipUV.x ||
         terrainUv.x > u.clipUV.x + u.clipUV.z ||
         terrainUv.y < u.clipUV.y ||
         terrainUv.y > u.clipUV.y + u.clipUV.w)) {
        discard_fragment();
    }
    float faceSign = frontFacing ? 1.0 : -1.0;
    float3 n = normalize(in.normal) * faceSign;
    float3 geometryN = n;
    float3 light = normalize(float3(u.lightDir));
    float2 baseColorUv = gltfTransformUv(
        gltfUvFromSet(in, u.textureCoordSets.x),
        u.baseColorTex.offsetScale,
        u.baseColorTex.rotationSinCos);
    float4 base = float4(u.baseColor) * in.color;
    if (u.hasBaseColorTexture > 0.5) {
        base *= u_baseColorTexture.sample(u_baseColorSampler, baseColorUv);
    }
    if (u.directRasterTextureCount > 0.5) {
        base = gltfApplyDirectRaster(
            base,
            in,
            u_directRasterTexture0,
            u_tileSharedSampler,
            u.directRasterTexCoordSet[0],
            float4(u.directRasterTileUv[0]),
            u.directRasterOpacity[0]);
    }
    if (u.directRasterTextureCount > 1.5) {
        base = gltfApplyDirectRaster(
            base,
            in,
            u_directRasterTexture1,
            u_tileSharedSampler,
            u.directRasterTexCoordSet[1],
            float4(u.directRasterTileUv[1]),
            u.directRasterOpacity[1]);
    }
    if (u.directRasterTextureCount > 2.5) {
        base = gltfApplyDirectRaster(
            base,
            in,
            u_directRasterTexture2,
            u_tileSharedSampler,
            u.directRasterTexCoordSet[2],
            float4(u.directRasterTileUv[2]),
            u.directRasterOpacity[2]);
    }
    if (u.directRasterTextureCount > 3.5) {
        base = gltfApplyDirectRaster(
            base,
            in,
            u_directRasterTexture3,
            u_tileSharedSampler,
            u.directRasterTexCoordSet[3],
            float4(u.directRasterTileUv[3]),
            u.directRasterOpacity[3]);
    }
    // SVT(Step B2b,镜像 GLSL):per-tile 间接纹理单次 NEAREST fetch 定位 array 层
    // 覆盖 directComposite;A 通道 resident 标志,miss 保留 directComposite(决策② 共存)。
    if (u.pageStoreParams.x > 0.5) {
        // 镜像 GLSL:cell 网格 = 影像源瓦片网格(单位=源瓦片),
        // t = origin + uv*span。标准 overlay 退化成 uv*gridN(零回归)。
        // 见 GLSL 侧 params.w 相位注释。
        float psPack = u.pageStoreParams.w;
        float2 psPhase = float2(fmod(floor(psPack / 8.0), 64.0),
                                floor(psPack / 512.0));
        float2 psUv = gltfUvFromSet(in, fmod(psPack, 8.0));
        float2 cells = max(float2(u.pageStoreParams.y, u.pageStoreParams.z),
                           float2(1.0));
        // 采样链收进单一治理点 eePageStoreCompose(PageStoreSamplingGLSL.h)。
        // 本变体 UV = details 逐顶点 texcoord,轴对齐 origin/span 退化仿射传入。
        base = eePageStoreCompose(
            u_pageStore, u_pageStoreIndir, u_roadField, u_roadFieldIndir,
            u_tileSharedSampler,
            base, psUv,
            float4(u.pageStoreUv.x, u.pageStoreUv.y, u.pageStoreUv.z, 0.0),
            float2(0.0, u.pageStoreUv.w), psPhase, cells,
            int(u.terrainLayers.y + 0.5), float(u.roadFieldParams[1]),
            float4(u.roadFieldParams), float4(u.roadFieldWidth),
            float4(u.roadFieldColor));
    }
    base = gltfApplyWaterMask(
        base,
        in,
        u_gltfWaterMaskTexture,
        u_tileSharedSampler,
        u.hasWaterMask,
        float4(u.waterMaskTranslationScale),
        float4(u.waterMaskState),
        n,
        light,
        normalize(float3(u.eyePositionRTC) - in.localPosition));
    if (u.alphaMode > 0.5 && u.alphaMode < 1.5 && base.a < u.alphaCutoff) {
        discard_fragment();
    }
    float alpha = u.alphaMode > 1.5 ? base.a : 1.0;
    if (u.unlit > 0.5) {
        return float4(base.rgb, alpha * clamp(u.renderOpacity, 0.0, 1.0));
    }
    float metallic = clamp(u.materialFactors.x, 0.0, 1.0);
    float roughness = clamp(u.materialFactors.y, 0.04, 1.0);
    if (u.hasMaterialTextures.x > 0.5) {
        float2 mrUv = gltfTransformUv(
            gltfUvFromSet(in, u.textureCoordSets.y),
            u.metallicRoughnessTex.offsetScale,
            u.metallicRoughnessTex.rotationSinCos);
        float4 mr = u_metallicRoughnessTexture.sample(
            u_metallicRoughnessSampler,
            mrUv);
        roughness = clamp(roughness * mr.g, 0.04, 1.0);
        metallic = clamp(metallic * mr.b, 0.0, 1.0);
    }
    float ndotl = max(dot(n, light), 0.0);
    if (u.hasMaterialTextures.y > 0.5) {
        float2 normalUv = gltfTransformUv(
            gltfUvFromSet(in, u.textureCoordSets.z),
            u.normalTex.offsetScale,
            u.normalTex.rotationSinCos);
        n = gltfPerturbNormal(
            n,
            normalUv,
            in.localPosition,
            in.tangent,
            u.materialFactors.z,
            u_normalTexture,
            u_normalSampler);
        ndotl = max(dot(n, light), 0.0);
    }
    float occlusion = 1.0;
    if (u.hasMaterialTextures.z > 0.5) {
        float2 occlusionUv = gltfTransformUv(
            gltfUvFromSet(in, u.textureCoordSets.w),
            u.occlusionTex.offsetScale,
            u.occlusionTex.rotationSinCos);
        float ao = u_occlusionTexture.sample(
            u_occlusionSampler,
            occlusionUv).r;
        occlusion = clamp(1.0 + u.materialFactors.w * (ao - 1.0), 0.0, 1.0);
    }
    float3 emissive = float3(u.emissiveFactor);
    if (u.hasMaterialTextures.w > 0.5) {
        float2 emissiveUv = gltfTransformUv(
            gltfUvFromSet(in, u.emissiveTexCoordSet),
            u.emissiveTex.offsetScale,
            u.emissiveTex.rotationSinCos);
        emissive *= u_emissiveTexture.sample(
            u_emissiveSampler,
            emissiveUv).rgb;
    }
    float3 sheenColor = max(float3(u.sheenColorFactor), float3(0.0));
    float sheenRoughness = clamp(u.sheenRoughnessFactor, 0.0, 1.0);
    if (u.hasSheenTextures.x > 0.5) {
        float2 sheenColorUv = gltfTransformUv(
            gltfUvFromSet(in, u.sheenTexCoordSets.x),
            u.sheenColorTex.offsetScale,
            u.sheenColorTex.rotationSinCos);
        sheenColor *= u_sheenColorTexture.sample(
            u_sheenColorSampler,
            sheenColorUv).rgb;
    }
    if (u.hasSheenTextures.y > 0.5) {
        float2 sheenRoughnessUv = gltfTransformUv(
            gltfUvFromSet(in, u.sheenTexCoordSets.y),
            u.sheenRoughnessTex.offsetScale,
            u.sheenRoughnessTex.rotationSinCos);
        sheenRoughness = clamp(
            sheenRoughness *
                u_sheenRoughnessTexture.sample(
                    u_sheenRoughnessSampler,
                    sheenRoughnessUv).a,
            0.0,
            1.0);
    }
    float clearcoat = clamp(u.clearcoatFactors.x, 0.0, 1.0);
    float clearcoatRoughness = clamp(u.clearcoatFactors.y, 0.0, 1.0);
    if (u.hasClearcoatTextures.x > 0.5) {
        float2 clearcoatUv = gltfTransformUv(
            gltfUvFromSet(in, u.clearcoatTexCoordSets.x),
            u.clearcoatTex.offsetScale,
            u.clearcoatTex.rotationSinCos);
        clearcoat *= u_clearcoatTexture.sample(
            u_clearcoatSampler,
            clearcoatUv).r;
    }
    if (u.hasClearcoatTextures.y > 0.5) {
        float2 clearcoatRoughnessUv = gltfTransformUv(
            gltfUvFromSet(in, u.clearcoatTexCoordSets.y),
            u.clearcoatRoughnessTex.offsetScale,
            u.clearcoatRoughnessTex.rotationSinCos);
        clearcoatRoughness = clamp(
            clearcoatRoughness *
                u_clearcoatRoughnessTexture.sample(
                    u_clearcoatRoughnessSampler,
                    clearcoatRoughnessUv).g,
            0.0,
            1.0);
    }
    float3 clearcoatNormal = geometryN;
    if (u.hasClearcoatTextures.z > 0.5) {
        float2 clearcoatNormalUv = gltfTransformUv(
            gltfUvFromSet(in, u.clearcoatTexCoordSets.z),
            u.clearcoatNormalTex.offsetScale,
            u.clearcoatNormalTex.rotationSinCos);
        clearcoatNormal = gltfPerturbNormal(
            geometryN,
            clearcoatNormalUv,
            in.localPosition,
            in.tangent,
            u.clearcoatFactors.z,
            u_clearcoatNormalTexture,
            u_clearcoatNormalSampler);
    }
    float3 specGlossSpecularColor = float3(0.0);
    float specGlossMaxSpecular = 0.0;
    if (u.specularGlossinessWorkflow > 0.5) {
        float4 specGloss = float4(
            clamp(float4(u.specularGlossinessFactor).rgb, 0.0, 1.0),
            clamp(float4(u.specularGlossinessFactor).a, 0.0, 1.0));
        if (u.hasSpecularGlossinessTexture > 0.5) {
            float2 sgUv = gltfTransformUv(
                gltfUvFromSet(in, u.specularGlossinessTexCoordSet),
                u.specularGlossinessTex.offsetScale,
                u.specularGlossinessTex.rotationSinCos);
            specGloss *= u_specularGlossinessTexture.sample(
                u_specularGlossinessSampler,
                sgUv);
        }
        specGloss = clamp(specGloss, 0.0, 1.0);
        roughness = clamp(1.0 - specGloss.a, 0.04, 1.0);
        metallic = 0.0;
        specGlossSpecularColor = specGloss.rgb;
        specGlossMaxSpecular = max(
            max(specGlossSpecularColor.r, specGlossSpecularColor.g),
            specGlossSpecularColor.b);
    }
    float transmission = clamp(u.transmissionFactor, 0.0, 1.0);
    if (transmission > 0.0 && u.hasTransmissionTexture > 0.5) {
        float2 transmissionUv = gltfTransformUv(
            gltfUvFromSet(in, u.transmissionTexCoordSet),
            u.transmissionTex.offsetScale,
            u.transmissionTex.rotationSinCos);
        transmission *= u_transmissionTexture.sample(
            u_transmissionSampler,
            transmissionUv).r;
    }
    transmission = clamp(transmission, 0.0, 1.0);
    float anisotropyStrength = clamp(u.anisotropyFactors.x, 0.0, 1.0);
    float2 anisotropyDirection = float2(1.0, 0.0);
    GltfAnisotropyBasis anisotropyBasis;
    anisotropyBasis.tangent = float3(1.0, 0.0, 0.0);
    anisotropyBasis.bitangent = float3(0.0, 1.0, 0.0);
    anisotropyBasis.valid = false;
    if (anisotropyStrength > 0.0) {
        float2 anisotropyUv = gltfTransformUv(
            gltfUvFromSet(in, u.anisotropyTexCoordSet),
            u.anisotropyTex.offsetScale,
            u.anisotropyTex.rotationSinCos);
        if (u.hasAnisotropyTexture > 0.5) {
            float3 anisotropySample = u_anisotropyTexture.sample(
                u_anisotropySampler,
                anisotropyUv).rgb;
            anisotropyDirection = anisotropySample.rg * 2.0 - 1.0;
            anisotropyStrength *= anisotropySample.b;
        }
        anisotropyStrength = clamp(anisotropyStrength, 0.0, 1.0);
        if (anisotropyStrength > 0.0) {
            if (dot(anisotropyDirection, anisotropyDirection) < 1e-8) {
                anisotropyDirection = float2(1.0, 0.0);
            }
            anisotropyDirection = normalize(
                gltfRotateDirection(anisotropyDirection, u.anisotropyFactors.y));
            anisotropyBasis = gltfTangentSpaceBasis(
                n,
                anisotropyUv,
                in.localPosition,
                in.tangent);
            if (anisotropyBasis.valid) {
                float3 baseTangent = anisotropyBasis.tangent;
                float3 baseBitangent = anisotropyBasis.bitangent;
                anisotropyBasis.tangent = normalize(
                    baseTangent * anisotropyDirection.x +
                    baseBitangent * anisotropyDirection.y);
                anisotropyBasis.bitangent = normalize(
                    -baseTangent * anisotropyDirection.y +
                    baseBitangent * anisotropyDirection.x);
            }
        }
    }
    float diffuse = smoothstep(0.0, 1.0, ndotl);
    float specPower = mix(96.0, 8.0, roughness);
    float specular = pow(max(ndotl, 0.0), specPower) * (1.0 - roughness);
    if (anisotropyBasis.valid) {
        specular = gltfAnisotropicSpecular(
            specular,
            roughness,
            anisotropyStrength,
            n,
            light,
            anisotropyBasis.tangent,
            anisotropyBasis.bitangent);
    }
    float specularStrength = clamp(u.specularFactor, 0.0, 1.0);
    if (u.hasSpecularTextures.x > 0.5) {
        float2 specularUv = gltfTransformUv(
            gltfUvFromSet(in, u.specularTexCoordSets.x),
            u.specularTex.offsetScale,
            u.specularTex.rotationSinCos);
        specularStrength *= u_specularTexture.sample(
            u_specularSampler,
            specularUv).a;
    }
    float3 specularColor;
    float3 diffuseColor;
    if (u.specularGlossinessWorkflow > 0.5) {
        specularColor = specGlossSpecularColor;
        diffuseColor = base.rgb * (1.0 - specGlossMaxSpecular);
    } else {
        float3 dielectricSpecular =
            float3(clamp(u.dielectricSpecularF0, 0.0, 1.0)) *
            max(float3(u.specularColorFactor), float3(0.0));
        if (u.hasSpecularTextures.y > 0.5) {
            float2 specularColorUv = gltfTransformUv(
                gltfUvFromSet(in, u.specularTexCoordSets.y),
                u.specularColorTex.offsetScale,
                u.specularColorTex.rotationSinCos);
            dielectricSpecular *= u_specularColorTexture.sample(
                u_specularColorSampler,
                specularColorUv).rgb;
        }
        dielectricSpecular = clamp(dielectricSpecular, 0.0, 1.0) *
            specularStrength;
        specularColor = mix(dielectricSpecular, base.rgb, metallic);
        diffuseColor = base.rgb * (1.0 - metallic);
    }
    diffuseColor *= 1.0 - transmission;
    float3 color = diffuseColor * (0.38 * occlusion + 0.62 * diffuse) +
                   diffuseColor * float4(u.ambient).rgb * occlusion +
                   specularColor * specular +
                   emissive;
    if (dot(sheenColor, sheenColor) > 0.0) {
        float sheenNdotL = max(dot(n, light), 0.0);
        float sheenPower = mix(2.0, 0.5, sheenRoughness);
        float sheen = pow(1.0 - sheenNdotL, sheenPower) * sheenNdotL;
        color += sheenColor * sheen * (1.0 - metallic);
    }
    if (clearcoat > 0.0) {
        float clearcoatNdotL = max(dot(clearcoatNormal, light), 0.0);
        float clearcoatPower = mix(160.0, 8.0, clearcoatRoughness);
        float clearcoatSpecular =
            pow(clearcoatNdotL, clearcoatPower) *
            (1.0 - clearcoatRoughness);
        float clearcoatFresnel =
            0.04 + 0.96 * pow(1.0 - clearcoatNdotL, 5.0);
        float coatWeight = clamp(clearcoat * clearcoatFresnel, 0.0, 1.0);
        color = color * (1.0 - coatWeight) +
                float3(clearcoatSpecular) * coatWeight;
    }
    alpha *= 1.0 - transmission;
    return float4(color, alpha * clamp(u.renderOpacity, 0.0, 1.0));
}
)msl";

// ============================================================
// Terrain lightweight shader — MSL
// 32-byte compact TerrainGpuVertex: position(f32x3@0)
// normal(short4Normalized@12, w = pad) texcoord01(ushort4Normalized@20)
// geomorph heightDelta(f32@28).
// Fragment consumes the shared GltfUniforms struct at buffer(0) (byte-exact
// mirror of GltfUniformBlock.h), same as gltfFragment — one setFragmentBytes
// per draw, far under Metal's 31-buffer cap.
// Entry points terrainVertex / terrainFragment are UNIQUE; helper functions
// are defined BEFORE use.
// ============================================================

static const char* kTerrainVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct TerrainVertexIn {
    float3 position [[attribute(0)]];
    // short4Normalized: xyz = snorm16 normal, w = layout pad (ignored).
    float4 normal   [[attribute(1)]];
    float4 texcoord01 [[attribute(2)]];
    float heightDelta [[attribute(3)]];  // geomorph:粗起点−真实高度(米)
};

struct TerrainVertexOut {
    float4 position [[position]];
    float3 normal;
    float3 localPosition;
    float4 texcoord01;
};

// Phase 2c P2 morph:反量化 RG 16bit 高度纹素→米(mr = (minHeight, heightRange))。
// 合批 Step 1:高度纹理搬 texture2d_array,层号经 u_terrainLayers.x(buffer(4))。
static inline float eeSampleTerrainHeight(
    texture2d_array<float> tex, uint2 texel, uint layer, float2 mr) {
    float4 p = tex.read(texel, layer, 0);
    float t = (p.r * 255.0 * 256.0 + p.g * 255.0) / 65535.0;
    return mr.x + t * mr.y;
}

vertex TerrainVertexOut terrainVertex(
    TerrainVertexIn in [[stage_in]],
    constant float4x4& u_modelViewProjection [[buffer(1)]],
    constant float4& u_geomorphUpFactor [[buffer(2)]],
    constant float4& u_heightDisplace [[buffer(3)]],
    constant float4& u_terrainLayers [[buffer(4)]],
    texture2d_array<float> u_heightTexture [[texture(22)]]) {
    TerrainVertexOut out;
    // Phase 2c 裙墙自适应:裙顶点以 heightDelta=-1 哨兵标记(仅位移路径下有效)。
    // 对裙顶点 geomorph delta 归零、位移 h 归零 → 裙底停在椭球面,与位移后的边
    // 顶点撑成逐瓦片自适应墙。geomorph:沿瓦片中心椭球法线把顶点从粗起点(w=0)长
    // 到真实高度(w=1)。
    float heightDelta = in.heightDelta;
    float skirt = (u_heightDisplace.z > 0.5 && heightDelta < -0.5) ? 1.0 : 0.0;
    heightDelta = mix(heightDelta, 0.0, skirt);
    float3 morphPos = in.position +
        u_geomorphUpFactor.xyz * heightDelta * (1.0 - u_geomorphUpFactor.w);
    // Phase 2c P2:共享模板零高程面点沿法线采高度纹理位移。morph 连续生长——本纹理
    // 双分辨率采样:fine=本顶点栅格纹素,coarse=四个偶数格点双线性(osgEarth 邻居平均)。
    // 按 SSE 驱动 morphFactor(=u_geomorphUpFactor.w:0=粗起点≈父面,1=细真实)mix→
    // 跨 LOD 无 pop、相邻瓦片共享偶点高度一致→无接缝。enabled=0 跳过(零回归)。
    if (u_heightDisplace.z > 0.5) {
        float gridN = u_heightDisplace.w;
        uint hLayer = uint(u_terrainLayers.x + 0.5);
        float2 gf = in.texcoord01.xy * gridN;              // 栅格坐标 [0,gridN]
        float2 mr = u_heightDisplace.xy;                   // (minHeight, heightRange)
        float hFine = eeSampleTerrainHeight(
            u_heightTexture, uint2(gf + 0.5), hLayer, mr);
        float2 g0 = floor(gf * 0.5) * 2.0;                 // 左下偶数格点
        float2 fr = (gf - g0) * 0.5;                       // 2× 格内插值系数 [0,1]
        float e00 = eeSampleTerrainHeight(u_heightTexture, uint2(g0), hLayer, mr);
        float e10 = eeSampleTerrainHeight(
            u_heightTexture, uint2(min(g0.x + 2.0, gridN), g0.y), hLayer, mr);
        float e01 = eeSampleTerrainHeight(
            u_heightTexture, uint2(g0.x, min(g0.y + 2.0, gridN)), hLayer, mr);
        float e11 = eeSampleTerrainHeight(
            u_heightTexture,
            uint2(min(g0.x + 2.0, gridN), min(g0.y + 2.0, gridN)), hLayer, mr);
        float hCoarse = mix(mix(e00, e10, fr.x), mix(e01, e11, fr.x), fr.y);
        // 裙顶点(skirt=1)h 归零 → 停在椭球面,撑起自适应裙墙。
        float h = mix(hCoarse, hFine, u_geomorphUpFactor.w) * (1.0 - skirt);
        morphPos += normalize(in.normal.xyz) * h;
    }
    out.position = u_modelViewProjection * float4(morphPos, 1.0);
    out.normal = normalize(in.normal.xyz);
    out.localPosition = morphPos;
    out.texcoord01 = in.texcoord01;
    return out;
}
)msl";

static const char* kTerrainFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

// 逐字节镜像 C++ GltfUniformBlock（GltfUniformBlock.h）。所有成员 4 字节对
// 齐（float / packed_*），唯一 16 字节对齐成员 float4x4 固定在 offset 0，
// 两侧自然布局逐字节一致。改字段时三方同步（C++ 块 / 本 struct / GLES
// name→offset 描述表）。
struct GltfTextureTransform {
    packed_float4 offsetScale;
    packed_float2 rotationSinCos;
};
struct GltfUniforms {
    float4x4 modelViewProjection;   // 仅 vertex 语义占位，fragment 不读
    packed_float3 modelOrigin;      // CPU-only，shader 不读
    float _reservedOrigin;
    packed_float4 geomorphUpFactor; // xyz=瓦片中心椭球法线, w=morphFactor
    packed_float3 lightDir;
    float useNormalMap;
    float debugNormalMap;
    packed_float4 ambient;
    packed_float3 eyePositionRTC;
    float _reservedEye;
    packed_float4 baseColor;
    float hasBaseColorTexture;
    packed_float4 materialFactors;
    float dielectricSpecularF0;
    packed_float4 hasMaterialTextures;
    packed_float2 anisotropyFactors;
    float hasAnisotropyTexture;
    packed_float2 hasSpecularTextures;
    float specularFactor;
    packed_float3 specularColorFactor;
    float specularGlossinessWorkflow;
    packed_float4 specularGlossinessFactor;
    float hasSpecularGlossinessTexture;
    float transmissionFactor;
    float hasTransmissionTexture;
    packed_float3 clearcoatFactors;
    packed_float3 hasClearcoatTextures;
    packed_float3 sheenColorFactor;
    float sheenRoughnessFactor;
    packed_float2 hasSheenTextures;
    packed_float3 emissiveFactor;
    packed_float4 textureCoordSets;
    float emissiveTexCoordSet;
    float anisotropyTexCoordSet;
    packed_float2 specularTexCoordSets;
    float specularGlossinessTexCoordSet;
    float transmissionTexCoordSet;
    packed_float3 clearcoatTexCoordSets;
    packed_float2 sheenTexCoordSets;
    float alphaMode;
    float alphaCutoff;
    float renderOpacity;
    float unlit;
    GltfTextureTransform baseColorTex;
    GltfTextureTransform metallicRoughnessTex;
    GltfTextureTransform anisotropyTex;
    GltfTextureTransform specularTex;
    GltfTextureTransform specularColorTex;
    GltfTextureTransform specularGlossinessTex;
    GltfTextureTransform transmissionTex;
    GltfTextureTransform clearcoatTex;
    GltfTextureTransform clearcoatRoughnessTex;
    GltfTextureTransform clearcoatNormalTex;
    GltfTextureTransform sheenColorTex;
    GltfTextureTransform sheenRoughnessTex;
    GltfTextureTransform normalTex;
    GltfTextureTransform occlusionTex;
    GltfTextureTransform emissiveTex;
    float directRasterTextureCount;
    packed_float4 directRasterTileUv[4];
    float directRasterOpacity[4];
    float directRasterTexCoordSet[4];
    float hasWaterMask;
    packed_float4 waterMaskTranslationScale;
    packed_float4 waterMaskState;
    packed_float4 clipUV;
    float clipEnabled;
    packed_float4 pageStoreParams;
    packed_float4 pageStoreUv;
    packed_float4 heightDisplace;  // Phase 2c Stage B(顶点消费,fragment 仅占位对齐)
    packed_float4 terrainLayers;   // 合批 Step 1:x=高度纹理 array 层号(顶点消费)
    packed_float4 sunTint;         // 日落太阳色温(rgb;MSL 地形暂用内部常量,此处为字节对齐镜像)
    packed_float4 roadFieldParams; // 刀2 场解算:x=enable y=cellZoom z=边长 w=偏移范围
    packed_float4 roadFieldColor;  // 线色(RGBA 非预乘)
    packed_float4 roadFieldWidth;  // 宽度 ramp (z0,halfPx0,z1,halfPx1)
    packed_float4 pageGeomA;       // [瓦界对齐] 几何仿射 c0.xy, dU.xy(位移路径)
    packed_float4 pageGeomB;       // [瓦界对齐] 几何仿射 dV.xy + 保留
};

// TerrainVertexOut is provided by the vertex MSL (the backend concatenates the
// vertex and fragment sources into a single library), so it must NOT be
// redefined here.

float2 terrainUvFromSet(TerrainVertexOut in, float texCoordSet) {
    int setIndex = int(floor(texCoordSet + 0.5));
    return setIndex == 1 ? in.texcoord01.zw : in.texcoord01.xy;
}

float4 terrainAlphaOver(float4 base, float4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

float4 terrainApplyDirectRaster(float4 base,
                                TerrainVertexOut in,
                                texture2d<float> rasterTexture,
                                sampler rasterSampler,
                                float texCoordSet,
                                float4 tileUV,
                                float opacity,
                                float4 clipUv,
                                float clipEnabled) {
    float2 uv = terrainUvFromSet(in, texCoordSet);
    // 镜像 GLSL applyDirectRaster:set 0 在 VS 已被祖先模板 remap,其余 set
    // 须在此补同一 scale-bias —— directComposite 是页未驻留时的回落路,漏补则
    // GCJ 过渡瞬间错一个 LOD 窗口(imagery.md V11)。
    if (texCoordSet > 0.5 && clipEnabled > 1.5) {
        uv = float2(clipUv.x, clipUv.y) + uv * float2(clipUv.z, clipUv.w);
    }
    float2 overlayUv = tileUV.xy + uv * tileUV.zw;
    return terrainAlphaOver(
        base,
        rasterTexture.sample(rasterSampler, overlayUv),
        opacity);
}

float4 terrainApplyWaterMask(float4 base,
                             TerrainVertexOut in,
                             texture2d<float> waterMaskTexture,
                             sampler waterMaskSampler,
                             float hasWaterMask,
                             float4 translationScale,
                             float4 state,
                             float3 N,
                             float3 L,
                             float3 V) {
    if (hasWaterMask < 0.5 || state.x > 0.5) {
        return base;
    }
    float water = state.y;
    if (state.z > 0.5) {
        float2 waterUv =
            translationScale.xy + in.texcoord01.xy * translationScale.z;
        water = waterMaskTexture.sample(waterMaskSampler, waterUv).r;
    }
    // 见 GLSL 侧注释；tint + sun-glint 系数镜像保持一致。
    float3 waterRgb = base.rgb * 0.8 + float3(0.01, 0.04, 0.07);
    float3 H = normalize(L + V);
    float facing = smoothstep(0.0, 0.05, dot(N, L));
    float glint = pow(max(dot(N, H), 0.0), 400.0) * facing;
    waterRgb += float3(1.0, 0.95, 0.85) * glint * 0.5;
    return float4(mix(base.rgb, waterRgb, clamp(water, 0.0, 1.0)), base.a);
}

fragment float4 terrainFragment(
    TerrainVertexOut in [[stage_in]],
    bool frontFacing [[front_facing]],
    constant GltfUniforms& u [[buffer(0)]],
    texture2d<float> u_baseColorTexture [[texture(0)]],
    texture2d<float> u_directRasterTexture0 [[texture(15)]],
    texture2d<float> u_directRasterTexture1 [[texture(16)]],
    texture2d<float> u_directRasterTexture2 [[texture(17)]],
    texture2d<float> u_directRasterTexture3 [[texture(18)]],
    texture2d<float> u_gltfWaterMaskTexture [[texture(19)]],
    // 合成方案页存储(Step 3):sampler2DArray 页存储在 water mask 之后的槽 20,
    // 复用同一 clamp/linear 采样器(层间不插值 + 每层 clamp 无页缝,§13.1)。
    texture2d_array<float> u_pageStore [[texture(20)]],
    // 稀疏虚拟纹理(Step B1):间接纹理(RGBA8 编 layer 索引)。合批 Step 2:搬
    // array(64² 每层,层号 u.terrainLayers.y),read() 整数寻址不占 sampler 槽。
    texture2d_array<float> u_pageStoreIndir [[texture(21)]],
                             texture2d_array<float> u_roadField [[texture(23)]],
                             texture2d_array<float> u_roadFieldIndir [[texture(24)]],
    // Metal argument tables cap samplers at 0-15; terrain imagery all uses the
    // same clamp/linear sampling, so a single shared sampler at slot 0 covers
    // the base color, raster overlay (textures 15-18) and water mask (19)
    // textures without exceeding the sampler limit.
    sampler u_terrainSampler [[sampler(0)]]) {
    float2 terrainUv = in.texcoord01.xy;
    if (u.clipEnabled > 0.5 &&
        (terrainUv.x < u.clipUV.x ||
         terrainUv.x > u.clipUV.x + u.clipUV.z ||
         terrainUv.y < u.clipUV.y ||
         terrainUv.y > u.clipUV.y + u.clipUV.w)) {
        discard_fragment();
    }
    float faceSign = frontFacing ? 1.0 : -1.0;
    // Phase 2c P4 浮雕法线(同 GLES):位移面真实几何法线 = 局部位置屏幕导数叉积,
    // 替光滑椭球法线消除位移地形平光照。平坦时自然退化;用 in.normal 定向朝外。
    float3 dpx = dfdx(in.localPosition);
    float3 dpy = dfdy(in.localPosition);
    float3 reliefN = cross(dpx, dpy);
    float3 geomN = normalize(in.normal);
    if (dot(reliefN, reliefN) > 1e-12) {
        geomN = normalize(reliefN);
        if (dot(geomN, in.normal) < 0.0) { geomN = -geomN; }
    }
    float3 n = geomN * faceSign;
    float3 light = normalize(float3(u.lightDir));
    float NdotL = max(dot(n, light), 0.0);

    float4 base = float4(u.baseColor);
    if (u.hasBaseColorTexture > 0.5) {
        base *= u_baseColorTexture.sample(u_terrainSampler, terrainUv);
    }
    if (u.directRasterTextureCount > 0.5) {
        base = terrainApplyDirectRaster(
            base, in, u_directRasterTexture0, u_terrainSampler,
            u.directRasterTexCoordSet[0],
            float4(u.directRasterTileUv[0]), u.directRasterOpacity[0],
            u.clipUv, u.clipEnabled);
    }
    if (u.directRasterTextureCount > 1.5) {
        base = terrainApplyDirectRaster(
            base, in, u_directRasterTexture1, u_terrainSampler,
            u.directRasterTexCoordSet[1],
            float4(u.directRasterTileUv[1]), u.directRasterOpacity[1],
            u.clipUv, u.clipEnabled);
    }
    if (u.directRasterTextureCount > 2.5) {
        base = terrainApplyDirectRaster(
            base, in, u_directRasterTexture2, u_terrainSampler,
            u.directRasterTexCoordSet[2],
            float4(u.directRasterTileUv[2]), u.directRasterOpacity[2],
            u.clipUv, u.clipEnabled);
    }
    if (u.directRasterTextureCount > 3.5) {
        base = terrainApplyDirectRaster(
            base, in, u_directRasterTexture3, u_terrainSampler,
            u.directRasterTexCoordSet[3],
            float4(u.directRasterTileUv[3]), u.directRasterOpacity[3],
            u.clipUv, u.clipEnabled);
    }
    // 合成方案页存储(Step 3,镜像 GLSL 侧):目标 capped 瓦片改采页存储,
    // 覆盖上采样 directComposite → 真实高清影像。enabled=0 恒不进,零回归。
    if (u.pageStoreParams.x > 0.5) {
        // 镜像 GLSL。terrainUv 是 set 0,页存储可能要另一套 → 按 params.w 单独取。
        float psPack = u.pageStoreParams.w;
        float2 psPhase = float2(fmod(floor(psPack / 8.0), 64.0),
                                floor(psPack / 512.0));
        float2 psUv = terrainUvFromSet(in, fmod(psPack, 8.0));
        // set 0 在 VS 里已被祖先模板的 scale-bias 重映射(clipMode>1.5 那支),
        // 其余 set 是原始 ancestor UV —— 不补同一个 scale-bias 就会整片错位一
        // 个 LOD 窗口(一阶误差,远大于 GCJ 本身)。GCJ 空间的精确 scale-bias 与
        // mercator 的略有差异,但 warp 在单瓦片内近似仿射,二阶量可忽略。
        if (fmod(psPack, 8.0) > 0.5 && u.clipEnabled > 1.5) {
            psUv = float2(u.clipUv.x, u.clipUv.y) +
                   psUv * float2(u.clipUv.z, u.clipUv.w);
        }
        float2 cells = max(float2(u.pageStoreParams.y, u.pageStoreParams.z),
                           float2(1.0));
        // 采样链收进单一治理点 eePageStoreCompose(PageStoreSamplingGLSL.h)。
        // [瓦界对齐] 位移路径 UV = 共享模板几何 UV,传逐瓦仿射 pageGeomA/B。
        base = eePageStoreCompose(
            u_pageStore, u_pageStoreIndir, u_roadField, u_roadFieldIndir,
            u_terrainSampler,
            base, psUv, float4(u.pageGeomA),
            float2(u.pageGeomB.x, u.pageGeomB.y), psPhase, cells,
            int(u.terrainLayers.y + 0.5), float(u.roadFieldParams[1]),
            float4(u.roadFieldParams), float4(u.roadFieldWidth),
            float4(u.roadFieldColor));
    }
    base = terrainApplyWaterMask(
        base, in, u_gltfWaterMaskTexture, u_terrainSampler,
        u.hasWaterMask, float4(u.waterMaskTranslationScale),
        float4(u.waterMaskState),
        n, light,
        normalize(float3(u.eyePositionRTC) - in.localPosition));
    if (u.alphaMode > 0.5 && u.alphaMode < 1.5 && base.a < u.alphaCutoff) {
        discard_fragment();
    }
    float alpha = u.alphaMode > 1.5 ? base.a : 1.0;

    // GE 式半球光照(与 GLSL 侧共用 TerrainSurfaceLightGLSL.h 的单一函数;由
    // withTerrainLight() 注入 kTerrainLightMSL)。
    float3 color =
        terrainSurfaceLight(base.rgb, NdotL, float4(u.ambient).rgb);
    return float4(color, alpha * clamp(u.renderOpacity, 0.0, 1.0));
}
)msl";

static const char* kGltfInstancedVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct GltfInstancedVertexIn {
    float3 position     [[attribute(0)]];
    float3 normal       [[attribute(1)]];
    float4 texcoord01   [[attribute(2)]];
    float4 instanceCol0 [[attribute(3)]];
    float4 instanceCol1 [[attribute(4)]];
    float4 instanceCol2 [[attribute(5)]];
    float4 instanceCol3 [[attribute(6)]];
    float3 normalCol0   [[attribute(7)]];
    float3 normalCol1   [[attribute(8)]];
    float3 normalCol2   [[attribute(9)]];
    float4 color        [[attribute(10)]];
    float4 tangent      [[attribute(11)]];
    float4 texcoord23   [[attribute(12)]];
    float4 texcoord45   [[attribute(13)]];
    float4 texcoord67   [[attribute(14)]];
};

struct GltfVertexOut {
    float4 position [[position]];
    float3 normal;
    float3 localPosition;
    float4 texcoord01;
    float4 color;
    float4 tangent;
    float4 texcoord23;
    float4 texcoord45;
    float4 texcoord67;
};

vertex GltfVertexOut
gltfInstancedVertex(GltfInstancedVertexIn in [[stage_in]],
                    constant float4x4& u_modelViewProjection [[buffer(1)]]) {
    GltfVertexOut out;
    float4x4 instanceModel = float4x4(
        in.instanceCol0,
        in.instanceCol1,
        in.instanceCol2,
        in.instanceCol3);
    float3x3 instanceNormal = float3x3(
        in.normalCol0,
        in.normalCol1,
        in.normalCol2);
    float3x3 instanceTangent = float3x3(
        instanceModel[0].xyz,
        instanceModel[1].xyz,
        instanceModel[2].xyz);
    float4 localPosition = instanceModel * float4(in.position, 1.0);
    out.position = u_modelViewProjection * localPosition;
    out.normal = normalize(instanceNormal * in.normal);
    out.localPosition = localPosition.xyz;
    out.texcoord01 = in.texcoord01;
    out.color = in.color;
    float3 tangent = in.tangent.xyz;
    if (dot(tangent, tangent) > 0.0) {
        tangent = normalize(instanceTangent * tangent);
    }
    out.tangent = float4(tangent, in.tangent.w);
    out.texcoord23 = in.texcoord23;
    out.texcoord45 = in.texcoord45;
    out.texcoord67 = in.texcoord67;
    return out;
}
)msl";

// 地形实例化(合批 Step 3)MSL,镜像 kTerrainInstanced{Vertex,Fragment}GLSL。
// 逐顶点 attr 0-3 = 32B 位移模板;per-instance attr 4-9 = 96B 流(rel 3 行 +
// dispMorph + clipUv + layers),由顶点描述表 perInstance step 绑定。
static const char* kTerrainInstancedVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct TerrainInstancedVertexIn {
    float3 position   [[attribute(0)]];
    float4 normal     [[attribute(1)]];
    float4 texcoord01 [[attribute(2)]];
    float heightDelta [[attribute(3)]];
    float4 relRow0    [[attribute(4)]];
    float4 relRow1    [[attribute(5)]];
    float4 relRow2    [[attribute(6)]];
    float4 dispMorph  [[attribute(7)]];  // minH·fade, range·fade, morph, pageCellDesc
    float4 clipUv     [[attribute(8)]];
    float4 layers     [[attribute(9)]];  // heightLayer, indirLayer, clipEn, 模板 gridN
    float4 pageUv     [[attribute(10)]]; // 几何仿射:c0.xy, dU.xy(瓦界对齐)
    float4 pageAux    [[attribute(11)]]; // 相位 phase.xy + 几何仿射 dV.zw
};

struct TerrainInstancedVertexOut {
    float4 position [[position]];
    float3 normal;
    float3 localPosition;
    float4 texcoord01;
    float4 pageParams [[flat]];  // x=pageCellDesc y=indirLayer z=clipEnabled w=_
    float4 pageUv [[flat]];      // 页 cell 定位(单位=源瓦片):origin.xy, span.zw
    float4 pageAux [[flat]];     // 祖先寻址相位:phase.xy
    float4 clipUv [[flat]];
};

constant float kGridSize = 64.0;

static inline float eeSampleTerrainHeight(
    texture2d_array<float> tex, uint2 texel, uint layer, float2 mr) {
    float4 p = tex.read(texel, layer, 0);
    float t = (p.r * 255.0 * 256.0 + p.g * 255.0) / 65535.0;
    return mr.x + t * mr.y;
}

vertex TerrainInstancedVertexOut terrainInstancedVertex(
    TerrainInstancedVertexIn in [[stage_in]],
    constant float4x4& u_modelViewProjection [[buffer(1)]],
    texture2d_array<float> u_heightTexture [[texture(22)]]) {
    TerrainInstancedVertexOut out;
    float morph = in.dispMorph.z;
    uint hLayer = uint(in.layers.x + 0.5);
    float2 mr = in.dispMorph.xy;
    float heightDelta = in.heightDelta;
    float skirt = (heightDelta < -0.5) ? 1.0 : 0.0;
    heightDelta = mix(heightDelta, 0.0, skirt);
    float3 morphPos = in.position + float3(0.0, 0.0, 1.0) * heightDelta * (1.0 - morph);
    float2 gf = in.texcoord01.xy * kGridSize;
    float hFine = eeSampleTerrainHeight(u_heightTexture, uint2(gf + 0.5), hLayer, mr);
    float2 g0 = floor(gf * 0.5) * 2.0;
    float2 fr = (gf - g0) * 0.5;
    float e00 = eeSampleTerrainHeight(u_heightTexture, uint2(g0), hLayer, mr);
    float e10 = eeSampleTerrainHeight(
        u_heightTexture, uint2(min(g0.x + 2.0, kGridSize), g0.y), hLayer, mr);
    float e01 = eeSampleTerrainHeight(
        u_heightTexture, uint2(g0.x, min(g0.y + 2.0, kGridSize)), hLayer, mr);
    float e11 = eeSampleTerrainHeight(
        u_heightTexture,
        uint2(min(g0.x + 2.0, kGridSize), min(g0.y + 2.0, kGridSize)), hLayer, mr);
    float hCoarse = mix(mix(e00, e10, fr.x), mix(e01, e11, fr.x), fr.y);
    float h = mix(hCoarse, hFine, morph) * (1.0 - skirt);
    morphPos += normalize(in.normal.xyz) * h;
    float4 mp = float4(morphPos, 1.0);
    float3 world = float3(dot(in.relRow0, mp), dot(in.relRow1, mp), dot(in.relRow2, mp));
    float3 nrm = normalize(in.normal.xyz);
    float3 worldN = normalize(float3(
        dot(in.relRow0.xyz, nrm), dot(in.relRow1.xyz, nrm), dot(in.relRow2.xyz, nrm)));
    out.position = u_modelViewProjection * float4(world, 1.0);
    out.normal = worldN;
    out.localPosition = world;
    out.texcoord01 = in.texcoord01;
    out.pageParams = float4(in.dispMorph.w, in.layers.y, in.layers.z, 0.0);
    out.pageUv = in.pageUv;
    out.pageAux = in.pageAux;
    out.clipUv = in.clipUv;
    return out;
}
)msl";

static const char* kTerrainInstancedFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct TerrainInstancedVertexOut {
    float4 position [[position]];
    float3 normal;
    float3 localPosition;
    float4 texcoord01;
    float4 pageParams [[flat]];
    float4 pageUv [[flat]];
    float4 pageAux [[flat]];
    float4 clipUv [[flat]];
};

struct TerrainInstancedFragUniforms {
    packed_float3 lightDir;
    float _pad0;
    packed_float4 ambient;
    packed_float4 baseColor;
};

static inline float4 tiAlphaOver(float4 base, float4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

fragment float4 terrainInstancedFragment(
    TerrainInstancedVertexOut in [[stage_in]],
    constant TerrainInstancedFragUniforms& u [[buffer(0)]],
    texture2d_array<float> u_pageStore [[texture(20)]],
    texture2d_array<float> u_pageStoreIndir [[texture(21)]],
    // 刀2 场纹理已随命令绑定;实例化 MSL 的 frag uniforms 是精简 struct
    // (无 roadFieldParams),场解算暂缺 —— 与 Metal 侧 depth-only/法线场
    // 同类特性滞后,补齐时机见 GLES 版注释。参数保留占位。
    texture2d_array<float> u_roadField [[texture(23)]],
                             texture2d_array<float> u_roadFieldIndir [[texture(24)]],
    sampler u_pageSampler [[sampler(0)]]) {
    float2 terrainUv = in.texcoord01.xy;
    if (in.pageParams.z > 0.5 &&
        (terrainUv.x < in.clipUv.x || terrainUv.x > in.clipUv.x + in.clipUv.z ||
         terrainUv.y < in.clipUv.y || terrainUv.y > in.clipUv.y + in.clipUv.w)) {
        discard_fragment();
    }
    float3 dpx = dfdx(in.localPosition);
    float3 dpy = dfdy(in.localPosition);
    float3 reliefN = cross(dpx, dpy);
    float3 geomN = normalize(in.normal);
    if (dot(reliefN, reliefN) > 1e-12) {
        geomN = normalize(reliefN);
        if (dot(geomN, in.normal) < 0.0) { geomN = -geomN; }
    }
    bool front = in.position.z >= 0.0;  // (占位;Metal 无 gl_FrontFacing 语义差)
    float3 N = geomN;
    (void)front;
    float3 L = normalize(float3(u.lightDir));
    float NdotL = max(dot(N, L), 0.0);

    float4 base = float4(u.baseColor);
    // 镜像 GLSL 实例化:cellsX/cellsY/texCoordSet 打包在 pageParams.x
    // (见 TerrainInstanceBatcher::packPageCellDescriptor)。
    float packed = in.pageParams.x;
    float2 cells = max(float2(fmod(packed, 128.0),
                              fmod(floor(packed / 128.0), 128.0)),
                       float2(1.0));
    float psSet = fmod(floor(packed / 16384.0), 8.0);
    uint indirLayer = uint(in.pageParams.y + 0.5);
    float2 psUv = psSet > 0.5 ? in.texcoord01.zw : in.texcoord01.xy;
    // 采样链收进单一治理点 eePageStoreCompose(PageStoreSamplingGLSL.h)。
    // [瓦界对齐] UV = 共享模板几何 UV,仿射经实例流传入。
    // ⚠️ instanced MSL 的精简 uniform struct 尚无场参数 → 场参数传 0(场支路
    // 死代码),特性滞后同 depth-only;Metal 补齐时把 roadFieldParams/Color
    // 加进 TerrainInstancedFragUniforms 并改此两参即可。
    base = eePageStoreCompose(
        u_pageStore, u_pageStoreIndir, u_roadField, u_roadFieldIndir,
        u_pageSampler, base, psUv,
        float4(in.pageUv), float2(in.pageAux.z, in.pageAux.w),
        float2(in.pageAux.x, in.pageAux.y), cells, int(indirLayer),
        0.0, float4(0.0), float4(0.0), float4(0.0));

    // GE 式半球光照(与 GLSL 侧共用 TerrainSurfaceLightGLSL.h 的单一函数;由
    // withTerrainLight() 注入 kTerrainLightMSL)。
    float3 color =
        terrainSurfaceLight(base.rgb, NdotL, float3(u.ambient.rgb));
    return float4(color, 1.0);
}
)msl";


namespace renderer_testing {

const char* gltfVertexGLSL() {
    return kGltfVertexGLSL;
}

const char* gltfFragmentGLSL() {
    return kGltfFragmentGLSL;
}

const char* gltfFragmentMSL() {
    return kGltfFragmentMSL;
}

const char* gltfInstancedVertexGLSL() {
    return kGltfInstancedVertexGLSL;
}

const char* gltfInstancedVertexMSL() {
    return kGltfInstancedVertexMSL;
}

const char* terrainVertexGLSL() {
    return kTerrainVertexGLSL;
}

const char* terrainFragmentGLSL() {
    return kTerrainFragmentGLSL;
}

const char* terrainVertexMSL() {
    return kTerrainVertexMSL;
}

const char* terrainFragmentMSL() {
    return kTerrainFragmentMSL;
}

} // namespace renderer_testing

// 把共享地形光照函数(TerrainSurfaceLightGLSL.h)注入到片元 shader 的入口之
// 前 —— 四个地形片元 shader 各自内联的 GE 半球光照已收成单一函数,此处按后端
// 选 GLSL/MSL 变体、插到入口(GLSL `void main(` / MSL `fragment float4 `)前,
// 使其排在全部 precision/uniform/helper 声明之后、唯一调用者之前。镜像
// computeSkyColor 的字符串拼接惯例(AtmosphereSkyColorGLSL.h)。
// 页存储采样 + 场解算的单一治理点注入(PageStoreSamplingGLSL.h;照
// withTerrainLight 惯例,拼在片元入口前)。六个消费 shader:gltf / terrain /
// terrainInstanced × GLES/Metal(Metal instanced 待 Step 4 接线时同样须包)。
static std::string withPageStoreSampling(const std::string& fragmentSource,
                                         bool metal) {
    std::string src(fragmentSource);
    const char* anchor = metal ? "fragment float4 " : "void main(";
    const char* fn = metal ? kPageStoreSamplingMSL : kPageStoreSamplingGLSL;
    const size_t pos = src.find(anchor);
    // 锚点必存;缺失宁可返回原样让编译期炸(缺函数定义),不静默产出坏 shader。
    if (pos == std::string::npos) {
        return src;
    }
    src.insert(pos, std::string(fn) + "\n");
    return src;
}

static std::string withTerrainLight(const char* fragmentSource, bool metal) {
    std::string src(fragmentSource);
    const char* anchor = metal ? "fragment float4 " : "void main(";
    // kEnableHdrPipeline OFF → LDR/gamma 变体(= T0,零变化);ON → HDR 线性
    // 变体(base→线性,输出线性 HDR,tonemap 终端 encode)。见 PipelineConfig.h。
    const std::string fn =
        kEnableHdrPipeline ? (metal ? kTerrainLightHdrMSL : kTerrainLightHdrGLSL)
                           : (metal ? kTerrainLightMSL : kTerrainLightGLSL);
    const size_t pos = src.find(anchor);
    // 锚点在每个地形片元 shader 里唯一且必存;若未来编辑删掉入口,宁可越界抛
    // 也不静默产出缺光照函数的 shader(编译期即炸,而非上屏后无光)。
    if (pos == std::string::npos) {
        return src;
    }
    src.insert(pos, fn + "\n");
    return src;
}

// B2 刀1:场景内容输出编码。HDR 下场景画进线性 16F 靶、末端 tonemap+sRGB
// encode(见 kEnableHdrPipeline / OffscreenPostProcess Tonemap)。无光照的矢量/
// 标签/图标 shader 输出的是**显示空间**手调色,直接写进线性靶会被 tonemap 当线性
// 处理而偏亮/色偏。此处按 flag 注入 encodeSceneOutput():
//   HDR → srgbToLinear(把显示色转线性,过 tonemap+encode 后往返回原显示色,亮部
//         受 tonemap 轻压,与 terrain/sky 同调);LDR → 恒等(**零回归**,函数被优化掉)。
// 各 A 档 GLES 片元 shader 末尾 fragColor.rgb 包一层本函数,assembly 时注入定义。
// 仅 GLES(Metal HDR 终端未接线,留刀4);地形/天空/fog 已各自在 HDR 变体里输出线性。
static std::string withSceneOutput(const std::string& fragmentSource) {
    std::string src(fragmentSource);
    const char* fn =
        kEnableHdrPipeline
            ? "vec3 encodeSceneOutput(vec3 c){return pow(max(c,vec3(0.0)),vec3(2.2));}\n"
            : "vec3 encodeSceneOutput(vec3 c){return c;}\n";
    const size_t pos = src.find("void main(");
    if (pos == std::string::npos) {
        return src;  // 锚点必存;缺失宁可返回原样让编译期炸(缺函数定义)
    }
    src.insert(pos, fn);
    return src;
}

// B2 刀2:glTF PBR 的 HDR 输入解码。glTF 材质本为线性 PBR 设计,LDR 路径直接把
// sRGB 反照率当显示色算(gamma 空间,与 terrain LDR 同),HDR 下须把 sRGB 反照率/
// emissive 解到线性,BRDF 在线性域算,结果直接输出即线性 HDR(末端 tonemap encode)。
// 注入 hdrAlbedo():HDR → srgbToLinear;LDR → 恒等(零回归)。仅 glTF GLES 片元 shader
// (base/emissive 处调用),照 withTerrainLight/withSceneOutput 范式。⚠️ 光照常数
// (ambient/diffuse 权重/specPower)仍 provisional,对 tonemap 输出的重调留刀3。
static std::string withGltfHdr(const char* fragmentSource) {
    std::string src(fragmentSource);
    const char* fn =
        kEnableHdrPipeline
            ? "vec3 hdrAlbedo(vec3 c){return pow(max(c,vec3(0.0)),vec3(2.2));}\n"
            : "vec3 hdrAlbedo(vec3 c){return c;}\n";
    const size_t pos = src.find("void main(");
    if (pos == std::string::npos) {
        return src;
    }
    src.insert(pos, fn);
    return src;
}

// ============================================================
// Renderer::Impl
// ============================================================

struct Renderer::Impl {
    RenderDevice* device = nullptr;

    // 帧级资源保活集(见 Renderer::keepAliveThisFrame)。holds 承载真正的
    // shared_ptr(RAII 锚),seen 按裸指针去重避免同一资源被本帧多命令重复
    // 持有引发的冗余原子引用计数。每帧 clearFrameKeepAlive 一并清空。
    std::vector<std::shared_ptr<const void>> frameKeepAlive;
    std::unordered_set<const void*> frameKeepAliveSeen;

    // Surface tile (unified, cesium-native glTF layout)
    std::unique_ptr<Texture> surfacePlaceholderTexture;

    // glTF TileRenderContent
    std::unique_ptr<ShaderProgram> gltfShader;
    std::unique_ptr<ShaderProgram> gltfInstancedShader;

    // Terrain lightweight shader (32-byte compact vertex, no PBR extensions)
    std::unique_ptr<ShaderProgram> terrainShader;
    // Terrain instanced shader (合批 Step 3:32B 模板 + 96B per-instance 流)
    std::unique_ptr<ShaderProgram> terrainInstancedShader;
    // T2 深度 prepass:各顶点布局复用自己的顶点段 + 同一空片元。
    std::unique_ptr<ShaderProgram> gltfDepthShader;
    std::unique_ptr<ShaderProgram> gltfDepthInstancedShader;
    std::unique_ptr<ShaderProgram> terrainDepthShader;
    std::unique_ptr<ShaderProgram> terrainDepthInstancedShader;
    Renderer::TerrainOcclusionParams terrainOcclusion;

    // Color (vector)
    std::unique_ptr<ShaderProgram> colorShader;
    // 矢量线 ribbon(P1,§6.2 屏幕挤出)。fill 走 vectorFillShader(P6b
    // 顶点色);colorShader 留给 stencil 分类等 uniform 色路径。
    std::unique_ptr<ShaderProgram> vectorLineShader;
    // P6d stencil 贴地线(墙带体,两 stencil pass 共用)。
    std::unique_ptr<ShaderProgram> vectorLineStencilShader;
    std::unique_ptr<ShaderProgram> vectorFillShader;
    std::unique_ptr<ShaderProgram> vectorExtrusionShader;
    std::unique_ptr<ShaderProgram> vectorPageMeshShader;  // C-2c
    // 矢量点符号/图标 billboard(P5a 解析 SDF 形状 + P6c 位图图集)。
    std::unique_ptr<ShaderProgram> vectorPointShader;
    std::unique_ptr<IconAtlas> iconAtlas;
    // 矢量文字标注(P5b):SDF 字形图集 + 文字 shader。
    std::unique_ptr<GlyphAtlas> glyphAtlas;
    std::unique_ptr<ShaderProgram> vectorLabelShader;

    bool initialized = false;
};

// ============================================================
// Renderer
// ============================================================

Renderer::Renderer(RenderDevice* device)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
}

Renderer::~Renderer() {
    dispose();
}

bool Renderer::initialize() {
    if (impl_->initialized) dispose();
    auto* dev = impl_->device;
    if (!dev) return false;

    bool isMetal = (dev->backendType() == RenderDevice::Backend::Metal);

    const uint8_t placeholderPixel[4] = {174, 184, 170, 255};
    TextureDesc placeholderDesc;
    placeholderDesc.width = 1;
    placeholderDesc.height = 1;
    placeholderDesc.format = TextureDesc::Format::RGBA8;
    placeholderDesc.data = placeholderPixel;
    placeholderDesc.dataSize = sizeof(placeholderPixel);
    placeholderDesc.mipmap = false;
    placeholderDesc.minFilter = TextureDesc::Filter::Nearest;
    placeholderDesc.magFilter = TextureDesc::Filter::Nearest;
    impl_->surfacePlaceholderTexture = dev->createTexture(placeholderDesc);

    // ---- glTF primitive shader ----
    ShaderDesc gltfSd;
    gltfSd.vertexSource = isMetal ? kGltfVertexMSL : kGltfVertexGLSL;
    gltfSd.fragmentSource = withPageStoreSampling(
        isMetal ? kGltfFragmentMSL : withGltfHdr(kGltfFragmentGLSL), isMetal);
    impl_->gltfShader = dev->createShader(gltfSd);
    if (!impl_->gltfShader) {
        // Non-fatal on BOTH backends: a PBR shader link failure (Metal buffer
        // limit / fn ordering, or a GLES texture-unit overflow on an
        // unexpectedly tight driver) must not blank the globe. Terrain still
        // renders via terrainShader; only glTF model content is skipped, and
        // the null shader is dropped safely by the backend draw loop.
        fprintf(stderr, "[Renderer] gltfShader failed — glTF models unavailable\n");
    }

    // ---- Terrain lightweight shader (32-byte compact TerrainGpuVertex) ----
    // Unlike gltfShader, this is a small shader (<=31 Metal buffers) so it must
    // compile on BOTH backends. Treat link failure as fatal.
    ShaderDesc terrainSd;
    terrainSd.vertexSource = isMetal ? kTerrainVertexMSL : kTerrainVertexGLSL;
    terrainSd.fragmentSource = withPageStoreSampling(
        withTerrainLight(
            isMetal ? kTerrainFragmentMSL : kTerrainFragmentGLSL, isMetal),
        isMetal);
    impl_->terrainShader = dev->createShader(terrainSd);
    if (!impl_->terrainShader) {
        fprintf(stderr, "[Renderer] terrainShader failed\n");
        return false;
    }

    // T2 深度 prepass shader:顶点段与对应主 shader **同一份源**,只换空片元。
    // EllipsoidTerrainContentProvider 以及未切 compact 模板的 CPU baked terrain
    // 使用 120B glTF 布局，不能绑定 32B terrain 顶点 shader。
    // GLES 侧接线;Metal 侧不建(→ TerrainDepthPrepass::initialize 返回 false
    // → 符号保持原 u_depthPushNdc 行为,零回归)。创建失败非致命。
    if (!isMetal) {
        ShaderDesc gltfDepthSd;
        gltfDepthSd.vertexSource = kGltfVertexGLSL;
        gltfDepthSd.fragmentSource = kTerrainDepthOnlyFragmentGLSL;
        impl_->gltfDepthShader = dev->createShader(gltfDepthSd);
        if (!impl_->gltfDepthShader) {
            fprintf(stderr,
                    "[Renderer] gltfDepthShader failed — 椭球/CPU地形符号遮挡不可用\n");
        }

        ShaderDesc terrainDepthSd;
        terrainDepthSd.vertexSource = kTerrainVertexGLSL;
        terrainDepthSd.fragmentSource = kTerrainDepthOnlyFragmentGLSL;
        impl_->terrainDepthShader = dev->createShader(terrainDepthSd);
        if (!impl_->terrainDepthShader) {
            fprintf(stderr,
                    "[Renderer] terrainDepthShader failed — 符号地形遮挡不可用\n");
        }
    }

    ShaderDesc gltfInstancedSd;
    gltfInstancedSd.vertexSource =
        isMetal ? kGltfInstancedVertexMSL : kGltfInstancedVertexGLSL;
    gltfInstancedSd.fragmentSource = withPageStoreSampling(
        isMetal ? kGltfFragmentMSL : withGltfHdr(kGltfFragmentGLSL), isMetal);
    impl_->gltfInstancedShader = dev->createShader(gltfInstancedSd);
    if (!impl_->gltfInstancedShader) {
        // Non-fatal on both backends (see gltfShader above): instanced glTF
        // content is skipped, the globe still renders.
        fprintf(stderr, "[Renderer] gltfInstancedShader failed — instanced glTF unavailable\n");
    }

    // 地形实例化合批 shader(合批 Step 3)。GLES 侧本步接线 Terrain32Instanced
    // 顶点布局;Metal 侧的 per-instance 顶点描述表留 Step 4,此前不建(→
    // terrainInstancedShader()==null → TerrainInstanceBatcher no-op → Metal
    // 逐字节走逐 draw,零回归)。创建失败非致命,同样回落逐 draw。
    if (!isMetal) {
        ShaderDesc gltfDepthInstancedSd;
        gltfDepthInstancedSd.vertexSource = kGltfInstancedVertexGLSL;
        gltfDepthInstancedSd.fragmentSource = kTerrainDepthOnlyFragmentGLSL;
        impl_->gltfDepthInstancedShader =
            dev->createShader(gltfDepthInstancedSd);
        if (!impl_->gltfDepthInstancedShader) {
            fprintf(stderr,
                    "[Renderer] gltfDepthInstancedShader failed — 实例化glTF地形符号遮挡不可用\n");
        }

        ShaderDesc terrainInstancedSd;
        terrainInstancedSd.vertexSource = kTerrainInstancedVertexGLSL;
        terrainInstancedSd.fragmentSource = withPageStoreSampling(
            withTerrainLight(kTerrainInstancedFragmentGLSL, /*metal=*/false),
            /*metal=*/false);
        impl_->terrainInstancedShader = dev->createShader(terrainInstancedSd);
        if (!impl_->terrainInstancedShader) {
            fprintf(stderr,
                    "[Renderer] terrainInstancedShader failed — terrain "
                    "batching disabled, per-draw fallback\n");
        }
        // 合批地形的 depth-only 对应件。缺席时 prepass 整帧放弃(半张深度图
        // 比没有更糟,见 TerrainDepthPrepass::extractTerrainCommands)。
        ShaderDesc terrainDepthInstancedSd;
        terrainDepthInstancedSd.vertexSource = kTerrainInstancedVertexGLSL;
        terrainDepthInstancedSd.fragmentSource = kTerrainDepthOnlyFragmentGLSL;
        impl_->terrainDepthInstancedShader =
            dev->createShader(terrainDepthInstancedSd);
        if (!impl_->terrainDepthInstancedShader) {
            fprintf(stderr,
                    "[Renderer] terrainDepthInstancedShader failed — 符号"
                    "地形遮挡不可用\n");
        }
    } else {
        // Metal 实例化地形顶点描述表留合批 Step 4,此前不建。接线时片元源须走
        // withTerrainLight(kTerrainInstancedFragmentMSL, /*metal=*/true) —— 该
        // 字面量已改为调用 terrainSurfaceLight,注入函数定义方能编译。
        (void)kTerrainInstancedVertexMSL;
        (void)kTerrainInstancedFragmentMSL;
    }


    // ---- Color shader (vector layers) ----
    ShaderDesc colorSd;
    colorSd.vertexSource = isMetal ? kColorVertexMSL : kColorVertexGLSL;
    colorSd.fragmentSource = isMetal ? kColorFragmentMSL : withSceneOutput(kColorFragmentGLSL);
    impl_->colorShader = dev->createShader(colorSd);
    // colorShader failure is non-fatal (vector layers won't render but tiles still work)

    // ---- Vector fill shader (矢量 P6b 顶点色 fill) ----
    ShaderDesc vectorFillSd;
    vectorFillSd.vertexSource =
        isMetal ? kVectorFillVertexMSL : kVectorFillVertexGLSL;
    vectorFillSd.fragmentSource =
        isMetal ? kVectorFillFragmentMSL : withSceneOutput(kVectorFillFragmentGLSL);
    impl_->vectorFillShader = dev->createShader(vectorFillSd);
    if (!impl_->vectorFillShader) {
        // 非致命:fill 不出图,其余矢量/地形不受影响
        fprintf(stderr, "[Renderer] vectorFillShader failed — vector fills unavailable\n");
    }

    // ---- Vector extrusion shader (V6 建筑挤出) ----
    ShaderDesc vectorExtrusionSd;
    vectorExtrusionSd.vertexSource =
        isMetal ? kVectorExtrusionVertexMSL : kVectorExtrusionVertexGLSL;
    vectorExtrusionSd.fragmentSource =
        isMetal ? kVectorExtrusionFragmentMSL
                : withSceneOutput(kVectorExtrusionFragmentGLSL);
    impl_->vectorExtrusionShader = dev->createShader(vectorExtrusionSd);
    if (!impl_->vectorExtrusionShader) {
        fprintf(stderr,
                "[Renderer] vectorExtrusionShader failed — building "
                "extrusions unavailable\n");
    }

    // ---- Vector page mesh shader (C-2c 矢量画进页存储 array 层) ----
    ShaderDesc vectorPageMeshSd;
    vectorPageMeshSd.vertexSource =
        isMetal ? kVectorPageMeshVertexMSL : kVectorPageMeshVertexGLSL;
    vectorPageMeshSd.fragmentSource =
        isMetal ? kVectorPageMeshFragmentMSL : withSceneOutput(kVectorPageMeshFragmentGLSL);
    impl_->vectorPageMeshShader = dev->createShader(vectorPageMeshSd);
    if (!impl_->vectorPageMeshShader) {
        // 非致命:矢量不进页存储,cell 回落 directComposite 的栅格版(糊但有)。
        fprintf(stderr,
                "[Renderer] vectorPageMeshShader failed — vector draping falls "
                "back to rasterized overlay\n");
    }

    // ---- Vector line shader (矢量 P1 线 ribbon) ----
    ShaderDesc vectorLineSd;
    vectorLineSd.vertexSource =
        isMetal ? kVectorLineVertexMSL : kVectorLineVertexGLSL;
    vectorLineSd.fragmentSource =
        isMetal ? kVectorLineFragmentMSL : withSceneOutput(kVectorLineFragmentGLSL);
    impl_->vectorLineShader = dev->createShader(vectorLineSd);
    if (!impl_->vectorLineShader) {
        // 非致命:矢量线不出图,fill/地形不受影响
        fprintf(stderr, "[Renderer] vectorLineShader failed — vector lines unavailable\n");
    }

    // ---- Vector line stencil shader (P6d stencil 贴地线墙带体) ----
    ShaderDesc vectorLineStencilSd;
    vectorLineStencilSd.vertexSource =
        isMetal ? kVectorLineStencilVertexMSL : kVectorLineStencilVertexGLSL;
    vectorLineStencilSd.fragmentSource = isMetal
                                             ? kVectorLineStencilFragmentMSL
                                             : withSceneOutput(kVectorLineStencilFragmentGLSL);
    impl_->vectorLineStencilShader = dev->createShader(vectorLineStencilSd);
    if (!impl_->vectorLineStencilShader) {
        // 非致命:贴地线命令对不生成(shader 指针为空即跳过),不影响其余
        fprintf(stderr,
                "[Renderer] vectorLineStencilShader failed — stencil ground "
                "lines unavailable\n");
    }

    // ---- Vector point/symbol shader (矢量 P5a 点符号 + P6c 图标) ----
    ShaderDesc vectorPointSd;
    vectorPointSd.vertexSource =
        isMetal ? kVectorPointVertexMSL : kVectorPointVertexGLSL;
    vectorPointSd.fragmentSource =
        isMetal ? kVectorPointFragmentMSL : withSceneOutput(kVectorPointFragmentGLSL);
    impl_->vectorPointShader = dev->createShader(vectorPointSd);
    if (!impl_->vectorPointShader) {
        // 非致命:点符号不出图,其余矢量/地形不受影响
        fprintf(stderr, "[Renderer] vectorPointShader failed — vector points unavailable\n");
    }

    // ---- Vector label shader + glyph atlas (矢量 P5b 文字标注) ----
    ShaderDesc vectorLabelSd;
    vectorLabelSd.vertexSource =
        isMetal ? kVectorLabelVertexMSL : kVectorLabelVertexGLSL;
    vectorLabelSd.fragmentSource =
        isMetal ? kVectorLabelFragmentMSL : withSceneOutput(kVectorLabelFragmentGLSL);
    impl_->vectorLabelShader = dev->createShader(vectorLabelSd);
    if (!impl_->vectorLabelShader) {
        // 非致命:标注不出图,其余矢量/地形不受影响
        fprintf(stderr, "[Renderer] vectorLabelShader failed — vector labels unavailable\n");
    }
    impl_->glyphAtlas = std::make_unique<GlyphAtlas>(dev);
    // 图标图集(矢量 P6c):纹理延迟到首次 addImage 才建,无图标零开销。
    impl_->iconAtlas = std::make_unique<IconAtlas>(dev);

    impl_->initialized = true;
    return true;
}

void Renderer::submit(const RenderCommandList& commands) {
    if (!impl_->initialized || !impl_->device) return;
    impl_->device->submit(commands);
}

void Renderer::dispose() {
    impl_->surfacePlaceholderTexture.reset();
    impl_->gltfShader.reset();
    impl_->gltfInstancedShader.reset();
    impl_->gltfDepthShader.reset();
    impl_->gltfDepthInstancedShader.reset();
    impl_->terrainShader.reset();
    impl_->terrainDepthShader.reset();
    impl_->terrainDepthInstancedShader.reset();
    impl_->terrainInstancedShader.reset();
    impl_->colorShader.reset();
    impl_->vectorLineShader.reset();
    impl_->vectorLineStencilShader.reset();
    impl_->vectorPointShader.reset();
    impl_->vectorLabelShader.reset();
    impl_->glyphAtlas.reset();
    impl_->iconAtlas.reset();
    impl_->initialized = false;
}

// ---- 共享资源访问 ----

void Renderer::setTerrainOcclusion(const TerrainOcclusionParams& params) {
    impl_->terrainOcclusion = params;
}
const Renderer::TerrainOcclusionParams& Renderer::terrainOcclusion() const {
    return impl_->terrainOcclusion;
}
ShaderProgram* Renderer::gltfDepthShader() const {
    return impl_->gltfDepthShader.get();
}
ShaderProgram* Renderer::gltfDepthInstancedShader() const {
    return impl_->gltfDepthInstancedShader.get();
}
ShaderProgram* Renderer::terrainDepthShader() const {
    return impl_->terrainDepthShader.get();
}
ShaderProgram* Renderer::terrainDepthInstancedShader() const {
    return impl_->terrainDepthInstancedShader.get();
}
ShaderProgram* Renderer::colorShader() const { return impl_->colorShader.get(); }
ShaderProgram* Renderer::vectorLineShader() const {
    return impl_->vectorLineShader.get();
}
ShaderProgram* Renderer::vectorLineStencilShader() const {
    return impl_->vectorLineStencilShader.get();
}
ShaderProgram* Renderer::vectorPageMeshShader() const {
    return impl_->vectorPageMeshShader.get();
}

ShaderProgram* Renderer::vectorFillShader() const {
    return impl_->vectorFillShader.get();
}
ShaderProgram* Renderer::vectorExtrusionShader() const {
    return impl_->vectorExtrusionShader.get();
}
ShaderProgram* Renderer::vectorPointShader() const {
    return impl_->vectorPointShader.get();
}
ShaderProgram* Renderer::vectorLabelShader() const {
    return impl_->vectorLabelShader.get();
}
GlyphAtlas* Renderer::glyphAtlas() const { return impl_->glyphAtlas.get(); }

IconAtlas* Renderer::iconAtlas() const { return impl_->iconAtlas.get(); }
Texture* Renderer::surfacePlaceholderTexture() const {
    return impl_->surfacePlaceholderTexture.get();
}
ShaderProgram* Renderer::gltfShader() const { return impl_->gltfShader.get(); }

ShaderProgram* Renderer::gltfInstancedShader() const {
    return impl_->gltfInstancedShader.get();
}

ShaderProgram* Renderer::terrainShader() const {
    return impl_->terrainShader.get();
}

// ---- Command builders ----

RenderCommand Renderer::makeGltfPrimitiveCommand(Buffer* vertexBuffer,
                                                 Buffer* indexBuffer,
                                                 int indexCount,
                                                 int vertexCount) const {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::GltfPrimitive;
    cmd.owner = "gltf_primitive";
    cmd.pass = "color";
    cmd.shader = impl_->gltfShader.get();
    cmd.vertexBuffer = vertexBuffer;
    cmd.indexBuffer = indexBuffer;
    cmd.indexCount = indexCount;
    cmd.vertexCount = vertexCount;
    cmd.vertexStride = 120;  // POSITION/NORMAL + TEXCOORD_0..7 + COLOR_0 + TANGENT
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    cmd.depthTest = true;
    cmd.depthWrite = true;
    cmd.blend = false;
    cmd.cullFace = true;
    // 全部 uniform 默认值内聚在 GltfUniformBlock 成员初始化器中，构造即
    // 就绪；本命令不得写 uniforms string-map（热路径零堆分配契约）。
    cmd.hasGltfUniforms = true;
    return cmd;
}

RenderCommand Renderer::makeTerrainPrimitiveCommand(Buffer* vertexBuffer,
                                                    Buffer* indexBuffer,
                                                    int indexCount,
                                                    int vertexCount) const {
    RenderCommand cmd;
    // Command kind stays GltfPrimitive: GLES keys the vertex layout on
    // vertexStride (40 -> packed texcoord0/1 path, attribs 10-14 disabled) and
    // Metal keys the PSO on the terrainVertex/terrainFragment entry points.
    cmd.kind = RenderCommandKind::GltfPrimitive;
    cmd.owner = "terrain_primitive";
    cmd.pass = "color";
    cmd.shader = impl_->terrainShader.get();
    cmd.vertexBuffer = vertexBuffer;
    cmd.indexBuffer = indexBuffer;
    cmd.indexCount = indexCount;
    cmd.vertexCount = vertexCount;
    cmd.vertexStride = 32;  // POSITION f32(12) + NORMAL snorm16+pad(8) + TEXCOORD_0/1 unorm16(8) + geomorph heightDelta f32(4)
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    cmd.depthTest = true;
    cmd.depthWrite = true;
    cmd.blend = false;
    cmd.cullFace = true;
    // Uniform 默认值全部由 GltfUniformBlock 成员初始化器提供（terrain
    // shader 只消费其声明的子集）。GltfDrawCommandBuilder 逐 primitive 覆写。
    cmd.hasGltfUniforms = true;
    return cmd;
}

RenderCommand Renderer::makeGltfPrimitiveInstancedCommand(
    Buffer* vertexBuffer,
    Buffer* indexBuffer,
    Buffer* instanceBuffer,
    int indexCount,
    int vertexCount,
    int instanceCount) const {
    RenderCommand cmd = makeGltfPrimitiveCommand(
        vertexBuffer,
        indexBuffer,
        indexCount,
        vertexCount);
    cmd.kind = RenderCommandKind::GltfPrimitiveInstanced;
    cmd.owner = "gltf_primitive_instanced";
    cmd.shader = impl_->gltfInstancedShader.get();
    cmd.instanceBuffer = instanceBuffer;
    cmd.instanceCount = instanceCount;
    cmd.instanceStride = kGltfInstanceMatrixStride;
    return cmd;
}

RenderCommand Renderer::makeTerrainInstancedCommand(
    Buffer* vertexBuffer,
    Buffer* indexBuffer,
    Buffer* instanceBuffer,
    int indexCount,
    int vertexCount,
    int instanceCount) const {
    // 复用地形单命令(32B 模板布局 + terrain uniform 默认),仅换 kind/shader/
    // 实例流。vertexStride=32 + kind=Instanced + instanceStride=kTerrainInstance
    // Stride → 后端分派 Terrain32Instanced 顶点布局(见 RenderDeviceGLES)。
    RenderCommand cmd = makeTerrainPrimitiveCommand(
        vertexBuffer, indexBuffer, indexCount, vertexCount);
    cmd.kind = RenderCommandKind::GltfPrimitiveInstanced;
    cmd.owner = "terrain_instanced";
    cmd.shader = impl_->terrainInstancedShader.get();
    cmd.indexType = RenderCommand::IndexType::UInt32;  // 模板 IBO 恒 uint32
    cmd.instanceBuffer = instanceBuffer;
    cmd.instanceCount = instanceCount;
    cmd.instanceStride = kTerrainInstanceStride;
    return cmd;
}

ShaderProgram* Renderer::terrainInstancedShader() const {
    return impl_->terrainInstancedShader.get();
}

std::array<float, 16> Renderer::earthModelMatrix() {
    constexpr float kEarthRadius = 6378137.0f;
    glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(kEarthRadius));
    std::array<float, 16> result;
    std::memcpy(result.data(), glm::value_ptr(m), 16 * sizeof(float));
    return result;
}

// ── cesium-native IPrepareRendererResources notification hooks ──

void Renderer::attachRasterInMainThread(
    const TileKey&,
    int32_t,
    std::shared_ptr<const RasterOverlayTile>,
    Texture*,
    float, float,
    float, float) {
    // Surface raster ownership lives in DirectRasterMapping and
    // SurfaceRasterBinding. Renderer must not retain or query imagery state.
}

void Renderer::detachRasterInMainThread(
    const TileKey&,
    int32_t) noexcept {
    // Notification hook only; renderer does not store raster binding state.
}

void Renderer::keepAliveThisFrame(std::shared_ptr<const void> handle) {
    if (!handle) return;
    // 按裸指针去重:同一资源本帧只持一份 shared_ptr → 一次原子 inc,而非
    // 逐命令一次。首次见到才 push(移动进 holds),重复直接丢弃。
    if (impl_->frameKeepAliveSeen.insert(handle.get()).second) {
        impl_->frameKeepAlive.push_back(std::move(handle));
    }
}

void Renderer::clearFrameKeepAlive() {
    impl_->frameKeepAlive.clear();
    impl_->frameKeepAliveSeen.clear();
}

size_t Renderer::frameKeepAliveCount() const {
    return impl_->frameKeepAlive.size();
}

} // namespace earth_engine
