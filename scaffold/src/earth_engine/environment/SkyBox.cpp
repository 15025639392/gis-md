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

// 3D noise for star distribution
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

void main() {
    vec3 dir = normalize(v_textureCoord);

    // Starfield: randomly distributed bright points
    // Use multiple octaves of hash to create stars of varying brightness
    float starField = 0.0;

    // Bright stars (sparse)
    float star1 = hash(floor(dir * 200.0));
    star1 = smoothstep(0.998, 1.0, star1);

    // Medium stars
    float star2 = hash(floor(dir * 100.0 + 0.5));
    star2 = smoothstep(0.997, 1.0, star2) * 0.6;

    // Dim stars (dense)
    float star3 = hash(floor(dir * 50.0 + 1.0));
    star3 = smoothstep(0.996, 1.0, star3) * 0.3;

    starField = star1 + star2 + star3;

    // Fade stars below horizon slightly
    float horizonFade = smoothstep(-0.05, 0.1, dir.y);

    // Milky way band (galactic plane approximation)
    float galacticLat = abs(dir.y * 0.7 + dir.z * 0.7);
    float milkyWay = smoothstep(0.15, 0.0, galacticLat) * 0.15;
    milkyWay *= noise3D(dir * 40.0) * 0.5 + 0.5;

    // Night sky gradient: dark blue at zenith, slightly lighter at horizon
    vec3 nightZenith = vec3(0.01, 0.01, 0.04);
    vec3 nightHorizon = vec3(0.02, 0.02, 0.06);
    float t = smoothstep(-0.1, 0.3, dir.y);
    vec3 nightColor = mix(nightHorizon, nightZenith, t);

    // Combine stars with night sky
    vec3 starColor = vec3(1.0, 0.95, 0.8) * starField;
    vec3 milkyWayColor = vec3(0.7, 0.75, 0.9) * milkyWay;

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
    shader_ = device->createShader(shaderDesc).release();
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
    vertexBuffer_ = device->createBuffer(bufferDesc).release();
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
    cmd.shader = shader_;
    cmd.vertexBuffer = vertexBuffer_;
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
    shader_ = nullptr;
    vertexBuffer_ = nullptr;
    cubemapTexture_ = nullptr;
}

} // namespace earth_engine
