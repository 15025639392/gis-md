#include "FeatureRenderLayer.h"

#include "../data/PolygonTessellator.h"
#include "../data/LineTessellator.h"
#include "../renderer/GlyphAtlas.h"
#include "../renderer/IconAtlas.h"
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

/// line ribbon 顶点的 GPU 打包(48B,对齐 GLES VectorLine48 布局与
/// §6.2 shader attribute 0-5)。CPU 侧 LineVertex 是 double,不能直传。
constexpr int kLineVertexFloats = 12;

/// 点/图标符号顶点的 GPU 打包(36B,对齐 GLES VectorPoint36 布局:
/// anchor(3f)+offsetUnit(2f)+uv(2f)+color(RGBA8 占 1f)+shape(1f))。
constexpr int kPointVertexFloats = 9;

/// RGBA [0,1] → RGBA8 打包(小端布局 R 在低字节)。
uint32_t packColorU32(const std::array<float, 4>& c) {
    auto to8 = [](float v) -> uint32_t {
        const float x = std::min(1.0f, std::max(0.0f, v));
        return static_cast<uint32_t>(x * 255.0f + 0.5f);
    };
    return to8(c[0]) | (to8(c[1]) << 8) | (to8(c[2]) << 16) |
           (to8(c[3]) << 24);
}

/// RGBA8 打包值以 float 位模式进顶点流(顶点流是 float 数组,GLES 按
/// GL_UNSIGNED_BYTE×4 归一化读回)。
float packColorFloat(const std::array<float, 4>& c) {
    const uint32_t packed = packColorU32(c);
    float f;
    std::memcpy(&f, &packed, sizeof(f));
    return f;
}

void appendLineMesh(const TessellatedLine& line,
                    const Vec3& origin,
                    float packedColor,
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
        outVerts.push_back(packedColor);
    }
    outIndices.reserve(outIndices.size() + line.indices.size());
    for (uint32_t idx : line.indices) {
        outIndices.push_back(base + idx);
    }
}

/// 颜色表达式求值(P6b 数据驱动,镶嵌期上下文 = 属性,无 zoom)。
/// 求值失败/非颜色值 → 回落字面量(表达式永不让渲染断链)。
std::array<float, 4> resolveColor(
    const StyleExpression::Ptr& expr,
    const std::unordered_map<std::string, std::string>& properties,
    const std::array<float, 4>& fallback) {
    if (!expr) return fallback;
    const auto v = expr->evaluate(&properties, std::nan(""));
    if (!v || v->kind() != StyleValue::Kind::Color) return fallback;
    return v->color();
}

/// 字符串表达式求值(P6c 图形名,镶嵌期上下文 = 属性,无 zoom)。
/// 求值失败/非字符串 → 回落字面量。
std::string resolveString(
    const StyleExpression::Ptr& expr,
    const std::unordered_map<std::string, std::string>& properties,
    const std::string& fallback) {
    if (!expr) return fallback;
    const auto v = expr->evaluate(&properties, std::nan(""));
    if (!v || v->kind() != StyleValue::Kind::String) return fallback;
    return v->string();
}

/// 单个符号 quad 的几何/采样解析(P6c)。尺寸单位 = pointSizePx 的倍数:
/// 内置形状是边长 1 的正方 quad;位图图标高 1、宽按源图宽高比。
struct ResolvedSymbol {
    float shape = 0.0f;          ///< >=0 内置形状 id;<0 = 图集哨兵
    float halfWidthUnits = 0.5f; ///< quad 半宽(尺寸倍数)
    bool bottomAnchored = false; ///< true = quad 画在锚点上方
    /// 图集 uv 矩形(内置形状不用,fragment 走局部坐标)。
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

ResolvedSymbol resolveSymbol(const std::string& name,
                             SymbolAnchor anchor,
                             const IconAtlas* atlas) {
    ResolvedSymbol out;
    SymbolShape builtin = SymbolShape::Circle;
    const IconAtlas::Frame* frame = nullptr;
    if (!symbolShapeFromName(name, &builtin)) {
        // 非内置名 → 查位图图集;查不到(未注入/名字写错)回落 circle,
        // 让点仍然可见——图标缺失不该让要素凭空消失。
        frame = atlas ? atlas->frame(name) : nullptr;
    }
    if (frame) {
        out.shape = kSymbolShapeAtlas;
        const float aspect =
            frame->heightPx > 0.0f ? frame->widthPx / frame->heightPx : 1.0f;
        out.halfWidthUnits = 0.5f * aspect;
        out.u0 = frame->u0;
        out.v0 = frame->v0;
        out.u1 = frame->u1;
        out.v1 = frame->v1;
        out.bottomAnchored = anchor == SymbolAnchor::Bottom;
    } else {
        out.shape = static_cast<float>(static_cast<int>(builtin));
        out.bottomAnchored = anchor == SymbolAnchor::Bottom ||
                             (anchor == SymbolAnchor::Auto &&
                              symbolShapeIsBottomAnchored(builtin));
    }
    return out;
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

FeatureRenderLayer::FeatureRenderLayer(std::string layerId,
                                       RenderDevice* renderDevice,
                                       const Ellipsoid& ellipsoid,
                                       double bucketCellSizeRadians)
    : layerId_(std::move(layerId)),
      renderDevice_(renderDevice),
      ellipsoid_(ellipsoid),
      store_(bucketCellSizeRadians) {}

FeatureRenderLayer::~FeatureRenderLayer() = default;

void FeatureRenderLayer::setStyle(const FeatureRenderStyle& s) {
    style_ = s;
    // P6b 表达式语义校验:颜色 = 数据驱动(镶嵌期无 zoom 上下文,引用
    // zoom 的颜色表达式恒求值失败 → 直接剥离降级字面量并警告);宽度/
    // 尺寸 = zoom 驱动(每帧无属性上下文,引用属性同理)。
    auto sanitize = [this](StyleExpression::Ptr& expr, bool allowProperties,
                           bool allowZoom, const char* name) {
        if (!expr) return;
        if ((!allowZoom && expr->referencesZoom()) ||
            (!allowProperties && expr->referencesProperties())) {
            platformLog(LogLevel::Warning, "FeatureRenderLayer",
                        "style expr '%s' out of semantic scope "
                        "(%s), falling back to literal",
                        name,
                        allowZoom ? "properties-only 禁 zoom 之外引用"
                                  : "zoom-only 禁 properties 引用");
            expr = nullptr;
        }
    };
    sanitize(style_.fillColorExpr, true, false, "fillColor");
    sanitize(style_.lineColorExpr, true, false, "lineColor");
    sanitize(style_.pointColorExpr, true, false, "pointColor");
    sanitize(style_.lineWidthExpr, false, true, "lineWidth");
    sanitize(style_.pointSizeExpr, false, true, "pointSize");
    sanitize(style_.pointImageExpr, true, false, "pointImage");
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
    const TessellationContext& ctx,
    const Feature& feature,
    const AreaSampleFn& sample,
    std::vector<Cartographic>* outSteiner,
    double densifyMeters) {
    const double spacing = std::max(1.0, densifyMeters);
    auto clampHeight = [&](double lng, double lat) {
        // 无数据回落椭球面(对齐 no-fine-data-ellipsoid-fallback 约定)。
        const double ground =
            sample ? static_cast<double>(sample(lng, lat).value_or(0.0f))
                   : 0.0;
        return ground + ctx.style.heightOffset;
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
    const TessellationContext& ctx,
    const Feature& feature,
    const AreaSampleFn& sample,
    Vec3& origin,
    bool& hasOrigin,
    std::vector<float>& fillVerts,
    std::vector<uint32_t>& fillIndices,
    std::vector<float>& lineVerts,
    std::vector<uint32_t>& lineIndices,
    std::vector<float>& pointVerts,
    std::vector<uint32_t>& pointIndices,
    std::vector<float>& labelVerts,
    std::vector<uint32_t>& labelIndices,
    std::vector<LabelEntry>& labelEntries,
    VolumeCpuGroups& volumeGroups,
    VolumeCpuGroups& lineVolumeGroups) {
    // P6b 数据驱动色:镶嵌期逐要素求值(上下文 = 属性,无 zoom),失败
    // 回落图层字面量;打包 RGBA8 烘进顶点流。
    const std::array<float, 4> fillColor =
        resolveColor(ctx.style.fillColorExpr, feature.properties,
                     ctx.style.fillColor);
    const std::array<float, 4> lineColor =
        resolveColor(ctx.style.lineColorExpr, feature.properties,
                     ctx.style.lineColor);
    const std::array<float, 4> pointColor =
        resolveColor(ctx.style.pointColorExpr, feature.properties,
                     ctx.style.pointColor);
    const float fillColorPacked = packColorFloat(fillColor);
    const float lineColorPacked = packColorFloat(lineColor);
    const float pointColorPacked = packColorFloat(pointColor);

    // 贴地:预变换出细分+采样高度的副本(高度已含 offset),镶嵌时
    // heightOffset 传 0 防二次叠加;Absolute 走原几何 + offset。
    const bool clamp =
        ctx.style.altitudeMode == FeatureAltitudeMode::ClampToGround;
    // P6 方案 B:后端支持 stencil 分类 → clamp 面 fill 走挤出体双 pass
    // (像素级贴合,LOD 切换免重钳);不支持回落方案 A。
    const bool stencilFill =
        clamp && feature.type == GeometryType::Polygon &&
        ctx.supportsStencilClassification;
    // P6d:clamp 线(LineString + polygon outline)同走 stencil 双 pass
    // (墙带体,像素级贴地,宽度 VS 按眼深挤出);不支持回落方案 A ribbon。
    const bool stencilLine =
        clamp && ctx.supportsStencilClassification;
    std::vector<Cartographic> steinerPoints;
    Feature clampedStorage;
    const Feature* geometry = &feature;
    double tessHeightOffset = ctx.style.heightOffset;
    if (clamp) {
        // stencil fill 不需要内部 Steiner 撒点(fill 不再按网格采高)。
        // 细分密度解耦:stencil 线不靠细分防露头(贴地是像素级分类),
        // 细分只服务线形曲率 + 沿线高度采样(±margin 罩差),放宽到
        // ≥100m;方案 A 回落仍用 clampDensifyMeters(细分兼防扎地)。
        // 用户显式设得更粗时尊重更粗值。
        constexpr double kStencilLineDensifyMeters = 100.0;
        const double densify =
            stencilLine ? std::max(ctx.style.clampDensifyMeters,
                                   kStencilLineDensifyMeters)
                        : ctx.style.clampDensifyMeters;
        clampedStorage = prepareClampedFeature(
            ctx,
            feature, sample, stencilFill ? nullptr : &steinerPoints, densify);
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
            if (stencilFill) {
                // 体积从原始 footprint 出(2D 拓扑,高度由采样范围决定),
                // 按解析色归组(每组独立命令对,不同色互不污染)。
                appendFillVolume(ctx, feature, sample, fillColor, origin,
                                 hasOrigin, volumeGroups);
            } else {
                TessellatedFill fill = PolygonTessellator::tessellate(
                    *geometry, ctx.ellipsoid, tessHeightOffset,
                    steinerPoints.empty() ? nullptr : &steinerPoints);
                if (!fill.positions.empty() && !fill.fillIndices.empty()) {
                    ensureOrigin(fill.positions.front());
                    const uint32_t base =
                        static_cast<uint32_t>(fillVerts.size() / 4);
                    fillVerts.reserve(fillVerts.size() +
                                      fill.positions.size() * 4);
                    for (const Vec3& p : fill.positions) {
                        const Vec3 rel = p - origin;
                        fillVerts.push_back(static_cast<float>(rel.x()));
                        fillVerts.push_back(static_cast<float>(rel.y()));
                        fillVerts.push_back(static_cast<float>(rel.z()));
                        fillVerts.push_back(fillColorPacked);
                    }
                    for (uint32_t idx : fill.fillIndices) {
                        fillIndices.push_back(base + idx);
                    }
                }
            }
            // 外环 outline。孔环 outline 留后续。
            if (stencilLine) {
                // P6d:闭合墙带体(首尾 wrap)。
                appendLineVolume(ctx, geometry->rings.front(), /*closed=*/true,
                                 lineColor, origin, hasOrigin,
                                 lineVolumeGroups);
                break;
            }
            // 方案 A 闭合 ribbon。LineTessellator 契约只收 LineString
            // (有测试锁死),把外环包成临时 LineString。
            Feature outlineFeature;
            outlineFeature.type = GeometryType::LineString;
            outlineFeature.rings = {geometry->rings.front()};
            TessellatedLine outline = LineTessellator::tessellate(
                outlineFeature, ctx.ellipsoid, tessHeightOffset,
                /*closed=*/true);
            if (!outline.vertices.empty()) {
                ensureOrigin(outline.vertices.front().pos);
                appendLineMesh(outline, origin, lineColorPacked,
                               lineVerts, lineIndices);
            }
            break;
        }
        case GeometryType::LineString: {
            if (stencilLine) {
                // P6d:每条 ring 一条开放墙带体。
                for (const auto& ring : geometry->rings) {
                    appendLineVolume(ctx, ring, /*closed=*/false, lineColor,
                                     origin, hasOrigin, lineVolumeGroups);
                }
                break;
            }
            TessellatedLine line = LineTessellator::tessellate(
                *geometry, ctx.ellipsoid, tessHeightOffset,
                /*closed=*/false);
            if (!line.vertices.empty()) {
                ensureOrigin(line.vertices.front().pos);
                appendLineMesh(line, origin, lineColorPacked,
                               lineVerts, lineIndices);
            }
            break;
        }
        case GeometryType::Point: {
            // P5a/P6c 符号:每点 billboard quad(anchor 3f + offsetUnit 2f
            // + uv 2f + color 4B + shape 1f = 36B,quad 在顶点着色器按
            // u_pointSizePx 展开)。Point 几何 = rings[0][0];贴地时
            // geometry 已由预变换写好(采样 + offset)。
            if (geometry->rings.empty() || geometry->rings[0].empty()) break;
            const Cartographic& c = geometry->rings[0][0];
            const Vec3 anchor = ctx.ellipsoid.cartographicToCartesian(
                Cartographic(c.longitude(), c.latitude(),
                             c.height() + tessHeightOffset));
            ensureOrigin(anchor);
            const Vec3 rel = anchor - origin;
            const ResolvedSymbol sym = resolveSymbol(
                resolveString(ctx.style.pointImageExpr, feature.properties,
                              ctx.style.pointImage),
                ctx.style.pointAnchor, ctx.iconAtlas);
            const uint32_t base =
                static_cast<uint32_t>(pointVerts.size() / kPointVertexFloats);
            // corner ∈ {±1}²(x 右为正,y 上为正)。
            static constexpr float kCorners[4][2] = {
                {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
            for (const auto& corner : kCorners) {
                // 竖直对齐:居中 y∈[-0.5,0.5];底部锚定 y∈[0,1]。
                const float offsetY = sym.bottomAnchored
                                          ? (corner[1] + 1.0f) * 0.5f
                                          : corner[1] * 0.5f;
                // 图集通道 uv 取 frame 矩形(纹理 v 向下,屏幕 y 向上 →
                // 上边角取 v0);内置形状 uv 即 [-1,1]² 局部坐标。
                const float u = sym.shape < 0.0f
                                    ? (corner[0] < 0.0f ? sym.u0 : sym.u1)
                                    : corner[0];
                const float v = sym.shape < 0.0f
                                    ? (corner[1] > 0.0f ? sym.v0 : sym.v1)
                                    : corner[1];
                pointVerts.push_back(static_cast<float>(rel.x()));
                pointVerts.push_back(static_cast<float>(rel.y()));
                pointVerts.push_back(static_cast<float>(rel.z()));
                pointVerts.push_back(corner[0] * sym.halfWidthUnits);
                pointVerts.push_back(offsetY);
                pointVerts.push_back(u);
                pointVerts.push_back(v);
                pointVerts.push_back(pointColorPacked);
                pointVerts.push_back(sym.shape);
            }
            const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
            for (uint32_t idx : quad) pointIndices.push_back(base + idx);
            break;
        }
    }

    // ---- P5b/P5c 文字标注 ----
    // properties[labelProperty] 非空且字体就绪 → 锚点处 glyph quads
    // (32B:anchor+offsetPx+uv+opacity)。锚点:Point 本体/LineString 弧长
    // 中点/Polygon 环 bbox 中心;贴地时中心点单独采样。顶点 opacity 初始
    // 0(placement fade-in 起点),同时登记 LabelEntry 供逐帧避让。
    if (ctx.glyphAtlas && ctx.glyphAtlas->ready() &&
        !geometry->rings.empty() && !geometry->rings[0].empty()) {
        const auto propIt = feature.properties.find(ctx.style.labelProperty);
        if (propIt == feature.properties.end() || propIt->second.empty()) {
            return;
        }
        // 锚点地理坐标(高度语义:clamp 时 geometry 顶点已含 offset,中心
        // 点走 sample+offset;absolute 时统一 + tessHeightOffset)。
        Cartographic anchorCarto = geometry->rings[0][0];
        if (geometry->type == GeometryType::LineString) {
            // 弧长中点(§8.2:线锚点按测地弧长):累计相邻顶点 ECEF 弦长,
            // 取半程所在段线性插值。细分后段短,弦长≈弧长;插值跨反经线
            // 的段会走短端错侧,与既有镶嵌同限(demo 尺度不触及)。
            const auto& ring = geometry->rings[0];
            std::vector<double> cumulative(ring.size(), 0.0);
            for (size_t i = 1; i < ring.size(); ++i) {
                const Vec3 a = ctx.ellipsoid.cartographicToCartesian(ring[i - 1]);
                const Vec3 b = ctx.ellipsoid.cartographicToCartesian(ring[i]);
                cumulative[i] = cumulative[i - 1] + (b - a).length();
            }
            const double half = cumulative.back() * 0.5;
            anchorCarto = ring[ring.size() / 2];
            for (size_t i = 1; i < ring.size(); ++i) {
                if (cumulative[i] >= half) {
                    const double segLen = cumulative[i] - cumulative[i - 1];
                    const double t = segLen > 0.0
                        ? (half - cumulative[i - 1]) / segLen
                        : 0.0;
                    const Cartographic& p0 = ring[i - 1];
                    const Cartographic& p1 = ring[i];
                    anchorCarto = Cartographic(
                        p0.longitude() +
                            (p1.longitude() - p0.longitude()) * t,
                        p0.latitude() + (p1.latitude() - p0.latitude()) * t,
                        p0.height() + (p1.height() - p0.height()) * t);
                    break;
                }
            }
        } else if (geometry->type == GeometryType::Polygon) {
            double west = std::numeric_limits<double>::max();
            double east = std::numeric_limits<double>::lowest();
            double south = std::numeric_limits<double>::max();
            double north = std::numeric_limits<double>::lowest();
            for (const auto& c : geometry->rings[0]) {
                west = std::min(west, c.longitude());
                east = std::max(east, c.longitude());
                south = std::min(south, c.latitude());
                north = std::max(north, c.latitude());
            }
            const double lng = (west + east) * 0.5;
            const double lat = (south + north) * 0.5;
            const bool clampMode =
                ctx.style.altitudeMode == FeatureAltitudeMode::ClampToGround;
            const double h =
                clampMode
                    ? (sample ? static_cast<double>(
                                    sample(lng, lat).value_or(0.0f))
                              : 0.0) +
                          ctx.style.heightOffset
                    : geometry->rings[0][0].height();
            anchorCarto = Cartographic(lng, lat, h);
        }
        const Vec3 anchor = ctx.ellipsoid.cartographicToCartesian(Cartographic(
            anchorCarto.longitude(), anchorCarto.latitude(),
            anchorCarto.height() + tessHeightOffset));
        ensureOrigin(anchor);
        const Vec3 rel = anchor - origin;
        const float ax = static_cast<float>(rel.x());
        const float ay = static_cast<float>(rel.y());
        const float az = static_cast<float>(rel.z());

        // 布局:单行 LTR advance,水平居中,基线抬 labelOffsetPx。
        const float s =
            ctx.style.labelSizePx /
            static_cast<float>(GlyphAtlas::kGlyphPixelHeight);
        const std::vector<uint32_t> codepoints =
            GlyphAtlas::decodeUtf8(propIt->second);
        float totalAdvance = 0.0f;
        for (uint32_t cp : codepoints) {
            if (const GlyphAtlas::Glyph* g = ctx.glyphAtlas->ensureGlyph(cp)) {
                totalAdvance += g->advance * s;
            }
        }
        float penX = -totalAdvance * 0.5f;
        const float baseY = ctx.style.labelOffsetPx;
        const size_t entryVertexStart = labelVerts.size();
        for (uint32_t cp : codepoints) {
            const GlyphAtlas::Glyph* g = ctx.glyphAtlas->ensureGlyph(cp);
            if (!g) continue;
            if (g->hasBitmap) {
                const float x0 = penX + g->offsetX * s;
                const float x1 = x0 + g->width * s;
                const float yTop = baseY + g->offsetY * s;
                const float yBot = yTop - g->height * s;
                const uint32_t base =
                    static_cast<uint32_t>(labelVerts.size() / 8);
                const float corners[4][4] = {
                    {x0, yBot, g->u0, g->v1},
                    {x1, yBot, g->u1, g->v1},
                    {x1, yTop, g->u1, g->v0},
                    {x0, yTop, g->u0, g->v0}};
                for (const auto& c : corners) {
                    labelVerts.push_back(ax);
                    labelVerts.push_back(ay);
                    labelVerts.push_back(az);
                    labelVerts.push_back(c[0]);
                    labelVerts.push_back(c[1]);
                    labelVerts.push_back(0.0f);  // offset.z=opacity(placement 回写)
                    labelVerts.push_back(c[2]);
                    labelVerts.push_back(c[3]);
                }
                const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
                for (uint32_t idx : quad) labelIndices.push_back(base + idx);
            }
            penX += g->advance * s;
        }
        // 登记 placement 候选:碰撞盒 = 整行文字盒(行度量 ascent/descent
        // 换算标注字号)+ halo 外扩。空文本/字形全缺 → 无顶点不登记。
        if (labelVerts.size() > entryVertexStart) {
            LabelEntry entry;
            entry.featureId = feature.id;
            entry.anchorEcef = anchor;
            entry.boxMinXPx = -totalAdvance * 0.5f - ctx.style.labelHaloPx;
            entry.boxMaxXPx = totalAdvance * 0.5f + ctx.style.labelHaloPx;
            // descent() 已取正(基线下距离),下缘 = 基线减。
            entry.boxMinYPx =
                baseY - ctx.glyphAtlas->descent() * s - ctx.style.labelHaloPx;
            entry.boxMaxYPx =
                baseY + ctx.glyphAtlas->ascent() * s + ctx.style.labelHaloPx;
            entry.vertexFloatStart = entryVertexStart;
            entry.vertexFloatCount = labelVerts.size() - entryVertexStart;
            labelEntries.push_back(entry);
        }
    }
}

// ============================================================
// P6 stencil 贴地(方案 B):footprint 挤出水密体
// ============================================================

namespace {

/// 体积上下越出地形采样范围的保险余量(m)。采样缺失(椭球回落地形恒 0)
/// 时单靠它罩住;采样存在时叠加在 min/max 外侧,吸收粗网格漏峰。
constexpr double kVolumeMarginMeters = 120.0;

} // namespace

void FeatureRenderLayer::appendFillVolume(
    const TessellationContext& ctx,
    const Feature& feature,
    const AreaSampleFn& sample,
    const std::array<float, 4>& fillColor,
    Vec3& origin,
    bool& hasOrigin,
    VolumeCpuGroups& volumeGroups) {
    if (feature.rings.empty() || feature.rings.front().size() < 3) return;

    // P6b:按解析色归组(同色体积并集计数,组间独立命令对)。
    VolumeCpuGroup& group = volumeGroups[packColorU32(fillColor)];
    group.color = fillColor;
    std::vector<float>& volumeVerts = group.verts;
    std::vector<uint32_t>& volumeIndices = group.indices;

    // ---- 高度范围:环顶点 + 粗内部网格采样 min/max ± margin ----
    double minH = std::numeric_limits<double>::max();
    double maxH = std::numeric_limits<double>::lowest();
    auto probe = [&](double lng, double lat) {
        const double h =
            sample ? static_cast<double>(sample(lng, lat).value_or(0.0f))
                   : 0.0;
        minH = std::min(minH, h);
        maxH = std::max(maxH, h);
    };
    double west = std::numeric_limits<double>::max();
    double east = std::numeric_limits<double>::lowest();
    double south = std::numeric_limits<double>::max();
    double north = std::numeric_limits<double>::lowest();
    for (const auto& ring : feature.rings) {
        for (const auto& c : ring) {
            probe(c.longitude(), c.latitude());
            west = std::min(west, c.longitude());
            east = std::max(east, c.longitude());
            south = std::min(south, c.latitude());
            north = std::max(north, c.latitude());
        }
    }
    // 内部网格采样:面内山峰/深谷高于(低于)环顶点是常态(demo 面即横跨
    // 山体),只测环会漏 → 体顶不够高/体底不够低 → 分类在峰顶或谷底断面。
    // 网格步长跟随 clampDensifyMeters(与线路径同一密度语义),单边上限
    // kMaxGrid 防大面采样爆炸。**保证边界**:尺度大于网格步长的地形特征
    // 不会漏;更尖的特征仍靠 ±kVolumeMarginMeters 兜底(纯采样无法根治,
    // 根治需地形侧的区域 min/max 元数据)。
    const int kMaxGrid = 64;
    const double spacing = std::max(1.0, ctx.style.clampDensifyMeters);
    const double midLat = (south + north) * 0.5;
    const double lngSpanMeters =
        (east - west) * kEarthRadiusMeters * std::max(0.01, std::cos(midLat));
    const double latSpanMeters = (north - south) * kEarthRadiusMeters;
    const int gridX = std::clamp(
        static_cast<int>(std::ceil(lngSpanMeters / spacing)), 1, kMaxGrid);
    const int gridY = std::clamp(
        static_cast<int>(std::ceil(latSpanMeters / spacing)), 1, kMaxGrid);
    for (int gy = 0; gy <= gridY; ++gy) {
        for (int gx = 0; gx <= gridX; ++gx) {
            const double lng =
                west + (east - west) * gx / gridX;
            const double lat =
                south + (north - south) * gy / gridY;
            if (pointInRings2D(lng, lat, feature.rings)) probe(lng, lat);
        }
    }
    const double bottom = minH - kVolumeMarginMeters;
    const double top = maxH + kVolumeMarginMeters;

    // ---- 两层同拓扑 cap:压平高度后同一 2D 输入,CDT 确定性保证底/顶
    // 顶点一一对应(索引可复用) ----
    Feature flat;
    flat.id = feature.id;
    flat.type = GeometryType::Polygon;
    flat.rings.reserve(feature.rings.size());
    for (const auto& ring : feature.rings) {
        std::vector<Cartographic> flatRing;
        flatRing.reserve(ring.size());
        for (const auto& c : ring) {
            flatRing.emplace_back(c.longitude(), c.latitude(), 0.0);
        }
        flat.rings.push_back(std::move(flatRing));
    }
    const TessellatedFill capBottom =
        PolygonTessellator::tessellate(flat, ctx.ellipsoid, bottom);
    const TessellatedFill capTop =
        PolygonTessellator::tessellate(flat, ctx.ellipsoid, top);
    if (capBottom.positions.empty() || capBottom.fillIndices.empty() ||
        capTop.positions.size() != capBottom.positions.size()) {
        return;  // 退化/拓扑不一致(不应发生):放弃体积,宁缺勿错
    }

    if (!hasOrigin) {
        origin = capBottom.positions.front();
        hasOrigin = true;
    }

    const uint32_t base = static_cast<uint32_t>(volumeVerts.size() / 3);
    auto pushPos = [&](const Vec3& p) {
        const Vec3 rel = p - origin;
        volumeVerts.push_back(static_cast<float>(rel.x()));
        volumeVerts.push_back(static_cast<float>(rel.y()));
        volumeVerts.push_back(static_cast<float>(rel.z()));
    };
    for (const Vec3& p : capBottom.positions) pushPos(p);
    for (const Vec3& p : capTop.positions) pushPos(p);
    const uint32_t topOffset =
        static_cast<uint32_t>(capBottom.positions.size());

    // 底 cap 翻转绕向(外向下),顶 cap 原样(外向上)。INCR/DECR wrap 的
    // 非零计数对整体绕向翻转免疫,这里仍保持一致朝外,便于日后复用。
    const auto& tris = capBottom.fillIndices;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        volumeIndices.push_back(base + tris[i]);
        volumeIndices.push_back(base + tris[i + 2]);
        volumeIndices.push_back(base + tris[i + 1]);
    }
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        volumeIndices.push_back(base + topOffset + tris[i]);
        volumeIndices.push_back(base + topOffset + tris[i + 1]);
        volumeIndices.push_back(base + topOffset + tris[i + 2]);
    }

    // ---- 侧墙:从 cap 三角化提取**边界边**(无向计数恰为 1 的边),用
    // cap 自身顶点成墙。不能按原始 ring 走——编辑可以把面拖成自交,
    // PolygonTessellator 会做自交预分裂,预分裂后的真实轮廓与原始 ring
    // 路径不再一致,墙与 cap 对不上 → 体出悬边 → z-fail 计数错乱 →
    // fill 破碎/泄漏(真机复现,SelfIntersectingFillVolumeIsWatertight
    // 锁死)。走边界边还顺带正确处理孔环,并让墙与 cap 共享顶点(索引级
    // 水密,不再依赖"数值同源"的隐式约定)。 ----
    std::map<uint64_t, int> edgeUse;
    auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        const uint32_t lo = std::min(a, b);
        const uint32_t hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32) | hi;
    };
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        for (int e = 0; e < 3; ++e) {
            ++edgeUse[edgeKey(tris[i + e], tris[i + (e + 1) % 3])];
        }
    }
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = tris[i + e];
            const uint32_t b = tris[i + (e + 1) % 3];
            if (edgeUse[edgeKey(a, b)] != 1) continue;  // 内部边跳过
            // 有向边 a→b 取自 cap 三角形绕向 → 墙面朝外。
            const uint32_t ba = base + a;
            const uint32_t bb = base + b;
            const uint32_t ta = base + topOffset + a;
            const uint32_t tb = base + topOffset + b;
            volumeIndices.push_back(ba);
            volumeIndices.push_back(bb);
            volumeIndices.push_back(tb);
            volumeIndices.push_back(ba);
            volumeIndices.push_back(tb);
            volumeIndices.push_back(ta);
        }
    }
}

void FeatureRenderLayer::appendLineVolume(
    const TessellationContext& ctx,
    const std::vector<Cartographic>& points,
    bool closed,
    const std::array<float, 4>& lineColor,
    Vec3& origin,
    bool& hasOrigin,
    VolumeCpuGroups& lineVolumeGroups) {
    // 闭合环:末点 = 首点时去重,横截面靠 wrap 共享。
    size_t n = points.size();
    if (closed && n >= 2 &&
        points.front().longitude() == points.back().longitude() &&
        points.front().latitude() == points.back().latitude()) {
        --n;
    }
    if (n < 2) return;
    if (closed && n < 3) closed = false;  // 退化环按开放线处理

    // ---- 逐点中心/上方向(点高度已含采样 + heightOffset;±margin 吞掉
    // offset 差异,stencil 染色本身与抬升无关) ----
    std::vector<Vec3> centers(n);
    std::vector<Vec3> ups(n);
    for (size_t i = 0; i < n; ++i) {
        centers[i] = ctx.ellipsoid.cartographicToCartesian(points[i]);
        ups[i] = ctx.ellipsoid.geodeticSurfaceNormal(points[i]);
    }

    // 切平面内单位方向(对齐 cesium computeVertexMiterNormal 的正交化,
    // 见 .ref/cesiumjs/groundpolyline/GroundPolylineGeometry.js:380-438)。
    auto tangentDir = [](const Vec3& from, const Vec3& to,
                         const Vec3& up) -> Vec3 {
        Vec3 d = to - from;
        d = d - up * d.dot(up);
        const double len = d.length();
        if (len < 1e-9) return Vec3::zero();
        return d / len;
    };

    // miter 长度下限 = 屏幕线 shader 同款 miter-limit 4。尖角只会宽度过冲,
    // 横截面共享保证不会破洞(与 cesium breakMiter 不同,那是平面裁剪需要)。
    constexpr double kMiterMin = 0.25;

    // ---- 逐点挤出向量:extrude = 右向 miter 方向 × miter 缩放(左侧顶点
    // 取负)。方向/缩放全烘进向量,VS 只做 pos + extrude * halfWidthMeters。
    std::vector<Vec3> extrudes(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3& up = ups[i];
        const bool hasIn = closed || i > 0;
        const bool hasOut = closed || i + 1 < n;
        const Vec3 dirIn =
            hasIn ? tangentDir(centers[(i + n - 1) % n], centers[i], up)
                  : Vec3::zero();
        const Vec3 dirOut =
            hasOut ? tangentDir(centers[i], centers[(i + 1) % n], up)
                   : Vec3::zero();
        const bool okIn = dirIn.lengthSquared() > 0.25;
        const bool okOut = dirOut.lengthSquared() > 0.25;
        Vec3 lateral;
        double scale = 1.0;
        if (okIn && okOut) {
            Vec3 bisect = dirIn + dirOut;
            if (bisect.lengthSquared() < 1e-12) {
                // 180° 折返:任取一段右向,不放缩。
                lateral = dirIn.cross(up).normalized();
            } else {
                bisect = bisect.normalized();
                lateral = bisect.cross(up).normalized();
                const Vec3 rightOut = dirOut.cross(up).normalized();
                scale = 1.0 / std::max(lateral.dot(rightOut), kMiterMin);
            }
        } else if (okIn) {
            lateral = dirIn.cross(up).normalized();
        } else if (okOut) {
            lateral = dirOut.cross(up).normalized();
        } else {
            // 相邻点重合退化:零挤出(该横截面塌成线,仍水密)。
            lateral = Vec3::zero();
        }
        extrudes[i] = lateral * scale;
    }

    // ---- 沿线累计弧长(m,chord 累加;dash 切分的世界米制参数) ----
    std::vector<double> lengthSoFar(n, 0.0);
    for (size_t i = 1; i < n; ++i) {
        lengthSoFar[i] =
            lengthSoFar[i - 1] + (centers[i] - centers[i - 1]).length();
    }
    const double totalLength =
        lengthSoFar[n - 1] +
        (closed ? (centers[0] - centers[n - 1]).length() : 0.0);
    if (totalLength <= 0.0) return;

    VolumeCpuGroup& group = lineVolumeGroups[packColorU32(lineColor)];
    group.color = lineColor;
    std::vector<float>& verts = group.verts;
    std::vector<uint32_t>& indices = group.indices;

    // ---- 沿线任意里程处的截面样本。段内**线性插值**(而非重算 miter):
    // 保证切分出的划体恰是原连续墙带的一个子段,几何逐点吻合光栅化插值
    // 结果,dash 切口不会改变线的形状/宽度。 ----
    struct SectionSample {
        Vec3 center;
        Vec3 up;
        Vec3 extrude;
    };
    auto sectionAt = [&](size_t i) -> SectionSample {
        return {centers[i], ups[i], extrudes[i]};
    };
    auto sampleAlong = [&](double s) -> SectionSample {
        // 定位 s 所在段 [i, i+1)(闭环末段回绕到 0)。
        size_t i = 0;
        while (i + 1 < n && lengthSoFar[i + 1] <= s) ++i;
        const size_t j = (i + 1) % n;
        const double segStart = lengthSoFar[i];
        const double segEnd =
            (i + 1 < n) ? lengthSoFar[i + 1] : totalLength;
        const double segLen = segEnd - segStart;
        const double t =
            segLen > 1e-9 ? std::clamp((s - segStart) / segLen, 0.0, 1.0)
                          : 0.0;
        SectionSample out;
        out.center = centers[i] + (centers[j] - centers[i]) * t;
        const Vec3 up = ups[i] + (ups[j] - ups[i]) * t;
        out.up = up.lengthSquared() > 1e-18 ? up.normalized() : ups[i];
        out.extrude = extrudes[i] + (extrudes[j] - extrudes[i]) * t;
        return out;
    };

    // ---- 发射一条墙带(每截面 4 顶点:0 底左 1 底右 2 顶左 3 顶右;
    // 段间底/顶/左/右各 1 quad;wrap=false 补首尾端 cap)。顶点全共享 →
    // 任意边恰被 2 三角引用(水密,z-fail 双面计数的前提)。 ----
    auto emitStrip = [&](const std::vector<SectionSample>& secs, bool wrap) {
        const size_t sectionCount = secs.size();
        if (sectionCount < 2) return;
        const uint32_t base = static_cast<uint32_t>(verts.size() / 6);
        for (const SectionSample& sec : secs) {
            const Vec3 bottom = sec.center - sec.up * kVolumeMarginMeters;
            const Vec3 top = sec.center + sec.up * kVolumeMarginMeters;
            if (!hasOrigin) {
                origin = bottom;
                hasOrigin = true;
            }
            auto pushVert = [&](const Vec3& p, double side) {
                const Vec3 rel = p - origin;
                const Vec3 ext = sec.extrude * side;
                verts.push_back(static_cast<float>(rel.x()));
                verts.push_back(static_cast<float>(rel.y()));
                verts.push_back(static_cast<float>(rel.z()));
                verts.push_back(static_cast<float>(ext.x()));
                verts.push_back(static_cast<float>(ext.y()));
                verts.push_back(static_cast<float>(ext.z()));
            };
            pushVert(bottom, -1.0);
            pushVert(bottom, 1.0);
            pushVert(top, -1.0);
            pushVert(top, 1.0);
        }
        auto vi = [&](size_t section, int corner) -> uint32_t {
            return base + static_cast<uint32_t>(section % sectionCount) * 4 +
                   static_cast<uint32_t>(corner);
        };
        auto pushQuad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(d);
        };
        const size_t segCount = wrap ? sectionCount : sectionCount - 1;
        for (size_t s = 0; s < segCount; ++s) {
            const size_t a = s;
            const size_t b = s + 1;
            pushQuad(vi(a, 0), vi(a, 1), vi(b, 1), vi(b, 0));  // 底
            pushQuad(vi(a, 3), vi(a, 2), vi(b, 2), vi(b, 3));  // 顶
            pushQuad(vi(a, 0), vi(b, 0), vi(b, 2), vi(a, 2));  // 左墙
            pushQuad(vi(a, 1), vi(a, 3), vi(b, 3), vi(b, 1));  // 右墙
        }
        if (!wrap) {
            const size_t last = sectionCount - 1;
            pushQuad(vi(0, 0), vi(0, 2), vi(0, 3), vi(0, 1));        // 首 cap
            pushQuad(vi(last, 0), vi(last, 1), vi(last, 3),
                     vi(last, 2));                                   // 尾 cap
        }
    };

    // ---- dash:**几何切分**(不在 FS 判里程)。色 pass 光栅化的是体、
    // 着色的却是地形像素,任何从体面插值来的里程在侧视低角度下都有百米
    // 级视差且相邻像素混取不同面 → 花纹撕裂(真机复现)。改为每一「划」
    // 生成独立封闭墙带体、空隙不出几何:dash 边界 = 几何边界,零视差、
    // 像素级锐利,并省掉空隙处的 overdraw。
    // (对照:cesium PolylineDashMaterial.glsl:22-25 用 gl_FragCoord 旋转
    // 取相位规避视差,但花纹锚在屏幕上、相机一动就在地面游动;maplibre
    // line_sdf 的精确里程来自「光栅化的就是线本身」,我们给不了。) ----
    const double period = static_cast<double>(ctx.style.lineDashPeriodMeters);
    const double onFraction = std::clamp(
        static_cast<double>(ctx.style.lineDashOnFraction), 0.0, 1.0);
    /// 划段短于此长度直接跳过(避免退化体与顶点爆炸)。
    constexpr double kMinDashMeters = 0.5;
    /// 单条线的划数上限(period 远小于线长时的护栏,超限退化实线)。
    constexpr double kMaxDashCount = 4096.0;
    const bool dashed = period > 0.0 && onFraction > 0.0 &&
                        onFraction < 1.0 &&
                        totalLength / period <= kMaxDashCount;

    if (!dashed) {
        std::vector<SectionSample> secs;
        secs.reserve(n);
        for (size_t i = 0; i < n; ++i) secs.push_back(sectionAt(i));
        emitStrip(secs, closed);
        return;
    }

    // 闭环把周期微调成整数节:seam 处花纹连续(开放线保持标称周期)。
    double effPeriod = period;
    if (closed) {
        const double cycles = std::max(1.0, std::round(totalLength / period));
        effPeriod = totalLength / cycles;
    }
    const double onLength = effPeriod * onFraction;

    std::vector<SectionSample> secs;
    for (double start = 0.0; start < totalLength - 1e-6;
         start += effPeriod) {
        const double end = std::min(start + onLength, totalLength);
        if (end - start < kMinDashMeters) continue;
        secs.clear();
        secs.push_back(sampleAlong(start));
        // 划内的原折线点必须保留,否则划会直线穿过弯道。
        for (size_t i = 0; i < n; ++i) {
            const double s = lengthSoFar[i];
            if (s > start + 1e-6 && s < end - 1e-6) {
                secs.push_back(sectionAt(i));
            }
        }
        secs.push_back(sampleAlong(end));
        emitStrip(secs, /*wrap=*/false);
    }
}


bool FeatureRenderLayer::uploadBucketGpu(
    const Vec3& origin,
    const std::vector<float>& fillVerts,
    const std::vector<uint32_t>& fillIndices,
    const std::vector<float>& lineVerts,
    const std::vector<uint32_t>& lineIndices,
    const std::vector<float>& pointVerts,
    const std::vector<uint32_t>& pointIndices,
    std::vector<float>&& labelVerts,
    const std::vector<uint32_t>& labelIndices,
    std::vector<LabelEntry>&& labelEntries,
    const VolumeCpuGroups& volumeGroups,
    const VolumeCpuGroups& lineVolumeGroups,
    BucketGpu& out) const {
    out = BucketGpu{};
    out.origin = origin;
    auto uploadGroups =
        [&](const VolumeCpuGroups& cpu,
            std::vector<BucketGpu::VolumeGroupGpu>& gpuOut) {
            for (const auto& [colorKey, group] : cpu) {
                if (group.indices.empty()) continue;
                BucketGpu::VolumeGroupGpu gpu;
                gpu.color = group.color;
                gpu.vertexBuffer = makeBuffer(renderDevice_,
                                              group.verts.data(),
                                              group.verts.size() *
                                                  sizeof(float),
                                              BufferDesc::Type::Vertex);
                gpu.indexBuffer = makeBuffer(renderDevice_,
                                             group.indices.data(),
                                             group.indices.size() *
                                                 sizeof(uint32_t),
                                             BufferDesc::Type::Index);
                if (!gpu.vertexBuffer || !gpu.indexBuffer) continue;
                gpu.indexCount = static_cast<int>(group.indices.size());
                gpuOut.push_back(std::move(gpu));
            }
        };
    uploadGroups(volumeGroups, out.volumeGroups);
    uploadGroups(lineVolumeGroups, out.lineVolumeGroups);
    if (!labelIndices.empty()) {
        out.labelVertexBuffer = makeBuffer(
            renderDevice_, labelVerts.data(),
            labelVerts.size() * sizeof(float), BufferDesc::Type::Vertex);
        out.labelIndexBuffer = makeBuffer(
            renderDevice_, labelIndices.data(),
            labelIndices.size() * sizeof(uint32_t), BufferDesc::Type::Index);
        if (out.labelVertexBuffer && out.labelIndexBuffer) {
            out.labelIndexCount = static_cast<int>(labelIndices.size());
            // CPU 副本随桶常驻:placement 改 opacity 分量后整桶重传。
            out.labelVertsCpu = std::move(labelVerts);
            out.labelEntries = std::move(labelEntries);
        }
    }
    if (!pointIndices.empty()) {
        out.pointVertexBuffer = makeBuffer(
            renderDevice_, pointVerts.data(),
            pointVerts.size() * sizeof(float), BufferDesc::Type::Vertex);
        out.pointIndexBuffer = makeBuffer(
            renderDevice_, pointIndices.data(),
            pointIndices.size() * sizeof(uint32_t), BufferDesc::Type::Index);
        if (out.pointVertexBuffer && out.pointIndexBuffer) {
            out.pointIndexCount = static_cast<int>(pointIndices.size());
        }
    }
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
    return out.fillIndexCount > 0 || out.lineIndexCount > 0 ||
           out.pointIndexCount > 0 || out.labelIndexCount > 0 ||
           !out.volumeGroups.empty() || !out.lineVolumeGroups.empty();
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
    std::vector<float> pointVerts;     // kPointVertexFloats/顶点
    std::vector<uint32_t> pointIndices;
    std::vector<float> labelVerts;     // 8 float/顶点(anchor+offsetPx+uv+opacity)
    std::vector<uint32_t> labelIndices;
    std::vector<LabelEntry> labelEntries;
    VolumeCpuGroups volumeGroups;      // P6 stencil 挤出体(按色分组)
    VolumeCpuGroups lineVolumeGroups;  // P6d stencil 线墙带(按色分组)
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
        tessellateFeatureInto(tessellationContext(), *feature, sample,
                              origin, hasOrigin,
                              fillVerts, fillIndices, lineVerts, lineIndices,
                              pointVerts, pointIndices,
                              labelVerts, labelIndices, labelEntries,
                              volumeGroups, lineVolumeGroups);
    }

    if (fillIndices.empty() && lineIndices.empty() && pointIndices.empty() &&
        labelIndices.empty() && volumeGroups.empty() &&
        lineVolumeGroups.empty()) {
        buckets_.erase(key);
        return;
    }

    BucketGpu gpu;
    if (!uploadBucketGpu(origin, fillVerts, fillIndices,
                         lineVerts, lineIndices,
                         pointVerts, pointIndices,
                         std::move(labelVerts), labelIndices,
                         std::move(labelEntries),
                         volumeGroups, lineVolumeGroups, gpu)) {
        // buffer 创建失败:丢弃本桶,脏区已消费 → 下次编辑该桶时重试。
        buckets_.erase(key);
        return;
    }
    buckets_[key] = std::move(gpu);
}

// ================= E1:MVT 瓦片桶(worker 全链镶嵌) =================

FeatureRenderLayer::TileMeshCpu FeatureRenderLayer::tessellateTileMesh(
    const TessellationContext& ctx, const std::vector<Feature>& features) {
    TileMeshCpu mesh;
    // v1 只收 fill/line;point/label/stencil 体的产物接在这些临时容器里,
    // 出了本函数即丢弃(见头文件 v1 边界说明)。**不能**图省事传同一组
    // 容器 —— point 顶点混进 fill 流会让 stride 对不上,画出乱码三角。
    std::vector<float> pointVerts;
    std::vector<uint32_t> pointIndices;
    std::vector<float> labelVerts;
    std::vector<uint32_t> labelIndices;
    std::vector<LabelEntry> labelEntries;
    VolumeCpuGroups volumeGroups;
    VolumeCpuGroups lineVolumeGroups;

    // 贴地采样器留空:worker 拿不到地形瓦片注册表(那是渲染线程状态),
    // v1 底图走 Absolute。传空 = 走原几何 + heightOffset。
    const AreaSampleFn noSample;

    for (const Feature& feature : features) {
        tessellateFeatureInto(ctx, feature, noSample, mesh.origin,
                              mesh.hasOrigin, mesh.fillVerts, mesh.fillIndices,
                              mesh.lineVerts, mesh.lineIndices, pointVerts,
                              pointIndices, labelVerts, labelIndices,
                              labelEntries, volumeGroups, lineVolumeGroups);
    }
    return mesh;
}

void FeatureRenderLayer::commitTileMesh(const TileKey& key, TileMeshCpu&& mesh) {
    if (mesh.empty() || !mesh.hasOrigin) {
        dropTileMesh(key);
        return;
    }
    // 整瓦原子替换:先建好新 GPU 资源,成功了才换掉旧的。中途失败保留旧瓦
    // 而不是留半张 —— 半张瓦片在画面上是缺口,比旧数据糟。
    BucketGpu gpu;
    static const std::vector<float> kNoVerts;
    static const std::vector<uint32_t> kNoIndices;
    VolumeCpuGroups noVolumes;
    if (!uploadBucketGpu(mesh.origin, mesh.fillVerts, mesh.fillIndices,
                         mesh.lineVerts, mesh.lineIndices, kNoVerts, kNoIndices,
                         std::vector<float>(), kNoIndices,
                         std::vector<LabelEntry>(), noVolumes, noVolumes, gpu)) {
        return;
    }
    tileBuckets_[key] = std::move(gpu);
}

void FeatureRenderLayer::dropTileMesh(const TileKey& key) {
    tileBuckets_.erase(key);
}

void FeatureRenderLayer::buildRenderCommands(const FrameState& frameState,
                                             Renderer& renderer,
                                             RenderCommandList& commands) {
    if (!visible_ || !renderDevice_) return;
    if (!frameState.camera) return;

    // 文字标注(P5b):缓存图集指针(重镶/预览路径无 Renderer 引用);字体
    // 就绪状态翻转 → 全部桶重镶补标注(字体注入通常晚于要素导入)。
    // 图标图集(P6c)同构:图标注入通常也晚于要素导入,代次变化 → 重镶
    // 补 uv(顶点里烘的是 frame 的 uv 与宽高比,新图标不重镶就画不出)。
    iconAtlas_ = renderer.iconAtlas();
    const uint64_t iconRevision = iconAtlas_ ? iconAtlas_->revision() : 0;
    glyphAtlas_ = renderer.glyphAtlas();
    const bool atlasReady = glyphAtlas_ && glyphAtlas_->ready();
    if (atlasReady != lastAtlasReady_ || iconRevision != lastIconRevision_) {
        lastAtlasReady_ = atlasReady;
        lastIconRevision_ = iconRevision;
        std::vector<BucketKey> keys;
        keys.reserve(buckets_.size());
        for (const auto& entry : buckets_) keys.push_back(entry.first);
        for (BucketKey key : keys) rebuildBucket(key);
        previewDirty_ = true;
    }

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
        std::vector<float> pointVerts;
        std::vector<uint32_t> pointIndices;
        std::vector<float> labelVerts;
        std::vector<uint32_t> labelIndices;
        std::vector<LabelEntry> labelEntries;
        VolumeCpuGroups volumeGroups;
        VolumeCpuGroups lineVolumeGroups;
        Vec3 origin = Vec3::zero();
        bool hasOrigin = false;
        tessellateFeatureInto(tessellationContext(), previewFeature,
                              makeClampSampler(previewRings_),
                              origin, hasOrigin,
                              fillVerts, fillIndices,
                              lineVerts, lineIndices,
                              pointVerts, pointIndices,
                              labelVerts, labelIndices, labelEntries,
                              volumeGroups, lineVolumeGroups);
        if (hasOrigin) {
            previewGpuValid_ = uploadBucketGpu(
                origin, fillVerts, fillIndices, lineVerts, lineIndices,
                pointVerts, pointIndices, std::move(labelVerts),
                labelIndices, std::move(labelEntries),
                volumeGroups, lineVolumeGroups, previewGpu_);
        }
    }

    // 视口桶裁剪:命令生成与标签避让只覆盖地平线圆内的桶(见
    // visibleBucketKeys)。视野外桶保留 GPU 资源、跳过每帧成本——帧成本
    // 从"总桶数"收敛到"可见桶数"。
    const std::vector<BucketKey> visibleKeys = visibleBucketKeys(frameState);

    // P5c:逐帧标签避让 placement + fade 回写(在命令生成前,顶点流为准)。
    updateLabelPlacement(frameState, visibleKeys);

    if (buckets_.empty() && tileBuckets_.empty() && !previewGpuValid_) return;

    // E1:MVT 瓦片桶先发(垫底,与 store 路径同一命令层)。可见性由上游
    // (VectorTileTree 的视口选择)负责 —— 驻留的瓦片桶本就是「本帧该画的」,
    // 这里再套一遍空间桶可见性判定是重复且会误杀(瓦片矩形与桶网格不对齐)。
    for (const auto& entry : tileBuckets_) {
        appendBucketCommands(entry.second, frameState, renderer, commands);
    }

    for (BucketKey key : visibleKeys) {
        auto it = buckets_.find(key);
        if (it == buckets_.end()) continue;
        appendBucketCommands(it->second, frameState, renderer, commands);
    }
    if (previewFeatureId_ != kInvalidFeatureId && previewGpuValid_) {
        appendBucketCommands(previewGpu_, frameState, renderer, commands);
    }
}

std::vector<BucketKey> FeatureRenderLayer::visibleBucketKeys(
    const FrameState& frameState) const {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kHalfPi = 0.5 * kPi;
    if (!frameState.camera) {
        // 防御:无相机退化为全桶(与接线前行为一致)。
        std::vector<BucketKey> all;
        all.reserve(buckets_.size());
        for (const auto& [key, gpu] : buckets_) all.push_back(key);
        std::sort(all.begin(), all.end());
        return all;
    }

    const Cartographic camCarto =
        ellipsoid_.cartesianToCartographic(frameState.camera->position());
    const Vec3& radii = ellipsoid_.radii();
    const double minRadius =
        std::min(radii.x(), std::min(radii.y(), radii.z()));
    const double camAltitude = std::max(0.0, camCarto.height());
    // 要素侧地平线延伸:贴地要素含地形起伏 + heightOffset,上界取 10km
    // (珠峰 8.8km + 余量)。两侧地平线角相加 = "相机与要素互见"的经典条件。
    constexpr double kMaxFeatureAltitudeMeters = 10000.0;
    const double theta =
        std::acos(std::clamp(minRadius / (minRadius + camAltitude), 0.0, 1.0)) +
        std::acos(std::clamp(
            minRadius / (minRadius + kMaxFeatureAltitudeMeters), 0.0, 1.0));

    double south = camCarto.latitude() - theta;
    double north = camCarto.latitude() + theta;
    double west, east;
    if (south <= -kHalfPi || north >= kHalfPi) {
        // 圆覆盖极点:该侧纬度到极,经度必须全量。
        west = -kPi;
        east = kPi;
    } else {
        // 球冠经度包围界:Δλ = asin(sinθ / cos(lat₀))。本分支保证
        // |lat₀|+θ < π/2(否则走上面极点分支),asin 入参必 < 1。
        const double halfLngSpan = std::asin(std::clamp(
            std::sin(theta) / std::cos(camCarto.latitude()), 0.0, 1.0));
        west = camCarto.longitude() - halfLngSpan;
        east = camCarto.longitude() + halfLngSpan;
    }
    south = std::max(south, -kHalfPi);
    north = std::min(north, kHalfPi);

    std::vector<BucketKey> keys;
    auto queryRect = [&](double w, double e) {
        for (BucketKey key :
             store_.bucketsInView(Rectangle(w, south, e, north))) {
            keys.push_back(key);
        }
    };
    if (east - west >= 2.0 * kPi) {
        queryRect(-kPi, kPi);
    } else if (west < -kPi) {
        // 反经线跨界:拆两段(桶网格是裸平面 AABB,不懂 wrap)。
        queryRect(-kPi, east);
        queryRect(west + 2.0 * kPi, kPi);
    } else if (east > kPi) {
        queryRect(west, kPi);
        queryRect(-kPi, east - 2.0 * kPi);
    } else {
        queryRect(west, east);
    }
    // 去重(oversized 桶在拆段查询时两段都会返回)。
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

void FeatureRenderLayer::updateLabelPlacement(
    const FrameState& frameState,
    const std::vector<BucketKey>& visibleKeys) {
    // collect:可见桶 + 预览的 LabelEntry → 候选。视野外桶不进候选:
    // 它们的 fade 状态由 placement 状态机按"消失要素"清扫,重入视野按
    // 新候选淡入;其顶点 opacity 停留旧值无妨——桶本身不出命令。
    std::vector<LabelCandidate> candidates;
    auto collect = [&](const BucketGpu& gpu) {
        for (const LabelEntry& e : gpu.labelEntries) {
            LabelCandidate c;
            c.featureId = e.featureId;
            c.anchorEcef = e.anchorEcef;
            c.boxMinXPx = e.boxMinXPx;
            c.boxMinYPx = e.boxMinYPx;
            c.boxMaxXPx = e.boxMaxXPx;
            c.boxMaxYPx = e.boxMaxYPx;
            candidates.push_back(c);
        }
    };
    for (BucketKey key : visibleKeys) {
        auto it = buckets_.find(key);
        if (it != buckets_.end()) collect(it->second);
    }
    if (previewGpuValid_) collect(previewGpu_);

    const Camera& cam = *frameState.camera;
    LabelPlacement::FrameInput in;
    in.viewProj = cam.viewProjectionMatrix(
        static_cast<double>(frameState.viewportWidthPixels),
        static_cast<double>(frameState.viewportHeightPixels));
    in.cameraEcef = cam.position();
    in.ellipsoidRadii = ellipsoid_.radii();
    in.viewportWidthPx = frameState.viewportWidthPixels;
    in.viewportHeightPx = frameState.viewportHeightPixels;
    in.deltaSeconds = frameState.deltaSeconds;
    // 候选为空也要跑:fade 状态机清扫已消失要素。
    labelPlacement_.update(in, candidates);

    // commit 回写:每帧按 appliedOpacity vs 当前值的偏差同步,**不依赖
    // update() 的变化位** —— 桶重镶(贴地 revision/字体翻转/编辑)会把顶点
    // 流 opacity 重置 0,而 fade 状态可能早已收敛"无变化",若以变化位做
    // 早退,重镶后的桶永远不再回写(真机曾表现为标签 10s 后集体隐形)。
    // 稳态下逐 entry 比较即免写,成本 O(标签数) 浮点比较。
    auto apply = [&](BucketGpu& gpu) {
        bool dirty = false;
        for (LabelEntry& e : gpu.labelEntries) {
            const float op = labelPlacement_.opacity(e.featureId);
            if (op == e.appliedOpacity) continue;
            // 顶点 8 float,opacity 在 offset.z(下标 5)。
            for (size_t i = e.vertexFloatStart + 5;
                 i < e.vertexFloatStart + e.vertexFloatCount; i += 8) {
                gpu.labelVertsCpu[i] = op;
            }
            e.appliedOpacity = op;
            dirty = true;
        }
        if (dirty && gpu.labelVertexBuffer) {
            const bool ok = renderDevice_->updateBuffer(
                gpu.labelVertexBuffer.get(), 0, gpu.labelVertsCpu.data(),
                gpu.labelVertsCpu.size() * sizeof(float));
            if (!ok) {
                // 上传断链 = 标签隐形(顶点 opacity 停 0),必须可见。
                platformLog(LogLevel::Warning, "FeatureRenderLayer",
                            "label opacity updateBuffer FAILED size=%zu",
                            gpu.labelVertsCpu.size() * sizeof(float));
            }
        }
    };
    for (BucketKey key : visibleKeys) {
        auto it = buckets_.find(key);
        if (it != buckets_.end()) apply(it->second);
    }
    if (previewGpuValid_) apply(previewGpu_);
}

// T2:把地形深度纹理与遮挡参数挂到符号命令上。
//
// 纹理**恒占 textures[1]**(后端按下标 1:1 绑纹理单元;标注的字形图集、点的
// 图标图集各自占 [0])。通路不可用时挂 nullptr 并把 enabled 置 0 —— shader
// 里整条判定早退,符号回到原 u_depthPushNdc 行为,零回归。
void FeatureRenderLayer::appendTerrainOcclusion(const Renderer& renderer,
                                                RenderCommand& cmd) const {
    const Renderer::TerrainOcclusionParams& occ = renderer.terrainOcclusion();
    const bool enabled = occ.depthTexture != nullptr;
    cmd.textures.push_back(occ.depthTexture);
    cmd.uniforms["u_terrainOcclusion"] = {
        enabled ? 1.0f : 0.0f,
        occ.nearPlaneMeters,
        occ.farPlaneMeters,
        occ.biasMeters};
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

    // P6b zoom 驱动宽度/尺寸:每帧按相机大地高求 zoom(web 墨卡托惯例
    // zoom ≈ log2(赤道周长 4e7m / 视高),z0 ≈ 全球一屏),表达式求值进
    // uniform;求值失败/非数值回落字面量。
    const double camHeight =
        ellipsoid_.cartesianToCartographic(cam.position()).height();
    // 高空符号深度顶近平面(语义见 FeatureRenderStyle 字段注释)。0.9999
    // 而非 1.0:留一线近平面余量,避免恰在 near 上被裁。
    constexpr float kSymbolDepthPushNdc = 0.9999f;
    const float symbolDepthPush =
        (style_.symbolDepthPushCameraHeightMeters > 0.0f &&
         camHeight > style_.symbolDepthPushCameraHeightMeters)
            ? kSymbolDepthPushNdc
            : 0.0f;
    float lineWidthPx = style_.lineWidthPx;
    float pointSizePx = style_.pointSizePx;
    if (style_.lineWidthExpr || style_.pointSizeExpr) {
        const double zoomLevel = std::min(
            24.0,
            std::max(0.0, std::log2(4.0e7 / std::max(1.0, camHeight))));
        auto evalNumber = [&](const StyleExpression::Ptr& expr,
                              float fallback) -> float {
            if (!expr) return fallback;
            const auto v = expr->evaluate(nullptr, zoomLevel);
            if (!v || v->kind() != StyleValue::Kind::Number) return fallback;
            return static_cast<float>(v->number());
        };
        lineWidthPx = evalNumber(style_.lineWidthExpr, lineWidthPx);
        pointSizePx = evalNumber(style_.pointSizeExpr, pointSizePx);
    }

    {
        // 双精度 compose 后降 float(RTE):顶点已相对 origin,mvp 吸收平移。
        const glm::dmat4 model = glm::translate(
            glm::dmat4(1.0),
            glm::dvec3(gpu.origin.x(), gpu.origin.y(), gpu.origin.z()));
        const glm::mat4 mvp = glm::mat4(viewProj * model);
        std::vector<float> mvpUniform(16);
        std::memcpy(mvpUniform.data(), glm::value_ptr(mvp),
                    16 * sizeof(float));

        // P6 stencil 贴地(方案 B):体积按解析色分组(P6b),每组一对相邻
        // 命令。stable_sort 按 order(29)保持插入序 → 体 pass 必在色 pass
        // 前;色 pass op ZERO 顺手清零,组间不串。组内多要素并集计数。
        // **契约**:色 pass 恒用 pos-only + uniform 纯色。分类着色的是地形
        // 像素、光栅化的却是体表面,任何"从体面插值 varying 再决定 fragment
        // 外观"的做法(pattern/渐变/沿线里程)在侧视下都有视差,线 dash 已
        // 为此付过代价(终态改镶嵌期几何切分)。fill 要加图案同理走几何或
        // 地形深度重建,别加 varying。
        for (const auto& group : gpu.volumeGroups) {
            if (group.indexCount <= 0 || !fillShader) continue;
            RenderCommand vol;
            vol.kind = RenderCommandKind::VectorStencil;
            vol.stencilPhase = StencilPhase::ClassifyVolume;
            vol.owner = layerId_;
            vol.pass = "color";
            vol.frameId = frameState.frameId;
            vol.shader = fillShader;
            vol.vertexBuffer = group.vertexBuffer.get();
            vol.indexBuffer = group.indexBuffer.get();
            vol.indexCount = group.indexCount;
            vol.indexType = RenderCommand::IndexType::UInt32;
            vol.vertexStride = 12;
            vol.primitive = RenderCommand::PrimitiveType::Triangles;
            vol.depthTest = true;   // z-fail 计数依赖地形深度
            vol.depthWrite = false;
            vol.blend = false;      // 后端关颜色写,blend 无意义
            vol.cullFace = false;   // 两侧 stencil op 需要双面
            vol.uniforms["u_modelViewProjection"] = mvpUniform;
            vol.uniforms["u_color"] = {0.0f, 0.0f, 0.0f, 0.0f};

            RenderCommand col = vol;
            col.stencilPhase = StencilPhase::ClassifyColor;
            col.depthTest = false;  // 覆盖面自身别被地形挡
            col.blend = true;
            col.uniforms["u_color"] = {group.color[0], group.color[1],
                                       group.color[2], group.color[3]};
            commands.push_back(std::move(vol));
            commands.push_back(std::move(col));
        }

        // P6d stencil 贴地线:墙带体命令对(同 order 29 + 插入序紧邻契约;
        // 每组色 pass op ZERO 清零,与 fill 组间互不串)。宽度在 VS 按眼深
        // 换算世界米挤出:halfW = u_halfWidthPerEyeZ * |ec.z|,
        // u_halfWidthPerEyeZ = lineWidthPx * tan(fovy/2) / vpH(即每米眼深
        // 对应的半宽米数),像素语义与方案 A ribbon 一致。
        if (!gpu.lineVolumeGroups.empty() &&
            renderer.vectorLineStencilShader()) {
            const glm::dmat4 view(cam.viewMatrix().raw());
            const glm::mat4 modelView = glm::mat4(view * model);
            std::vector<float> modelViewUniform(16);
            std::memcpy(modelViewUniform.data(), glm::value_ptr(modelView),
                        16 * sizeof(float));
            const float halfWidthPerEyeZ = static_cast<float>(
                static_cast<double>(lineWidthPx) *
                std::tan(cam.verticalFovRadians() * 0.5) /
                std::max(1.0, vpH));
            for (const auto& group : gpu.lineVolumeGroups) {
                if (group.indexCount <= 0) continue;
                RenderCommand vol;
                vol.kind = RenderCommandKind::VectorStencil;
                vol.stencilPhase = StencilPhase::ClassifyVolume;
                vol.owner = layerId_;
                vol.pass = "color";
                vol.frameId = frameState.frameId;
                vol.shader = renderer.vectorLineStencilShader();
                vol.vertexBuffer = group.vertexBuffer.get();
                vol.indexBuffer = group.indexBuffer.get();
                vol.indexCount = group.indexCount;
                vol.indexType = RenderCommand::IndexType::UInt32;
                vol.vertexStride = 24;  // pos(12)+extrude(12)
                vol.primitive = RenderCommand::PrimitiveType::Triangles;
                vol.depthTest = true;   // z-fail 计数依赖地形深度
                vol.depthWrite = false;
                vol.blend = false;
                vol.cullFace = false;   // 两侧 stencil op 需要双面
                vol.uniforms["u_modelViewProjection"] = mvpUniform;
                vol.uniforms["u_modelView"] = modelViewUniform;
                vol.uniforms["u_halfWidthPerEyeZ"] = {halfWidthPerEyeZ};
                // dash 已在镶嵌期切成独立划体(几何边界),FS 无需判里程。
                vol.uniforms["u_color"] = {0.0f, 0.0f, 0.0f, 0.0f};

                RenderCommand col = vol;
                col.stencilPhase = StencilPhase::ClassifyColor;
                col.depthTest = false;
                col.blend = true;
                col.uniforms["u_color"] = {group.color[0], group.color[1],
                                           group.color[2], group.color[3]};
                commands.push_back(std::move(vol));
                commands.push_back(std::move(col));
            }
        }

        if (gpu.fillIndexCount > 0 && renderer.vectorFillShader()) {
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorFill;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = renderer.vectorFillShader();
            cmd.vertexBuffer = gpu.fillVertexBuffer.get();
            cmd.indexBuffer = gpu.fillIndexBuffer.get();
            cmd.indexCount = gpu.fillIndexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            cmd.vertexStride = 16;  // pos(12)+color(4,RGBA8)
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            cmd.depthTest = true;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            cmd.uniforms["u_modelViewProjection"] = mvpUniform;
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
                static_cast<int>(kLineVertexFloats * sizeof(float));  // 48
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            cmd.depthTest = true;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            cmd.uniforms["u_modelViewProjection"] = mvpUniform;
            cmd.uniforms["u_viewport"] = {static_cast<float>(vpW),
                                          static_cast<float>(vpH)};
            cmd.uniforms["u_lineWidthPx"] = {lineWidthPx};
            // dash(与 stencil 路径同语义,回落观感一致)。
            cmd.uniforms["u_dashPeriodMeters"] = {
                style_.lineDashPeriodMeters};
            cmd.uniforms["u_dashOnFraction"] = {style_.lineDashOnFraction};
            commands.push_back(std::move(cmd));
        }

        if (gpu.pointIndexCount > 0 && renderer.vectorPointShader()) {
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorPoint;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = renderer.vectorPointShader();
            cmd.vertexBuffer = gpu.pointVertexBuffer.get();
            cmd.indexBuffer = gpu.pointIndexBuffer.get();
            cmd.indexCount = gpu.pointIndexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            // anchor(12)+offsetUnit(8)+uv(8)+color(4,RGBA8)+shape(4)
            cmd.vertexStride = 36;
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            cmd.depthTest = true;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            cmd.uniforms["u_modelViewProjection"] = mvpUniform;
            cmd.uniforms["u_viewport"] = {static_cast<float>(vpW),
                                          static_cast<float>(vpH)};
            cmd.uniforms["u_pointSizePx"] = {pointSizePx};
            cmd.uniforms["u_depthPushNdc"] = {symbolDepthPush};
            // P6c:图集通道的顶点(shape<0)要采位图。纹理常挂(有图标才
            // 存在),无图集时桶里也不会有图集顶点,shader 分支不触达。
            // ⚠️ 无图集时也必须占位 nullptr:T2 的地形深度纹理恒取
            // textures[1](后端按下标 1:1 绑 unit),下标浮动会把深度绑到
            // 图集的采样器上。后端对 nullptr 项直接跳过,占位无副作用。
            cmd.textures.push_back(
                (iconAtlas_ && iconAtlas_->texture()) ? iconAtlas_->texture()
                                                      : nullptr);
            appendTerrainOcclusion(renderer, cmd);
            commands.push_back(std::move(cmd));
        }

        if (gpu.labelIndexCount > 0 && renderer.vectorLabelShader() &&
            glyphAtlas_ && glyphAtlas_->texture()) {
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorLabel;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = renderer.vectorLabelShader();
            cmd.vertexBuffer = gpu.labelVertexBuffer.get();
            cmd.indexBuffer = gpu.labelIndexBuffer.get();
            cmd.indexCount = gpu.labelIndexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            cmd.vertexStride = 32;  // anchor(12)+offsetPx(8)+uv(8)+opacity(4)
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            cmd.depthTest = true;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            cmd.textures.push_back(glyphAtlas_->texture());
            appendTerrainOcclusion(renderer, cmd);
            cmd.uniforms["u_modelViewProjection"] = mvpUniform;
            cmd.uniforms["u_color"] = {style_.labelColor[0],
                                       style_.labelColor[1],
                                       style_.labelColor[2],
                                       style_.labelColor[3]};
            cmd.uniforms["u_haloColor"] = {style_.labelHaloColor[0],
                                           style_.labelHaloColor[1],
                                           style_.labelHaloColor[2],
                                           style_.labelHaloColor[3]};
            cmd.uniforms["u_viewport"] = {static_cast<float>(vpW),
                                          static_cast<float>(vpH)};
            cmd.uniforms["u_sdfEdge"] = {
                static_cast<float>(GlyphAtlas::kSdfOnEdge) / 255.0f};
            cmd.uniforms["u_depthPushNdc"] = {symbolDepthPush};
            // halo 宽(px,标注字号尺度)→ 字形栅格 px → SDF 值差。
            const float glyphScale =
                style_.labelSizePx /
                static_cast<float>(GlyphAtlas::kGlyphPixelHeight);
            cmd.uniforms["u_sdfHaloDelta"] = {
                style_.labelHaloPx / std::max(0.01f, glyphScale) *
                GlyphAtlas::kSdfDistScale / 255.0f};
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
