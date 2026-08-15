#pragma once

#include "../tiling/SurfaceTile.h"
#include "../core/math/Mat4.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace earth_engine {

constexpr size_t kGltfMaxTexCoordSets = 8;

enum class GltfAlphaMode {
    Opaque,
    Mask,
    Blend
};

enum class GltfPrimitiveMode {
    Points,
    Lines,
    LineStrip,
    Triangles,
    TriangleStrip,
    TriangleFan
};

enum class GltfTextureFilter {
    Nearest,
    Linear
};

enum class GltfTextureWrap {
    ClampToEdge,
    Repeat,
    MirroredRepeat
};

struct GltfImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> pixels;
};

struct GltfSampler {
    GltfTextureFilter minFilter = GltfTextureFilter::Linear;
    GltfTextureFilter magFilter = GltfTextureFilter::Linear;
    bool mipmap = true;
    GltfTextureWrap wrapS = GltfTextureWrap::Repeat;
    GltfTextureWrap wrapT = GltfTextureWrap::Repeat;
};

struct GltfTexture {
    GltfImage image;
    GltfSampler sampler;
};

struct GltfTextureTransform {
    std::array<float, 2> offset = {0.0f, 0.0f};
    std::array<float, 2> scale = {1.0f, 1.0f};
    float rotation = 0.0f;
};

struct GltfTextureBinding {
    size_t textureIndex = 0;
    int texCoord = 0;
    GltfTextureTransform transform;
};

using GltfFeaturePropertyValue =
    std::variant<std::monostate, bool, int64_t, uint64_t, double, std::string>;

struct GltfInstance {
    Mat4 transform = Mat4::identity();
    uint32_t featureId = std::numeric_limits<uint32_t>::max();
    std::map<std::string, GltfFeaturePropertyValue> featureProperties;
};

struct GltfVertexSkinning {
    std::array<uint32_t, 4> joints = {0, 0, 0, 0};
    std::array<double, 4> weights = {0.0, 0.0, 0.0, 0.0};
};

struct GltfMorphTarget {
    std::vector<Vec3> positionDeltas;
    std::vector<Vec3> normalDeltas;
    std::vector<Vec3> tangentDeltas;
};

struct GltfInstanceRuntimeTransform {
    // Transform outside the glTF node hierarchy, such as an I3DM instance.
    Mat4 outerTransform = Mat4::identity();
    // EXT_mesh_gpu_instancing transform in the owning node's local space.
    Mat4 nodeLocalTransform = Mat4::identity();
};

struct GltfPrimitiveRuntime {
    int nodeIndex = -1;
    int skinIndex = -1;
    std::vector<SurfaceVertex> baseVertices;
    std::vector<std::array<float, 4>> baseTangents;
    std::vector<GltfVertexSkinning> skinning;
    std::vector<GltfMorphTarget> morphTargets;
    std::vector<GltfInstanceRuntimeTransform> instanceTransforms;
    bool hasNormals = false;
    bool hasTangents = false;
};

struct GltfPrimitive {
    std::vector<SurfaceVertex> vertices;
    // Pre-built TerrainGpuVertex bytes (32-byte compact format). Populated during
    // decode (worker thread) so main-thread GPU upload can create vertex
    // buffers directly without re-building vertices from SurfaceVertex.
    std::vector<uint8_t> terrainGpuVertexBytes;
    std::array<
        std::vector<std::array<float, 2>>,
        kGltfMaxTexCoordSets> vertexTexCoords;
    std::vector<std::array<float, 4>> vertexColors;
    std::vector<std::array<float, 4>> vertexTangents;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> featureIds;
    std::vector<std::map<std::string, GltfFeaturePropertyValue>>
        featureProperties;
    std::vector<GltfInstance> instances;
    GltfPrimitiveRuntime runtime;
    std::optional<SkirtMetadata> skirtMetadata;

    // ── 无几何地形 primitive（摘 glTF 第一级）────────────────────────────────
    // 共享位移模板路下，地形瓦片的网格从不被绘制：draw 侧必换模板 VBO，
    // prepare 侧早已跳过上传，摘幽灵网格之后连常驻也没了。既然如此，**先前
    // 还要把这幅网格造出来再扔掉**（65×65 栅格 + 裙边 + 法线 + ECEF 高低位
    // 拆分 + texcoord 重投影，每瓦片一次，在解码线程上）就是纯浪费。
    //
    // true 时本 primitive **不带任何顶点/索引**，只带下面三项元数据：
    // 顶点数/索引数（供 metadata 与命令构建填数）与排序中心（供深度排序；
    // 正常路径由 primitiveSortCenterEcef 从顶点算，这里没有顶点，改由建造方
    // 按瓦片中心直接给）。
    //
    // ⚠️ 与 `GltfPrimitiveRenderResources::sharedTemplateGeometry` 不是同一层：
    // 那个说的是「GPU buffer 有意为空」，这个说的是「CPU 网格根本没造」。
    // 前者由后者蕴含，反之不然（旧瓦片仍可能造了网格再被跳过上传）。
    bool templateGeometryOnly = false;
    int templateVertexCount = 0;
    int templateIndexCount = 0;
    Vec3 templateSortCenterEcef = Vec3::zero();

    bool hasTerrainWaterMaskMetadata = false;
    bool terrainOnlyWater = false;
    bool terrainOnlyLand = true;
    std::optional<size_t> terrainWaterMaskTextureIndex;
    double terrainWaterMaskTranslationX = 0.0;
    double terrainWaterMaskTranslationY = 0.0;
    double terrainWaterMaskScale = 1.0;
    GltfPrimitiveMode primitiveMode = GltfPrimitiveMode::Triangles;
    std::array<float, 4> baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    std::optional<size_t> baseColorTextureIndex;
    std::optional<GltfTextureBinding> baseColorTexture;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float dielectricSpecularF0 = 0.04f;
    float specularFactor = 1.0f;
    std::array<float, 3> specularColorFactor = {1.0f, 1.0f, 1.0f};
    bool specularGlossinessWorkflow = false;
    std::array<float, 3> specularGlossinessSpecularFactor = {
        1.0f,
        1.0f,
        1.0f};
    float specularGlossinessGlossinessFactor = 1.0f;
    float transmissionFactor = 0.0f;
    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    float clearcoatNormalTextureScale = 1.0f;
    std::array<float, 3> sheenColorFactor = {0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.0f;
    std::optional<GltfTextureBinding> metallicRoughnessTexture;
    std::optional<GltfTextureBinding> anisotropyTexture;
    std::optional<GltfTextureBinding> specularTexture;
    std::optional<GltfTextureBinding> specularColorTexture;
    std::optional<GltfTextureBinding> specularGlossinessTexture;
    std::optional<GltfTextureBinding> transmissionTexture;
    std::optional<GltfTextureBinding> clearcoatTexture;
    std::optional<GltfTextureBinding> clearcoatRoughnessTexture;
    std::optional<GltfTextureBinding> clearcoatNormalTexture;
    std::optional<GltfTextureBinding> sheenColorTexture;
    std::optional<GltfTextureBinding> sheenRoughnessTexture;
    std::optional<GltfTextureBinding> normalTexture;
    float normalTextureScale = 1.0f;
    std::optional<GltfTextureBinding> occlusionTexture;
    float occlusionTextureStrength = 1.0f;
    std::array<float, 3> emissiveFactor = {0.0f, 0.0f, 0.0f};
    std::optional<GltfTextureBinding> emissiveTexture;
    GltfAlphaMode alphaMode = GltfAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
    bool skinned = false;
};

struct GltfNodeRuntime {
    Mat4 baseLocalTransform = Mat4::identity();
    Mat4 localTransform = Mat4::identity();
    Mat4 globalTransform = Mat4::identity();
    std::array<double, 3> baseTranslation = {0.0, 0.0, 0.0};
    std::array<double, 4> baseRotation = {0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> baseScale = {1.0, 1.0, 1.0};
    std::array<double, 3> translation = {0.0, 0.0, 0.0};
    std::array<double, 4> rotation = {0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> scale = {1.0, 1.0, 1.0};
    std::vector<double> baseWeights;
    std::vector<double> weights;
    std::vector<int> children;
    int parent = -1;
    int mesh = -1;
    int skin = -1;
    bool hasMatrix = false;
};

struct GltfSkinRuntime {
    std::vector<int> joints;
    std::vector<Mat4> inverseBindMatrices;
};

enum class GltfAnimationPath {
    Translation,
    Rotation,
    Scale,
    Weights
};

enum class GltfAnimationInterpolation {
    Linear,
    Step,
    CubicSpline
};

struct GltfAnimationSamplerRuntime {
    std::vector<double> inputTimes;
    std::vector<double> outputValues;
    GltfAnimationInterpolation interpolation =
        GltfAnimationInterpolation::Linear;
    int outputComponents = 0;
};

struct GltfAnimationChannelRuntime {
    int samplerIndex = -1;
    int targetNode = -1;
    GltfAnimationPath path = GltfAnimationPath::Translation;
};

struct GltfAnimationRuntime {
    std::vector<GltfAnimationSamplerRuntime> samplers;
    std::vector<GltfAnimationChannelRuntime> channels;
    double durationSeconds = 0.0;
};

struct GltfModel {
    std::vector<GltfPrimitive> primitives;
    std::vector<GltfTexture> textures;
    std::vector<GltfNodeRuntime> nodes;
    std::vector<int> sceneRootNodes;
    std::vector<GltfSkinRuntime> skins;
    std::vector<GltfAnimationRuntime> animations;
    std::vector<std::string> credits;
    RasterOverlayDetails rasterOverlayDetails;
    WaterMask terrainWaterMask;
    std::optional<size_t> terrainWaterMaskTextureIndex;
    std::optional<Vec3> preferredLocalOriginEcef;
    int activeAnimationIndex = 0;
    bool animationLooping = true;
    bool animationPaused = false;
    uint64_t animationRevision = 0;
    double lastAnimationTimeSeconds = -1.0;

    size_t vertexCount() const;
    size_t indexCount() const;
    int64_t byteSize() const;
    bool hasRuntimeAnimation() const;
    size_t animationCount() const;
    int activeAnimation() const;
    bool setActiveAnimation(int index);
    bool isAnimationLooping() const;
    void setAnimationLooping(bool looping);
    bool isAnimationPaused() const;
    void setAnimationPaused(bool paused);
    uint64_t currentAnimationRevision() const;
    bool rebuildRuntime();
    bool updateAnimation(double timeSeconds);
};

struct GltfParserOptions {
    bool allowLegacyBatchIdAttribute = false;
};

class GltfParser {
public:
    using ExternalResourceResolver =
        std::function<std::vector<uint8_t>(const std::string& uri)>;
    using ImageDecoder =
        std::function<std::optional<GltfImage>(const uint8_t* data,
                                               size_t size)>;

    static std::unique_ptr<GltfModel> parse(const uint8_t* data, size_t size);
    static std::unique_ptr<GltfModel> parse(
        const uint8_t* data,
        size_t size,
        const ExternalResourceResolver& externalResourceResolver);
    static std::unique_ptr<GltfModel> parse(
        const uint8_t* data,
        size_t size,
        const ExternalResourceResolver& externalResourceResolver,
        const ImageDecoder& imageDecoder);
    static std::unique_ptr<GltfModel> parse(
        const uint8_t* data,
        size_t size,
        const ExternalResourceResolver& externalResourceResolver,
        const ImageDecoder& imageDecoder,
        const GltfParserOptions& options);

    /// Enumerate the external resource URIs (buffer and image uri fields
    /// that are not data: URIs) referenced by a glTF/GLB payload, without
    /// resolving or validating them. Returns raw, unresolved URI strings.
    /// Used to prefetch external resources asynchronously so parse-time
    /// resolvers never have to block on network I/O (P2-2).
    static std::vector<std::string> collectExternalResourceUris(
        const uint8_t* data,
        size_t size);
};

} // namespace earth_engine
