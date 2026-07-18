#include "SkyBox.h"
#include <cmath>
#include <cstring>

namespace earth_engine {

// ============================================================
// GLSL ES 3.0 SkyBox Shader
// 对标 openglobus/src/shaders/skybox.ts
// ============================================================

namespace {

// Cubemap skybox shader
const char* kSkyBoxCubemapVert = R"(#version 300 es
precision highp float;

in vec3 a_vertexPosition;
uniform mat4 u_projectionViewMatrix;

out vec3 v_textureCoord;

void main() {
    v_textureCoord = normalize(a_vertexPosition);
    vec4 clipPos = u_projectionViewMatrix * vec4(a_vertexPosition, 1.0);
    // xyww trick: force depth to 1.0 (far plane)
    gl_Position = clipPos.xyww;
}
)";

const char* kSkyBoxCubemapFrag = R"(#version 300 es
precision highp float;

in vec3 v_textureCoord;
uniform samplerCube u_cubemap;

out vec4 fragColor;

void main() {
    fragColor = texture(u_cubemap, v_textureCoord);
}
)";

// Procedural starfield shader
const char* kSkyBoxStarfieldVert = R"(#version 300 es
precision highp float;

in vec3 a_vertexPosition;
uniform mat4 u_projectionViewMatrix;

out vec3 v_textureCoord;

void main() {
    v_textureCoord = normalize(a_vertexPosition);
    vec4 clipPos = u_projectionViewMatrix * vec4(a_vertexPosition, 1.0);
    // xyww trick: force depth to 1.0 (far plane)
    gl_Position = clipPos.xyww;
}
)";

const char* kSkyBoxStarfieldFrag = R"(#version 300 es
precision highp float;

in vec3 v_textureCoord;

uniform float u_time;
uniform float u_nightFactor; // 0=day (no stars), 1=night (full stars)

out vec4 fragColor;

// Simple pseudo-random hash
float hash(vec3 p) {
    float h = dot(p, vec3(127.1, 311.7, 74.7));
    return fract(sin(h) * 43758.5453);
}

// 3D noise for soft painted bands
float noise3D(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    return mix(
        mix(mix(hash(i), hash(i + vec3(1,0,0)), f.x),
            mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
        mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x),
            mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y),
        f.z
    );
}

vec3 starTint(float r) {
    vec3 warm = vec3(1.0, 0.82, 0.48);
    vec3 cool = vec3(0.55, 0.78, 1.0);
    vec3 rose = vec3(1.0, 0.55, 0.78);
    vec3 tint = mix(warm, cool, smoothstep(0.22, 0.72, r));
    return mix(tint, rose, smoothstep(0.82, 0.98, r) * 0.45);
}

vec2 billboardUv(vec3 dir, vec3 localCell) {
    vec3 ad = abs(dir);
    if (ad.x > ad.y && ad.x > ad.z) {
        return localCell.yz;
    }
    if (ad.y > ad.z) {
        return localCell.xz;
    }
    return localCell.xy;
}

vec3 artStarLayer(vec3 dir, float scale, float threshold, float radius, float flareScale) {
    vec3 p = dir * scale;
    vec3 id = floor(p);
    float r = hash(id);
    float exists = smoothstep(threshold, 1.0, r);
    if (exists <= 0.0) {
        return vec3(0.0);
    }

    vec2 uv = billboardUv(dir, fract(p) - 0.5);
    float d = length(uv);
    float diamond = abs(uv.x) + abs(uv.y);

    float core = smoothstep(radius, 0.0, d);
    float halo = smoothstep(radius * 4.2, 0.0, d) * 0.28;
    float diamondGlow = smoothstep(radius * 2.1, 0.0, diamond) * 0.38;
    float horizontal = smoothstep(radius * 7.0 * flareScale, 0.0, abs(uv.x)) *
                       smoothstep(radius * 0.42, 0.0, abs(uv.y));
    float vertical = smoothstep(radius * 7.0 * flareScale, 0.0, abs(uv.y)) *
                     smoothstep(radius * 0.42, 0.0, abs(uv.x));
    float rays = (horizontal + vertical) * 0.34;
    float handDrawn = 0.82 + 0.18 * noise3D(id + dir * 8.0);
    float shape = (core + halo + diamondGlow + rays) * exists * handDrawn;

    return starTint(r) * shape;
}

void main() {
    vec3 dir = normalize(v_textureCoord);

    // Fade stars below horizon slightly
    float horizonFade = smoothstep(-0.05, 0.1, dir.y);

    // Painted galactic ribbon: broader and softer than a photographic Milky Way.
    float galacticLat = abs(dir.y * 0.7 + dir.z * 0.7);
    float milkyWay = smoothstep(0.22, 0.0, galacticLat) * 0.16;
    float ribbonNoise = noise3D(dir * 18.0) * 0.55 + noise3D(dir * 42.0 + 2.0) * 0.25;
    milkyWay *= 0.45 + ribbonNoise;

    vec3 largeStars = artStarLayer(dir, 48.0, 0.982, 0.030, 1.15);
    vec3 mediumStars = artStarLayer(dir, 84.0, 0.989, 0.018, 0.85) * 0.72;
    vec3 accentStars = artStarLayer(dir, 28.0, 0.972, 0.038, 1.35) * 0.55;
    vec3 starColor = (largeStars + mediumStars + accentStars) * horizonFade;

    // Night sky gradient: dark blue at zenith, slightly lighter at horizon
    vec3 nightZenith = vec3(0.006, 0.008, 0.035);
    vec3 nightHorizon = vec3(0.025, 0.022, 0.065);
    float t = smoothstep(-0.1, 0.3, dir.y);
    vec3 nightColor = mix(nightHorizon, nightZenith, t);

    vec3 milkyWayColor = mix(vec3(0.38, 0.42, 0.75), vec3(0.95, 0.56, 0.82), ribbonNoise) * milkyWay;

    vec3 color = nightColor + (starColor + milkyWayColor) * u_nightFactor;

    // Day/night blend: day sky would be from atmosphere pass,
    // so this becomes fully transparent during day
    float alpha = u_nightFactor;

    fragColor = vec4(color, alpha);
}
)";

// Cube vertices for skybox (36 vertices, 6 faces, 2 triangles each)
// Each face: 6 vertices (2 triangles × 3 vertices)
float cubeVertices[] = {
    // Right (+X)
     1, -1, -1,   1, -1,  1,   1,  1,  1,
     1,  1,  1,   1,  1, -1,   1, -1, -1,
    // Left (-X)
    -1, -1,  1,  -1, -1, -1,  -1,  1, -1,
    -1,  1, -1,  -1,  1,  1,  -1, -1,  1,
    // Top (+Y)
    -1,  1, -1,   1,  1, -1,   1,  1,  1,
     1,  1,  1,  -1,  1,  1,  -1,  1, -1,
    // Bottom (-Y)
    -1, -1,  1,   1, -1,  1,   1, -1, -1,
     1, -1, -1,  -1, -1, -1,  -1, -1,  1,
    // Front (+Z)
    -1, -1,  1,   1, -1,  1,   1,  1,  1,
     1,  1,  1,  -1,  1,  1,  -1, -1,  1,
    // Back (-Z)
     1, -1, -1,  -1, -1, -1,  -1,  1, -1,
    -1,  1, -1,   1,  1, -1,   1, -1, -1,
};

constexpr int kCubeVertexCount = 36;

} // anonymous namespace

// ============================================================
// SkyBox
// ============================================================

SkyBox::SkyBox() = default;

SkyBox::~SkyBox() {
    dispose();
}

void SkyBox::setCubemapPaths(const CubemapPaths& paths) {
    cubemapPaths_ = paths;
    useCubemap_ = (paths.positiveX != nullptr || paths.negativeX != nullptr ||
                   paths.positiveY != nullptr || paths.negativeY != nullptr ||
                   paths.positiveZ != nullptr || paths.negativeZ != nullptr);
}

bool SkyBox::initialize(RenderDevice* device) {
    if (!device) return false;

    // Create shader — prefer cubemap if paths provided, else procedural starfield
    ShaderDesc shaderDesc;
    if (useCubemap_) {
        shaderDesc.vertexSource = kSkyBoxCubemapVert;
        shaderDesc.fragmentSource = kSkyBoxCubemapFrag;
    } else {
        shaderDesc.vertexSource = kSkyBoxStarfieldVert;
        shaderDesc.fragmentSource = kSkyBoxStarfieldFrag;
    }
    shader_ = device->createShader(shaderDesc);
    if (!shader_) return false;

    // Load cubemap texture if paths provided
    if (useCubemap_) {
        // For now, cubemap loading is platform-specific.
        // The RenderDevice should support cubemap creation.
        // We'll set the texture to nullptr and let the platform backend
        // handle loading via the cubemap paths.
        cubemapTexture_ = nullptr;
    }

    // Scale vertices by size
    float scaledVertices[kCubeVertexCount * 3];
    for (int i = 0; i < kCubeVertexCount * 3; ++i) {
        scaledVertices[i] = cubeVertices[i] * size_;
    }

    // Create vertex buffer
    BufferDesc bufferDesc;
    bufferDesc.size = sizeof(scaledVertices);
    bufferDesc.data = scaledVertices;
    bufferDesc.usage = BufferDesc::Usage::Static;
    bufferDesc.type = BufferDesc::Type::Vertex;
    vertexBuffer_ = device->createBuffer(bufferDesc);
    vertexCount_ = kCubeVertexCount;

    return vertexBuffer_ != nullptr;
}

RenderCommand SkyBox::buildCommand(
    const float* viewMatrix,
    const float* projMatrix,
    bool isOrthographic,
    float nightFactor) const {

    RenderCommand cmd;
    cmd.kind = RenderCommandKind::SkyBackground;
    cmd.owner = "skybox";
    cmd.pass = "color";
    cmd.shader = shader_.get();
    cmd.vertexBuffer = vertexBuffer_.get();
    cmd.indexBuffer = nullptr;
    cmd.vertexCount = vertexCount_;
    cmd.vertexStride = 3 * sizeof(float);
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.depthTest = false;
    cmd.depthWrite = false;
    cmd.blend = true;
    cmd.blendSrc = RenderCommand::BlendFactor::SrcAlpha;
    cmd.blendDst = RenderCommand::BlendFactorDst::OneMinusSrcAlpha;
    cmd.cullFace = false;

    // For skybox, we only want the rotational part of the view matrix
    // Set translation to zero by using only the upper-left 3x3
    float viewRotOnly[16];
    std::memcpy(viewRotOnly, viewMatrix, 16 * sizeof(float));
    viewRotOnly[12] = 0.0f;  // tx
    viewRotOnly[13] = 0.0f;  // ty
    viewRotOnly[14] = 0.0f;  // tz

    // Compute projectionViewMatrix = proj * viewRotOnly
    // (Column-major: result = proj * viewRot)
    float projView[16];
    // Matrix multiplication: projView = proj * viewRot
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += projMatrix[row + k * 4] * viewRotOnly[k + col * 4];
            }
            projView[row + col * 4] = sum;
        }
    }

    cmd.uniforms["u_projectionViewMatrix"] =
        std::vector<float>(projView, projView + 16);

    // Cubemap texture (if using cubemap mode)
    if (useCubemap_ && cubemapTexture_) {
        cmd.textures.push_back(cubemapTexture_);
    }

    // For procedural starfield, set night factor (0=day, 1=night)
    cmd.uniforms["u_nightFactor"] = {nightFactor};
    cmd.uniforms["u_time"] = {0.0f};

    return cmd;
}

void SkyBox::dispose() {
    shader_.reset();
    vertexBuffer_.reset();
    cubemapTexture_ = nullptr;
}

} // namespace earth_engine
