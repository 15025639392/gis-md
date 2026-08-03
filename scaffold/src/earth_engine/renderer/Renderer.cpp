#include "Renderer.h"
#include "GlyphAtlas.h"
#include "IconAtlas.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../core/math/Vec3.h"
#include "../core/math/Mat4.h"
#include "../tiling/TileKey.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace earth_engine {

// ============================================================
// Unified SurfaceTile Shader — cesium-native glTF vertex layout
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

static const char* kSurfaceTileVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texcoord;

uniform mat4 u_modelViewProjection;
uniform vec4 u_tileUV;
uniform float u_tileOpacity;
uniform float u_transitionOpacity;

out vec2 v_texcoord;
out vec2 v_gridUv;
out vec3 v_normal;
out float v_tileOpacity;
out float v_transitionOpacity;

void main() {
    // cesium-native RTC: tile origin is baked into the MVP matrix
    // (computed in CPU double precision). a_position is relative
    // to the tile center — small values, good float precision.
    v_texcoord = u_tileUV.xy + a_texcoord * u_tileUV.zw;
    v_gridUv = a_texcoord;
    v_normal = normalize(a_normal);
    v_tileOpacity = u_tileOpacity;
    v_transitionOpacity = u_transitionOpacity;
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kSurfaceTileFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

in vec2 v_texcoord;
in vec2 v_gridUv;
in vec3 v_normal;
in float v_tileOpacity;
in float v_transitionOpacity;
uniform sampler2D u_tileTexture;
uniform sampler2D u_overlayTexture0;
uniform sampler2D u_overlayTexture1;
uniform sampler2D u_overlayTexture2;
uniform sampler2D u_overlayTexture3;
uniform vec3 u_lightDir;
uniform int u_overlayTextureCount;
uniform vec4 u_overlayTileUV0;
uniform vec4 u_overlayTileUV1;
uniform vec4 u_overlayTileUV2;
uniform vec4 u_overlayTileUV3;
uniform vec4 u_clipUV;
uniform float u_overlayOpacity0;
uniform float u_overlayOpacity1;
uniform float u_overlayOpacity2;
uniform float u_overlayOpacity3;
uniform float u_clipEnabled;
out vec4 fragColor;

vec4 alphaOver(vec4 base, vec4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

void main() {
    if (u_clipEnabled > 0.5 && u_clipEnabled < 1.5 &&
        (v_gridUv.x < u_clipUV.x || v_gridUv.x > u_clipUV.x + u_clipUV.z ||
         v_gridUv.y < u_clipUV.y || v_gridUv.y > u_clipUV.y + u_clipUV.w)) {
        discard;
    }

    vec4 baseColor = texture(u_tileTexture, v_texcoord);
    if (u_overlayTextureCount > 0) {
        vec2 overlayUv = u_overlayTileUV0.xy + v_gridUv * u_overlayTileUV0.zw;
        baseColor = alphaOver(baseColor, texture(u_overlayTexture0, overlayUv), u_overlayOpacity0);
    }
    if (u_overlayTextureCount > 1) {
        vec2 overlayUv = u_overlayTileUV1.xy + v_gridUv * u_overlayTileUV1.zw;
        baseColor = alphaOver(baseColor, texture(u_overlayTexture1, overlayUv), u_overlayOpacity1);
    }
    if (u_overlayTextureCount > 2) {
        vec2 overlayUv = u_overlayTileUV2.xy + v_gridUv * u_overlayTileUV2.zw;
        baseColor = alphaOver(baseColor, texture(u_overlayTexture2, overlayUv), u_overlayOpacity2);
    }
    if (u_overlayTextureCount > 3) {
        vec2 overlayUv = u_overlayTileUV3.xy + v_gridUv * u_overlayTileUV3.zw;
        baseColor = alphaOver(baseColor, texture(u_overlayTexture3, overlayUv), u_overlayOpacity3);
    }
    vec3 N = normalize(v_normal);
    vec3 L = normalize(u_lightDir);
    float NdotL = max(dot(N, L), 0.0);

    // Imagery is the primary diagnostic surface in this demo. Keep it readable
    // even when the light vector is behind the tile; directional light should
    // hint at curvature, not turn missing-resource states into a black globe.
    float shade = mix(0.72, 1.0, smoothstep(0.0, 1.0, NdotL));
    baseColor.rgb *= shade;
    baseColor.a *= clamp(v_tileOpacity, 0.0, 1.0) * clamp(v_transitionOpacity, 0.0, 1.0);
    fragColor = baseColor;
}
)glsl";

// ============================================================
// glTF primitive shader — TileRenderContent render resources
// POSITION(vec3) + NORMAL(vec3) + TEXCOORD_0..7(packed vec4 pairs)
// + COLOR_0(vec4) + TANGENT(vec4) = 120 bytes
// ============================================================

static const char* kGltfVertexGLSL = R"glsl(
#version 300 es
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

static const char* kGltfFragmentGLSL = R"glsl(
#version 300 es
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
uniform sampler2D u_mappedRasterTexture0;
uniform sampler2D u_mappedRasterTexture1;
uniform sampler2D u_mappedRasterTexture2;
uniform sampler2D u_mappedRasterTexture3;
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
uniform float u_mappedRasterTextureCount;
uniform vec4 u_mappedRasterTileUV0;
uniform vec4 u_mappedRasterTileUV1;
uniform vec4 u_mappedRasterTileUV2;
uniform vec4 u_mappedRasterTileUV3;
uniform float u_mappedRasterOpacity0;
uniform float u_mappedRasterOpacity1;
uniform float u_mappedRasterOpacity2;
uniform float u_mappedRasterOpacity3;
uniform float u_mappedRasterTexCoordSet0;
uniform float u_mappedRasterTexCoordSet1;
uniform float u_mappedRasterTexCoordSet2;
uniform float u_mappedRasterTexCoordSet3;
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
uniform vec4 u_terrainLayers;  // x=高度纹理层(顶点) y=间接纹理层(片元)

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

vec4 applyMappedRaster(
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
    if (u_mappedRasterTextureCount > 0.5) {
        base = applyMappedRaster(
            base,
            u_mappedRasterTexture0,
            u_mappedRasterTexCoordSet0,
            u_mappedRasterTileUV0,
            u_mappedRasterOpacity0);
    }
    if (u_mappedRasterTextureCount > 1.5) {
        base = applyMappedRaster(
            base,
            u_mappedRasterTexture1,
            u_mappedRasterTexCoordSet1,
            u_mappedRasterTileUV1,
            u_mappedRasterOpacity1);
    }
    if (u_mappedRasterTextureCount > 2.5) {
        base = applyMappedRaster(
            base,
            u_mappedRasterTexture2,
            u_mappedRasterTexCoordSet2,
            u_mappedRasterTileUV2,
            u_mappedRasterOpacity2);
    }
    if (u_mappedRasterTextureCount > 3.5) {
        base = applyMappedRaster(
            base,
            u_mappedRasterTexture3,
            u_mappedRasterTexCoordSet3,
            u_mappedRasterTileUV3,
            u_mappedRasterOpacity3);
    }
    // 稀疏虚拟纹理(Step B2b):capped 真实地形瓦片经 per-tile 间接纹理单次 NEAREST
    // fetch 定位共享 array 层 → 覆盖 mappedRaster 显更细影像。cell resident(A=1)才
    // 覆盖,miss(A=0)保留 mappedRaster(决策② 共存优雅降级)。UV 复用 set 0 mercator。
    if (u_pageStoreParams.x > 0.5) {
        vec2 psUv = uvFromSet(0.0);
        float gridN = max(u_pageStoreParams.y, 1.0);
        vec2 g = clamp(psUv, 0.0, 1.0) * gridN;
        vec2 cell = clamp(floor(g), vec2(0.0), vec2(gridN - 1.0));
        vec4 e = texelFetch(
            u_pageStoreIndir,
            ivec3(ivec2(cell), int(u_terrainLayers.y + 0.5)), 0);
        float layer = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
        // per-cell 渐变 LOD(§16.3):d>0 → cell 采粗祖先页(覆盖 span=2^d 个精 cell)。
        // origin=该粗页在精网格的左下角,sampleUv=片元在粗页内 [0,1] 子区。
        // d=0:span=1、origin=cell → sampleUv=g-cell(逐字节=现状精页,无回归)。
        float d = floor(e.b * 255.0 + 0.5);
        vec2 span = vec2(exp2(d));
        vec2 origin = floor(cell / span) * span;
        vec2 sampleUv = (g - origin) / span;
        base = alphaOver(base, texture(u_pageStore, vec3(sampleUv, layer)), e.a);
    }
    base = applyGltfWaterMask(base, N, L, normalize(u_eyePositionRTC - v_position));
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

static const char* kGltfInstancedVertexGLSL = R"glsl(
#version 300 es
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
// Terrain lightweight shader — 28-byte compact TerrainGpuVertex layout
// POSITION(f32x3@0) + NORMAL(snorm16x3+pad@12) + TEXCOORD_0/1(unorm16x4@20)
// = 28 bytes. Quantized attributes arrive in the shader as normalized floats.
// This is the glTF shader MINUS all PBR-extension uniforms: it keeps only
// base color, raster-overlay compositing (slots 15-18), water mask (slot 19),
// terrain clip, directional lighting and render opacity. RTC origin stays
// baked into u_modelViewProjection (double precision on the CPU side).
// ============================================================

static const char* kTerrainVertexGLSL = R"glsl(
#version 300 es
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
        float snapPacked = u_terrainLayers.z;
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

static const char* kTerrainFragmentGLSL = R"glsl(
#version 300 es
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
uniform vec3 u_eyePositionRTC;
uniform vec4 u_baseColor;
uniform float u_hasBaseColorTexture;
uniform sampler2D u_baseColorTexture;
uniform float u_alphaMode;
uniform float u_alphaCutoff;
uniform float u_renderOpacity;
uniform sampler2D u_mappedRasterTexture0;
uniform sampler2D u_mappedRasterTexture1;
uniform sampler2D u_mappedRasterTexture2;
uniform sampler2D u_mappedRasterTexture3;
uniform sampler2D u_gltfWaterMaskTexture;
uniform float u_mappedRasterTextureCount;
uniform vec4 u_mappedRasterTileUV0;
uniform vec4 u_mappedRasterTileUV1;
uniform vec4 u_mappedRasterTileUV2;
uniform vec4 u_mappedRasterTileUV3;
uniform float u_mappedRasterOpacity0;
uniform float u_mappedRasterOpacity1;
uniform float u_mappedRasterOpacity2;
uniform float u_mappedRasterOpacity3;
uniform float u_mappedRasterTexCoordSet0;
uniform float u_mappedRasterTexCoordSet1;
uniform float u_mappedRasterTexCoordSet2;
uniform float u_mappedRasterTexCoordSet3;
uniform float u_gltfHasWaterMask;
uniform vec4 u_gltfWaterMaskTranslationScale;
uniform vec4 u_gltfWaterMaskState;
uniform vec4 u_clipUV;
uniform float u_clipEnabled;
// 北极星合成方案页存储(Step 3):x=enabled y=gridN z=layerBase(B1 起由间接
// 纹理承载,shader 不再用)w=保留。
uniform highp sampler2DArray u_pageStore;
uniform vec4 u_pageStoreParams;
// 稀疏虚拟纹理(Step B1):间接纹理(RGBA8 编 layer 索引)。合批 Step 2:搬共享
// texture2DArray(固定 64² 每层,texel 写左上 gridN² 区),层号 u_terrainLayers.y。
uniform highp sampler2DArray u_pageStoreIndir;
uniform vec4 u_terrainLayers;  // x=高度纹理层(顶点) y=间接纹理层(片元)

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

    vec2 g = clamp(uv, 0.0, 1.0) * gridN;
    vec2 f = fract(g);
    ivec2 i0 = max(ivec2(floor(g)), ivec2(0));
    ivec2 i1 = min(i0 + ivec2(1), ivec2(int(gridN)));
    vec2 n00 = texelFetch(tex, ivec3(i0.x, i0.y, layer), 0).ba;
    vec2 n10 = texelFetch(tex, ivec3(i1.x, i0.y, layer), 0).ba;
    vec2 n01 = texelFetch(tex, ivec3(i0.x, i1.y, layer), 0).ba;
    vec2 n11 = texelFetch(tex, ivec3(i1.x, i1.y, layer), 0).ba;
    vec2 nxy = mix(mix(n00, n10, f.x), mix(n01, n11, f.x), f.y) * 2.0 - 1.0;
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

vec4 applyMappedRaster(
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
    if (u_mappedRasterTextureCount > 0.5) {
        base = applyMappedRaster(
            base,
            u_mappedRasterTexture0,
            u_mappedRasterTexCoordSet0,
            u_mappedRasterTileUV0,
            u_mappedRasterOpacity0);
    }
    if (u_mappedRasterTextureCount > 1.5) {
        base = applyMappedRaster(
            base,
            u_mappedRasterTexture1,
            u_mappedRasterTexCoordSet1,
            u_mappedRasterTileUV1,
            u_mappedRasterOpacity1);
    }
    if (u_mappedRasterTextureCount > 2.5) {
        base = applyMappedRaster(
            base,
            u_mappedRasterTexture2,
            u_mappedRasterTexCoordSet2,
            u_mappedRasterTileUV2,
            u_mappedRasterOpacity2);
    }
    if (u_mappedRasterTextureCount > 3.5) {
        base = applyMappedRaster(
            base,
            u_mappedRasterTexture3,
            u_mappedRasterTexCoordSet3,
            u_mappedRasterTileUV3,
            u_mappedRasterOpacity3);
    }
    // 合成方案页存储(Step 3):目标 capped 瓦片改采 sampler2DArray 页存储
    // (enabled=1),覆盖上采样 mappedRaster → 显示真实高清影像。瓦片规则切
    // gridN×gridN 页,mesh UV 落格算 layer + 层内局部 UV,单次索引 + 单次采样;
    // 层间不插值 + 每层 CLAMP_TO_EDGE 天然无页缝(§13.1)。enabled=0 恒不进。
    if (u_pageStoreParams.x > 0.5) {
        float gridN = max(u_pageStoreParams.y, 1.0);
        vec2 g = clamp(terrainUv, 0.0, 1.0) * gridN;
        vec2 cell = clamp(floor(g), vec2(0.0), vec2(gridN - 1.0));
        // Step B1:经间接纹理单次 fetch 定位层(替代闭式 layerBase+…)。
        // RGBA8 解码 R+G*256(floor(x*255+0.5) 从 unorm 取回整数字节)。
        // 合批 Step 2:texelFetch 整数寻址 array 层(texel 在左上 gridN² 区)。
        vec4 e = texelFetch(
            u_pageStoreIndir,
            ivec3(ivec2(cell), int(u_terrainLayers.y + 0.5)), 0);
        float layer = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
        // per-cell 渐变 LOD(§16.3,镜像 gltf):d>0 采粗祖先页;d=0=现状精页。
        float d = floor(e.b * 255.0 + 0.5);
        vec2 span = vec2(exp2(d));
        vec2 origin = floor(cell / span) * span;
        vec2 sampleUv = (g - origin) / span;
        base = alphaOver(
            base, texture(u_pageStore, vec3(sampleUv, layer)), e.a);
    }
    base = applyGltfWaterMask(base, N, L, normalize(u_eyePositionRTC - v_position));
    if (u_alphaMode > 0.5 && u_alphaMode < 1.5 && base.a < u_alphaCutoff) {
        discard;
    }
    float alpha = u_alphaMode > 1.5 ? base.a : 1.0;

    // GE 式半球光照:蓝天 ambient 补光集中在阴影侧,太阳做方向 relief。
    // 受光面≈base(不过曝、不蓝 cast);阴影侧被抬 + 天空蓝染,而非洗白或
    // 死黑。directional:0=背光→1=受光。旧模型把 ambient 加在满 shade 之
    // 上导致受光面 base*1.24 过曝偏蓝(非 GE),此处改为 hemisphere 分配。
    // 方向项 = 线性 lambert × 增益 + 阴影底,clamp 收尾。系数取自 CesiumJS
    // Globe.js 默认(lambertDiffuseMultiplier=0.9 / vertexShadowDarkness=0.3),
    // 对应 GlobeFS.glsl 的 ENABLE_VERTEX_LIGHTING 路径。范围 0.3→1.0(3.3×)。
    //
    // **不用 smoothstep**:它在 NdotL→1(高太阳角,最常见)处导数恰为 0,会把地形
    // 法线的全部贡献吃掉。实测本 demo 钉死场景 directional 中位 0.992,导致法线
    // 改动前后画面只有 0.16% 像素有差(见 docs/issues/
    // terrain-visual-maturity-gap-2026-08-02.md §4.4)。osgEarth(PhongLighting.glsl)
    // 同样用线性 max(dot(N,L),0) —— 两个参考实现在"线性"上一致。
    const float kLambertGain = 0.9;      // = Cesium lambertDiffuseMultiplier
    const float kShadowFloor = 0.3;      // = Cesium vertexShadowDarkness
    float directional = clamp(NdotL * kLambertGain + kShadowFloor, 0.0, 1.0);
    // 暖阳/冷阴影(GE/摄影级):受光面乘微暖太阳色(红+蓝−),背光面由
    // 天空蓝 ambient 补光——冷暖分离让 relief 更立体、更像真实日照。
    // 冷暖/ambient 的分配仍按纯 NdotL(端点语义与改动前一致,中间由 smoothstep
    // 改为线性),与上面的亮度增益解耦。
    float sunlit = clamp(NdotL, 0.0, 1.0);
    vec3 sunTint = vec3(1.05, 1.0, 0.91);
    vec3 color = base.rgb * directional
                          * mix(vec3(1.0), sunTint, sunlit)
               + base.rgb * u_ambient.rgb * (1.0 - sunlit);
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
static const char* kTerrainInstancedVertexGLSL = R"glsl(
#version 300 es
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

uniform mat4 u_modelViewProjection;  // = viewProj · frame0
uniform highp sampler2DArray u_heightTexture;

out vec3 v_normal;
out vec3 v_position;
out vec4 v_texcoord01;
out float v_skirt;            // 1=裙顶点(片元据此跳过法线场,见 terrainShader 注释)
flat out float v_heightLayer; // 高度纹理层号(片元读 B/A 法线要用)
flat out vec4 v_pageParams;   // x=pageGridN y=indirLayer z=clipEnabled w=模板 gridN
flat out vec4 v_clipUv;

// 模板栅格边长逐实例给(i_layers.w):自适应密度后不再是常量,coarse=64、
// dense=256(见 TerrainDisplacementTemplatePool.h terrainGridSizeForSse)。

float eeSampleTerrainHeight(
    highp sampler2DArray tex, ivec2 texel, int layer, vec2 mr) {
    vec4 p = texelFetch(tex, ivec3(texel, layer), 0);
    float t = (p.r * 255.0 * 256.0 + p.g * 255.0) / 65535.0;
    return mr.x + t * mr.y;
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
    v_clipUv = i_clipUv;
    gl_Position = u_modelViewProjection * vec4(world, 1.0);
}
)glsl";

static const char* kTerrainInstancedFragmentGLSL = R"glsl(
#version 300 es
precision highp float;

in vec3 v_normal;
in vec3 v_position;
in vec4 v_texcoord01;
in float v_skirt;
flat in float v_heightLayer;
flat in vec4 v_pageParams;  // x=pageGridN y=indirLayer z=clipEnabled w=模板 gridN
flat in vec4 v_clipUv;

uniform vec3 u_lightDir;
uniform vec4 u_ambient;
uniform vec4 u_baseColor;
uniform highp sampler2DArray u_pageStore;
uniform highp sampler2DArray u_pageStoreIndir;
uniform highp sampler2DArray u_heightTexture;

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
    vec2 g = clamp(uv, 0.0, 1.0) * gridN;
    vec2 f = fract(g);
    ivec2 i0 = max(ivec2(floor(g)), ivec2(0));
    ivec2 i1 = min(i0 + ivec2(1), ivec2(int(gridN)));
    vec2 n00 = texelFetch(tex, ivec3(i0.x, i0.y, layer), 0).ba;
    vec2 n10 = texelFetch(tex, ivec3(i1.x, i0.y, layer), 0).ba;
    vec2 n01 = texelFetch(tex, ivec3(i0.x, i1.y, layer), 0).ba;
    vec2 n11 = texelFetch(tex, ivec3(i1.x, i1.y, layer), 0).ba;
    vec2 nxy = mix(mix(n00, n10, f.x), mix(n01, n11, f.x), f.y) * 2.0 - 1.0;
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
    // 页存储:资格闸保证全 cell 驻留 → 直接覆盖(无 mappedRaster fallback)。
    float gridN = max(v_pageParams.x, 1.0);
    int indirLayer = int(v_pageParams.y + 0.5);
    vec2 g = clamp(terrainUv, 0.0, 1.0) * gridN;
    vec2 cell = clamp(floor(g), vec2(0.0), vec2(gridN - 1.0));
    vec4 e = texelFetch(u_pageStoreIndir, ivec3(ivec2(cell), indirLayer), 0);
    float layer = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
    float d = floor(e.b * 255.0 + 0.5);
    vec2 span = vec2(exp2(d));
    vec2 origin = floor(cell / span) * span;
    vec2 sampleUv = (g - origin) / span;
    base = alphaOver(base, texture(u_pageStore, vec3(sampleUv, layer)), e.a);

    // GE 式半球光照(与 terrainShader 一致;系数来源与不用 smoothstep 的理由
    // 见 kTerrainFragmentGLSL 同处注释)。
    const float kLambertGain = 0.9;
    const float kShadowFloor = 0.3;
    float directional = clamp(NdotL * kLambertGain + kShadowFloor, 0.0, 1.0);
    float sunlit = clamp(NdotL, 0.0, 1.0);
    vec3 sunTint = vec3(1.05, 1.0, 0.91);
    vec3 color = base.rgb * directional
                          * mix(vec3(1.0), sunTint, sunlit)
               + base.rgb * u_ambient.rgb * (1.0 - sunlit);
    fragColor = vec4(color, 1.0);
}
)glsl";

// ============================================================
// Deprecated shaders (kept for reference)
// ============================================================

#if 0
static const char* kTileVertexGLSL_deprecated = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 2) in vec2 a_texcoord;

uniform mat4 u_modelViewProjection;
uniform vec4 u_tileUV;
uniform vec3 u_tileOrigin;

out vec2 v_texcoord;
out vec2 v_gridUv;
out vec3 v_normal;

// WGS84 inverse radii squared
const vec3 kInvRadiiSq = vec3(
    1.0 / (6378137.0 * 6378137.0),
    1.0 / (6378137.0 * 6378137.0),
    1.0 / (6356752.314245 * 6356752.314245));

void main() {
    v_texcoord = u_tileUV.xy + a_texcoord * u_tileUV.zw;
    v_gridUv = a_texcoord;
    vec3 worldPos = a_position + u_tileOrigin;
    v_normal = normalize(worldPos * kInvRadiiSq);
    gl_Position = u_modelViewProjection * vec4(worldPos, 1.0);
}
)glsl";

static const char* kTileFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

in vec2 v_texcoord;
in vec2 v_gridUv;
in vec3 v_normal;
uniform sampler2D u_tileTexture;
uniform sampler2D u_overlayTexture0;
uniform sampler2D u_overlayTexture1;
uniform sampler2D u_overlayTexture2;
uniform sampler2D u_overlayTexture3;
uniform sampler2D u_waterMask;
uniform vec3 u_lightDir;
uniform vec3 u_fogColor;
uniform float u_fogDensity;
uniform float u_tileOpacity;
uniform float u_transitionOpacity;
uniform int u_overlayTextureCount;
uniform float u_hasWaterMask;
uniform vec4 u_overlayTileUV0;
uniform vec4 u_overlayTileUV1;
uniform vec4 u_overlayTileUV2;
uniform vec4 u_overlayTileUV3;
uniform float u_overlayOpacity0;
uniform float u_overlayOpacity1;
uniform float u_overlayOpacity2;
uniform float u_overlayOpacity3;
out vec4 fragColor;

vec4 alphaOver(vec4 base, vec4 overlay, float opacity) {
    overlay.a *= clamp(opacity, 0.0, 1.0);
    base.rgb = mix(base.rgb, overlay.rgb, overlay.a);
    base.a = max(base.a, overlay.a);
    return base;
}

void main() {
    vec4 color = texture(u_tileTexture, v_texcoord);
    color.a = 1.0;
    color.a *= clamp(u_tileOpacity, 0.0, 1.0) * clamp(u_transitionOpacity, 0.0, 1.0);
    if (u_overlayTextureCount > 0) {
        vec2 overlayUv = u_overlayTileUV0.xy + v_gridUv * u_overlayTileUV0.zw;
        color = alphaOver(color, texture(u_overlayTexture0, overlayUv), u_overlayOpacity0);
    }
    if (u_overlayTextureCount > 1) {
        vec2 overlayUv = u_overlayTileUV1.xy + v_gridUv * u_overlayTileUV1.zw;
        color = alphaOver(color, texture(u_overlayTexture1, overlayUv), u_overlayOpacity1);
    }
    if (u_overlayTextureCount > 2) {
        vec2 overlayUv = u_overlayTileUV2.xy + v_gridUv * u_overlayTileUV2.zw;
        color = alphaOver(color, texture(u_overlayTexture2, overlayUv), u_overlayOpacity2);
    }
    if (u_overlayTextureCount > 3) {
        vec2 overlayUv = u_overlayTileUV3.xy + v_gridUv * u_overlayTileUV3.zw;
        color = alphaOver(color, texture(u_overlayTexture3, overlayUv), u_overlayOpacity3);
    }
    fragColor = color;
}
)glsl";
#endif

// ============================================================
// Metal Shading Language 源码
// ============================================================

static const char* kTileVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    // normal removed — GPU computes geodetic normal from position
    float2 texcoord [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 texcoord;
    float3 normal;
};

vertex VertexOut tileVertex(VertexIn in [[stage_in]],
                             constant float4x4& u_modelViewProjection [[buffer(1)]],
                             constant float4& u_tileUV [[buffer(3)]]) {
    VertexOut out;
    out.position = u_modelViewProjection *
        float4(in.position, 1.0);
    out.texcoord = u_tileUV.xy + in.texcoord * u_tileUV.zw;
    // WGS84 geodetic normal from ECEF position
    constexpr float3 kInvRadiiSq = float3(
        1.0f / (6378137.0f * 6378137.0f),
        1.0f / (6378137.0f * 6378137.0f),
        1.0f / (6356752.314245f * 6356752.314245f));
    out.normal = normalize(in.position * kInvRadiiSq);
    return out;
}
)msl";

// ============================================================
// Color Shader (Vector Layers) — GLSL ES 3.0
// ============================================================

static const char* kColorVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;

uniform mat4 u_modelViewProjection;

void main() {
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kColorFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

uniform vec4 u_color;
out vec4 fragColor;

void main() {
    fragColor = u_color;
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

static const char* kVectorFillVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;   // RGBA8 归一化

uniform mat4 u_modelViewProjection;

out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kVectorFillFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

in vec4 v_color;
out vec4 fragColor;

void main() {
    fragColor = v_color;
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

static const char* kVectorLineVertexGLSL = R"glsl(
#version 300 es
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

static const char* kVectorLineFragmentGLSL = R"glsl(
#version 300 es
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
    fragColor = vec4(v_color.rgb, a);
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

static const char* kVectorLineStencilVertexGLSL = R"glsl(
#version 300 es
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
static const char* kVectorLineStencilFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

uniform vec4 u_color;
out vec4 fragColor;

void main() {
    fragColor = u_color;
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

static const char* kVectorPointVertexGLSL = R"glsl(
#version 300 es
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

void main() {
    vec4 cp = u_modelViewProjection * vec4(a_anchor, 1.0);
    v_uv = a_uv;
    v_color = a_color;
    v_shape = a_shape;
    if (cp.w <= 0.0) {
        gl_Position = cp;      // 相机后方:不展开
        return;
    }
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
        fragColor = vec4(tex.rgb * v_color.rgb, alpha);
        return;
    }
    // 解析 SDF:fwidth 自适应软边(尺寸无关,任意缩放都是 ~1px)。
    float d = symbolSdf(v_shape, v_uv);
    float w = max(fwidth(d), 1e-5);
    float alpha = (1.0 - smoothstep(-w, w, d)) * v_color.a;
    if (alpha <= 0.004) discard;
    fragColor = vec4(v_color.rgb, alpha);
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

static const char* kVectorLabelVertexGLSL = R"glsl(
#version 300 es
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

void main() {
    vec4 cp = u_modelViewProjection * vec4(a_anchor, 1.0);
    v_uv = a_uv;
    v_opacity = a_offsetPx.z;
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

static const char* kVectorLabelFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

uniform sampler2D u_glyphAtlas;
uniform vec4 u_color;
uniform vec4 u_haloColor;
uniform float u_sdfEdge;       // 轮廓阈值(kSdfOnEdge/255)
uniform float u_sdfHaloDelta;  // halo 宽换算的 SDF 值差

in vec2 v_uv;
in float v_opacity;
out vec4 fragColor;

void main() {
    float d = texture(u_glyphAtlas, v_uv).r;
    float w = fwidth(d);
    float fill = smoothstep(u_sdfEdge - w, u_sdfEdge + w, d);
    float halo = smoothstep(u_sdfEdge - u_sdfHaloDelta - w,
                            u_sdfEdge - u_sdfHaloDelta + w, d);
    float alpha = max(fill * u_color.a, halo * u_haloColor.a) * v_opacity;
    if (alpha <= 0.004) discard;
    vec3 rgb = mix(u_haloColor.rgb, u_color.rgb, fill);
    fragColor = vec4(rgb, alpha);
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
        texture2d<float> u_glyphAtlas [[texture(0)]],
        sampler u_sampler [[sampler(0)]],
        constant float4& u_color [[buffer(0)]],
        constant float4& u_haloColor [[buffer(1)]],
        constant float& u_sdfEdge [[buffer(2)]],
        constant float& u_sdfHaloDelta [[buffer(3)]]) {
    float d = u_glyphAtlas.sample(u_sampler, in.uv).r;
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

static const char* kTileFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

fragment float4 tileFragment(VertexOut in [[stage_in]],
                             texture2d<float> u_tileTexture [[texture(0)]],
                             sampler u_sampler [[sampler(0)]],
                             constant float3& u_lightDir [[buffer(0)]],
                             constant float& u_tileOpacity [[buffer(1)]],
                             constant float& u_transitionOpacity [[buffer(2)]]) {
    float4 color = u_tileTexture.sample(u_sampler, in.texcoord);
    float luma = dot(color.rgb, float3(0.299, 0.587, 0.114));
    color.rgb = mix(float3(luma), color.rgb, 1.08);
    color.rgb = (color.rgb - float3(0.5)) * 1.06 + float3(0.5);
    color.rgb = clamp(color.rgb, 0.0, 1.0);

    float3 n = normalize(in.normal);
    float diffuse = max(dot(n, normalize(u_lightDir)), 0.0);
    float shade = mix(0.72, 1.0, smoothstep(0.0, 1.0, diffuse));
    color.rgb *= shade;
    color.a *= clamp(u_tileOpacity, 0.0, 1.0) * clamp(u_transitionOpacity, 0.0, 1.0);
    return color;
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
    float mappedRasterTextureCount;
    packed_float4 mappedRasterTileUV[4];
    float mappedRasterOpacity[4];
    float mappedRasterTexCoordSet[4];
    float hasWaterMask;
    packed_float4 waterMaskTranslationScale;
    packed_float4 waterMaskState;
    packed_float4 clipUV;
    float clipEnabled;
    packed_float4 pageStoreParams;
    packed_float4 heightDisplace;  // Phase 2c Stage B(顶点消费,fragment 仅占位对齐)
    packed_float4 terrainLayers;   // 合批 Step 1:x=高度纹理 array 层号(顶点消费)
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

float4 gltfApplyMappedRaster(float4 base,
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
                             texture2d<float> u_mappedRasterTexture0 [[texture(15)]],
                             texture2d<float> u_mappedRasterTexture1 [[texture(16)]],
                             texture2d<float> u_mappedRasterTexture2 [[texture(17)]],
                             texture2d<float> u_mappedRasterTexture3 [[texture(18)]],
                             texture2d<float> u_gltfWaterMaskTexture [[texture(19)]],
                             // SVT(Step B2b):真实 DEM 表面走此 glTF shader,页存储在此。
                             // 合批 Step 2:间接纹理搬 array(64² 每层,层号 u.terrainLayers.y)。
                             texture2d_array<float> u_pageStore [[texture(20)]],
                             texture2d_array<float> u_pageStoreIndir [[texture(21)]],
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
    if (u.mappedRasterTextureCount > 0.5) {
        base = gltfApplyMappedRaster(
            base,
            in,
            u_mappedRasterTexture0,
            u_tileSharedSampler,
            u.mappedRasterTexCoordSet[0],
            float4(u.mappedRasterTileUV[0]),
            u.mappedRasterOpacity[0]);
    }
    if (u.mappedRasterTextureCount > 1.5) {
        base = gltfApplyMappedRaster(
            base,
            in,
            u_mappedRasterTexture1,
            u_tileSharedSampler,
            u.mappedRasterTexCoordSet[1],
            float4(u.mappedRasterTileUV[1]),
            u.mappedRasterOpacity[1]);
    }
    if (u.mappedRasterTextureCount > 2.5) {
        base = gltfApplyMappedRaster(
            base,
            in,
            u_mappedRasterTexture2,
            u_tileSharedSampler,
            u.mappedRasterTexCoordSet[2],
            float4(u.mappedRasterTileUV[2]),
            u.mappedRasterOpacity[2]);
    }
    if (u.mappedRasterTextureCount > 3.5) {
        base = gltfApplyMappedRaster(
            base,
            in,
            u_mappedRasterTexture3,
            u_tileSharedSampler,
            u.mappedRasterTexCoordSet[3],
            float4(u.mappedRasterTileUV[3]),
            u.mappedRasterOpacity[3]);
    }
    // SVT(Step B2b,镜像 GLSL):per-tile 间接纹理单次 NEAREST fetch 定位 array 层
    // 覆盖 mappedRaster;A 通道 resident 标志,miss 保留 mappedRaster(决策② 共存)。
    if (u.pageStoreParams.x > 0.5) {
        float gridN = max(u.pageStoreParams.y, 1.0);
        float2 g = clamp(terrainUv, 0.0, 1.0) * gridN;
        float2 cell = clamp(floor(g), float2(0.0), float2(gridN - 1.0));
        // 合批 Step 2:read() 整数寻址 array 层(texel 在左上 gridN² 区)。
        float4 e = u_pageStoreIndir.read(
            uint2(cell), uint(u.terrainLayers.y + 0.5), 0);
        float layer = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
        // per-cell 渐变 LOD(§16.3,镜像 GLSL):d>0 采粗祖先页;d=0=现状精页。
        float d = floor(e.b * 255.0 + 0.5);
        float2 span = float2(exp2(d));
        float2 origin = floor(cell / span) * span;
        float2 sampleUv = (g - origin) / span;
        base = gltfAlphaOver(
            base,
            u_pageStore.sample(u_tileSharedSampler, sampleUv, uint(layer)),
            e.a);
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
// 28-byte compact TerrainGpuVertex: position(f32x3@0)
// normal(short4Normalized@12, w = pad) texcoord01(ushort4Normalized@20).
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
    float mappedRasterTextureCount;
    packed_float4 mappedRasterTileUV[4];
    float mappedRasterOpacity[4];
    float mappedRasterTexCoordSet[4];
    float hasWaterMask;
    packed_float4 waterMaskTranslationScale;
    packed_float4 waterMaskState;
    packed_float4 clipUV;
    float clipEnabled;
    packed_float4 pageStoreParams;
    packed_float4 heightDisplace;  // Phase 2c Stage B(顶点消费,fragment 仅占位对齐)
    packed_float4 terrainLayers;   // 合批 Step 1:x=高度纹理 array 层号(顶点消费)
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

float4 terrainApplyMappedRaster(float4 base,
                                TerrainVertexOut in,
                                texture2d<float> rasterTexture,
                                sampler rasterSampler,
                                float texCoordSet,
                                float4 tileUV,
                                float opacity) {
    float2 overlayUv =
        tileUV.xy + terrainUvFromSet(in, texCoordSet) * tileUV.zw;
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
    texture2d<float> u_mappedRasterTexture0 [[texture(15)]],
    texture2d<float> u_mappedRasterTexture1 [[texture(16)]],
    texture2d<float> u_mappedRasterTexture2 [[texture(17)]],
    texture2d<float> u_mappedRasterTexture3 [[texture(18)]],
    texture2d<float> u_gltfWaterMaskTexture [[texture(19)]],
    // 合成方案页存储(Step 3):sampler2DArray 页存储在 water mask 之后的槽 20,
    // 复用同一 clamp/linear 采样器(层间不插值 + 每层 clamp 无页缝,§13.1)。
    texture2d_array<float> u_pageStore [[texture(20)]],
    // 稀疏虚拟纹理(Step B1):间接纹理(RGBA8 编 layer 索引)。合批 Step 2:搬
    // array(64² 每层,层号 u.terrainLayers.y),read() 整数寻址不占 sampler 槽。
    texture2d_array<float> u_pageStoreIndir [[texture(21)]],
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
    if (u.mappedRasterTextureCount > 0.5) {
        base = terrainApplyMappedRaster(
            base, in, u_mappedRasterTexture0, u_terrainSampler,
            u.mappedRasterTexCoordSet[0],
            float4(u.mappedRasterTileUV[0]), u.mappedRasterOpacity[0]);
    }
    if (u.mappedRasterTextureCount > 1.5) {
        base = terrainApplyMappedRaster(
            base, in, u_mappedRasterTexture1, u_terrainSampler,
            u.mappedRasterTexCoordSet[1],
            float4(u.mappedRasterTileUV[1]), u.mappedRasterOpacity[1]);
    }
    if (u.mappedRasterTextureCount > 2.5) {
        base = terrainApplyMappedRaster(
            base, in, u_mappedRasterTexture2, u_terrainSampler,
            u.mappedRasterTexCoordSet[2],
            float4(u.mappedRasterTileUV[2]), u.mappedRasterOpacity[2]);
    }
    if (u.mappedRasterTextureCount > 3.5) {
        base = terrainApplyMappedRaster(
            base, in, u_mappedRasterTexture3, u_terrainSampler,
            u.mappedRasterTexCoordSet[3],
            float4(u.mappedRasterTileUV[3]), u.mappedRasterOpacity[3]);
    }
    // 合成方案页存储(Step 3,镜像 GLSL 侧):目标 capped 瓦片改采页存储,
    // 覆盖上采样 mappedRaster → 真实高清影像。enabled=0 恒不进,零回归。
    if (u.pageStoreParams.x > 0.5) {
        float gridN = max(u.pageStoreParams.y, 1.0);
        float2 g = clamp(terrainUv, 0.0, 1.0) * gridN;
        float2 cell = clamp(floor(g), float2(0.0), float2(gridN - 1.0));
        // Step B1(镜像 GLSL):经间接纹理单次 fetch 定位层;RGBA8 解码 R+G*256。
        // 合批 Step 2:read() 整数寻址 array 层(texel 在左上 gridN² 区)。
        float4 e = u_pageStoreIndir.read(
            uint2(cell), uint(u.terrainLayers.y + 0.5), 0);
        float layer = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
        // per-cell 渐变 LOD(§16.3,镜像 GLSL):d>0 采粗祖先页;d=0=现状精页。
        float d = floor(e.b * 255.0 + 0.5);
        float2 span = float2(exp2(d));
        float2 origin = floor(cell / span) * span;
        float2 sampleUv = (g - origin) / span;
        // B2b(镜像 GLSL):factor = e.a(resident 标志)。miss=0 保留 mappedRaster。
        base = terrainAlphaOver(
            base,
            u_pageStore.sample(u_terrainSampler, sampleUv, uint(layer)),
            e.a);
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

    // GE 式半球光照(与 kTerrainFragmentGLSL 一致):蓝天 ambient 补阴影、
    // 太阳做方向 relief、受光面≈base 不过曝。
    // 系数来源与不用 smoothstep 的理由见 kTerrainFragmentGLSL 同处注释。
    const float kLambertGain = 0.9;
    const float kShadowFloor = 0.3;
    float directional = clamp(NdotL * kLambertGain + kShadowFloor, 0.0, 1.0);
    float sunlit = clamp(NdotL, 0.0, 1.0);
    // 暖阳/冷阴影(与 kTerrainFragmentGLSL 一致)。
    float3 sunTint = float3(1.05, 1.0, 0.91);
    float3 color = base.rgb * directional
                            * mix(float3(1.0), sunTint, sunlit)
                 + base.rgb * float4(u.ambient).rgb * (1.0 - sunlit);
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
    float4 dispMorph  [[attribute(7)]];  // minH·fade, range·fade, morph, gridN
    float4 clipUv     [[attribute(8)]];
    float4 layers     [[attribute(9)]];  // heightLayer, indirLayer, clipEn, _
};

struct TerrainInstancedVertexOut {
    float4 position [[position]];
    float3 normal;
    float3 localPosition;
    float4 texcoord01;
    float4 pageParams [[flat]];  // x=gridN y=indirLayer z=clipEnabled w=_
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
    float gridN = max(in.pageParams.x, 1.0);
    uint indirLayer = uint(in.pageParams.y + 0.5);
    float2 g = clamp(terrainUv, 0.0, 1.0) * gridN;
    float2 cell = clamp(floor(g), float2(0.0), float2(gridN - 1.0));
    float4 e = u_pageStoreIndir.read(uint2(cell), indirLayer, 0);
    float layer = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
    float d = floor(e.b * 255.0 + 0.5);
    float2 span = float2(exp2(d));
    float2 origin = floor(cell / span) * span;
    float2 sampleUv = (g - origin) / span;
    base = tiAlphaOver(
        base, u_pageStore.sample(u_pageSampler, sampleUv, uint(layer)), e.a);

    // 系数来源与不用 smoothstep 的理由见 kTerrainFragmentGLSL 同处注释。
    const float kLambertGain = 0.9;
    const float kShadowFloor = 0.3;
    float directional = clamp(NdotL * kLambertGain + kShadowFloor, 0.0, 1.0);
    float sunlit = clamp(NdotL, 0.0, 1.0);
    float3 sunTint = float3(1.05, 1.0, 0.91);
    float3 color = base.rgb * directional
                            * mix(float3(1.0), sunTint, sunlit)
                 + base.rgb * float3(u.ambient.rgb) * (1.0 - sunlit);
    return float4(color, 1.0);
}
)msl";

// ============================================================
// Shared SurfaceTile geometry: unit grid mesh
// ============================================================

struct TileVertex {
    float texcoord[2];
};

static std::pair<std::vector<TileVertex>, std::vector<uint32_t>>
makeTileGeometry(int gridSize) {
    std::vector<TileVertex> verts;
    std::vector<uint32_t> indices;

    int n = gridSize + 1;
    verts.reserve(static_cast<size_t>(n * n));

    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(gridSize);
            float v = static_cast<float>(y) / static_cast<float>(gridSize);
            verts.push_back({{u, v}});
        }
    }

    indices.reserve(static_cast<size_t>(gridSize * gridSize * 6));
    for (int y = 0; y < gridSize; ++y) {
        for (int x = 0; x < gridSize; ++x) {
            uint32_t a = static_cast<uint32_t>(y * n + x);
            uint32_t b = static_cast<uint32_t>(y * n + x + 1);
            uint32_t c = static_cast<uint32_t>((y + 1) * n + x);
            uint32_t d = static_cast<uint32_t>((y + 1) * n + x + 1);
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }

    return {verts, indices};
}

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

// ============================================================
// Renderer::Impl
// ============================================================

struct Renderer::Impl {
    RenderDevice* device = nullptr;

    // Surface tile (unified, cesium-native glTF layout)
    std::unique_ptr<ShaderProgram> surfaceTileShader;
    std::unique_ptr<Buffer> tileIndexBuffer;  // shared 64×64 grid IBO
    std::unique_ptr<Texture> surfacePlaceholderTexture;
    int tileIndexCount = 0;

    // glTF TileRenderContent
    std::unique_ptr<ShaderProgram> gltfShader;
    std::unique_ptr<ShaderProgram> gltfInstancedShader;

    // Terrain lightweight shader (28-byte compact vertex, no PBR extensions)
    std::unique_ptr<ShaderProgram> terrainShader;
    // Terrain instanced shader (合批 Step 3:32B 模板 + 96B per-instance 流)
    std::unique_ptr<ShaderProgram> terrainInstancedShader;

    // Color (vector)
    std::unique_ptr<ShaderProgram> colorShader;
    // 矢量线 ribbon(P1,§6.2 屏幕挤出)。fill 走 vectorFillShader(P6b
    // 顶点色);colorShader 留给 stencil 分类等 uniform 色路径。
    std::unique_ptr<ShaderProgram> vectorLineShader;
    // P6d stencil 贴地线(墙带体,两 stencil pass 共用)。
    std::unique_ptr<ShaderProgram> vectorLineStencilShader;
    std::unique_ptr<ShaderProgram> vectorFillShader;
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

    // ---- Unified SurfaceTile shader (cesium-native glTF layout) ----
    if (!isMetal) {
        ShaderDesc surfaceTileSd;
        surfaceTileSd.vertexSource = kSurfaceTileVertexGLSL;
        surfaceTileSd.fragmentSource = kSurfaceTileFragmentGLSL;
        impl_->surfaceTileShader = dev->createShader(surfaceTileSd);
        if (!impl_->surfaceTileShader) {
            fprintf(stderr, "[Renderer] surfaceTileShader failed\n");
            return false;
        }
    }
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
    gltfSd.fragmentSource = isMetal ? kGltfFragmentMSL : kGltfFragmentGLSL;
    impl_->gltfShader = dev->createShader(gltfSd);
    if (!impl_->gltfShader) {
        // Non-fatal on BOTH backends: a PBR shader link failure (Metal buffer
        // limit / fn ordering, or a GLES texture-unit overflow on an
        // unexpectedly tight driver) must not blank the globe. Terrain still
        // renders via terrainShader; only glTF model content is skipped, and
        // the null shader is dropped safely by the backend draw loop.
        fprintf(stderr, "[Renderer] gltfShader failed — glTF models unavailable\n");
    }

    // ---- Terrain lightweight shader (28-byte compact TerrainGpuVertex) ----
    // Unlike gltfShader, this is a small shader (<=31 Metal buffers) so it must
    // compile on BOTH backends. Treat failure as fatal like surfaceTileShader.
    ShaderDesc terrainSd;
    terrainSd.vertexSource = isMetal ? kTerrainVertexMSL : kTerrainVertexGLSL;
    terrainSd.fragmentSource =
        isMetal ? kTerrainFragmentMSL : kTerrainFragmentGLSL;
    impl_->terrainShader = dev->createShader(terrainSd);
    if (!impl_->terrainShader) {
        fprintf(stderr, "[Renderer] terrainShader failed\n");
        return false;
    }

    ShaderDesc gltfInstancedSd;
    gltfInstancedSd.vertexSource =
        isMetal ? kGltfInstancedVertexMSL : kGltfInstancedVertexGLSL;
    gltfInstancedSd.fragmentSource =
        isMetal ? kGltfFragmentMSL : kGltfFragmentGLSL;
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
        ShaderDesc terrainInstancedSd;
        terrainInstancedSd.vertexSource = kTerrainInstancedVertexGLSL;
        terrainInstancedSd.fragmentSource = kTerrainInstancedFragmentGLSL;
        impl_->terrainInstancedShader = dev->createShader(terrainInstancedSd);
        if (!impl_->terrainInstancedShader) {
            fprintf(stderr,
                    "[Renderer] terrainInstancedShader failed — terrain "
                    "batching disabled, per-draw fallback\n");
        }
    } else {
        (void)kTerrainInstancedVertexMSL;
        (void)kTerrainInstancedFragmentMSL;
    }

    // Shared index buffer for surface tiles (64×64 grid)
    auto [tileVerts, tileIndices] = makeTileGeometry(64);
    (void)tileVerts;  // VBOs are per-tile now

    BufferDesc tibDesc;
    tibDesc.size = tileIndices.size() * sizeof(uint32_t);
    tibDesc.data = tileIndices.data();
    tibDesc.usage = BufferDesc::Usage::Static;
    tibDesc.type = BufferDesc::Type::Index;
    impl_->tileIndexBuffer = dev->createBuffer(tibDesc);
    if (!impl_->tileIndexBuffer) return false;
    impl_->tileIndexCount = static_cast<int>(tileIndices.size());

    // ---- Color shader (vector layers) ----
    ShaderDesc colorSd;
    colorSd.vertexSource = isMetal ? kColorVertexMSL : kColorVertexGLSL;
    colorSd.fragmentSource = isMetal ? kColorFragmentMSL : kColorFragmentGLSL;
    impl_->colorShader = dev->createShader(colorSd);
    // colorShader failure is non-fatal (vector layers won't render but tiles still work)

    // ---- Vector fill shader (矢量 P6b 顶点色 fill) ----
    ShaderDesc vectorFillSd;
    vectorFillSd.vertexSource =
        isMetal ? kVectorFillVertexMSL : kVectorFillVertexGLSL;
    vectorFillSd.fragmentSource =
        isMetal ? kVectorFillFragmentMSL : kVectorFillFragmentGLSL;
    impl_->vectorFillShader = dev->createShader(vectorFillSd);
    if (!impl_->vectorFillShader) {
        // 非致命:fill 不出图,其余矢量/地形不受影响
        fprintf(stderr, "[Renderer] vectorFillShader failed — vector fills unavailable\n");
    }

    // ---- Vector line shader (矢量 P1 线 ribbon) ----
    ShaderDesc vectorLineSd;
    vectorLineSd.vertexSource =
        isMetal ? kVectorLineVertexMSL : kVectorLineVertexGLSL;
    vectorLineSd.fragmentSource =
        isMetal ? kVectorLineFragmentMSL : kVectorLineFragmentGLSL;
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
                                             : kVectorLineStencilFragmentGLSL;
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
        isMetal ? kVectorPointFragmentMSL : kVectorPointFragmentGLSL;
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
        isMetal ? kVectorLabelFragmentMSL : kVectorLabelFragmentGLSL;
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
    impl_->surfaceTileShader.reset();
    impl_->tileIndexBuffer.reset();
    impl_->surfacePlaceholderTexture.reset();
    impl_->gltfShader.reset();
    impl_->gltfInstancedShader.reset();
    impl_->terrainShader.reset();
    impl_->terrainInstancedShader.reset();
    impl_->colorShader.reset();
    impl_->vectorLineShader.reset();
    impl_->vectorLineStencilShader.reset();
    impl_->vectorPointShader.reset();
    impl_->vectorLabelShader.reset();
    impl_->glyphAtlas.reset();
    impl_->iconAtlas.reset();
    impl_->tileIndexCount = 0;
    impl_->initialized = false;
}

// ---- 共享资源访问 ----

ShaderProgram* Renderer::colorShader() const { return impl_->colorShader.get(); }
ShaderProgram* Renderer::vectorLineShader() const {
    return impl_->vectorLineShader.get();
}
ShaderProgram* Renderer::vectorLineStencilShader() const {
    return impl_->vectorLineStencilShader.get();
}
ShaderProgram* Renderer::vectorFillShader() const {
    return impl_->vectorFillShader.get();
}
ShaderProgram* Renderer::vectorPointShader() const {
    return impl_->vectorPointShader.get();
}
ShaderProgram* Renderer::vectorLabelShader() const {
    return impl_->vectorLabelShader.get();
}
GlyphAtlas* Renderer::glyphAtlas() const { return impl_->glyphAtlas.get(); }

IconAtlas* Renderer::iconAtlas() const { return impl_->iconAtlas.get(); }
Buffer* Renderer::tileIndexBuffer() const { return impl_->tileIndexBuffer.get(); }
int Renderer::tileIndexCount() const { return impl_->tileIndexCount; }
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

RenderCommand Renderer::makeSurfaceTileCommand(Texture* texture,
                                                Buffer* vertexBuffer,
                                                Buffer* indexBuffer,
                                                int indexCount) const {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::SurfaceTile;
    cmd.owner = "surface_tile";
    cmd.pass = "color";
    cmd.shader = impl_->surfaceTileShader.get();
    cmd.vertexBuffer = vertexBuffer;
    cmd.indexBuffer = indexBuffer ? indexBuffer : impl_->tileIndexBuffer.get();
    cmd.indexCount = indexBuffer ? indexCount : impl_->tileIndexCount;
    cmd.vertexStride = 32;  // POSITION(12) + NORMAL(12) + TEXCOORD_0(8)
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    cmd.depthTest = true;
    cmd.depthWrite = true;
    cmd.blend = false;
    cmd.cullFace = true;
    cmd.hasSurfaceTileUniforms = true;
    cmd.surfaceHasWaterMask = 0.0f;
    if (texture) {
        cmd.textures.push_back(texture);
    }
    return cmd;
}

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
    // Surface raster ownership lives in RasterMappedToTilesetTile and
    // SurfaceRasterBinding. Renderer must not retain or query imagery state.
}

void Renderer::detachRasterInMainThread(
    const TileKey&,
    int32_t) noexcept {
    // Notification hook only; renderer does not store raster binding state.
}

} // namespace earth_engine
