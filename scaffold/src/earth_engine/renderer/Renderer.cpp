#include "Renderer.h"
#include "../globe/Globe.h"
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
// Globe Shader — GLSL ES 3.0
// ============================================================

static const char* kGlobeVertexGLSL = R"glsl(
#version 300 es
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texcoord;

uniform mat4 u_modelViewProjection;
uniform mat4 u_model;

out vec3 v_normal;
out vec2 v_texcoord;

void main() {
    v_normal = normalize(mat3(u_model) * a_normal);
    v_texcoord = a_texcoord;
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
}
)glsl";

static const char* kGlobeFragmentGLSL = R"glsl(
#version 300 es
precision mediump float;

in vec3 v_normal;
in vec2 v_texcoord;

uniform vec3 u_lightDir;

out vec4 fragColor;

void main() {
    vec3 n = normalize(v_normal);
    float diffuse = max(dot(n, normalize(u_lightDir)), 0.0);
    vec3 ocean = vec3(0.05, 0.26, 0.58);
    vec3 land = vec3(0.18, 0.48, 0.24);
    float band = smoothstep(0.42, 0.58,
        sin(v_texcoord.x * 37.0) * 0.5 + sin(v_texcoord.y * 23.0) * 0.5 + 0.5);
    vec3 base = mix(ocean, land, band * 0.45);
    vec3 color = base * (0.22 + diffuse * 0.88);
    fragColor = vec4(color, 1.0);
}
)glsl";

// ============================================================
// Unified SurfaceTile Shader — cesium-native glTF vertex layout
// POSITION(vec3) + NORMAL(vec3) + TEXCOORD_0(vec2) = 32 bytes
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
uniform vec4 u_baseColor;
uniform sampler2D u_baseColorTexture;
uniform sampler2D u_metallicRoughnessTexture;
uniform sampler2D u_normalTexture;
uniform sampler2D u_occlusionTexture;
uniform sampler2D u_emissiveTexture;
uniform sampler2D u_anisotropyTexture;
uniform sampler2D u_specularTexture;
uniform sampler2D u_specularColorTexture;
uniform sampler2D u_specularGlossinessTexture;
uniform sampler2D u_transmissionTexture;
uniform sampler2D u_clearcoatTexture;
uniform sampler2D u_clearcoatRoughnessTexture;
uniform sampler2D u_clearcoatNormalTexture;
uniform sampler2D u_sheenColorTexture;
uniform sampler2D u_sheenRoughnessTexture;
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

static const char* kGlobeVertexMSL = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 texcoord [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float2 texcoord;
};

vertex VertexOut globeVertex(VertexIn in [[stage_in]],
                             constant float4x4& u_modelViewProjection [[buffer(1)]],
                             constant float4x4& u_model [[buffer(2)]]) {
    VertexOut out;
    out.position = u_modelViewProjection * float4(in.position, 1.0);
    out.normal = normalize((u_model * float4(in.normal, 0.0)).xyz);
    out.texcoord = in.texcoord;
    return out;
}
)msl";

static const char* kGlobeFragmentMSL = R"msl(
#include <metal_stdlib>
using namespace metal;
fragment float4 globeFragment(VertexOut in [[stage_in]],
                              constant float3& u_lightDir [[buffer(0)]]) {
    float3 n = normalize(in.normal);
    float diffuse = max(dot(n, normalize(u_lightDir)), 0.0f);
    float3 ocean = float3(0.05, 0.26, 0.58);
    float3 land = float3(0.18, 0.48, 0.24);
    float band = smoothstep(0.42, 0.58,
        sin(in.texcoord.x * 37.0) * 0.5 + sin(in.texcoord.y * 23.0) * 0.5 + 0.5);
    float3 base = mix(ocean, land, band * 0.45);
    float3 color = base * (0.22 + diffuse * 0.88);
    return float4(color, 1.0);
}
)msl";

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
    out.normal = normalize(in.normal);
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

float2 gltfTransformUv(float2 uv, float4 offsetScale, float2 sinCos) {
    float2 scaled = uv * offsetScale.zw;
    return float2(
        scaled.x * sinCos.y + scaled.y * sinCos.x,
        scaled.y * sinCos.y - scaled.x * sinCos.x) + offsetScale.xy;
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
                             constant float3& u_lightDir [[buffer(0)]],
                             constant float4& u_baseColor [[buffer(5)]],
                             constant float& u_renderOpacity [[buffer(6)]],
                             constant float& u_hasBaseColorTexture [[buffer(7)]],
                             constant float& u_alphaMode [[buffer(8)]],
                             constant float& u_alphaCutoff [[buffer(9)]],
                             constant float4& u_materialFactors [[buffer(10)]],
                             constant float4& u_hasMaterialTextures [[buffer(11)]],
                             constant float3& u_emissiveFactor [[buffer(12)]],
                             constant float4& u_baseColorTexOffsetScale [[buffer(13)]],
                             constant float2& u_baseColorTexRotationSinCos [[buffer(14)]],
                             constant float4& u_metallicRoughnessTexOffsetScale [[buffer(15)]],
                             constant float2& u_metallicRoughnessTexRotationSinCos [[buffer(16)]],
                             constant float4& u_normalTexOffsetScale [[buffer(17)]],
                             constant float2& u_normalTexRotationSinCos [[buffer(18)]],
                             constant float4& u_occlusionTexOffsetScale [[buffer(19)]],
                             constant float2& u_occlusionTexRotationSinCos [[buffer(20)]],
                             constant float4& u_emissiveTexOffsetScale [[buffer(21)]],
                             constant float2& u_emissiveTexRotationSinCos [[buffer(22)]],
                             constant float4& u_textureCoordSets [[buffer(23)]],
                             constant float& u_emissiveTexCoordSet [[buffer(24)]],
                             constant float& u_unlit [[buffer(25)]],
                             constant float& u_dielectricSpecularF0 [[buffer(26)]],
                             constant float2& u_hasSpecularTextures [[buffer(27)]],
                             constant float& u_specularFactor [[buffer(28)]],
                             constant float3& u_specularColorFactor [[buffer(29)]],
                             constant float4& u_specularTexOffsetScale [[buffer(30)]],
                             constant float2& u_specularTexRotationSinCos [[buffer(31)]],
                             constant float4& u_specularColorTexOffsetScale [[buffer(32)]],
                             constant float2& u_specularColorTexRotationSinCos [[buffer(33)]],
                             constant float2& u_specularTexCoordSets [[buffer(34)]],
                             constant float3& u_clearcoatFactors [[buffer(35)]],
                             constant float3& u_hasClearcoatTextures [[buffer(36)]],
                             constant float4& u_clearcoatTexOffsetScale [[buffer(37)]],
                             constant float2& u_clearcoatTexRotationSinCos [[buffer(38)]],
                             constant float4& u_clearcoatRoughnessTexOffsetScale [[buffer(39)]],
                             constant float2& u_clearcoatRoughnessTexRotationSinCos [[buffer(40)]],
                             constant float4& u_clearcoatNormalTexOffsetScale [[buffer(41)]],
                             constant float2& u_clearcoatNormalTexRotationSinCos [[buffer(42)]],
                             constant float3& u_clearcoatTexCoordSets [[buffer(43)]],
                             constant float3& u_sheenColorFactor [[buffer(44)]],
                             constant float& u_sheenRoughnessFactor [[buffer(45)]],
                             constant float2& u_hasSheenTextures [[buffer(46)]],
                             constant float4& u_sheenColorTexOffsetScale [[buffer(47)]],
                             constant float2& u_sheenColorTexRotationSinCos [[buffer(48)]],
                             constant float4& u_sheenRoughnessTexOffsetScale [[buffer(49)]],
                             constant float2& u_sheenRoughnessTexRotationSinCos [[buffer(50)]],
                             constant float2& u_sheenTexCoordSets [[buffer(51)]],
                             constant float2& u_anisotropyFactors [[buffer(52)]],
                             constant float& u_hasAnisotropyTexture [[buffer(53)]],
                             constant float4& u_anisotropyTexOffsetScale [[buffer(54)]],
                             constant float2& u_anisotropyTexRotationSinCos [[buffer(55)]],
                             constant float& u_anisotropyTexCoordSet [[buffer(56)]],
                             constant float& u_specularGlossinessWorkflow [[buffer(57)]],
                             constant float4& u_specularGlossinessFactor [[buffer(58)]],
                             constant float& u_hasSpecularGlossinessTexture [[buffer(59)]],
                             constant float4& u_specularGlossinessTexOffsetScale [[buffer(60)]],
                             constant float2& u_specularGlossinessTexRotationSinCos [[buffer(61)]],
                             constant float& u_specularGlossinessTexCoordSet [[buffer(62)]],
                             constant float& u_transmissionFactor [[buffer(63)]],
                             constant float& u_hasTransmissionTexture [[buffer(64)]],
                             constant float4& u_transmissionTexOffsetScale [[buffer(65)]],
                             constant float2& u_transmissionTexRotationSinCos [[buffer(66)]],
                             constant float& u_transmissionTexCoordSet [[buffer(67)]],
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
                             sampler u_transmissionSampler [[sampler(14)]]) {
    float faceSign = frontFacing ? 1.0 : -1.0;
    float3 n = normalize(in.normal) * faceSign;
    float3 geometryN = n;
    float3 light = normalize(u_lightDir);
    float2 baseColorUv = gltfTransformUv(
        gltfUvFromSet(in, u_textureCoordSets.x),
        u_baseColorTexOffsetScale,
        u_baseColorTexRotationSinCos);
    float4 base = u_baseColor * in.color;
    if (u_hasBaseColorTexture > 0.5) {
        base *= u_baseColorTexture.sample(u_baseColorSampler, baseColorUv);
    }
    if (u_alphaMode > 0.5 && u_alphaMode < 1.5 && base.a < u_alphaCutoff) {
        discard_fragment();
    }
    float alpha = u_alphaMode > 1.5 ? base.a : 1.0;
    if (u_unlit > 0.5) {
        return float4(base.rgb, alpha * clamp(u_renderOpacity, 0.0, 1.0));
    }
    float metallic = clamp(u_materialFactors.x, 0.0, 1.0);
    float roughness = clamp(u_materialFactors.y, 0.04, 1.0);
    if (u_hasMaterialTextures.x > 0.5) {
        float2 mrUv = gltfTransformUv(
            gltfUvFromSet(in, u_textureCoordSets.y),
            u_metallicRoughnessTexOffsetScale,
            u_metallicRoughnessTexRotationSinCos);
        float4 mr = u_metallicRoughnessTexture.sample(
            u_metallicRoughnessSampler,
            mrUv);
        roughness = clamp(roughness * mr.g, 0.04, 1.0);
        metallic = clamp(metallic * mr.b, 0.0, 1.0);
    }
    float ndotl = max(dot(n, light), 0.0);
    if (u_hasMaterialTextures.y > 0.5) {
        float2 normalUv = gltfTransformUv(
            gltfUvFromSet(in, u_textureCoordSets.z),
            u_normalTexOffsetScale,
            u_normalTexRotationSinCos);
        n = gltfPerturbNormal(
            n,
            normalUv,
            in.localPosition,
            in.tangent,
            u_materialFactors.z,
            u_normalTexture,
            u_normalSampler);
        ndotl = max(dot(n, light), 0.0);
    }
    float occlusion = 1.0;
    if (u_hasMaterialTextures.z > 0.5) {
        float2 occlusionUv = gltfTransformUv(
            gltfUvFromSet(in, u_textureCoordSets.w),
            u_occlusionTexOffsetScale,
            u_occlusionTexRotationSinCos);
        float ao = u_occlusionTexture.sample(
            u_occlusionSampler,
            occlusionUv).r;
        occlusion = clamp(1.0 + u_materialFactors.w * (ao - 1.0), 0.0, 1.0);
    }
    float3 emissive = u_emissiveFactor;
    if (u_hasMaterialTextures.w > 0.5) {
        float2 emissiveUv = gltfTransformUv(
            gltfUvFromSet(in, u_emissiveTexCoordSet),
            u_emissiveTexOffsetScale,
            u_emissiveTexRotationSinCos);
        emissive *= u_emissiveTexture.sample(
            u_emissiveSampler,
            emissiveUv).rgb;
    }
    float3 sheenColor = max(u_sheenColorFactor, float3(0.0));
    float sheenRoughness = clamp(u_sheenRoughnessFactor, 0.0, 1.0);
    if (u_hasSheenTextures.x > 0.5) {
        float2 sheenColorUv = gltfTransformUv(
            gltfUvFromSet(in, u_sheenTexCoordSets.x),
            u_sheenColorTexOffsetScale,
            u_sheenColorTexRotationSinCos);
        sheenColor *= u_sheenColorTexture.sample(
            u_sheenColorSampler,
            sheenColorUv).rgb;
    }
    if (u_hasSheenTextures.y > 0.5) {
        float2 sheenRoughnessUv = gltfTransformUv(
            gltfUvFromSet(in, u_sheenTexCoordSets.y),
            u_sheenRoughnessTexOffsetScale,
            u_sheenRoughnessTexRotationSinCos);
        sheenRoughness = clamp(
            sheenRoughness *
                u_sheenRoughnessTexture.sample(
                    u_sheenRoughnessSampler,
                    sheenRoughnessUv).a,
            0.0,
            1.0);
    }
    float clearcoat = clamp(u_clearcoatFactors.x, 0.0, 1.0);
    float clearcoatRoughness = clamp(u_clearcoatFactors.y, 0.0, 1.0);
    if (u_hasClearcoatTextures.x > 0.5) {
        float2 clearcoatUv = gltfTransformUv(
            gltfUvFromSet(in, u_clearcoatTexCoordSets.x),
            u_clearcoatTexOffsetScale,
            u_clearcoatTexRotationSinCos);
        clearcoat *= u_clearcoatTexture.sample(
            u_clearcoatSampler,
            clearcoatUv).r;
    }
    if (u_hasClearcoatTextures.y > 0.5) {
        float2 clearcoatRoughnessUv = gltfTransformUv(
            gltfUvFromSet(in, u_clearcoatTexCoordSets.y),
            u_clearcoatRoughnessTexOffsetScale,
            u_clearcoatRoughnessTexRotationSinCos);
        clearcoatRoughness = clamp(
            clearcoatRoughness *
                u_clearcoatRoughnessTexture.sample(
                    u_clearcoatRoughnessSampler,
                    clearcoatRoughnessUv).g,
            0.0,
            1.0);
    }
    float3 clearcoatNormal = geometryN;
    if (u_hasClearcoatTextures.z > 0.5) {
        float2 clearcoatNormalUv = gltfTransformUv(
            gltfUvFromSet(in, u_clearcoatTexCoordSets.z),
            u_clearcoatNormalTexOffsetScale,
            u_clearcoatNormalTexRotationSinCos);
        clearcoatNormal = gltfPerturbNormal(
            geometryN,
            clearcoatNormalUv,
            in.localPosition,
            in.tangent,
            u_clearcoatFactors.z,
            u_clearcoatNormalTexture,
            u_clearcoatNormalSampler);
    }
    float3 specGlossSpecularColor = float3(0.0);
    float specGlossMaxSpecular = 0.0;
    if (u_specularGlossinessWorkflow > 0.5) {
        float4 specGloss = float4(
            clamp(u_specularGlossinessFactor.rgb, 0.0, 1.0),
            clamp(u_specularGlossinessFactor.a, 0.0, 1.0));
        if (u_hasSpecularGlossinessTexture > 0.5) {
            float2 sgUv = gltfTransformUv(
                gltfUvFromSet(in, u_specularGlossinessTexCoordSet),
                u_specularGlossinessTexOffsetScale,
                u_specularGlossinessTexRotationSinCos);
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
    float transmission = clamp(u_transmissionFactor, 0.0, 1.0);
    if (transmission > 0.0 && u_hasTransmissionTexture > 0.5) {
        float2 transmissionUv = gltfTransformUv(
            gltfUvFromSet(in, u_transmissionTexCoordSet),
            u_transmissionTexOffsetScale,
            u_transmissionTexRotationSinCos);
        transmission *= u_transmissionTexture.sample(
            u_transmissionSampler,
            transmissionUv).r;
    }
    transmission = clamp(transmission, 0.0, 1.0);
    float anisotropyStrength = clamp(u_anisotropyFactors.x, 0.0, 1.0);
    float2 anisotropyDirection = float2(1.0, 0.0);
    GltfAnisotropyBasis anisotropyBasis;
    anisotropyBasis.tangent = float3(1.0, 0.0, 0.0);
    anisotropyBasis.bitangent = float3(0.0, 1.0, 0.0);
    anisotropyBasis.valid = false;
    if (anisotropyStrength > 0.0) {
        float2 anisotropyUv = gltfTransformUv(
            gltfUvFromSet(in, u_anisotropyTexCoordSet),
            u_anisotropyTexOffsetScale,
            u_anisotropyTexRotationSinCos);
        if (u_hasAnisotropyTexture > 0.5) {
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
                gltfRotateDirection(anisotropyDirection, u_anisotropyFactors.y));
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
    float specularStrength = clamp(u_specularFactor, 0.0, 1.0);
    if (u_hasSpecularTextures.x > 0.5) {
        float2 specularUv = gltfTransformUv(
            gltfUvFromSet(in, u_specularTexCoordSets.x),
            u_specularTexOffsetScale,
            u_specularTexRotationSinCos);
        specularStrength *= u_specularTexture.sample(
            u_specularSampler,
            specularUv).a;
    }
    float3 specularColor;
    float3 diffuseColor;
    if (u_specularGlossinessWorkflow > 0.5) {
        specularColor = specGlossSpecularColor;
        diffuseColor = base.rgb * (1.0 - specGlossMaxSpecular);
    } else {
        float3 dielectricSpecular =
            float3(clamp(u_dielectricSpecularF0, 0.0, 1.0)) *
            max(u_specularColorFactor, float3(0.0));
        if (u_hasSpecularTextures.y > 0.5) {
            float2 specularColorUv = gltfTransformUv(
                gltfUvFromSet(in, u_specularTexCoordSets.y),
                u_specularColorTexOffsetScale,
                u_specularColorTexRotationSinCos);
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
    return float4(color, alpha * clamp(u_renderOpacity, 0.0, 1.0));
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

} // namespace renderer_testing

// ============================================================
// Renderer::Impl
// ============================================================

struct Renderer::Impl {
    RenderDevice* device = nullptr;

    // Globe
    std::unique_ptr<ShaderProgram> globeShader;
    std::unique_ptr<Buffer> globeVertexBuffer;
    std::unique_ptr<Buffer> globeIndexBuffer;
    int globeIndexCount = 0;

    // Surface tile (unified, cesium-native glTF layout)
    std::unique_ptr<ShaderProgram> surfaceTileShader;
    std::unique_ptr<Buffer> tileIndexBuffer;  // shared 64×64 grid IBO
    std::unique_ptr<Texture> surfaceFallbackTexture;
    int tileIndexCount = 0;

    // glTF TileRenderContent
    std::unique_ptr<ShaderProgram> gltfShader;
    std::unique_ptr<ShaderProgram> gltfInstancedShader;

    // Color (vector)
    std::unique_ptr<ShaderProgram> colorShader;

    // cesium-native: retained raster attachments (IPrepareRendererResources)
    // Keyed by geometry tile cache key (schemeId/z/x/y).
    std::unordered_map<std::string, RasterAttachment> rasterAttachments;

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

bool Renderer::initialize(const GlobeMesh& mesh) {
    if (impl_->initialized) dispose();
    auto* dev = impl_->device;
    if (!dev) return false;

    bool isMetal = (dev->backendType() == RenderDevice::Backend::Metal);

    // ---- Globe shader ----
    ShaderDesc globeSd;
    globeSd.vertexSource = isMetal ? kGlobeVertexMSL : kGlobeVertexGLSL;
    globeSd.fragmentSource = isMetal ? kGlobeFragmentMSL : kGlobeFragmentGLSL;
    impl_->globeShader = dev->createShader(globeSd);
    if (!impl_->globeShader) { fprintf(stderr, "[Renderer] globeShader failed\n"); return false; }

    // Globe vertex buffer
    BufferDesc vbDesc;
    vbDesc.size = mesh.vertices.size() * sizeof(GlobeVertex);
    vbDesc.data = mesh.vertices.data();
    vbDesc.usage = BufferDesc::Usage::Static;
    vbDesc.type = BufferDesc::Type::Vertex;
    impl_->globeVertexBuffer = dev->createBuffer(vbDesc);
    if (!impl_->globeVertexBuffer) return false;

    // Globe index buffer
    BufferDesc ibDesc;
    ibDesc.size = mesh.indices.size() * sizeof(uint32_t);
    ibDesc.data = mesh.indices.data();
    ibDesc.usage = BufferDesc::Usage::Static;
    ibDesc.type = BufferDesc::Type::Index;
    impl_->globeIndexBuffer = dev->createBuffer(ibDesc);
    if (!impl_->globeIndexBuffer) return false;
    impl_->globeIndexCount = static_cast<int>(mesh.indices.size());

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

    // ---- glTF primitive shader ----
    ShaderDesc gltfSd;
    gltfSd.vertexSource = isMetal ? kGltfVertexMSL : kGltfVertexGLSL;
    gltfSd.fragmentSource = isMetal ? kGltfFragmentMSL : kGltfFragmentGLSL;
    impl_->gltfShader = dev->createShader(gltfSd);
    if (!impl_->gltfShader) {
        fprintf(stderr, "[Renderer] gltfShader failed\n");
        return false;
    }

    ShaderDesc gltfInstancedSd;
    gltfInstancedSd.vertexSource =
        isMetal ? kGltfInstancedVertexMSL : kGltfInstancedVertexGLSL;
    gltfInstancedSd.fragmentSource =
        isMetal ? kGltfFragmentMSL : kGltfFragmentGLSL;
    impl_->gltfInstancedShader = dev->createShader(gltfInstancedSd);
    if (!impl_->gltfInstancedShader) {
        fprintf(stderr, "[Renderer] gltfInstancedShader failed\n");
        return false;
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

    const uint8_t fallbackPixel[4] = {118, 132, 136, 255};
    TextureDesc fallbackDesc;
    fallbackDesc.width = 1;
    fallbackDesc.height = 1;
    fallbackDesc.format = TextureDesc::Format::RGBA8;
    fallbackDesc.data = fallbackPixel;
    fallbackDesc.dataSize = sizeof(fallbackPixel);
    fallbackDesc.mipmap = false;
    fallbackDesc.minFilter = TextureDesc::Filter::Linear;
    fallbackDesc.magFilter = TextureDesc::Filter::Linear;
    fallbackDesc.wrapS = TextureDesc::Wrap::Clamp;
    fallbackDesc.wrapT = TextureDesc::Wrap::Clamp;
    impl_->surfaceFallbackTexture = dev->createTexture(fallbackDesc);

    // ---- Color shader (vector layers) ----
    ShaderDesc colorSd;
    colorSd.vertexSource = isMetal ? kColorVertexMSL : kColorVertexGLSL;
    colorSd.fragmentSource = isMetal ? kColorFragmentMSL : kColorFragmentGLSL;
    impl_->colorShader = dev->createShader(colorSd);
    // colorShader failure is non-fatal (vector layers won't render but globe still works)

    impl_->initialized = true;
    return true;
}

void Renderer::submit(const RenderCommandList& commands) {
    if (!impl_->initialized || !impl_->device) return;
    impl_->device->submit(commands);
}

void Renderer::dispose() {
    impl_->globeShader.reset();
    impl_->globeVertexBuffer.reset();
    impl_->globeIndexBuffer.reset();
    impl_->surfaceTileShader.reset();
    impl_->tileIndexBuffer.reset();
    impl_->surfaceFallbackTexture.reset();
    impl_->gltfShader.reset();
    impl_->gltfInstancedShader.reset();
    impl_->colorShader.reset();
    impl_->globeIndexCount = 0;
    impl_->tileIndexCount = 0;
    impl_->initialized = false;
}

// ---- 共享资源访问 ----

ShaderProgram* Renderer::globeShader() const { return impl_->globeShader.get(); }
Buffer* Renderer::globeVertexBuffer() const { return impl_->globeVertexBuffer.get(); }
Buffer* Renderer::globeIndexBuffer() const { return impl_->globeIndexBuffer.get(); }
int Renderer::globeIndexCount() const { return impl_->globeIndexCount; }

ShaderProgram* Renderer::colorShader() const { return impl_->colorShader.get(); }
Buffer* Renderer::tileIndexBuffer() const { return impl_->tileIndexBuffer.get(); }
int Renderer::tileIndexCount() const { return impl_->tileIndexCount; }
Texture* Renderer::surfaceFallbackTexture() const {
    return impl_->surfaceFallbackTexture.get();
}
ShaderProgram* Renderer::gltfShader() const { return impl_->gltfShader.get(); }

ShaderProgram* Renderer::gltfInstancedShader() const {
    return impl_->gltfInstancedShader.get();
}

// ---- Command builders ----

RenderCommand Renderer::makeGlobeCommand(const FrameState& frameState) const {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::GlobeSurface;
    cmd.owner = "globe";
    cmd.pass = "color";
    cmd.shader = impl_->globeShader.get();
    cmd.vertexBuffer = impl_->globeVertexBuffer.get();
    cmd.indexBuffer = impl_->globeIndexBuffer.get();
    cmd.indexCount = impl_->globeIndexCount;
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    cmd.depthTest = true;
    cmd.depthWrite = true;
    cmd.blend = false;
    cmd.cullFace = true;

    if (frameState.camera) {
        const Camera& cam = *frameState.camera;
        float vpW = static_cast<float>(frameState.viewportWidthPixels);
        float vpH = static_cast<float>(frameState.viewportHeightPixels);

        glm::mat4 model = glm::make_mat4(earthModelMatrix().data());
        glm::mat4 view(cam.viewMatrix().raw());
        glm::mat4 proj(cam.projectionMatrix(vpW, vpH).raw());
        glm::mat4 mvp = proj * view * model;

        auto& mvpU = cmd.uniforms["u_modelViewProjection"];
        mvpU.resize(16);
        std::memcpy(mvpU.data(), glm::value_ptr(mvp), 16 * sizeof(float));

        auto& modelU = cmd.uniforms["u_model"];
        modelU.resize(16);
        std::memcpy(modelU.data(), glm::value_ptr(model), 16 * sizeof(float));
    }

    cmd.uniforms["u_lightDir"] = {
        frameState.lightDir.x,
        frameState.lightDir.y,
        frameState.lightDir.z
    };
    return cmd;
}

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
    cmd.uniforms["u_baseColor"] = {0.82f, 0.84f, 0.88f, 1.0f};
    cmd.uniforms["u_hasBaseColorTexture"] = {0.0f};
    cmd.uniforms["u_materialFactors"] = {1.0f, 1.0f, 1.0f, 1.0f};
    cmd.uniforms["u_dielectricSpecularF0"] = {0.04f};
    cmd.uniforms["u_hasMaterialTextures"] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd.uniforms["u_anisotropyFactors"] = {0.0f, 0.0f};
    cmd.uniforms["u_hasAnisotropyTexture"] = {0.0f};
    cmd.uniforms["u_hasSpecularTextures"] = {0.0f, 0.0f};
    cmd.uniforms["u_specularFactor"] = {1.0f};
    cmd.uniforms["u_specularColorFactor"] = {1.0f, 1.0f, 1.0f};
    cmd.uniforms["u_specularGlossinessWorkflow"] = {0.0f};
    cmd.uniforms["u_specularGlossinessFactor"] = {
        1.0f,
        1.0f,
        1.0f,
        1.0f};
    cmd.uniforms["u_hasSpecularGlossinessTexture"] = {0.0f};
    cmd.uniforms["u_transmissionFactor"] = {0.0f};
    cmd.uniforms["u_hasTransmissionTexture"] = {0.0f};
    cmd.uniforms["u_clearcoatFactors"] = {0.0f, 0.0f, 1.0f};
    cmd.uniforms["u_hasClearcoatTextures"] = {0.0f, 0.0f, 0.0f};
    cmd.uniforms["u_sheenColorFactor"] = {0.0f, 0.0f, 0.0f};
    cmd.uniforms["u_sheenRoughnessFactor"] = {0.0f};
    cmd.uniforms["u_hasSheenTextures"] = {0.0f, 0.0f};
    cmd.uniforms["u_emissiveFactor"] = {0.0f, 0.0f, 0.0f};
    cmd.uniforms["u_textureCoordSets"] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd.uniforms["u_emissiveTexCoordSet"] = {0.0f};
    cmd.uniforms["u_anisotropyTexCoordSet"] = {0.0f};
    cmd.uniforms["u_specularTexCoordSets"] = {0.0f, 0.0f};
    cmd.uniforms["u_specularGlossinessTexCoordSet"] = {0.0f};
    cmd.uniforms["u_transmissionTexCoordSet"] = {0.0f};
    cmd.uniforms["u_clearcoatTexCoordSets"] = {0.0f, 0.0f, 0.0f};
    cmd.uniforms["u_sheenTexCoordSets"] = {0.0f, 0.0f};
    cmd.uniforms["u_alphaMode"] = {0.0f};
    cmd.uniforms["u_alphaCutoff"] = {0.5f};
    cmd.uniforms["u_renderOpacity"] = {1.0f};
    cmd.uniforms["u_unlit"] = {0.0f};
    auto setTextureTransformDefaults = [&cmd](
        const char* offsetScaleName,
        const char* rotationName) {
        cmd.uniforms[offsetScaleName] = {0.0f, 0.0f, 1.0f, 1.0f};
        cmd.uniforms[rotationName] = {0.0f, 1.0f};
    };
    setTextureTransformDefaults(
        "u_baseColorTexOffsetScale",
        "u_baseColorTexRotationSinCos");
    setTextureTransformDefaults(
        "u_metallicRoughnessTexOffsetScale",
        "u_metallicRoughnessTexRotationSinCos");
    setTextureTransformDefaults(
        "u_anisotropyTexOffsetScale",
        "u_anisotropyTexRotationSinCos");
    setTextureTransformDefaults(
        "u_specularTexOffsetScale",
        "u_specularTexRotationSinCos");
    setTextureTransformDefaults(
        "u_specularColorTexOffsetScale",
        "u_specularColorTexRotationSinCos");
    setTextureTransformDefaults(
        "u_specularGlossinessTexOffsetScale",
        "u_specularGlossinessTexRotationSinCos");
    setTextureTransformDefaults(
        "u_transmissionTexOffsetScale",
        "u_transmissionTexRotationSinCos");
    setTextureTransformDefaults(
        "u_clearcoatTexOffsetScale",
        "u_clearcoatTexRotationSinCos");
    setTextureTransformDefaults(
        "u_clearcoatRoughnessTexOffsetScale",
        "u_clearcoatRoughnessTexRotationSinCos");
    setTextureTransformDefaults(
        "u_clearcoatNormalTexOffsetScale",
        "u_clearcoatNormalTexRotationSinCos");
    setTextureTransformDefaults(
        "u_sheenColorTexOffsetScale",
        "u_sheenColorTexRotationSinCos");
    setTextureTransformDefaults(
        "u_sheenRoughnessTexOffsetScale",
        "u_sheenRoughnessTexRotationSinCos");
    setTextureTransformDefaults(
        "u_normalTexOffsetScale",
        "u_normalTexRotationSinCos");
    setTextureTransformDefaults(
        "u_occlusionTexOffsetScale",
        "u_occlusionTexRotationSinCos");
    setTextureTransformDefaults(
        "u_emissiveTexOffsetScale",
        "u_emissiveTexRotationSinCos");
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

// ── cesium-native IPrepareRendererResources retained attachment ──

std::string Renderer::attachmentKey(const TileKey& key) {
    return key.schemeId + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

static std::string attachmentKeyWithOverlay(const TileKey& key, int32_t overlayIndex) {
    return key.schemeId + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y) +
           "/o" + std::to_string(overlayIndex);
}

void Renderer::attachRasterInMainThread(
    const TileKey& geometryKey,
    int32_t overlayIndex,
    std::shared_ptr<const RasterOverlayTile> rasterTile,
    Texture* texture,
    float translationU, float translationV,
    float scaleU, float scaleV) {
    std::string key = attachmentKeyWithOverlay(geometryKey, overlayIndex);
    impl_->rasterAttachments[key] = RasterAttachment{
        std::move(rasterTile), texture, translationU, translationV, scaleU, scaleV
    };
}

void Renderer::detachRasterInMainThread(
    const TileKey& geometryKey,
    int32_t overlayIndex) noexcept {
    std::string key = attachmentKeyWithOverlay(geometryKey, overlayIndex);
    impl_->rasterAttachments.erase(key);
}

const RasterAttachment* Renderer::getAttachedRaster(
    const TileKey& geometryKey, int32_t overlayIndex) const {
    std::string key = attachmentKeyWithOverlay(geometryKey, overlayIndex);
    auto it = impl_->rasterAttachments.find(key);
    if (it != impl_->rasterAttachments.end()) {
        return &it->second;
    }
    return nullptr;
}

} // namespace earth_engine
