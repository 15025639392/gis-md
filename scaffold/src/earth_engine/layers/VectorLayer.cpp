#include "VectorLayer.h"
#include "../renderer/RenderDevice.h"
#include "../renderer/Renderer.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/math/Mat4.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace earth_engine {

// ============================================================
// 共享几何 — 单位四边形（用于 billboard 点）
// ============================================================

static const uint32_t kQuadIndices[] = {0, 1, 2, 0, 2, 3};

// ============================================================
// 辅助：地理坐标 → ECEF
// ============================================================

static Vec3 geoToECEF(const Cartographic& c) {
    return Ellipsoid::WGS84().cartographicToCartesian(c);
}

// ============================================================
// 辅助：沿椭球面细分大圆弧
// ============================================================

static std::vector<Vec3> subdivideArc(const Vec3& a, const Vec3& b,
                                       int segments = 16) {
    std::vector<Vec3> result;
    result.reserve(segments + 1);

    const auto& ellipsoid = Ellipsoid::WGS84();
    for (int i = 0; i <= segments; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(segments);
        // Slerp on unit sphere, then scale to ellipsoid
        Vec3 mid = (a * (1.0 - t) + b * t).normalized();
        result.push_back(ellipsoid.scaleToGeodeticSurface(mid));
    }
    return result;
}

// ============================================================
// Ear-clipping 三角化（简单凸/非凸多边形）
// ============================================================

namespace {

/// 判断 polygon[idxA], polygon[idxB], polygon[idxC] 是否为耳（ear）
/// 在局部 ENU 切平面上工作；投影坐标存放在 x/y。
static bool isEar(const std::vector<Vec3>& poly,
                  const std::vector<uint32_t>& indices,
                  size_t a, size_t b, size_t c) {
    const Vec3& va = poly[indices[a]];
    const Vec3& vb = poly[indices[b]];
    const Vec3& vc = poly[indices[c]];

    double cross = (vb.x() - va.x()) * (vc.y() - va.y()) -
                   (vb.y() - va.y()) * (vc.x() - va.x());
    if (cross <= 0.0) return false; // 凹角或共线

    // 检查三角形内是否包含其他顶点
    for (size_t i = 0; i < indices.size(); ++i) {
        if (i == a || i == b || i == c) continue;
        const Vec3& p = poly[indices[i]];

        // Barycentric test in local 2D.
        double d0 = (vc.y() - va.y()) * (p.x() - va.x()) - (vc.x() - va.x()) * (p.y() - va.y());
        double d1 = (va.y() - vb.y()) * (p.x() - vb.x()) - (va.x() - vb.x()) * (p.y() - vb.y());
        double d2 = (vb.y() - vc.y()) * (p.x() - vc.x()) - (vb.x() - vc.x()) * (p.y() - vc.y());

        bool hasNeg = (d0 < 0) || (d1 < 0) || (d2 < 0);
        bool hasPos = (d0 > 0) || (d1 > 0) || (d2 > 0);

        if (!(hasNeg && hasPos)) return false; // point inside triangle
    }
    return true;
}

static double signedArea2D(const std::vector<Vec3>& vertices) {
    double area = 0.0;
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vec3& a = vertices[i];
        const Vec3& b = vertices[(i + 1) % vertices.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    return area * 0.5;
}

static std::vector<Vec3> projectToLocalTangentPlane(
    const std::vector<Vec3>& ecefVertices) {
    std::vector<Vec3> projected;
    projected.reserve(ecefVertices.size());
    if (ecefVertices.empty()) return projected;

    Vec3 origin = Vec3::zero();
    for (const auto& p : ecefVertices) origin += p;
    origin = origin / static_cast<double>(ecefVertices.size());

    Vec3 up = origin.normalized();
    Vec3 east = Vec3::unitZ().cross(up);
    if (east.lengthSquared() < 1e-12) {
        east = Vec3::unitY();
    } else {
        east = east.normalized();
    }
    Vec3 north = up.cross(east).normalized();

    for (const auto& p : ecefVertices) {
        Vec3 delta = p - origin;
        projected.emplace_back(delta.dot(east), delta.dot(north), 0.0);
    }
    return projected;
}

static std::vector<uint32_t> triangulatePolygon(
    const std::vector<Vec3>& vertices) {
    std::vector<uint32_t> result;
    size_t n = vertices.size();
    if (n < 3) return result;

    std::vector<Vec3> projected = projectToLocalTangentPlane(vertices);

    // Build index list
    std::vector<uint32_t> indices(n);
    for (size_t i = 0; i < n; ++i) indices[i] = static_cast<uint32_t>(i);
    if (signedArea2D(projected) < 0.0) {
        std::reverse(indices.begin(), indices.end());
    }

    // Ear clipping
    size_t count = 0;
    size_t maxIter = n * n; // safety
    while (indices.size() > 2 && count < maxIter) {
        bool found = false;
        size_t m = indices.size();
        for (size_t i = 0; i < m; ++i) {
            size_t a = (i + m - 1) % m;
            size_t b = i;
            size_t c = (i + 1) % m;
            if (isEar(projected, indices, a, b, c)) {
                result.push_back(indices[a]);
                result.push_back(indices[b]);
                result.push_back(indices[c]);
                indices.erase(indices.begin() + static_cast<ptrdiff_t>(i));
                found = true;
                break;
            }
        }
        if (!found) break;
        ++count;
    }

    // Fallback: fan triangulation if ear clipping failed
    if (result.empty() && n >= 3) {
        for (size_t i = 1; i < n - 1; ++i) {
            result.push_back(0);
            result.push_back(static_cast<uint32_t>(i));
            result.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    return result;
}

} // anonymous namespace

// ============================================================
// VectorLayer
// ============================================================

VectorLayer::VectorLayer(std::string layerId,
                          std::vector<GeoFeature> features,
                          OverlayStyle style,
                          RenderDevice* renderDevice)
    : layerId_(std::move(layerId)),
      features_(std::move(features)),
      style_(std::move(style)),
      renderDevice_(renderDevice) {}

VectorLayer::~VectorLayer() {
    dispose();
}

void VectorLayer::setOpacity(float o) {
    style_.layer.opacity = std::max(0.0f, std::min(1.0f, o));
}

void VectorLayer::setStyle(const OverlayStyle& s) {
    style_ = s;
    tessCache_.clear();  // style change may affect tessellation
}

void VectorLayer::setFeatureState(const std::string& featureId,
                                   const std::string& state) {
    if (state.empty()) {
        featureStates_.erase(featureId);
    } else {
        featureStates_[featureId] = state;
    }
}

void VectorLayer::clearAllStates() {
    featureStates_.clear();
}

std::string VectorLayer::featureState(const std::string& featureId) const {
    auto it = featureStates_.find(featureId);
    return (it != featureStates_.end()) ? it->second : "";
}

const GeoFeature* VectorLayer::findFeature(const std::string& featureId) const {
    for (const auto& f : features_) {
        if (f.id == featureId) return &f;
    }
    return nullptr;
}

bool VectorLayer::initialize(RenderDevice* device) {
    renderDevice_ = device;
    initialized_ = true;
    return true;
}

void VectorLayer::dispose() {
    tessCache_.clear();
    initialized_ = false;
}

// ============================================================
// Tessellation
// ============================================================

VectorLayer::TessellateResult
VectorLayer::tessellateFeature(const GeoFeature& feature) const {
    // 检查缓存
    auto it = tessCache_.find(feature.id);
    if (it != tessCache_.end()) {
        return it->second;
    }

    TessellateResult result;
    switch (feature.type) {
        case GeoFeature::Type::Point:
            result = tessellatePoint(feature);
            break;
        case GeoFeature::Type::LineString:
            result = tessellateLine(feature);
            break;
        case GeoFeature::Type::Polygon:
            result = tessellatePolygon(feature);
            break;
    }
    tessCache_[feature.id] = result;
    return result;
}

VectorLayer::TessellateResult
VectorLayer::tessellatePoint(const GeoFeature& feature) {
    TessellateResult r;
    if (feature.rings.empty() || feature.rings[0].empty()) return r;

    const auto& c = feature.rings[0][0];
    r.centroid = geoToECEF(c);
    r.vertexCount = 4;
    r.indexCount = 6;

    // Position is stored as centroid; actual quad geometry is shared
    // vertices: unit quad centered at 0, positioned via uniform
    r.vertices = {0, 0, 0};  // placeholder
    r.indices = {0, 1, 2, 0, 2, 3};

    return r;
}

VectorLayer::TessellateResult
VectorLayer::tessellateLine(const GeoFeature& feature) {
    TessellateResult r;
    if (feature.rings.empty() || feature.rings[0].size() < 2) return r;

    const auto& ring = feature.rings[0];

    // 沿椭球面细分每段
    std::vector<Vec3> ecefPts;
    for (size_t i = 0; i < ring.size(); ++i) {
        Vec3 pt = geoToECEF(ring[i]);
        if (i > 0) {
            auto sub = subdivideArc(geoToECEF(ring[i - 1]), pt, 8);
            // 跳过第一个点（已在上段末尾）
            for (size_t j = 1; j < sub.size(); ++j) {
                ecefPts.push_back(sub[j]);
            }
        } else {
            ecefPts.push_back(pt);
        }
    }

    // 计算 centroid
    Vec3 sum = Vec3::zero();
    for (const auto& p : ecefPts) sum = sum + p;
    r.centroid = sum / static_cast<double>(ecefPts.size());

    // 打包顶点
    r.vertices.reserve(ecefPts.size() * 3);
    for (const auto& p : ecefPts) {
        r.vertices.push_back(static_cast<float>(p.x()));
        r.vertices.push_back(static_cast<float>(p.y()));
        r.vertices.push_back(static_cast<float>(p.z()));
    }
    r.vertexCount = ecefPts.size();

    // Line strip indices
    for (size_t i = 0; i < ecefPts.size(); ++i) {
        r.indices.push_back(static_cast<uint32_t>(i));
    }

    return r;
}

VectorLayer::TessellateResult
VectorLayer::tessellatePolygon(const GeoFeature& feature) {
    TessellateResult r;
    if (feature.rings.empty() || feature.rings[0].size() < 3) return r;

    // 外环 → ECEF
    std::vector<Vec3> outerRing;
    for (const auto& c : feature.rings[0]) {
        outerRing.push_back(geoToECEF(c));
    }

    // 三角化
    auto triIndices = triangulatePolygon(outerRing);
    if (triIndices.empty()) return r;

    // 计算 centroid
    Vec3 sum = Vec3::zero();
    for (const auto& p : outerRing) sum = sum + p;
    r.centroid = sum / static_cast<double>(outerRing.size());

    // 打包顶点（只需外环顶点）
    r.vertices.reserve(outerRing.size() * 3);
    for (const auto& p : outerRing) {
        r.vertices.push_back(static_cast<float>(p.x()));
        r.vertices.push_back(static_cast<float>(p.y()));
        r.vertices.push_back(static_cast<float>(p.z()));
    }
    r.vertexCount = outerRing.size();
    r.indices = std::move(triIndices);
    r.indexCount = r.indices.size();

    return r;
}

// ============================================================
// 样式解析
// ============================================================

InteractionStyleOverride VectorLayer::resolveStyleOverride(
    const std::string& featureId) const {
    auto it = featureStates_.find(featureId);
    if (it == featureStates_.end()) {
        return style_.interaction.normalOverride;
    }

    const auto* override = style_.interaction.find(it->second);
    return override ? *override : style_.interaction.normalOverride;
}

Color VectorLayer::resolveColor(
    const GeoFeature& /*feature*/,
    const InteractionStyleOverride& override) const {

    Color base;
    if (std::holds_alternative<PointStyle>(style_.layer.geometry)) {
        base = std::get<PointStyle>(style_.layer.geometry).color;
    } else if (std::holds_alternative<LineStyle>(style_.layer.geometry)) {
        base = std::get<LineStyle>(style_.layer.geometry).color;
    } else if (std::holds_alternative<PolygonStyle>(style_.layer.geometry)) {
        base = std::get<PolygonStyle>(style_.layer.geometry).fillColor;
    }

    Color result;
    result.r = std::min(1.0f, base.r + override.colorShift.r);
    result.g = std::min(1.0f, base.g + override.colorShift.g);
    result.b = std::min(1.0f, base.b + override.colorShift.b);
    result.a = base.a * style_.layer.opacity;
    return result;
}

// ============================================================
// 渲染命令生成
// ============================================================

void VectorLayer::buildRenderCommands(const FrameState& frameState,
                                       Renderer& renderer,
                                       RenderCommandList& commands) {
    if (!visible_ || !initialized_) return;
    if (!frameState.camera) return;

    const Camera& cam = *frameState.camera;
    float vpW = static_cast<float>(frameState.viewportWidthPixels);
    float vpH = static_cast<float>(frameState.viewportHeightPixels);

    glm::mat4 viewProj = glm::mat4(
        cam.viewProjectionMatrix(
            static_cast<double>(vpW),
            static_cast<double>(vpH)).raw());

    // 释放上帧的临时 buffer
    frameBuffers_.clear();

    // 将 viewProj 存为 uniform 数据
    auto vpArr = std::array<float, 16>();
    std::memcpy(vpArr.data(), glm::value_ptr(viewProj), 16 * sizeof(float));
    std::vector<float> vpUniform(vpArr.begin(), vpArr.end());

    for (const auto& feature : features_) {
        auto tess = tessellateFeature(feature);
        if (tess.vertexCount == 0) continue;

        auto override = resolveStyleOverride(feature.id);
        Color color = resolveColor(feature, override);

        RenderCommand cmd;
        cmd.owner = layerId_ + ":" + feature.id;
        cmd.depthTest = true;

        switch (feature.type) {
            case GeoFeature::Type::Point: {
                cmd.pass = "color";
                cmd.primitive = RenderCommand::PrimitiveType::Triangles;
                cmd.shader = renderer.colorShader();
                cmd.vertexStride = 12;  // float3 position

                // World-space billboard: quad at centroid, facing camera
                Vec3 centroid = tess.centroid;
                Vec3 camPos = frameState.camera->position();
                double distMeters = (centroid - camPos).length();
                if (distMeters < 1.0) continue;  // too close or invalid

                float pointSize = std::get<PointStyle>(
                    style_.layer.geometry).size * override.scaleMultiplier;

                // pixel → world-space: halfWorld ≈ dist * pointSize / (2 * vpH)
                float halfWorld = static_cast<float>(distMeters) * pointSize /
                                  (2.0f * vpH);

                // Camera-local right and up for billboard orientation
                Vec3 r = frameState.camera->right();
                Vec3 u = frameState.camera->up();
                float cx = static_cast<float>(centroid.x());
                float cy = static_cast<float>(centroid.y());
                float cz = static_cast<float>(centroid.z());
                float rx = static_cast<float>(r.x()) * halfWorld;
                float ry = static_cast<float>(r.y()) * halfWorld;
                float rz = static_cast<float>(r.z()) * halfWorld;
                float ux = static_cast<float>(u.x()) * halfWorld;
                float uy = static_cast<float>(u.y()) * halfWorld;
                float uz = static_cast<float>(u.z()) * halfWorld;

                // 4 vertices: TL, BL, BR, TR (CCW from camera)
                std::vector<float> verts = {
                    cx - rx + ux, cy - ry + uy, cz - rz + uz,  // TL
                    cx - rx - ux, cy - ry - uy, cz - rz - uz,  // BL
                    cx + rx - ux, cy + ry - uy, cz + rz - uz,  // BR
                    cx + rx + ux, cy + ry + uy, cz + rz + uz,  // TR
                };

                // Vertex buffer
                if (renderDevice_ && cmd.shader) {
                    BufferDesc vbDesc;
                    vbDesc.size = verts.size() * sizeof(float);
                    vbDesc.data = verts.data();
                    vbDesc.usage = BufferDesc::Usage::Dynamic;
                    vbDesc.type = BufferDesc::Type::Vertex;
                    auto vb = renderDevice_->createBuffer(vbDesc);
                    if (vb) {
                        cmd.vertexBuffer = vb.get();
                        frameBuffers_.push_back(std::move(vb));
                    }
                }

                // Index buffer (reuse kQuadIndices: 0,1,2,0,2,3)
                if (renderDevice_ && cmd.shader) {
                    BufferDesc ibDesc;
                    ibDesc.size = sizeof(kQuadIndices);
                    ibDesc.data = kQuadIndices;
                    ibDesc.usage = BufferDesc::Usage::Dynamic;
                    ibDesc.type = BufferDesc::Type::Index;
                    auto ib = renderDevice_->createBuffer(ibDesc);
                    if (ib) {
                        cmd.indexBuffer = ib.get();
                        cmd.indexCount = 6;
                        cmd.indexType = RenderCommand::IndexType::UInt32;
                        frameBuffers_.push_back(std::move(ib));
                    }
                }

                auto colorArr = color.toArray();
                cmd.uniforms["u_color"] =
                    std::vector<float>(colorArr.begin(), colorArr.end());
                cmd.uniforms["u_modelViewProjection"] = vpUniform;
                cmd.vertexCount = 4;
                break;
            }

            case GeoFeature::Type::LineString: {
                cmd.pass = "color";
                cmd.primitive = RenderCommand::PrimitiveType::LineStrip;
                cmd.shader = renderer.colorShader();
                cmd.vertexStride = 12;  // float3 position

                // 创建 vertex buffer（x/y/z float32）
                if (renderDevice_ && cmd.shader && !tess.vertices.empty()) {
                    BufferDesc vbDesc;
                    vbDesc.size = tess.vertices.size() * sizeof(float);
                    vbDesc.data = tess.vertices.data();
                    vbDesc.usage = BufferDesc::Usage::Dynamic;
                    vbDesc.type = BufferDesc::Type::Vertex;
                    auto vb = renderDevice_->createBuffer(vbDesc);
                    if (vb) {
                        cmd.vertexBuffer = vb.get();
                        frameBuffers_.push_back(std::move(vb));
                    }
                }

                auto colorArr = color.toArray();
                cmd.uniforms["u_color"] =
                    std::vector<float>(colorArr.begin(), colorArr.end());
                cmd.uniforms["u_modelViewProjection"] = vpUniform;
                cmd.vertexCount = static_cast<int>(tess.vertexCount);
                break;
            }

            case GeoFeature::Type::Polygon: {
                cmd.pass = "color";
                cmd.primitive = RenderCommand::PrimitiveType::Triangles;
                cmd.shader = renderer.colorShader();
                cmd.vertexStride = 12;  // float3 position

                // 创建 vertex buffer
                if (renderDevice_ && cmd.shader && !tess.vertices.empty()) {
                    BufferDesc vbDesc;
                    vbDesc.size = tess.vertices.size() * sizeof(float);
                    vbDesc.data = tess.vertices.data();
                    vbDesc.usage = BufferDesc::Usage::Dynamic;
                    vbDesc.type = BufferDesc::Type::Vertex;
                    auto vb = renderDevice_->createBuffer(vbDesc);
                    if (vb) {
                        cmd.vertexBuffer = vb.get();
                        frameBuffers_.push_back(std::move(vb));
                    }
                }

                // 创建 index buffer
                if (renderDevice_ && cmd.shader && !tess.indices.empty()) {
                    BufferDesc ibDesc;
                    ibDesc.size = tess.indices.size() * sizeof(uint32_t);
                    ibDesc.data = tess.indices.data();
                    ibDesc.usage = BufferDesc::Usage::Dynamic;
                    ibDesc.type = BufferDesc::Type::Index;
                    auto ib = renderDevice_->createBuffer(ibDesc);
                    if (ib) {
                        cmd.indexBuffer = ib.get();
                        cmd.indexCount = static_cast<int>(tess.indices.size());
                        cmd.indexType = RenderCommand::IndexType::UInt32;
                        frameBuffers_.push_back(std::move(ib));
                    }
                }

                auto colorArr = color.toArray();
                cmd.uniforms["u_color"] =
                    std::vector<float>(colorArr.begin(), colorArr.end());
                cmd.uniforms["u_modelViewProjection"] = vpUniform;
                cmd.vertexCount = static_cast<int>(tess.vertexCount);
                break;
            }
        }

        commands.push_back(std::move(cmd));
    }
}

} // namespace earth_engine
