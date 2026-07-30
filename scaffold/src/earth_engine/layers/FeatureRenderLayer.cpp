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
#include "../debug/PlatformLog.h"

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

void FeatureRenderLayer::setStyle(const FeatureRenderStyle& s) {
    style_ = s;
    // 高度/细分/模式都影响几何:已建桶按新样式全部重镶。
    std::vector<BucketKey> keys;
    keys.reserve(buckets_.size());
    for (const auto& entry : buckets_) keys.push_back(entry.first);
    for (BucketKey key : keys) rebuildBucket(key);
    previewDirty_ = true;
}

// ============================================================
// 贴地钳制(P3 方案 A)
// ============================================================

namespace {

constexpr double kEarthRadiusMeters = 6.378137e6;

double segmentLengthMeters(const Cartographic& a, const Cartographic& b) {
    const double cosLat = std::cos((a.latitude() + b.latitude()) * 0.5);
    const double dLng = (b.longitude() - a.longitude()) * cosLat;
    const double dLat = b.latitude() - a.latitude();
    return kEarthRadiusMeters * std::sqrt(dLng * dLng + dLat * dLat);
}

/// 2D(lng,lat) even-odd 点包含测试(内部 Steiner 撒点用)。
bool pointInRings2D(double lng, double lat,
                    const std::vector<std::vector<Cartographic>>& rings) {
    bool inside = false;
    for (const auto& ring : rings) {
        const size_t n = ring.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const double yi = ring[i].latitude();
            const double yj = ring[j].latitude();
            if ((yi > lat) != (yj > lat)) {
                const double xAtY = ring[j].longitude() +
                    (ring[i].longitude() - ring[j].longitude()) *
                        (lat - ring[j].latitude()) / (yi - yj);
                if (lng < xAtY) inside = !inside;
            }
        }
    }
    return inside;
}

} // namespace

FeatureRenderLayer::AreaSampleFn FeatureRenderLayer::makeClampSampler(
    const std::vector<std::vector<Cartographic>>& rings) const {
    if (style_.altitudeMode != FeatureAltitudeMode::ClampToGround ||
        !terrainSampling_.makeAreaSampler) {
        return nullptr;
    }
    double west = std::numeric_limits<double>::max();
    double east = std::numeric_limits<double>::lowest();
    double south = std::numeric_limits<double>::max();
    double north = std::numeric_limits<double>::lowest();
    bool any = false;
    for (const auto& ring : rings) {
        for (const auto& c : ring) {
            west = std::min(west, c.longitude());
            east = std::max(east, c.longitude());
            south = std::min(south, c.latitude());
            north = std::max(north, c.latitude());
            any = true;
        }
    }
    if (!any) return nullptr;
    // 细分点仍在原边上,bbox 不需膨胀;留一格余量吸收数值边界。
    const double pad = style_.clampDensifyMeters / kEarthRadiusMeters;
    return terrainSampling_.makeAreaSampler(
        Rectangle(west - pad, south - pad, east + pad, north + pad));
}

Feature FeatureRenderLayer::prepareClampedFeature(
    const Feature& feature,
    const AreaSampleFn& sample,
    std::vector<Cartographic>* outSteiner) const {
    const double spacing = std::max(1.0, style_.clampDensifyMeters);
    auto clampHeight = [&](double lng, double lat) {
        // 无数据回落椭球面(对齐 no-fine-data-ellipsoid-fallback 约定)。
        const double ground =
            sample ? static_cast<double>(sample(lng, lat).value_or(0.0f))
                   : 0.0;
        return ground + style_.heightOffset;
    };

    Feature clamped;
    clamped.id = feature.id;
    clamped.sourceId = feature.sourceId;
    clamped.type = feature.type;
    clamped.rings.reserve(feature.rings.size());

    for (const auto& ring : feature.rings) {
        std::vector<Cartographic> densified;
        densified.reserve(ring.size() * 2);
        for (size_t i = 0; i < ring.size(); ++i) {
            const Cartographic& a = ring[i];
            densified.emplace_back(
                a.longitude(), a.latitude(),
                clampHeight(a.longitude(), a.latitude()));
            if (i + 1 >= ring.size()) break;
            const Cartographic& b = ring[i + 1];
            const int pieces = static_cast<int>(
                std::ceil(segmentLengthMeters(a, b) / spacing));
            for (int p = 1; p < pieces; ++p) {
                const double t = static_cast<double>(p) / pieces;
                const double lng =
                    a.longitude() + (b.longitude() - a.longitude()) * t;
                const double lat =
                    a.latitude() + (b.latitude() - a.latitude()) * t;
                densified.emplace_back(lng, lat, clampHeight(lng, lat));
            }
        }
        clamped.rings.push_back(std::move(densified));
    }

    // polygon 内部网格 Steiner 点:CDT 散点,面几何真正跟随地形起伏
    // (只钳外环时面内部仍是平面,横跨山谷即穿插)。
    if (outSteiner && feature.type == GeometryType::Polygon &&
        !feature.rings.empty()) {
        const double dLat = spacing / kEarthRadiusMeters;
        double west = std::numeric_limits<double>::max();
        double east = std::numeric_limits<double>::lowest();
        double south = std::numeric_limits<double>::max();
        double north = std::numeric_limits<double>::lowest();
        for (const auto& c : feature.rings.front()) {
            west = std::min(west, c.longitude());
            east = std::max(east, c.longitude());
            south = std::min(south, c.latitude());
            north = std::max(north, c.latitude());
        }
        const double cosLat =
            std::max(0.01, std::cos((south + north) * 0.5));
        const double dLng = dLat / cosLat;
        // 从半步进开始,避免撒在边界上(边界已由环细分覆盖)。
        for (double lat = south + dLat * 0.5; lat < north; lat += dLat) {
            for (double lng = west + dLng * 0.5; lng < east; lng += dLng) {
                if (!pointInRings2D(lng, lat, feature.rings)) continue;
                outSteiner->emplace_back(lng, lat, clampHeight(lng, lat));
            }
        }
    }
    return clamped;
}

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
    const AreaSampleFn& sample,
    Vec3& origin,
    bool& hasOrigin,
    std::vector<float>& fillVerts,
    std::vector<uint32_t>& fillIndices,
    std::vector<float>& lineVerts,
    std::vector<uint32_t>& lineIndices) const {
    // 贴地:预变换出细分+采样高度的副本(高度已含 offset),镶嵌时
    // heightOffset 传 0 防二次叠加;Absolute 走原几何 + offset。
    const bool clamp =
        style_.altitudeMode == FeatureAltitudeMode::ClampToGround;
    std::vector<Cartographic> steinerPoints;
    Feature clampedStorage;
    const Feature* geometry = &feature;
    double tessHeightOffset = style_.heightOffset;
    if (clamp) {
        clampedStorage =
            prepareClampedFeature(feature, sample, &steinerPoints);
        geometry = &clampedStorage;
        tessHeightOffset = 0.0;
    }

    // 原点 = 首个 ECEF 顶点。桶尺度 ~0.02rad(≈128km)→ 相对坐标幅值
    // ~1e5 m 级,float 精度 ~0.01m,满足编辑显示。
    auto ensureOrigin = [&](const Vec3& candidate) {
        if (!hasOrigin) {
            origin = candidate;
            hasOrigin = true;
        }
    };

    switch (geometry->type) {
        case GeometryType::Polygon: {
            TessellatedFill fill = PolygonTessellator::tessellate(
                *geometry, ellipsoid_, tessHeightOffset,
                steinerPoints.empty() ? nullptr : &steinerPoints);
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
            outlineFeature.rings = {geometry->rings.front()};
            TessellatedLine outline = LineTessellator::tessellate(
                outlineFeature, ellipsoid_, tessHeightOffset,
                /*closed=*/true);
            if (!outline.vertices.empty()) {
                ensureOrigin(outline.vertices.front().pos);
                appendLineMesh(outline, origin, lineVerts, lineIndices);
            }
            break;
        }
        case GeometryType::LineString: {
            TessellatedLine line = LineTessellator::tessellate(
                *geometry, ellipsoid_, tessHeightOffset,
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

    // 贴地采样器:桶内全部要素的 rings 并集区域,一次收集候选瓦片。
    std::vector<std::vector<Cartographic>> allRings;
    for (FeatureId fid : ids) {
        if (fid == previewFeatureId_) continue;
        if (const Feature* f = store_.getFeature(fid)) {
            for (const auto& ring : f->rings) allRings.push_back(ring);
        }
    }
    const AreaSampleFn sample = makeClampSampler(allRings);

    for (FeatureId fid : ids) {
        if (fid == previewFeatureId_) continue;  // 预览摘除中,走瞬态路径
        const Feature* feature = store_.getFeature(fid);
        if (!feature) continue;
        tessellateFeatureInto(*feature, sample, origin, hasOrigin,
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

    // 贴地重钳(P3 方案 A 过渡态):地形代次变化 → 节流重镶全部桶
    // (LOD 细化/加载会改高度;不重钳则要素浮沉)。120 帧节流防加载期
    // 重镶风暴;万级桶规模需配可见性门控(后续)。
    if (style_.altitudeMode == FeatureAltitudeMode::ClampToGround &&
        terrainSampling_.revision) {
        const uint64_t rev = terrainSampling_.revision();
        if (rev != lastClampRevision_ &&
            frameState.frameId >= lastReclampFrameId_ + 120) {
            lastClampRevision_ = rev;
            lastReclampFrameId_ = frameState.frameId;
            std::vector<BucketKey> keys;
            keys.reserve(buckets_.size());
            for (const auto& entry : buckets_) keys.push_back(entry.first);
            for (BucketKey key : keys) rebuildBucket(key);
            previewDirty_ = true;
        }
    }

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
        tessellateFeatureInto(previewFeature,
                              makeClampSampler(previewRings_),
                              origin, hasOrigin,
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

    // 1. 射线∩"要素实际所在面"定拾取邻域中心。必须抬到位:斜视下裸椭球
    //    交点沿视线偏出 ~面高·tan(俯角),800m 面 45° 斜视即偏 ~800m,足以
    //    让 R-tree 预筛整体落空(真机踩过:小要素 pick 恒 miss)。
    //    Clamp 模式两段法:先裸椭球定 lng/lat → 采地形高 → 抬高面重交
    //    (一次迭代对贴地面足够收敛)。
    const bool clampMode =
        style_.altitudeMode == FeatureAltitudeMode::ClampToGround;
    const Ray ray = cam.getPickRay(screenXPx, screenYPx, vpW, vpH);
    auto intersectAtHeight =
        [&](double surfaceHeight) -> std::optional<Vec3> {
        const Ellipsoid surface(ellipsoid_.radii().x() + surfaceHeight,
                                ellipsoid_.radii().y() + surfaceHeight,
                                ellipsoid_.radii().z() + surfaceHeight);
        const auto interval =
            surface.rayIntersectionInterval(ray.origin(), ray.direction());
        if (!interval) return std::nullopt;
        const double t = interval->entryDistance > 0.0
                             ? interval->entryDistance
                             : interval->exitDistance;
        if (t <= 0.0) return std::nullopt;
        return ray.pointAt(t);
    };

    double surfaceHeight = style_.heightOffset;
    if (clampMode) {
        surfaceHeight = style_.heightOffset;  // 无地形回落椭球+offset
        if (const auto ground0 = intersectAtHeight(0.0);
            ground0 && terrainSampling_.makeAreaSampler) {
            const Cartographic c0 =
                ellipsoid_.cartesianToCartographic(*ground0);
            const double padRad = 2000.0 / 6.378137e6;  // 邻域 2km 采样窗
            const AreaSampleFn probe = terrainSampling_.makeAreaSampler(
                Rectangle(c0.longitude() - padRad, c0.latitude() - padRad,
                          c0.longitude() + padRad, c0.latitude() + padRad));
            if (probe) {
                surfaceHeight =
                    static_cast<double>(
                        probe(c0.longitude(), c0.latitude()).value_or(0.0f)) +
                    style_.heightOffset;
            }
        }
    }
    const auto hit = intersectAtHeight(surfaceHeight);
    if (!hit) return result;
    const Vec3 hitEcef = *hit;
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

    // Clamp 模式:投影须用与渲染同源的采样高度(候选要素 rings 并集区域)。
    AreaSampleFn pickSample;
    if (clampMode) {
        std::vector<std::vector<Cartographic>> candidateRings;
        for (FeatureId fid : candidates) {
            if (const Feature* f = store_.getFeature(fid)) {
                for (const auto& ring : f->rings) {
                    candidateRings.push_back(ring);
                }
            }
        }
        pickSample = makeClampSampler(candidateRings);
    }

    auto projectVertex = [&](const Cartographic& c) {
        ScreenVertex sv;
        const double h =
            clampMode
                ? static_cast<double>(
                      pickSample
                          ? pickSample(c.longitude(), c.latitude())
                                .value_or(0.0f)
                          : 0.0f) +
                      style_.heightOffset
                : c.height() + style_.heightOffset;
        const Vec3 ecef = ellipsoid_.cartographicToCartesian(
            Cartographic(c.longitude(), c.latitude(), h));
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

    // [PICKDIAG] clamp 模式命中质量诊断:最近顶点像素距离与采样状态。
    // 只在 debug 且拾取被调用时打(每次 tap 一条),定位投影/渲染偏差。
    if (clampMode) {
        platformLog(LogLevel::Info, "FeatureRenderLayer",
                    "PickDiag surfaceH=%.1f sampler=%d cand=%zu "
                    "bestVtx=%.1fpx bestEdge=%.1fpx fill=%d",
                    surfaceHeight,
                    pickSample ? 1 : 0,
                    candidates.size(),
                    bestVertex.distancePx,
                    bestEdge.distancePx,
                    bestFill.isValid() ? 1 : 0);
    }

    if (bestVertex.distancePx <= tol) return bestVertex;
    if (bestEdge.distancePx <= tol) return bestEdge;
    if (bestFill.isValid()) return bestFill;
    return result;
}

} // namespace earth_engine
