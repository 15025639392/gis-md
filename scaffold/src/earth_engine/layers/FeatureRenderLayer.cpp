#include "FeatureRenderLayer.h"

#include "../data/PolygonTessellator.h"
#include "../data/LineTessellator.h"
#include "../renderer/RenderDevice.h"
#include "../renderer/Renderer.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Mat4.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace earth_engine {

namespace {

/// line ribbon 顶点的 GPU 打包(44B,对齐 GLES VectorLine44 布局与
/// §6.2 shader attribute 0-4)。CPU 侧 LineVertex 是 double,不能直传。
constexpr int kLineVertexFloats = 11;

void appendLineMesh(const TessellatedLine& line,
                    const Vec3& origin,
                    std::vector<float>& outVerts,
                    std::vector<uint32_t>& outIndices) {
    if (line.vertices.empty() || line.indices.empty()) return;
    const uint32_t base =
        static_cast<uint32_t>(outVerts.size() / kLineVertexFloats);
    outVerts.reserve(outVerts.size() +
                     line.vertices.size() * kLineVertexFloats);
    for (const LineVertex& v : line.vertices) {
        const Vec3 p = v.pos - origin;
        const Vec3 pr = v.prev - origin;
        const Vec3 nx = v.next - origin;
        outVerts.push_back(static_cast<float>(p.x()));
        outVerts.push_back(static_cast<float>(p.y()));
        outVerts.push_back(static_cast<float>(p.z()));
        outVerts.push_back(static_cast<float>(pr.x()));
        outVerts.push_back(static_cast<float>(pr.y()));
        outVerts.push_back(static_cast<float>(pr.z()));
        outVerts.push_back(static_cast<float>(nx.x()));
        outVerts.push_back(static_cast<float>(nx.y()));
        outVerts.push_back(static_cast<float>(nx.z()));
        outVerts.push_back(v.side);
        outVerts.push_back(v.lengthSoFar);
    }
    outIndices.reserve(outIndices.size() + line.indices.size());
    for (uint32_t idx : line.indices) {
        outIndices.push_back(base + idx);
    }
}

std::unique_ptr<Buffer> makeBuffer(RenderDevice* device,
                                   const void* data,
                                   size_t size,
                                   BufferDesc::Type type) {
    BufferDesc desc;
    desc.size = size;
    desc.data = data;
    desc.usage = BufferDesc::Usage::Static;
    desc.type = type;
    return device->createBuffer(desc);
}

} // namespace

FeatureRenderLayer::FeatureRenderLayer(std::string layerId,
                                       RenderDevice* renderDevice,
                                       const Ellipsoid& ellipsoid)
    : layerId_(std::move(layerId)),
      renderDevice_(renderDevice),
      ellipsoid_(ellipsoid) {}

FeatureRenderLayer::~FeatureRenderLayer() = default;

int FeatureRenderLayer::syncDirtyBuckets() {
    if (!renderDevice_) return 0;
    const auto dirty = store_.consumeDirtyBuckets();
    for (BucketKey key : dirty) {
        rebuildBucket(key);
    }
    return static_cast<int>(dirty.size());
}

void FeatureRenderLayer::rebuildBucket(BucketKey key) {
    const auto* memberIds = store_.featuresInBucket(key);
    if (!memberIds || memberIds->empty()) {
        buckets_.erase(key);
        return;
    }

    // 按 ID 升序镶嵌:桶内 buffer 布局确定性(测试可对拍,编辑重镶稳定)。
    std::vector<FeatureId> ids(memberIds->begin(), memberIds->end());
    std::sort(ids.begin(), ids.end());

    std::vector<float> fillVerts;      // xyz,相对桶原点
    std::vector<uint32_t> fillIndices;
    std::vector<float> lineVerts;      // 11 float/顶点(VectorLine44)
    std::vector<uint32_t> lineIndices;
    Vec3 origin = Vec3::zero();
    bool hasOrigin = false;

    // 桶原点 = 本桶第一个 ECEF 顶点。桶尺度 ~0.02rad(≈128km)→ 相对
    // 坐标幅值 ~1e5 m 级,float 精度 ~0.01m,满足编辑显示。
    auto ensureOrigin = [&](const Vec3& candidate) {
        if (!hasOrigin) {
            origin = candidate;
            hasOrigin = true;
        }
    };

    for (FeatureId fid : ids) {
        const Feature* feature = store_.getFeature(fid);
        if (!feature) continue;
        switch (feature->type) {
            case GeometryType::Polygon: {
                TessellatedFill fill = PolygonTessellator::tessellate(
                    *feature, ellipsoid_, style_.heightOffset);
                if (!fill.positions.empty() && !fill.fillIndices.empty()) {
                    ensureOrigin(fill.positions.front());
                    const uint32_t base =
                        static_cast<uint32_t>(fillVerts.size() / 3);
                    fillVerts.reserve(fillVerts.size() +
                                      fill.positions.size() * 3);
                    for (const Vec3& p : fill.positions) {
                        const Vec3 rel = p - origin;
                        fillVerts.push_back(static_cast<float>(rel.x()));
                        fillVerts.push_back(static_cast<float>(rel.y()));
                        fillVerts.push_back(static_cast<float>(rel.z()));
                    }
                    for (uint32_t idx : fill.fillIndices) {
                        fillIndices.push_back(base + idx);
                    }
                }
                // 外环 outline(闭合 ribbon)。LineTessellator 契约只收
                // LineString(有测试锁死),把外环包成临时 LineString。
                // 孔环 outline 留后续。
                Feature outlineFeature;
                outlineFeature.type = GeometryType::LineString;
                outlineFeature.rings = {feature->rings.front()};
                TessellatedLine outline = LineTessellator::tessellate(
                    outlineFeature, ellipsoid_, style_.heightOffset,
                    /*closed=*/true);
                if (!outline.vertices.empty()) {
                    ensureOrigin(outline.vertices.front().pos);
                    appendLineMesh(outline, origin, lineVerts, lineIndices);
                }
                break;
            }
            case GeometryType::LineString: {
                TessellatedLine line = LineTessellator::tessellate(
                    *feature, ellipsoid_, style_.heightOffset,
                    /*closed=*/false);
                if (!line.vertices.empty()) {
                    ensureOrigin(line.vertices.front().pos);
                    appendLineMesh(line, origin, lineVerts, lineIndices);
                }
                break;
            }
            case GeometryType::Point:
                // P5 符号系统前不渲染点要素。
                break;
        }
    }

    if (fillIndices.empty() && lineIndices.empty()) {
        buckets_.erase(key);
        return;
    }

    BucketGpu gpu;
    gpu.origin = origin;
    if (!fillIndices.empty()) {
        gpu.fillVertexBuffer = makeBuffer(
            renderDevice_, fillVerts.data(),
            fillVerts.size() * sizeof(float), BufferDesc::Type::Vertex);
        gpu.fillIndexBuffer = makeBuffer(
            renderDevice_, fillIndices.data(),
            fillIndices.size() * sizeof(uint32_t), BufferDesc::Type::Index);
        if (gpu.fillVertexBuffer && gpu.fillIndexBuffer) {
            gpu.fillIndexCount = static_cast<int>(fillIndices.size());
        }
    }
    if (!lineIndices.empty()) {
        gpu.lineVertexBuffer = makeBuffer(
            renderDevice_, lineVerts.data(),
            lineVerts.size() * sizeof(float), BufferDesc::Type::Vertex);
        gpu.lineIndexBuffer = makeBuffer(
            renderDevice_, lineIndices.data(),
            lineIndices.size() * sizeof(uint32_t), BufferDesc::Type::Index);
        if (gpu.lineVertexBuffer && gpu.lineIndexBuffer) {
            gpu.lineIndexCount = static_cast<int>(lineIndices.size());
        }
    }
    if (gpu.fillIndexCount == 0 && gpu.lineIndexCount == 0) {
        // buffer 创建失败:丢弃本桶,脏区已消费 → 下次编辑该桶时重试。
        buckets_.erase(key);
        return;
    }
    buckets_[key] = std::move(gpu);
}

void FeatureRenderLayer::buildRenderCommands(const FrameState& frameState,
                                             Renderer& renderer,
                                             RenderCommandList& commands) {
    if (!visible_ || !renderDevice_) return;
    if (!frameState.camera) return;

    syncDirtyBuckets();
    if (buckets_.empty()) return;

    const Camera& cam = *frameState.camera;
    const double vpW = static_cast<double>(frameState.viewportWidthPixels);
    const double vpH = static_cast<double>(frameState.viewportHeightPixels);
    const glm::dmat4 viewProj(cam.viewProjectionMatrix(vpW, vpH).raw());

    ShaderProgram* fillShader = renderer.colorShader();
    ShaderProgram* lineShader = renderer.vectorLineShader();

    for (const auto& [key, gpu] : buckets_) {
        // 双精度 compose 后降 float(RTE):顶点已相对 origin,mvp 吸收平移。
        const glm::dmat4 model = glm::translate(
            glm::dmat4(1.0),
            glm::dvec3(gpu.origin.x(), gpu.origin.y(), gpu.origin.z()));
        const glm::mat4 mvp = glm::mat4(viewProj * model);
        std::vector<float> mvpUniform(16);
        std::memcpy(mvpUniform.data(), glm::value_ptr(mvp),
                    16 * sizeof(float));

        if (gpu.fillIndexCount > 0 && fillShader) {
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorFill;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = fillShader;
            cmd.vertexBuffer = gpu.fillVertexBuffer.get();
            cmd.indexBuffer = gpu.fillIndexBuffer.get();
            cmd.indexCount = gpu.fillIndexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            cmd.vertexStride = 12;
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            cmd.depthTest = true;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            cmd.uniforms["u_modelViewProjection"] = mvpUniform;
            cmd.uniforms["u_color"] = {style_.fillColor[0],
                                       style_.fillColor[1],
                                       style_.fillColor[2],
                                       style_.fillColor[3]};
            commands.push_back(std::move(cmd));
        }

        if (gpu.lineIndexCount > 0 && lineShader) {
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorLine;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = lineShader;
            cmd.vertexBuffer = gpu.lineVertexBuffer.get();
            cmd.indexBuffer = gpu.lineIndexBuffer.get();
            cmd.indexCount = gpu.lineIndexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            cmd.vertexStride =
                static_cast<int>(kLineVertexFloats * sizeof(float));  // 44
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            cmd.depthTest = true;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            cmd.uniforms["u_modelViewProjection"] = mvpUniform;
            cmd.uniforms["u_color"] = {style_.lineColor[0],
                                       style_.lineColor[1],
                                       style_.lineColor[2],
                                       style_.lineColor[3]};
            cmd.uniforms["u_viewport"] = {static_cast<float>(vpW),
                                          static_cast<float>(vpH)};
            cmd.uniforms["u_lineWidthPx"] = {style_.lineWidthPx};
            commands.push_back(std::move(cmd));
        }
    }
}

} // namespace earth_engine
