#include "Renderer.h"
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
    if (u_clipEnabled > 0.5 &&
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
uniform highp sampler2D u_pageStoreIndir;
uniform vec4 u_pageStoreParams;

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
    if (u_clipEnabled > 0.5 &&
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
        vec2 indirUv = (cell + 0.5) / gridN;
        vec4 e = texture(u_pageStoreIndir, indirUv);
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

out vec3 v_normal;
out vec3 v_position;
out vec4 v_texcoord01;

void main() {
    // cesium-native RTC: tile origin is baked into the MVP matrix (computed in
    // CPU double precision). a_position is relative to the tile center.
    // geomorph:沿瓦片中心椭球法线把顶点从粗起点(morph=0)长到真实高度(morph=1)。
    // heightDelta=0(如上采样子瓦片)或 w=1(无 morph)时 offset 为 0,退化为原样。
    vec3 morphPos = a_position +
        u_geomorphUpFactor.xyz * a_heightDelta * (1.0 - u_geomorphUpFactor.w);
    v_normal = normalize(a_normal);
    v_position = morphPos;
    v_texcoord01 = a_texcoord01;
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
// 稀疏虚拟纹理(Step B1):per-tile 间接纹理(RGBA8 编 layer 索引),NEAREST 采样。
uniform highp sampler2D u_pageStoreIndir;

out vec4 fragColor;

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
    if (u_clipEnabled > 0.5 &&
        (terrainUv.x < u_clipUV.x ||
         terrainUv.x > u_clipUV.x + u_clipUV.z ||
         terrainUv.y < u_clipUV.y ||
         terrainUv.y > u_clipUV.y + u_clipUV.w)) {
        discard;
    }
    float faceSign = gl_FrontFacing ? 1.0 : -1.0;
    vec3 N = normalize(v_normal) * faceSign;
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
        // Step B1:经 per-tile 间接纹理单次 NEAREST fetch 定位层(替代闭式
        // layerBase+cell.y*gridN+cell.x)。(cell+0.5)/gridN 命中 texel 中心;
        // RGBA8 解码 R+G*256(floor(x*255+0.5) 从 unorm 取回整数字节)。
        vec2 indirUv = (cell + 0.5) / gridN;
        vec4 e = texture(u_pageStoreIndir, indirUv);
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
    float directional = smoothstep(0.0, 1.0, NdotL);
    // 暖阳/冷阴影(GE/摄影级):受光面乘微暖太阳色(红+蓝−),背光面由
    // 天空蓝 ambient 补光——冷暖分离让 relief 更立体、更像真实日照。
    vec3 sunTint = vec3(1.05, 1.0, 0.91);
    vec3 color = base.rgb * (0.72 + 0.28 * directional)
                          * mix(vec3(1.0), sunTint, directional)
               + base.rgb * u_ambient.rgb * (1.0 - directional);
    fragColor = vec4(color, alpha * clamp(u_renderOpacity, 0.0, 1.0));
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
                             texture2d_array<float> u_pageStore [[texture(20)]],
                             texture2d<float> u_pageStoreIndir [[texture(21)]],
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
        constexpr sampler u_pageStoreIndirSampler(coord::normalized,
                                                  filter::nearest,
                                                  address::clamp_to_edge);
        float gridN = max(u.pageStoreParams.y, 1.0);
        float2 g = clamp(terrainUv, 0.0, 1.0) * gridN;
        float2 cell = clamp(floor(g), float2(0.0), float2(gridN - 1.0));
        float2 indirUv = (cell + 0.5) / gridN;
        float4 e = u_pageStoreIndir.sample(u_pageStoreIndirSampler, indirUv);
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

vertex TerrainVertexOut terrainVertex(
    TerrainVertexIn in [[stage_in]],
    constant float4x4& u_modelViewProjection [[buffer(1)]],
    constant float4& u_geomorphUpFactor [[buffer(2)]]) {
    TerrainVertexOut out;
    // geomorph:沿瓦片中心椭球法线把顶点从粗起点(w=0)长到真实高度(w=1)。
    float3 morphPos = in.position +
        u_geomorphUpFactor.xyz * in.heightDelta * (1.0 - u_geomorphUpFactor.w);
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
    // 稀疏虚拟纹理(Step B1):per-tile 间接纹理(RGBA8 编 layer 索引)。用下方
    // 着色器内声明的 NEAREST constexpr sampler 点采样,不占共享 sampler 槽。
    texture2d<float> u_pageStoreIndir [[texture(21)]],
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
    float3 n = normalize(in.normal) * faceSign;
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
        // Step B1(镜像 GLSL):经 per-tile 间接纹理单次 NEAREST fetch 定位层。
        // 着色器内 constexpr 点采样 sampler(clamp),(cell+0.5)/gridN 命中 texel
        // 中心;RGBA8 解码 R+G*256。
        constexpr sampler u_pageStoreIndirSampler(coord::normalized,
                                                  filter::nearest,
                                                  address::clamp_to_edge);
        float2 indirUv = (cell + 0.5) / gridN;
        float4 e = u_pageStoreIndir.sample(u_pageStoreIndirSampler, indirUv);
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
    float directional = smoothstep(0.0, 1.0, NdotL);
    // 暖阳/冷阴影(与 kTerrainFragmentGLSL 一致)。
    float3 sunTint = float3(1.05, 1.0, 0.91);
    float3 color = base.rgb * (0.72 + 0.28 * directional)
                            * mix(float3(1.0), sunTint, directional)
                 + base.rgb * float4(u.ambient).rgb * (1.0 - directional);
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

    // Color (vector)
    std::unique_ptr<ShaderProgram> colorShader;

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
    impl_->colorShader.reset();
    impl_->tileIndexCount = 0;
    impl_->initialized = false;
}

// ---- 共享资源访问 ----

ShaderProgram* Renderer::colorShader() const { return impl_->colorShader.get(); }
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
