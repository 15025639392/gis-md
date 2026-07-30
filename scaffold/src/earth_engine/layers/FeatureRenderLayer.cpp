#include "FeatureRenderLayer.h"

#include "../data/PolygonTessellator.h"
#include "../data/LineTessellator.h"
#include "../renderer/RenderDevice.h"
#include "../renderer/Renderer.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Mat4.h"
#include "../core/math/Ray.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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

void FeatureRenderLayer::tessellateFeatureInto(
    const Feature& feature,
    Vec3& origin,
    bool& hasOrigin,
    std::vector<float>& fillVerts,
    std::vector<uint32_t>& fillIndices,
    std::vector<float>& lineVerts,
    std::vector<uint32_t>& lineIndices) const {
    // 原点 = 首个 ECEF 顶点。桶尺度 ~0.02rad(≈128km)→ 相对坐标幅值
    // ~1e5 m 级,float 精度 ~0.01m,满足编辑显示。
    auto ensureOrigin = [&](const Vec3& candidate) {
        if (!hasOrigin) {
            origin = candidate;
            hasOrigin = true;
        }
    };

    switch (feature.type) {
        case GeometryType::Polygon: {
            TessellatedFill fill = PolygonTessellator::tessellate(
                feature, ellipsoid_, style_.heightOffset);
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
            outlineFeature.rings = {feature.rings.front()};
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
                feature, ellipsoid_, style_.heightOffset,
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

bool FeatureRenderLayer::uploadBucketGpu(
    const Vec3& origin,
    const std::vector<float>& fillVerts,
    const std::vector<uint32_t>& fillIndices,
    const std::vector<float>& lineVerts,
    const std::vector<uint32_t>& lineIndices,
    BucketGpu& out) const {
    out = BucketGpu{};
    out.origin = origin;
    if (!fillIndices.empty()) {
        out.fillVertexBuffer = makeBuffer(
            renderDevice_, fillVerts.data(),
            fillVerts.size() * sizeof(float), BufferDesc::Type::Vertex);
        out.fillIndexBuffer = makeBuffer(
            renderDevice_, fillIndices.data(),
            fillIndices.size() * sizeof(uint32_t), BufferDesc::Type::Index);
        if (out.fillVertexBuffer && out.fillIndexBuffer) {
            out.fillIndexCount = static_cast<int>(fillIndices.size());
        }
    }
    if (!lineIndices.empty()) {
        out.lineVertexBuffer = makeBuffer(
            renderDevice_, lineVerts.data(),
            lineVerts.size() * sizeof(float), BufferDesc::Type::Vertex);
        out.lineIndexBuffer = makeBuffer(
            renderDevice_, lineIndices.data(),
            lineIndices.size() * sizeof(uint32_t), BufferDesc::Type::Index);
        if (out.lineVertexBuffer && out.lineIndexBuffer) {
            out.lineIndexCount = static_cast<int>(lineIndices.size());
        }
    }
    return out.fillIndexCount > 0 || out.lineIndexCount > 0;
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

    for (FeatureId fid : ids) {
        if (fid == previewFeatureId_) continue;  // 预览摘除中,走瞬态路径
        const Feature* feature = store_.getFeature(fid);
        if (!feature) continue;
        tessellateFeatureInto(*feature, origin, hasOrigin,
                              fillVerts, fillIndices, lineVerts, lineIndices);
    }

    if (fillIndices.empty() && lineIndices.empty()) {
        buckets_.erase(key);
        return;
    }

    BucketGpu gpu;
    if (!uploadBucketGpu(origin, fillVerts, fillIndices,
                         lineVerts, lineIndices, gpu)) {
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

    // 编辑预览:rings 变了才重镶(拖拽帧间未动 = 零成本),瞬态 buffer
    // 每次重建(几何一直在变,orphan 语义交给 VAO purge)。
    if (previewFeatureId_ != kInvalidFeatureId && previewDirty_) {
        previewDirty_ = false;
        previewGpuValid_ = false;
        Feature previewFeature;
        previewFeature.id = previewFeatureId_;
        previewFeature.type = previewType_;
        previewFeature.rings = previewRings_;
        std::vector<float> fillVerts;
        std::vector<uint32_t> fillIndices;
        std::vector<float> lineVerts;
        std::vector<uint32_t> lineIndices;
        Vec3 origin = Vec3::zero();
        bool hasOrigin = false;
        tessellateFeatureInto(previewFeature, origin, hasOrigin,
                              fillVerts, fillIndices,
                              lineVerts, lineIndices);
        if (hasOrigin) {
            previewGpuValid_ = uploadBucketGpu(
                origin, fillVerts, fillIndices, lineVerts, lineIndices,
                previewGpu_);
        }
    }

    if (buckets_.empty() && !previewGpuValid_) return;

    for (const auto& [key, gpu] : buckets_) {
        appendBucketCommands(gpu, frameState, renderer, commands);
    }
    if (previewFeatureId_ != kInvalidFeatureId && previewGpuValid_) {
        appendBucketCommands(previewGpu_, frameState, renderer, commands);
    }
}

void FeatureRenderLayer::appendBucketCommands(
    const BucketGpu& gpu,
    const FrameState& frameState,
    Renderer& renderer,
    RenderCommandList& commands) const {
    const Camera& cam = *frameState.camera;
    const double vpW = static_cast<double>(frameState.viewportWidthPixels);
    const double vpH = static_cast<double>(frameState.viewportHeightPixels);
    const glm::dmat4 viewProj(cam.viewProjectionMatrix(vpW, vpH).raw());

    ShaderProgram* fillShader = renderer.colorShader();
    ShaderProgram* lineShader = renderer.vectorLineShader();

    {
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

// ============================================================
// 编辑预览通道
// ============================================================

bool FeatureRenderLayer::beginEditPreview(FeatureId id) {
    if (!renderDevice_) return false;
    if (previewFeatureId_ != kInvalidFeatureId) return false;  // 单预览
    const Feature* feature = store_.getFeature(id);
    if (!feature) return false;

    previewFeatureId_ = id;
    previewType_ = feature->type;
    previewRings_ = feature->rings;
    previewDirty_ = true;
    previewGpuValid_ = false;
    // 把该要素从常驻桶摘出(rebuildBucket 内按 previewFeatureId_ 跳过)。
    rebuildBucket(store_.bucketOf(id));
    return true;
}

void FeatureRenderLayer::updateEditPreview(
    std::vector<std::vector<Cartographic>> rings) {
    if (previewFeatureId_ == kInvalidFeatureId) return;
    previewRings_ = std::move(rings);
    previewDirty_ = true;
}

void FeatureRenderLayer::endEditPreview() {
    if (previewFeatureId_ == kInvalidFeatureId) return;
    const FeatureId id = previewFeatureId_;
    previewFeatureId_ = kInvalidFeatureId;
    previewRings_.clear();
    previewDirty_ = false;
    previewGpuValid_ = false;
    previewGpu_ = BucketGpu{};
    // 要素回归常驻桶。commit 路径(调用方已 updateFeature)下 store 已标脏
    // 新旧桶,syncDirtyBuckets 会重镶;这里直接重镶当前桶覆盖 cancel 路径
    // (未 commit,桶成员未变)。要素已被删除时 bucketOf 返回无效桶,no-op。
    rebuildBucket(store_.bucketOf(id));
}

// ============================================================
// 拾取
// ============================================================

namespace {

struct ScreenVertex {
    double x = 0.0;
    double y = 0.0;
    bool valid = false;  // 相机前方(clip.w > 0)
};

double pointSegmentDistance2D(double px, double py,
                              const ScreenVertex& a,
                              const ScreenVertex& b,
                              double& outT) {
    const double abx = b.x - a.x;
    const double aby = b.y - a.y;
    const double len2 = abx * abx + aby * aby;
    double t = 0.0;
    if (len2 > 0.0) {
        t = ((px - a.x) * abx + (py - a.y) * aby) / len2;
        t = std::clamp(t, 0.0, 1.0);
    }
    const double cx = a.x + abx * t;
    const double cy = a.y + aby * t;
    outT = t;
    const double dx = px - cx;
    const double dy = py - cy;
    return std::sqrt(dx * dx + dy * dy);
}

/// 屏幕空间 even-odd 多边形包含测试(全环参与 → 孔自动挖除)。
bool pointInRingsEvenOdd(double px, double py,
                         const std::vector<std::vector<ScreenVertex>>& rings) {
    bool inside = false;
    for (const auto& ring : rings) {
        const size_t n = ring.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            if (!ring[i].valid || !ring[j].valid) return false;
            const bool crosses =
                (ring[i].y > py) != (ring[j].y > py);
            if (crosses) {
                const double xAtY = ring[j].x +
                    (ring[i].x - ring[j].x) * (py - ring[j].y) /
                        (ring[i].y - ring[j].y);
                if (px < xAtY) inside = !inside;
            }
        }
    }
    return inside;
}

} // namespace

FeaturePickResult FeatureRenderLayer::pick(const FrameState& frameState,
                                           float screenXPx,
                                           float screenYPx,
                                           float tolerancePx) const {
    FeaturePickResult result;
    if (!frameState.camera || store_.empty()) return result;

    const Camera& cam = *frameState.camera;
    const double vpW = static_cast<double>(frameState.viewportWidthPixels);
    const double vpH = static_cast<double>(frameState.viewportHeightPixels);
    if (vpW <= 0.0 || vpH <= 0.0) return result;

    // 1. 射线∩"抬高 heightOffset 的椭球面"定拾取邻域中心。必须用要素
    //    实际所在面:斜视下裸椭球交点沿视线偏出 ~heightOffset·tan(俯角),
    //    800m 面高 45° 斜视即偏 ~800m,足以让 R-tree 预筛整体落空
    //    (真机踩过:小要素 pick 恒 miss)。
    const Ray ray = cam.getPickRay(screenXPx, screenYPx, vpW, vpH);
    const Ellipsoid offsetSurface(
        ellipsoid_.radii().x() + style_.heightOffset,
        ellipsoid_.radii().y() + style_.heightOffset,
        ellipsoid_.radii().z() + style_.heightOffset);
    const auto interval =
        offsetSurface.rayIntersectionInterval(ray.origin(), ray.direction());
    if (!interval) return result;
    const double tHit = interval->entryDistance > 0.0
                            ? interval->entryDistance
                            : interval->exitDistance;
    if (tHit <= 0.0) return result;
    const Vec3 hitEcef = ray.pointAt(tHit);
    const Cartographic hitCarto = ellipsoid_.cartesianToCartographic(hitEcef);

    // 2. R-tree 预筛:容差像素 → 地面米(距离 × 每像素弧度)→ 弧度 bbox。
    //    宽松 ×4 吸收斜视拉伸与 heightOffset 偏差;查询是广相交,多筛无害。
    const double distMeters = (hitEcef - cam.position()).length();
    const double radiansPerPx = cam.verticalFovRadians() / vpH;
    const double toleranceMeters =
        std::max(1.0, distMeters * radiansPerPx *
                          static_cast<double>(tolerancePx) * 4.0);
    const double dLat = toleranceMeters / 6.378137e6;
    const double cosLat = std::max(0.01, std::cos(hitCarto.latitude()));
    const double dLng = dLat / cosLat;
    const Rectangle queryBbox(hitCarto.longitude() - dLng,
                              hitCarto.latitude() - dLat,
                              hitCarto.longitude() + dLng,
                              hitCarto.latitude() + dLat);
    std::vector<FeatureId> candidates = store_.queryVisible(queryBbox);
    if (candidates.empty()) return result;
    std::sort(candidates.begin(), candidates.end());  // 结果确定性

    // 3. 候选投影到屏幕(渲染态几何 = 含 heightOffset),逐类比距离。
    const glm::dmat4 viewProj(cam.viewProjectionMatrix(vpW, vpH).raw());
    const double px = static_cast<double>(screenXPx);
    const double py = static_cast<double>(screenYPx);
    const double tol = static_cast<double>(tolerancePx);

    auto projectVertex = [&](const Cartographic& c) {
        ScreenVertex sv;
        const Vec3 ecef = ellipsoid_.cartographicToCartesian(Cartographic(
            c.longitude(), c.latitude(), c.height() + style_.heightOffset));
        const glm::dvec4 clip =
            viewProj * glm::dvec4(ecef.x(), ecef.y(), ecef.z(), 1.0);
        if (clip.w <= 0.0) return sv;
        sv.x = (clip.x / clip.w + 1.0) * 0.5 * vpW;
        sv.y = (1.0 - clip.y / clip.w) * 0.5 * vpH;
        sv.valid = true;
        return sv;
    };

    FeaturePickResult bestVertex;
    bestVertex.distancePx = tol + 1.0;
    FeaturePickResult bestEdge;
    bestEdge.distancePx = tol + 1.0;
    FeaturePickResult bestFill;
    double bestFillEdgeDist = std::numeric_limits<double>::infinity();

    for (FeatureId fid : candidates) {
        const Feature* feature = store_.getFeature(fid);
        if (!feature || feature->rings.empty()) continue;

        std::vector<std::vector<ScreenVertex>> projectedRings;
        projectedRings.reserve(feature->rings.size());
        for (const auto& ring : feature->rings) {
            std::vector<ScreenVertex> projected;
            projected.reserve(ring.size());
            for (const auto& c : ring) projected.push_back(projectVertex(c));
            projectedRings.push_back(std::move(projected));
        }

        double nearestEdgeDist = std::numeric_limits<double>::infinity();
        for (size_t r = 0; r < projectedRings.size(); ++r) {
            const auto& ring = projectedRings[r];
            const auto& cartoRing = feature->rings[r];
            const size_t n = ring.size();
            // 顶点
            for (size_t i = 0; i < n; ++i) {
                if (!ring[i].valid) continue;
                const double dx = ring[i].x - px;
                const double dy = ring[i].y - py;
                const double d = std::sqrt(dx * dx + dy * dy);
                if (d < bestVertex.distancePx) {
                    bestVertex.featureId = fid;
                    bestVertex.part = FeaturePickResult::Part::Vertex;
                    bestVertex.ringIndex = static_cast<int>(r);
                    bestVertex.vertexIndex = static_cast<int>(i);
                    bestVertex.distancePx = d;
                    bestVertex.position = cartoRing[i];
                }
            }
            // 边(polygon 环含闭合末边;LineString 不闭合;Point 无边)
            if (feature->type == GeometryType::Point || n < 2) continue;
            const size_t edgeCount =
                feature->type == GeometryType::Polygon ? n : n - 1;
            for (size_t i = 0; i < edgeCount; ++i) {
                const size_t j = (i + 1) % n;
                if (!ring[i].valid || !ring[j].valid) continue;
                double t = 0.0;
                const double d =
                    pointSegmentDistance2D(px, py, ring[i], ring[j], t);
                nearestEdgeDist = std::min(nearestEdgeDist, d);
                if (d < bestEdge.distancePx) {
                    const Cartographic& a = cartoRing[i];
                    const Cartographic& b = cartoRing[j];
                    bestEdge.featureId = fid;
                    bestEdge.part = FeaturePickResult::Part::Edge;
                    bestEdge.ringIndex = static_cast<int>(r);
                    bestEdge.vertexIndex = static_cast<int>(i);
                    bestEdge.distancePx = d;
                    bestEdge.position = Cartographic(
                        a.longitude() + (b.longitude() - a.longitude()) * t,
                        a.latitude() + (b.latitude() - a.latitude()) * t,
                        a.height() + (b.height() - a.height()) * t);
                }
            }
        }
        // fill 包含测试(多个 fill 重叠时取"最近边"者 = 最特定)
        if (feature->type == GeometryType::Polygon &&
            pointInRingsEvenOdd(px, py, projectedRings) &&
            nearestEdgeDist < bestFillEdgeDist) {
            bestFillEdgeDist = nearestEdgeDist;
            bestFill.featureId = fid;
            bestFill.part = FeaturePickResult::Part::Fill;
            bestFill.distancePx = 0.0;
            bestFill.position = Cartographic(
                hitCarto.longitude(), hitCarto.latitude(), 0.0);
        }
    }

    if (bestVertex.distancePx <= tol) return bestVertex;
    if (bestEdge.distancePx <= tol) return bestEdge;
    if (bestFill.isValid()) return bestFill;
    return result;
}

} // namespace earth_engine
