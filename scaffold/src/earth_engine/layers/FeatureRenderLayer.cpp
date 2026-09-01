#include "FeatureRenderLayer.h"
#include "ProjectedPathSampler.h"

#include "../data/PolygonTessellator.h"
#include "../data/LineTessellator.h"
#include "../renderer/GlyphAtlas.h"
#include "../renderer/IconAtlas.h"
#include "../renderer/RenderDevice.h"
#include "../renderer/Renderer.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../debug/PerfTimer.h"
#include "../core/math/IntersectionTests.h"
#include "../core/math/Mat4.h"
#include "../core/math/Ray.h"
#include "../debug/PlatformLog.h"
#include "../style/AmapClassicLabelStyleInternal.h"
#include "../style/AmapClassicRoadStyle.h"
#include "../style/AmapClassicStyleInternal.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace earth_engine {

void FeatureRenderStyle::installAmapClassicScope(AmapClassicScope scope) {
    constexpr auto bit = [](OfficialRequirement requirement) {
        return static_cast<uint32_t>(requirement);
    };
    uint32_t requirements = 0;
    switch (scope) {
        case AmapClassicScope::Surface:
            requirements = bit(OfficialRequirement::DrawOrder) |
                           bit(OfficialRequirement::ZoomWindow) |
                           bit(OfficialRequirement::FillIdentity);
            break;
        case AmapClassicScope::Transport:
            requirements = bit(OfficialRequirement::DrawOrder) |
                           bit(OfficialRequirement::ZoomWindow) |
                           bit(OfficialRequirement::LineIdentity);
            break;
        case AmapClassicScope::RoadLabel:
            requirements = bit(OfficialRequirement::DrawOrder) |
                           bit(OfficialRequirement::ZoomWindow) |
                           bit(OfficialRequirement::Rank) |
                           bit(OfficialRequirement::LineIdentity) |
                           bit(OfficialRequirement::LabelIdentity);
            break;
        case AmapClassicScope::Poi:
            requirements = bit(OfficialRequirement::DrawOrder) |
                           bit(OfficialRequirement::ZoomWindow) |
                           bit(OfficialRequirement::Rank) |
                           bit(OfficialRequirement::LabelIdentity) |
                           bit(OfficialRequirement::PointIdentity);
            break;
    }
    providerContract_ = ProviderContract::AmapClassicOfficial;
    officialRequirements_ |= requirements;
}

namespace {

double visibilityZoom(const FeatureRenderStyle& style, double zoom) {
    const double threshold = style.visibilityZoomCeilFraction;
    if (!(threshold > 0.0 && threshold <= 1.0) || !std::isfinite(zoom)) {
        return zoom;
    }
    const double lo = std::floor(zoom);
    const double tolerance =
        8.0 * std::numeric_limits<double>::epsilon() *
        std::max({1.0, std::abs(zoom), std::abs(threshold)});
    return zoom - lo + tolerance < threshold ? lo : std::ceil(zoom);
}

/// line ribbon 顶点的 GPU 打包(48B,对齐 GLES VectorLine48 布局与
/// §6.2 shader attribute 0-5)。CPU 侧 LineVertex 是 double,不能直传。
constexpr int kLineVertexFloats = 12;

/// 点/图标符号顶点的 GPU 打包(36B,对齐 GLES VectorPoint36 布局:
/// anchor(3f)+offsetUnit(2f)+uv(2f)+color(RGBA8 占 1f)+shape(1f))。
constexpr int kPointVertexFloats = 9;
constexpr size_t kMaxSymbolsPerTile = 128;

struct OfficialIconBounds {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

OfficialIconBounds officialIconBounds(
    const FeatureRenderStyle::ProviderLabelLayout& layout,
    float inflationPx = 0.0f) {
    using IconAnchor =
        FeatureRenderStyle::ProviderLabelLayout::IconAnchor;
    if (layout.iconAnchor == IconAnchor::BottomCenter) {
        const float halfWidth = layout.iconWidthPx * 0.5f;
        return {-halfWidth - inflationPx, -inflationPx,
                halfWidth + inflationPx,
                layout.iconHeightPx + inflationPx};
    }
    return {-layout.iconAnchorXPx - inflationPx,
            layout.iconAnchorYPx - layout.iconHeightPx - inflationPx,
            layout.iconWidthPx - layout.iconAnchorXPx + inflationPx,
            layout.iconAnchorYPx + inflationPx};
}

std::array<float, 2> officialIconTopLeftDown(
    const FeatureRenderStyle::ProviderLabelLayout& layout) {
    using IconAnchor =
        FeatureRenderStyle::ProviderLabelLayout::IconAnchor;
    if (layout.iconAnchor == IconAnchor::BottomCenter) {
        return {-layout.iconWidthPx * 0.5f, -layout.iconHeightPx};
    }
    return {-layout.iconAnchorXPx, -layout.iconAnchorYPx};
}

// Amap's broad views show only a small set of city/region anchors. Keep the
// existing 128-symbol near-view ceiling, but make the candidate budget scale
// with the view zoom so low-priority village labels do not flood a wide map.
size_t maxSymbolsForViewZoom(int viewZoomBucket) {
    if (viewZoomBucket <= 11) return 16;
    if (viewZoomBucket <= 13) return 32;
    if (viewZoomBucket <= 15) return 64;
    return kMaxSymbolsPerTile;
}

std::optional<std::array<double, 2>> projectScreenPoint(
    const Vec3& point, const Mat4& viewProjection,
    double viewportWidth, double viewportHeight) {
    const glm::dvec4 clip = viewProjection.raw() * glm::dvec4(
        point.x(), point.y(), point.z(), 1.0);
    if (!(clip.w > 0.0)) return std::nullopt;
    return std::array<double, 2>{
        (clip.x / clip.w * 0.5 + 0.5) * viewportWidth,
        (clip.y / clip.w * 0.5 + 0.5) * viewportHeight};
}

double labelViewZoom(const Ellipsoid& ellipsoid, const Camera& camera) {
    const double camHeight =
        ellipsoid.cartesianToCartographic(camera.position()).height();
    return std::min(
        24.0,
        std::max(0.0, std::log2(4.0e7 / std::max(1.0, camHeight))));
}

std::vector<std::array<double, 3>> officialLineLabelPath(
    const std::vector<Cartographic>& ring) {
    std::vector<std::array<double, 3>> out;
    out.reserve(ring.size());
    for (const Cartographic& p : ring) {
        out.push_back({p.longitude(), p.latitude(), p.height()});
    }
    return out;
}

FeatureId officialPathFragmentId(FeatureId sourceId, uint32_t fragment) {
    if (fragment == 0) return sourceId;
    uint64_t value = sourceId ^ 0xA4D94E5F6B7C813Dull;
    value ^= static_cast<uint64_t>(fragment) + 0x9E3779B97F4A7C15ull +
             (value << 6) + (value >> 2);
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    value ^= value >> 31;
    return value == kInvalidFeatureId ? sourceId ^ 0x8000000000000000ull
                                      : value;
}

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
                    std::vector<uint32_t>& outIndices,
                    const std::vector<float>* perVertexColors = nullptr) {
    if (line.vertices.empty() || line.indices.empty()) return;
    // 逐顶点色(海拔着色轨迹 demo):长度须与 ribbon 顶点一致,否则整线
    // 回落统一色(调用方 bug 不炸渲染)。
    const bool hasPerVertexColors =
        perVertexColors && perVertexColors->size() == line.vertices.size();
    const uint32_t base =
        static_cast<uint32_t>(outVerts.size() / kLineVertexFloats);
    outVerts.reserve(outVerts.size() +
                     line.vertices.size() * kLineVertexFloats);
    for (size_t i = 0; i < line.vertices.size(); ++i) {
        const LineVertex& v = line.vertices[i];
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
        outVerts.push_back(hasPerVertexColors ? (*perVertexColors)[i]
                                              : packedColor);
    }
    outIndices.reserve(outIndices.size() + line.indices.size());
    for (uint32_t idx : line.indices) {
        outIndices.push_back(base + idx);
    }
}

/// 海拔着色轨迹(2026-08-23 demo):逐顶点椭球高 → 线性渐变 RGBA8 打包。
/// 复用 VectorLine48 既有 a_color 槽;lengthSoFar 不参与上色(dash 仍独立)。
/// min==max 时 t 按整线取中(0.5),不会除零。
std::vector<float> packLineHeightGradientColors(
    const TessellatedLine& line,
    const Ellipsoid& ellipsoid,
    const FeatureRenderStyle& style) {
    std::vector<float> out;
    out.reserve(line.vertices.size());
    const float range = style.lineColorGradientHeightMaxMeters -
                        style.lineColorGradientHeightMinMeters;
    const float tDenom = range > 1e-3f ? range : 1.0f;
    for (const LineVertex& v : line.vertices) {
        const double h = ellipsoid.cartesianToCartographic(v.pos).height();
        const float t = std::clamp(
            (static_cast<float>(h) - style.lineColorGradientHeightMinMeters) /
                tDenom,
            0.0f, 1.0f);
        std::array<float, 4> c;
        for (int k = 0; k < 4; ++k) {
            c[k] = style.lineColorGradientLow[k] +
                   (style.lineColorGradientHigh[k] -
                    style.lineColorGradientLow[k]) * t;
        }
        out.push_back(packColorFloat(c));
    }
    return out;
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

float resolvePositiveFloat(
    const StyleExpression::Ptr& expr,
    const std::unordered_map<std::string, std::string>& properties,
    float fallback) {
    if (!expr) return fallback;
    const auto v = expr->evaluate(&properties, std::nan(""));
    if (!v || v->kind() != StyleValue::Kind::Number ||
        !std::isfinite(v->number()) || v->number() <= 0.0) {
        return fallback;
    }
    return static_cast<float>(std::clamp(v->number(), 6.0, 96.0));
}

float resolveNonNegativeFloat(
    const StyleExpression::Ptr& expr,
    const std::unordered_map<std::string, std::string>& properties,
    float fallback) {
    if (!expr) return fallback;
    const auto v = expr->evaluate(&properties, std::nan(""));
    if (!v || v->kind() != StyleValue::Kind::Number ||
        !std::isfinite(v->number()) || v->number() < 0.0) {
        return fallback;
    }
    return static_cast<float>(std::clamp(v->number(), 0.0, 96.0));
}

int resolveInteger(const StyleExpression::Ptr& expr,
                   const std::unordered_map<std::string, std::string>& properties,
                   int fallback) {
    if (!expr) return fallback;
    const auto v = expr->evaluate(&properties, std::nan(""));
    if (!v || v->kind() != StyleValue::Kind::Number ||
        !std::isfinite(v->number())) {
        return fallback;
    }
    const double bounded = std::max(
        static_cast<double>(std::numeric_limits<int>::min()),
        std::min(static_cast<double>(std::numeric_limits<int>::max()),
                 v->number()));
    return static_cast<int>(std::lround(bounded));
}

int integerProperty(
    const std::unordered_map<std::string, std::string>& properties,
    const char* key, int fallback) {
    const auto it = properties.find(key);
    if (it == properties.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const long value = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || *end != '\0') return fallback;
    return static_cast<int>(std::clamp<long>(
        value, std::numeric_limits<int>::min(),
        std::numeric_limits<int>::max()));
}

std::optional<int> strictIntegerProperty(
    const std::unordered_map<std::string, std::string>& properties,
    const char* key) {
    const auto it = properties.find(key);
    if (it == properties.end() || it->second.empty()) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(it->second.c_str(), &end, 10);
    if (errno == ERANGE || end == it->second.c_str() || *end != '\0' ||
        value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<int> officialAmapPlacementRank(
    const std::unordered_map<std::string, std::string>& properties) {
    const auto providerRank = strictIntegerProperty(properties, "amap_rank");
    if (!providerRank || *providerRank == std::numeric_limits<int>::min()) {
        return std::nullopt;
    }
    // Official AMap rank uses larger = more important. The engine placement
    // queue uses smaller = earlier, so convert direction exactly once at the
    // renderer contract boundary without publishing a generic rank alias.
    return -*providerRank;
}

int resolveLabelPaintOrder(
    const FeatureRenderStyle& style,
    const std::unordered_map<std::string, std::string>& properties,
    int fallback) {
    if (style.requiresOfficial(
            FeatureRenderStyle::OfficialRequirement::DrawOrder)) {
        return strictIntegerProperty(properties, "amap_draworder").value_or(0);
    }
    return resolveInteger(style.labelPaintOrderExpr, properties, fallback);
}

bool hasValidOfficialDrawOrder(const FeatureRenderStyle& style,
                               const Feature& feature) {
    if (!style.requiresOfficial(
            FeatureRenderStyle::OfficialRequirement::DrawOrder)) {
        return true;
    }
    return strictIntegerProperty(feature.properties, "amap_draworder").has_value();
}

int resolveLabelPropertyStyleGroup(
    const FeatureRenderStyle& style,
    const std::unordered_map<std::string, std::string>& properties,
    int fallback) {
    if (style.labelStyleGroupPropertyA.empty() ||
        style.labelStyleGroupPropertyB.empty() ||
        style.labelStyleGroupByProperty.empty()) {
        return fallback;
    }
    const auto a = properties.find(style.labelStyleGroupPropertyA);
    const auto b = properties.find(style.labelStyleGroupPropertyB);
    if (a == properties.end() || b == properties.end()) return fallback;
    const std::string key = a->second + ":" + b->second;
    const auto found = style.labelStyleGroupByProperty.find(key);
    return found == style.labelStyleGroupByProperty.end() ? fallback
                                                          : found->second;
}

int resolveLabelVisualStyleGroup(
    const FeatureRenderStyle& style,
    const std::unordered_map<std::string, std::string>& properties,
    int fallback) {
    const int expressionGroup =
        resolveInteger(style.labelStyleGroupExpr, properties, fallback);
    return resolveLabelPropertyStyleGroup(style, properties,
                                          expressionGroup);
}

std::optional<std::pair<int, int>> featureZoomRange(
    const std::unordered_map<std::string, std::string>& properties,
    bool requireOfficialWindow = false) {
    if (properties.find("amap_minzoom") == properties.end() &&
        properties.find("amap_maxzoom") == properties.end()) {
        return requireOfficialWindow
            ? std::nullopt
            : std::optional<std::pair<int, int>>{{0, 30}};
    }
    // Amap feature metadata uses the same source/style zoom convention as its
    // official width stops. The renderer's camera zoom is the visible display
    // zoom, which is one level lower (source selection performs the inverse
    // +1). Normalize once at the adapter boundary so POI and road labels do
    // not each grow an independent compensation rule.
    const auto strictZoomProperty = [&](const char* key, int fallback,
                                        bool& valid) {
        const auto it = properties.find(key);
        if (it == properties.end()) return fallback;
        char* end = nullptr;
        const long value = std::strtol(it->second.c_str(), &end, 10);
        if (end == it->second.c_str() || *end != '\0' || value < 0 ||
            value > 30) {
            valid = false;
            return fallback;
        }
        return static_cast<int>(value);
    };
    bool valid = true;
    const int rawMin = strictZoomProperty("amap_minzoom", 0, valid);
    const int rawMax = strictZoomProperty("amap_maxzoom", 30, valid);
    if (!valid || rawMax < rawMin) {
        return requireOfficialWindow
            ? std::nullopt
            : std::optional<std::pair<int, int>>{{0, 30}};
    }
    if (requireOfficialWindow &&
        (properties.find("amap_minzoom") == properties.end() ||
         properties.find("amap_maxzoom") == properties.end())) {
        return std::nullopt;
    }

    constexpr int kAmapToDisplayZoomOffset = -1;
    int minZoom = std::clamp(
        rawMin + kAmapToDisplayZoomOffset,
        0, 30);
    // Official maxZoom is inclusive. AMap zoom N maps to display zoom N-1,
    // therefore the equivalent half-open upper bound is rawMax, not
    // rawMax-1. Example: style [15,21] -> display [14,21), retaining z20.
    int maxZoom = std::clamp(
        rawMax,
        0, 30);
    return std::pair<int, int>{minZoom, maxZoom};
}

/// 单个符号 quad 的几何/采样解析(P6c)。尺寸单位 = pointSizePx 的倍数:
/// 内置形状是边长 1 的正方 quad;位图图标高 1、宽按源图宽高比。
struct ResolvedSymbol {
    float shape = 0.0f;          ///< >=0 内置形状 id;<0 = 图集哨兵
    float halfWidthUnits = 0.5f; ///< quad 半宽(尺寸倍数)
    bool bottomAnchored = false; ///< true = quad 画在锚点上方
    /// 图集 uv 矩形(内置形状不用,fragment 走局部坐标)。
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    bool imageResolved = true;
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
        out.imageResolved = symbolShapeFromName(name, &builtin);
        out.shape = static_cast<float>(static_cast<int>(builtin));
        out.bottomAnchored = anchor == SymbolAnchor::Bottom ||
                             (anchor == SymbolAnchor::Auto &&
                              symbolShapeIsBottomAnchored(builtin));
    }
    return out;
}

std::optional<ResolvedSymbol> resolveOfficialAtlasSymbol(
    const std::string& name, const IconAtlas* atlas) {
    const IconAtlas::Frame* frame = atlas ? atlas->frame(name) : nullptr;
    if (!frame) return std::nullopt;
    ResolvedSymbol out;
    out.shape = kSymbolShapeAtlas;
    const float aspect =
        frame->heightPx > 0.0f ? frame->widthPx / frame->heightPx : 1.0f;
    out.halfWidthUnits = 0.5f * aspect;
    out.u0 = frame->u0;
    out.v0 = frame->v0;
    out.u1 = frame->u1;
    out.v1 = frame->v1;
    // Official provider layout supplies its own pixel anchor to
    // appendSymbolQuad. Never consult the generic SymbolAnchor contract.
    out.bottomAnchored = false;
    return out;
}

/// 追加一个符号 billboard quad(36B 布局:anchor 3f + offsetUnit 2f +
/// uv 2f + color 1f + shape 1f;VS 按 u_pointSizePx 展开)。
/// store 镶嵌(tessellateFeatureInto)与瓦片准入定型(commitTileMesh)
/// 共用 —— 顶点布局契约只此一份,不得各自内联。
void appendSymbolQuad(const std::array<float, 3>& rel,
                      const ResolvedSymbol& sym,
                      float colorPacked,
                      float sizeScale,
                      std::vector<float>& pointVerts,
                      std::vector<uint32_t>& pointIndices,
                      const FeatureRenderStyle::ProviderLabelLayout*
                          providerLayout = nullptr) {
    const uint32_t base =
        static_cast<uint32_t>(pointVerts.size() / kPointVertexFloats);
    // corner ∈ {±1}²(x 右为正,y 上为正)。
    static constexpr float kCorners[4][2] = {
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    for (const auto& corner : kCorners) {
        float offsetX = corner[0] * sym.halfWidthUnits * sizeScale;
        // 竖直对齐:居中 y∈[-0.5,0.5];底部锚定 y∈[0,1]。
        float offsetY = (sym.bottomAnchored
                             ? (corner[1] + 1.0f) * 0.5f
                             : corner[1] * 0.5f) * sizeScale;
        if (providerLayout) {
            // Official Igt publishes either a numeric top-left anchor or the
            // string anchor "bottom-center" for icons_9. Resolve that tagged
            // contract once so visible geometry and collision stay identical.
            const OfficialIconBounds bounds =
                officialIconBounds(*providerLayout);
            offsetX = corner[0] < 0.0f ? bounds.minX : bounds.maxX;
            offsetY = corner[1] < 0.0f ? bounds.minY : bounds.maxY;
        }
        // 图集通道 uv 取 frame 矩形(纹理 v 向下,屏幕 y 向上 → 上边角取
        // v0);内置形状 uv 即 [-1,1]² 局部坐标。
        const float u = sym.shape < 0.0f
                            ? (corner[0] < 0.0f ? sym.u0 : sym.u1)
                            : corner[0];
        const float v = sym.shape < 0.0f
                            ? (corner[1] > 0.0f ? sym.v0 : sym.v1)
                            : corner[1];
        pointVerts.push_back(rel[0]);
        pointVerts.push_back(rel[1]);
        pointVerts.push_back(rel[2]);
        pointVerts.push_back(offsetX);
        pointVerts.push_back(offsetY);
        pointVerts.push_back(u);
        pointVerts.push_back(v);
        pointVerts.push_back(colorPacked);
        pointVerts.push_back(sym.shape);
    }
    const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t idx : quad) pointIndices.push_back(base + idx);
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

void FeatureRenderLayer::setRenderDevice(RenderDevice* device) {
    if (renderDevice_ == device) return;

    buckets_.clear();
    tileBuckets_.clear();
    previewGpu_ = BucketGpu{};
    previewGpuValid_ = false;
    pendingReclamp_.clear();
    symbolBucketsAwaitingRebuild_ = false;
    labelsAwaitingPlacement_ = false;
    labelWorkActiveForCurrentView_ = false;
    labelWorkTicket_.release();
    glyphAtlas_ = nullptr;
    iconAtlas_ = nullptr;
    renderDevice_ = device;

    if (!renderDevice_) return;
    std::unordered_set<BucketKey> storeBuckets;
    storeBuckets.reserve(store_.features().size());
    for (const auto& [id, feature] : store_.features()) {
        (void)feature;
        storeBuckets.insert(store_.bucketOf(id));
    }
    for (BucketKey key : storeBuckets) {
        rebuildBucket(key);
    }
    previewDirty_ = previewFeatureId_ != kInvalidFeatureId;
}

void FeatureRenderLayer::setVisible(bool v) {
    if (visible_ == v) return;
    visible_ = v;
    if (!visible_) {
        labelWorkActiveForCurrentView_ = false;
        labelWorkTicket_.release();
    }
}

void FeatureRenderLayer::setStyle(const FeatureRenderStyle& s) {
    if (officialProfileSealed_) {
        platformLog(LogLevel::Warning, "FeatureRenderLayer",
                    "reject generic setStyle transition involving official provider profile");
        return;
    }
    if (s.usesOfficialProviderContract()) {
        platformLog(LogLevel::Warning, "FeatureRenderLayer",
                    "reject incoming unsealed official provider profile");
        return;
    }
    applyStyleUnchecked(s);
}

#if defined(EARTH_ENGINE_TESTING)
void FeatureRenderLayer::setStyleForContractTest(const FeatureRenderStyle& s) {
    applyStyleUnchecked(s);
}
#endif

void FeatureRenderLayer::applyStyleUnchecked(const FeatureRenderStyle& s) {
    style_ = s;
    // P6b 表达式语义校验:颜色 = 数据驱动(镶嵌期无 zoom 上下文,引用
    // zoom 的颜色表达式恒求值失败 → 直接剥离降级字面量并警告);宽度/
    // 尺寸 = zoom 驱动(每帧无属性上下文,引用属性同理)。
    auto sanitize = [](StyleExpression::Ptr& expr, bool allowProperties,
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
    sanitize(style_.labelSizeExpr, true, false, "labelSize");
    sanitize(style_.labelOffsetExpr, true, false, "labelOffset");
    sanitize(style_.paintOrderExpr, true, false, "paintOrder");
    sanitize(style_.fillStyleGroupExpr, true, false, "fillStyleGroup");
    sanitize(style_.labelPaintOrderExpr, true, false, "labelPaintOrder");
    sanitize(style_.labelStyleGroupExpr, true, false, "labelStyleGroup");
    sanitize(style_.lineStyleGroupExpr, true, false, "lineStyleGroup");
    sanitize(style_.lineWidthExpr, false, true, "lineWidth");
    for (auto& [styleGroup, expr] : style_.lineWidthExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "lineWidthByStyleGroup");
    }
    for (auto& [styleGroup, expr] :
         style_.lineCasingSolidCapExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "lineCasingSolidCapByStyleGroup");
    }
    for (auto& [styleGroup, expr] : style_.lineColorExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "lineColorByStyleGroup");
    }
    for (auto& [styleGroup, expr] : style_.fillColorExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "fillColorByStyleGroup");
    }
    for (auto& [styleGroup, expr] : style_.lineTypeExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "lineTypeByStyleGroup");
    }
    for (auto& [styleGroup, expr] :
         style_.lineCasingColorExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "lineCasingColorByStyleGroup");
    }
    for (auto& [styleGroup, expr] :
         style_.lineCasingTypeExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "lineCasingTypeByStyleGroup");
    }
    for (auto& [styleGroup, expr] :
         style_.lineCasingWidthExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "lineCasingWidthByStyleGroup");
    }
    for (auto& [styleGroup, expr] : style_.labelSizeExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "labelSizeByStyleGroup");
    }
    for (auto& [styleGroup, expr] : style_.labelColorExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "labelColorByStyleGroup");
    }
    for (auto& [styleGroup, expr] :
         style_.labelHaloColorExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "labelHaloColorByStyleGroup");
    }
    for (auto& [styleGroup, expr] :
         style_.labelHaloWidthExprByStyleGroup) {
        (void)styleGroup;
        sanitize(expr, false, true, "labelHaloWidthByStyleGroup");
    }
    sanitize(style_.pointSizeExpr, false, true, "pointSize");
    sanitize(style_.pointImageExpr, true, false, "pointImage");
    // 高度/细分/模式都影响几何:已建桶按新样式全部重镶。
    std::vector<BucketKey> keys;
    keys.reserve(buckets_.size());
    for (const auto& entry : buckets_) keys.push_back(entry.first);
    for (BucketKey key : keys) rebuildBucket(key);
    for (auto& entry : tileBuckets_) {
        entry.second.symbolViewZoomBucket = -1;
    }
    symbolBucketsAwaitingRebuild_ = !tileBuckets_.empty();
    previewDirty_ = true;
}

void FeatureRenderLayer::installAmapClassicProfile(
    AmapClassicProfile profile) {
    if (officialProfileSealed_ || style_.usesOfficialProviderContract()) return;
    FeatureRenderStyle official;
    // Provider paint/layout identity remains sealed to the official AMap
    // contract, while spatial placement belongs to the globe scene.  Always
    // use the single clamp contract: SceneRenderPipeline supplies the
    // render-grid-consistent terrain sampler when terrain exists, and the
    // existing no-sample rule falls back to the ellipsoid. Do not freeze the
    // official basemap at source height or revive any legacy drape path.
    official.altitudeMode = FeatureAltitudeMode::ClampToGround;
    // Official transport geometry must retain its provider paint identity,
    // so stencil wall volumes are not a valid replacement: those volumes
    // bake a generic color before the official zoom/style tables are
    // evaluated. Use the single ribbon contract instead. The worker keeps
    // lon/lat clamp source, commit samples the render-grid-consistent terrain,
    // and terrain revision changes rebuild only the line vertex buffer.
    official.terrainClampRibbon = true;
    switch (profile) {
        case AmapClassicProfile::Main:
            official.officialGeometryMask_ =
                (1u << static_cast<uint8_t>(GeometryType::LineString)) |
                (1u << static_cast<uint8_t>(GeometryType::Polygon));
            official.lineRoundJoin = false;
            AmapClassicStyleContract::applyTransport(official);
            AmapClassicStyleContract::applySurface(official);
            official.buildingExtrusion = true;
            break;
        case AmapClassicProfile::Regions:
            official.officialGeometryMask_ =
                (1u << static_cast<uint8_t>(GeometryType::LineString)) |
                (1u << static_cast<uint8_t>(GeometryType::Polygon));
            AmapClassicStyleContract::applySurface(official);
            AmapClassicStyleContract::applyTransport(official);
            official.stencilFillEnabled = false;
            official.globeFillMaxEdgeMeters = 10000.0;
            official.maxZoom = 12.0;
            break;
        case AmapClassicProfile::Poi:
            official.officialGeometryMask_ =
                (1u << static_cast<uint8_t>(GeometryType::Point)) |
                (1u << static_cast<uint8_t>(GeometryType::LineString));
            official.labelProperty = "name";
            AmapClassicStyleContract::applyRoadLabelPlacement(official);
            AmapClassicStyleContract::applyLineLabel(official);
            AmapClassicStyleContract::applyPoi(official);
            break;
        default:
            platformLog(LogLevel::Error, "FeatureRenderLayer",
                        "reject unknown AMap classic profile");
            return;
    }
    // Official AMap profiles are sealed single-path contracts.  Remove every
    // generic visual value after installing provider tables so a future
    // consumer cannot accidentally revive a legacy/default fallback merely
    // because an official lookup is missing.  Non-official profiles retain
    // the normal defaults in their own style objects.
    official.fillColor = {0, 0, 0, 0};
    official.lineColor = {0, 0, 0, 0};
    official.pointColor = {0, 0, 0, 0};
    official.labelColor = {0, 0, 0, 0};
    official.labelHaloColor = {0, 0, 0, 0};
    official.lineCasingColor = {0, 0, 0, 0};
    official.pointSizePx = 0.0f;
    official.lineWidthPx = 0.0f;
    official.labelSizePx = 0.0f;
    official.labelOffsetPx = 0.0f;
    official.labelHaloPx = 0.0f;
    official.pointImage.clear();
    official.fillColorExpr.reset();
    official.lineColorExpr.reset();
    official.pointColorExpr.reset();
    official.labelSizeExpr.reset();
    official.labelOffsetExpr.reset();
    official.lineWidthExpr.reset();
    official.pointSizeExpr.reset();
    official.pointImageExpr.reset();
    official.lineColorProperty.clear();
    official.lineColorByProperty.clear();
    official.lineDashPeriodMeters = 0.0f;
    official.lineDashOnFraction = 0.0f;
    official.lineDashByStyleGroup.clear();
    official.lineCasingDashByStyleGroup.clear();
    official.lineColorGradientByHeight = false;
    style_ = std::move(official);
    amapClassicProfile_ = profile;
    officialProfileSealed_ = true;
}

#if defined(EARTH_ENGINE_TESTING)
namespace testing {
FeatureRenderStyle amapOfficialStyleForTest(
    FeatureRenderLayer::AmapClassicProfile profile) {
    FeatureRenderLayer layer("amap-official-contract-test", nullptr,
                             Ellipsoid::WGS84());
    layer.installAmapClassicProfile(profile);
    return layer.style();
}
} // namespace testing
#endif

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
        // 区域元数据可用时:所有点放在范围**中点**,不采样。挤出体靠
        // margin(半跨 + 余量)覆盖整个范围,stencil 是像素级判定,顶点在
        // 哪个高度无所谓,只要体穿透地形。heightOffset 在这条路上无意义
        // (stencil 染色与抬升无关),故不叠加。
        if (ctx.hasTerrainHeightRange) {
            return (ctx.terrainMinHeight + ctx.terrainMaxHeight) * 0.5;
        }
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
    // Clamp changes only spatial coordinates.  Preserve provider properties
    // so downstream official geometry contracts (notably amap_height for
    // building extrusion) remain authoritative after terrain adaptation.
    clamped.properties = feature.properties;
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
    int paintOrder,
    int fillStyleGroup,
    int lineStyleGroup,
    int labelStyleGroup,
    const AreaSampleFn& sample,
    Vec3& origin,
    bool& hasOrigin,
    PaintGeometryCpu& fillRange,
    PaintGeometryCpu& lineRange,
    PaintGeometryCpu& pointRange,
    LabelGeometryCpu& labelRange,
    VolumeCpuGroups& volumeGroups,
    VolumeCpuGroups& lineVolumeGroups,
    PaintGeometryCpu& extrudeRange) {
    std::vector<float>& fillVerts = fillRange.verts;
    std::vector<uint32_t>& fillIndices = fillRange.indices;
    std::vector<float>& lineVerts = lineRange.verts;
    std::vector<uint32_t>& lineIndices = lineRange.indices;
    std::vector<float>& pointVerts = pointRange.verts;
    std::vector<uint32_t>& pointIndices = pointRange.indices;
    std::vector<float>& labelVerts = labelRange.verts;
    std::vector<uint32_t>& labelIndices = labelRange.indices;
    std::vector<LabelEntry>& labelEntries = labelRange.entries;
    std::vector<float>* lineClampSourceOut = &lineRange.clampSource;
    const bool officialFill = ctx.style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::FillIdentity);
    const bool officialLine = ctx.style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::LineIdentity);
    // Generic feature colors are a separate rendering contract. Official
    // geometry carries only its styleGroup identity and is colored from the
    // generated provider tables at command time; evaluating or baking generic
    // expressions here would retain a hidden second style path even when the
    // command later overwrote it.
    const std::array<float, 4> fillColor = officialFill
        ? std::array<float, 4>{0, 0, 0, 0}
        : resolveColor(ctx.style.fillColorExpr, feature.properties,
                       ctx.style.fillColor);
    std::array<float, 4> resolvedLineColor{0, 0, 0, 0};
    if (!officialLine) {
        resolvedLineColor = resolveColor(
            ctx.style.lineColorExpr, feature.properties, ctx.style.lineColor);
        if (!ctx.style.lineColorProperty.empty()) {
            const auto property =
                feature.properties.find(ctx.style.lineColorProperty);
            if (property != feature.properties.end()) {
                const auto table =
                    ctx.style.lineColorByProperty.find(property->second);
                if (table != ctx.style.lineColorByProperty.end()) {
                    resolvedLineColor = table->second;
                }
            }
        }
    }
    const float fillColorPacked = packColorFloat(fillColor);
    const float lineColorPacked = packColorFloat(resolvedLineColor);

    // 贴地:预变换出细分+采样高度的副本(高度已含 offset),镶嵌时
    // heightOffset 传 0 防二次叠加;Absolute 走原几何 + offset。
    const bool clamp =
        ctx.style.altitudeMode == FeatureAltitudeMode::ClampToGround;
    const bool bakedOfficialSurfaceFill =
        ctx.bakeOfficialSurfaceFill && officialFill &&
        feature.type == GeometryType::Polygon &&
        feature.properties.count("amap_height") == 0;
    // P6 方案 B:后端支持 stencil 分类 → clamp 面 fill 走挤出体双 pass
    // (像素级贴合,LOD 切换免重钳);不支持回落方案 A。
    const bool stencilFill =
        clamp && feature.type == GeometryType::Polygon &&
        ctx.supportsStencilClassification && ctx.style.stencilFillEnabled &&
        !officialFill;
    // P6d:clamp 线(LineString + polygon outline)同走 stencil 双 pass
    // (墙带体,像素级贴地,宽度 VS 按眼深挤出);不支持回落方案 A ribbon。
    const bool stencilLine =
        clamp && !ctx.style.terrainClampRibbon &&
        ctx.supportsStencilClassification && !officialLine;
    /// E 方案 P2:clamp 源只在「worker 无采样」的瓦片路径产出 —— store
    /// 路径地形可用时直接钳真高,靠 rebuildBucket 自愈,不需要源。
    const bool emitClampSource =
        clamp && ctx.style.terrainClampRibbon && !sample;
    // Endpoint quads are style candidates, not an altitude-mode feature.
    // Official roads are ClampToGround, so suppressing candidates for every
    // clamped line made lineType 13/14 uniforms ineffective: the fragment
    // shader had no endpoint geometry to reveal. Generate the shared quad if
    // either center or casing can select a solid cap at any zoom; the exact
    // current lineType still owns visibility and round/square clipping.
    const bool endpointCapPrimitives =
        ctx.style.lineSolidCapExprByStyleGroup.count(lineStyleGroup) != 0 ||
        ctx.style.lineCasingSolidCapExprByStyleGroup.count(lineStyleGroup) != 0;
    std::vector<Cartographic> steinerPoints;
    Feature clampedStorage;
    const Feature* geometry = &feature;
    double tessHeightOffset = ctx.style.heightOffset;
    const bool needsClampedGeometry =
        clamp && (!bakedOfficialSurfaceFill || ctx.style.fillOutlineEnabled);
    if (needsClampedGeometry) {
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
        if (ctx.style.terrainClampRibbon) {
            // E 方案 ribbon-clamp:高度交给 P2 的 VS 采高(位移地形高度
            // 纹理),worker 只做椭球面细分(几何密度服务贴地曲率)。
            // **必须屏蔽 hasTerrainHeightRange** —— 那是 stencil 体的
            // 中点高度语义,沿用会让整条路飘在范围中点。
            TessellationContext flatCtx = ctx;
            flatCtx.hasTerrainHeightRange = false;
            clampedStorage = prepareClampedFeature(
                flatCtx, feature, sample ? sample : AreaSampleFn(), nullptr,
                densify);
        } else {
            clampedStorage = prepareClampedFeature(
                ctx,
                feature, sample, stencilFill ? nullptr : &steinerPoints,
                densify);
        }
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
            // V6 建筑挤出:带 amap_height 的 Polygon → 墙带 + CDT 顶面,
            // 不产平 fill / stencil / outline(与面路径互斥)。
            const auto roofColor =
                ctx.style.extrusionRoofColorByStyleGroup.find(fillStyleGroup);
            const auto wallColor =
                ctx.style.extrusionWallColorByStyleGroup.find(fillStyleGroup);
            const bool extrudeBuilding =
                ctx.style.buildingExtrusion &&
                feature.properties.count("amap_height") &&
                roofColor != ctx.style.extrusionRoofColorByStyleGroup.end() &&
                wallColor != ctx.style.extrusionWallColorByStyleGroup.end();
            if (extrudeBuilding) {
                appendExtrusionVolume(ctx, *geometry, roofColor->second,
                                     wallColor->second, origin,
                                     hasOrigin, extrudeRange.verts,
                                     extrudeRange.indices,
                                     ctx.style.usesOfficialProviderContract()
                                         ? &extrudeRange.clampSource
                                         : nullptr);
                break;
            }
            if (bakedOfficialSurfaceFill) {
                // The final RGBA surface page is sampled by the terrain
                // fragment, so emitting a second terrain-dependent fill mesh
                // would reintroduce the piercing and CPU/CDT work this path
                // replaces. Continue below only for an explicitly configured
                // outline; buildings were handled above.
            } else if (stencilFill) {
                // 体积从原始 footprint 出(2D 拓扑,高度由采样范围决定),
                // 按解析色归组(每组独立命令对,不同色互不污染)。
                appendFillVolume(ctx, feature, paintOrder, sample, fillColor,
                                 origin, hasOrigin, volumeGroups);
            } else {
                PolygonTessellationDiagnostics polygonDiagnostics;
                TessellatedFill fill = PolygonTessellator::tessellate(
                    *geometry, ctx.ellipsoid, tessHeightOffset,
                    steinerPoints.empty() ? nullptr : &steinerPoints,
                    ctx.style.globeFillMaxEdgeMeters,
                    ctx.collectDiagnostics ? &polygonDiagnostics : nullptr);
                if (ctx.collectDiagnostics && ctx.tileMeshDiagnostics) {
                    auto& diagnostics = *ctx.tileMeshDiagnostics;
                    diagnostics.polygonSetupMs += polygonDiagnostics.setupMs;
                    diagnostics.polygonDensifyMs +=
                        polygonDiagnostics.globeDensifyMs;
                    diagnostics.polygonIntersectionMs +=
                        polygonDiagnostics.intersectionMs;
                    diagnostics.polygonCdtMs += polygonDiagnostics.cdtMs;
                    diagnostics.polygonCdtSuperMs +=
                        polygonDiagnostics.cdtSuperTriangleMs;
                    diagnostics.polygonCdtPointMs +=
                        polygonDiagnostics.cdtPointInsertMs;
                    diagnostics.polygonCdtConstraintMs +=
                        polygonDiagnostics.cdtConstraintInsertMs;
                    diagnostics.polygonCdtExtractMs +=
                        polygonDiagnostics.cdtExtractInsideMs;
                    diagnostics.polygonEcefMs += polygonDiagnostics.ecefMs;
                    diagnostics.polygonInputPoints +=
                        polygonDiagnostics.inputPoints;
                    diagnostics.polygonDensifiedPoints +=
                        polygonDiagnostics.densifiedPoints;
                    diagnostics.polygonInitialConstraints +=
                        polygonDiagnostics.initialConstraints;
                    diagnostics.polygonFinalConstraints +=
                        polygonDiagnostics.intersectionConstraints;
                    diagnostics.polygonIntersectionPairs +=
                        polygonDiagnostics.intersectionPairs;
                    diagnostics.polygonIntersectionCandidatePairs +=
                        polygonDiagnostics.intersectionCandidatePairs;
                    diagnostics.polygonCdtPointTriangleTests +=
                        polygonDiagnostics.cdtPointTriangleTests;
                    diagnostics.polygonCdtPointBadTriangles +=
                        polygonDiagnostics.cdtPointBadTriangles;
                    diagnostics.polygonCdtConstraintEdgeTests +=
                        polygonDiagnostics.cdtConstraintEdgeTests;
                    diagnostics.polygonCdtConstraintCrossTests +=
                        polygonDiagnostics.cdtConstraintCrossTests;
                    diagnostics.polygonCdtConstraintsAlreadyPresent +=
                        polygonDiagnostics.cdtConstraintsAlreadyPresent;
                    diagnostics.polygonCdtConstraintsInserted +=
                        polygonDiagnostics.cdtConstraintsInserted;
                    diagnostics.polygonCdtPeakTriangles = std::max(
                        diagnostics.polygonCdtPeakTriangles,
                        polygonDiagnostics.cdtPeakTriangles);
                    diagnostics.polygonCdtPointCapacityGrowths +=
                        polygonDiagnostics.cdtPointCapacityGrowths;
                    diagnostics.polygonCdtTriangleCapacityGrowths +=
                        polygonDiagnostics.cdtTriangleCapacityGrowths;
                    diagnostics.polygonTriangles +=
                        polygonDiagnostics.triangleCount;
                }
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
                        if (clamp && ctx.style.usesOfficialProviderContract()) {
                            const Cartographic source =
                                ctx.ellipsoid.cartesianToCartographic(p);
                            fillRange.clampSource.push_back(
                                static_cast<float>(source.longitude()));
                            fillRange.clampSource.push_back(
                                static_cast<float>(source.latitude()));
                            fillRange.clampSource.push_back(fillColorPacked);
                        }
                    }
                    for (uint32_t idx : fill.fillIndices) {
                        fillIndices.push_back(base + idx);
                    }
                }
            }
            // 外环 outline。孔环 outline 留后续。
            // ⚠️ 默认关(fillOutlineEnabled=false):裁剪到瓦片边界的面外环
            // 含瓦片角点,用路网配色描边会在瓦片角画出成簇灰色射线。
            if (!ctx.style.fillOutlineEnabled) break;
            if (stencilLine) {
                // P6d:闭合墙带体(首尾 wrap)。
                appendLineVolume(ctx, geometry->rings.front(), paintOrder,
                                 /*closed=*/true, resolvedLineColor, origin, hasOrigin,
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
                /*closed=*/true,
                ctx.style.lineRoundJoin && !emitClampSource,
                /*endpointCapPrimitives=*/false);
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
                    appendLineVolume(ctx, ring, paintOrder, /*closed=*/false,
                                     resolvedLineColor, origin, hasOrigin,
                                     lineVolumeGroups);
                }
                break;
            }
            TessellatedLine line = LineTessellator::tessellate(
                *geometry, ctx.ellipsoid, tessHeightOffset,
                /*closed=*/false,
                ctx.style.lineRoundJoin && !emitClampSource,
                endpointCapPrimitives);
            if (!line.vertices.empty()) {
                ensureOrigin(line.vertices.front().pos);
                std::vector<float> perVertexColors;
                const std::vector<float>* colors = nullptr;
                if (ctx.style.lineColorGradientByHeight) {
                    perVertexColors = packLineHeightGradientColors(
                        line, ctx.ellipsoid, ctx.style);
                    colors = &perVertexColors;
                }
                appendLineMesh(line, origin, lineColorPacked, lineVerts,
                               lineIndices, colors);
                if (lineClampSourceOut && emitClampSource) {
                    // 每个最终 line vertex 保存完整地理语义，使 terrain
                    // revision 重钳能原样保留 miter/join/endpoint-cap。
                    // 布局:pos/prev/next lon-lat + side/length/color = 9f。
                    lineClampSourceOut->reserve(
                        lineClampSourceOut->size() +
                        line.vertices.size() * 9);
                    for (size_t i = 0; i < line.vertices.size(); ++i) {
                        const LineVertex& vertex = line.vertices[i];
                        const Cartographic p = ctx.ellipsoid.cartesianToCartographic(vertex.pos);
                        const Cartographic previous = ctx.ellipsoid.cartesianToCartographic(vertex.prev);
                        const Cartographic next = ctx.ellipsoid.cartesianToCartographic(vertex.next);
                        lineClampSourceOut->push_back(
                            static_cast<float>(p.longitude()));
                        lineClampSourceOut->push_back(
                            static_cast<float>(p.latitude()));
                        lineClampSourceOut->push_back(static_cast<float>(previous.longitude()));
                        lineClampSourceOut->push_back(static_cast<float>(previous.latitude()));
                        lineClampSourceOut->push_back(static_cast<float>(next.longitude()));
                        lineClampSourceOut->push_back(static_cast<float>(next.latitude()));
                        lineClampSourceOut->push_back(vertex.side);
                        lineClampSourceOut->push_back(vertex.lengthSoFar);
                        lineClampSourceOut->push_back(
                            colors ? (*colors)[i] : lineColorPacked);
                    }
                }
            }
            break;
        }
        case GeometryType::Point: {
            // P5a/P6c 符号:每点 billboard quad(anchor 3f + offsetUnit 2f
            // + uv 2f + color 4B + shape 1f = 36B,quad 在顶点着色器按
            // u_pointSizePx 展开)。Point 几何 = rings[0][0];贴地时
            // geometry 已由预变换写好(采样 + offset)。
            if (geometry->rings.empty() || geometry->rings[0].empty()) break;
            // Transparent point styles are label-only contracts. Do not emit
            // invisible quad geometry or a point draw just to preserve the
            // label path below.
            const std::array<float, 4> pointColor = resolveColor(
                ctx.style.pointColorExpr, feature.properties,
                ctx.style.pointColor);
            if (pointColor[3] <= 0.0f) break;
            const float pointColorPacked = packColorFloat(pointColor);
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
            appendSymbolQuad({static_cast<float>(rel.x()),
                              static_cast<float>(rel.y()),
                              static_cast<float>(rel.z())},
                             sym, pointColorPacked, 1.0f, pointVerts,
                             pointIndices);
            break;
        }
    }

    // ---- P5b/P5c 文字标注 ----
    // properties[labelProperty] 非空且字体就绪 → 锚点处 glyph quads
    // (32B:anchor+offsetPx+uv+opacity)。锚点:Point 本体/LineString 弧长
    // 中点/Polygon 环 bbox 中心;贴地时中心点单独采样。顶点 opacity 初始
    // 0(placement fade-in 起点),同时登记 LabelEntry 供逐帧避让。
    if (!ctx.style.usesOfficialProviderContract() &&
        ctx.glyphAtlas && ctx.glyphAtlas->ready() &&
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

        const int labelRank = integerProperty(feature.properties, "rank", 6);
        const auto zoomRange = featureZoomRange(feature.properties);
        if (!zoomRange) return;
        const auto [featureMinZoom, featureMaxZoom] = *zoomRange;
        const int minZoom = effectiveLabelMinZoom(
            ctx.style, labelStyleGroup, featureMinZoom);
        const int maxZoom = effectiveLabelMaxZoom(
            ctx.style, labelStyleGroup, featureMaxZoom);
        if (!labelStyleVisibleAtZoom(ctx.style, labelStyleGroup,
                                     ctx.labelViewZoom)) return;
        const float featureLabelSize = resolvePositiveFloat(
            ctx.style.labelSizeExpr, feature.properties,
            ctx.style.labelSizePx);
        appendLabelTextQuads(*ctx.glyphAtlas, ctx.style, feature.id, anchor,
                             anchor, {ax, ay, az}, {ax, ay, az},
                             propIt->second, nullptr, labelVerts,
                             labelIndices, nullptr, labelEntries, labelRank, minZoom,
                             maxZoom,
                             resolvedLabelSizePx(ctx.style, labelStyleGroup,
                                                 ctx.labelViewZoom,
                                                 featureLabelSize),
                             resolveNonNegativeFloat(
                                 ctx.style.labelOffsetExpr,
                                 feature.properties,
                                 ctx.style.labelOffsetPx),
                             resolvedLabelHaloWidthPx(
                                 ctx.style, labelStyleGroup,
                                 ctx.labelViewZoom));
    }
}

float FeatureRenderLayer::labelLetterSpacingAdvancePx(
    size_t drawableGlyphCount, float letterSpacingEm, float labelSizePx) {
    if (drawableGlyphCount < 2) return 0.0f;
    return static_cast<float>(drawableGlyphCount - 1) *
           std::max(0.0f, letterSpacingEm) * std::max(0.0f, labelSizePx);
}

int FeatureRenderLayer::effectiveLabelMinZoom(const FeatureRenderStyle& style,
                                               int styleGroup,
                                               int featureMinZoom) {
    auto it = style.labelMinZoomByStyleGroup.find(styleGroup);
    return it == style.labelMinZoomByStyleGroup.end()
               ? featureMinZoom
               : std::max(featureMinZoom, it->second);
}

int FeatureRenderLayer::effectiveLabelMaxZoom(const FeatureRenderStyle& style,
                                               int styleGroup,
                                               int featureMaxZoom) {
    auto it = style.labelMaxZoomByStyleGroup.find(styleGroup);
    return it == style.labelMaxZoomByStyleGroup.end()
               ? featureMaxZoom
               : std::min(featureMaxZoom, it->second);
}

bool FeatureRenderLayer::labelStyleVisibleAtZoom(
    const FeatureRenderStyle& style, int styleGroup, double gateZoom) {
    const auto it = style.labelZoomWindowsByStyleGroup.find(styleGroup);
    if (it == style.labelZoomWindowsByStyleGroup.end()) return true;
    return std::any_of(it->second.begin(), it->second.end(),
                       [gateZoom](const auto& window) {
                           return gateZoom >= window.first &&
                                  gateZoom < window.second;
                       });
}

float FeatureRenderLayer::resolvedLabelSizePx(const FeatureRenderStyle& style,
                                               int styleGroup,
                                               double viewZoom,
                                               float featureSizePx) {
    const auto grouped = style.labelSizeExprByStyleGroup.find(styleGroup);
    const StyleExpression::Ptr expr =
        grouped == style.labelSizeExprByStyleGroup.end() ? nullptr
                                                         : grouped->second;
    if (!expr) {
        return style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LabelIdentity)
                   ? 0.0f : featureSizePx;
    }
    const auto value = expr->evaluate(nullptr, viewZoom);
    if (!value || value->kind() != StyleValue::Kind::Number ||
        !std::isfinite(value->number()) || value->number() <= 0.0) {
        return style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LabelIdentity)
                   ? 0.0f : featureSizePx;
    }
    return static_cast<float>(value->number());
}

std::array<float, 4> FeatureRenderLayer::resolvedLabelColor(
    const FeatureRenderStyle& style, int styleGroup, double viewZoom) {
    auto it = style.labelColorExprByStyleGroup.find(styleGroup);
    if (it != style.labelColorExprByStyleGroup.end() && it->second) {
        const auto value = it->second->evaluate(nullptr, viewZoom);
        if (value && value->kind() == StyleValue::Kind::Color) {
            return value->color();
        }
        return style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LabelIdentity)
                   ? std::array<float, 4>{0, 0, 0, 0}
                   : style.labelColor;
    }
    return style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LabelIdentity)
               ? std::array<float, 4>{0, 0, 0, 0}
               : style.labelColor;
}

std::array<float, 4> FeatureRenderLayer::resolvedLabelHaloColor(
    const FeatureRenderStyle& style, int styleGroup, double viewZoom) {
    const bool official = style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::LabelIdentity);
    const auto grouped =
        style.labelHaloColorExprByStyleGroup.find(styleGroup);
    const StyleExpression::Ptr expr =
        grouped == style.labelHaloColorExprByStyleGroup.end()
            ? nullptr
            : grouped->second;
    // Official halo appearance is expression-table owned. Missing, malformed,
    // or wrong-typed entries must not revive generic scalar/fixed-map state.
    if (official && !expr) return {0, 0, 0, 0};
    if (!expr) {
        const auto fixed = style.labelHaloColorByStyleGroup.find(styleGroup);
        return fixed == style.labelHaloColorByStyleGroup.end()
            ? style.labelHaloColor
            : fixed->second;
    }
    const auto value = expr->evaluate(nullptr, viewZoom);
    if (value && value->kind() == StyleValue::Kind::Color) return value->color();
    if (official) return {0, 0, 0, 0};
    const auto fixed = style.labelHaloColorByStyleGroup.find(styleGroup);
    return fixed == style.labelHaloColorByStyleGroup.end()
        ? style.labelHaloColor
        : fixed->second;
}

float FeatureRenderLayer::resolvedLabelHaloWidthPx(
    const FeatureRenderStyle& style, int styleGroup, double viewZoom) {
    const auto grouped = style.labelHaloWidthExprByStyleGroup.find(styleGroup);
    if (grouped == style.labelHaloWidthExprByStyleGroup.end() ||
        !grouped->second) {
        return style.requiresOfficial(
                   FeatureRenderStyle::OfficialRequirement::LabelIdentity)
                   ? -1.0f
                   : std::max(0.0f, style.labelHaloPx);
    }
    const auto value = grouped->second->evaluate(nullptr, viewZoom);
    if (!value || value->kind() != StyleValue::Kind::Number ||
        !std::isfinite(value->number()) || value->number() < 0.0) {
        return style.requiresOfficial(
                   FeatureRenderStyle::OfficialRequirement::LabelIdentity)
                   ? -1.0f
                   : std::max(0.0f, style.labelHaloPx);
    }
    return static_cast<float>(value->number());
}

void FeatureRenderLayer::appendLabelTextQuads(
    GlyphAtlas& atlas,
    const FeatureRenderStyle& style,
    FeatureId featureId,
    const Vec3& anchorEcef,
    const Vec3& tangentEcef,
    const std::array<float, 3>& rel,
    const std::array<float, 3>& tangentRel,
    const std::string& text,
    const std::vector<uint32_t>* splitIndicesUtf16,
    std::vector<float>& labelVerts,
    std::vector<uint32_t>& labelIndices,
    std::vector<uint32_t>* backgroundIndices,
    std::vector<LabelEntry>& labelEntries,
    int rank,
    int minZoom,
    int maxZoom,
    float labelSizePx,
    float labelOffsetPx,
    float labelHaloPx,
    const FeatureRenderStyle::ProviderLabelLayout* providerLayout,
    float providerPixelRatio,
    uint64_t repeatGroup,
    float repeatDistancePx,
    float angleRad,
    float letterSpacingEm,
    float paddingXPx,
    float paddingYPx,
    const std::vector<std::array<double, 3>>* pathCartographic,
    const Ellipsoid* pathEllipsoid,
    const Vec3* pathOrigin,
    double pathMetersPerPixel,
    const IconAtlas* iconAtlas,
    const ProjectedPathSampler* projectedPath,
    const std::vector<ProjectedPathSample>* officialGlyphSamples) {
    // 布局:单行 LTR advance,水平居中,基线抬 labelOffsetPx。
    const float resolvedLabelSizePx =
        labelSizePx > 0.0f ? labelSizePx : style.labelSizePx;
    if (!(resolvedLabelSizePx > 0.0f)) return;
    const float s = resolvedLabelSizePx / atlas.metricPixelHeight();
    struct MeasuredLine {
        std::vector<uint32_t> codepoints;
        float advance = 0.0f;
        size_t drawableGlyphs = 0;
    };
    const auto isEcmaScriptWhitespace = [](uint32_t cp) {
        return (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0020 ||
               cp == 0x00A0 || cp == 0x1680 ||
               (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
               cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
               cp == 0x3000 || cp == 0xFEFF;
    };
    std::vector<MeasuredLine> lines(1);
    const std::vector<uint32_t> decoded = GlyphAtlas::decodeUtf8(text);
    uint32_t totalUtf16Units = 0;
    std::vector<uint32_t> scalarEnds;
    scalarEnds.reserve(decoded.size());
    for (uint32_t cp : decoded) {
        totalUtf16Units += cp > 0xFFFF ? 2u : 1u;
        scalarEnds.push_back(totalUtf16Units);
    }
    size_t boundaryIndex = 0;
    uint32_t utf16Offset = 0;
    uint32_t previousBoundary = 0;
    bool validBoundaries = true;
    if (splitIndicesUtf16) {
        for (uint32_t boundary : *splitIndicesUtf16) {
            if (boundary <= previousBoundary || boundary > totalUtf16Units ||
                !std::binary_search(scalarEnds.begin(), scalarEnds.end(),
                                    boundary)) {
                validBoundaries = false;
                break;
            }
            previousBoundary = boundary;
        }
    }
    for (uint32_t cp : decoded) {
        if (cp == '\r') continue;
        if (cp == '\n') {
            lines.emplace_back();
            continue;
        }
        lines.back().codepoints.push_back(cp);
        utf16Offset += cp > 0xFFFF ? 2u : 1u;
        if (validBoundaries && splitIndicesUtf16) {
            while (boundaryIndex < splitIndicesUtf16->size() &&
                   utf16Offset == (*splitIndicesUtf16)[boundaryIndex]) {
                ++boundaryIndex;
                // Fixed getSpiltLineWithSpiltIndex treats Mii as split
                // positions, not as a closed line-end array: after the final
                // provided split it still appends the remaining suffix.
                if (utf16Offset < totalUtf16Units) {
                    lines.emplace_back();
                }
            }
        }
    }
    if (splitIndicesUtf16 &&
        (!validBoundaries || boundaryIndex != splitIndicesUtf16->size())) {
        return;
    }
    // Exact reachable getSpiltLineWithSpiltIndex whitespace contract. The
    // fixed j8t/tQ consumers call this helper only for a non-empty split list,
    // then preserve leading whitespace and apply `/\s+$/` to the assembled
    // string. Keep this at layout time so provider name identity stays exact.
    auto trimBack = [&]() {
        while (!lines.empty()) {
            auto& cps = lines.back().codepoints;
            while (!cps.empty() && isEcmaScriptWhitespace(cps.back())) {
                cps.pop_back();
            }
            if (!cps.empty() || lines.size() == 1) break;
            lines.pop_back();
        }
    };
    if (splitIndicesUtf16 && !splitIndicesUtf16->empty()) trimBack();
    const float letterSpacingPx = std::max(0.0f, letterSpacingEm) *
                                  resolvedLabelSizePx;
    float totalAdvance = 0.0f;
    for (MeasuredLine& line : lines) {
        for (uint32_t cp : line.codepoints) {
            if (const GlyphAtlas::Glyph* g = atlas.ensureGlyph(cp)) {
                line.advance += g->advance * s;
                ++line.drawableGlyphs;
            }
        }
        line.advance += labelLetterSpacingAdvancePx(
            line.drawableGlyphs, letterSpacingEm, resolvedLabelSizePx);
        totalAdvance = std::max(totalAdvance, line.advance);
    }
    const IconAtlas::Frame* dynamicBackgroundFrame =
        providerLayout && iconAtlas &&
                !providerLayout->dynamicBackgroundImage.empty()
            ? iconAtlas->frame(providerLayout->dynamicBackgroundImage)
            : nullptr;
    if (providerLayout &&
        !providerLayout->dynamicBackgroundImage.empty() &&
        !dynamicBackgroundFrame) {
        // q8t is one provider-owned marker: its measured text and stretched
        // background are published atomically. Re-check at the final geometry
        // consumer so a stale/pre-atlas label source cannot revive bare text.
        return;
    }
    std::optional<FeatureRenderStyle::ProviderLabelLayout> measuredLayout;
    if (providerLayout && dynamicBackgroundFrame) {
        measuredLayout = *providerLayout;
        const float dynamicInsetPx = 4.0f * providerPixelRatio;
        measuredLayout->iconWidthPx = totalAdvance + dynamicInsetPx;
        measuredLayout->iconHeightPx = static_cast<float>(lines.size()) *
                                           (resolvedLabelSizePx +
                                            dynamicInsetPx) +
                                       dynamicInsetPx;
        providerLayout = &*measuredLayout;
    }
    const float resolvedHaloPx = labelHaloPx >= 0.0f
        ? labelHaloPx
        : std::max(0.0f, style.labelHaloPx);
    const float genericBaseY =
        labelOffsetPx >= 0.0f ? labelOffsetPx : style.labelOffsetPx;
    float penX = -totalAdvance * 0.5f;
    float baseY = genericBaseY;
    float officialTextMinX = 0.0f;
    float officialTextMinY = 0.0f;
    float officialTextMaxX = 0.0f;
    float officialTextMaxY = 0.0f;
    float officialOriginX = 0.0f;
    if (providerLayout) {
        const auto labelDirection = providerLayout->direction;
        const float labelOffsetXPx = providerLayout->offsetXPx;
        const float labelOffsetYPx = providerLayout->offsetYPx;
        const float iconWidthPx = providerLayout->iconWidthPx;
        const float iconHeightPx = providerLayout->iconHeightPx;
        const auto iconTopLeftDown =
            officialIconTopLeftDown(*providerLayout);
        const float dx = iconTopLeftDown[0];
        const float dyDown = iconTopLeftDown[1];
        const bool hasIcon = iconWidthPx > 0.0f && iconHeightPx > 0.0f;
        const float officialLineGapPx = 3.0f * providerPixelRatio;
        const float blockHeight = resolvedLabelSizePx * lines.size() +
            officialLineGapPx * (lines.size() - 1);
        const float halfBlock = resolvedLabelSizePx * 0.5f * lines.size() +
            officialLineGapPx * 0.5f * (lines.size() - 1);
        const float textBoxHeight = resolvedLabelSizePx * 1.25f * lines.size() +
            officialLineGapPx * (lines.size() - 1);
        // NebulaLabelFormat emits padding [top=0,right=1,bottom=0,left=1].
        // AO applies right/left padding to horizontal icon/text separation,
        // while top/bottom add no vertical gap. Keep this separate from BO's
        // 3px inter-line advance and scale it exactly once with provider DPR.
        const float officialHorizontalPaddingPx = providerPixelRatio;
        float originX = dx;
        float originYDown = dyDown;
        float boxX = 0.0f;
        float boxYDown = 0.0f;
        switch (labelDirection) {
            case FeatureRenderStyle::LabelDirection::Right:
                originX = hasIcon
                    ? dx + iconWidthPx + officialHorizontalPaddingPx
                    : dx - officialHorizontalPaddingPx;
                originYDown = hasIcon
                    ? dyDown + iconHeightPx * 0.5f - halfBlock
                    : dyDown - halfBlock;
                penX = originX;
                boxX = originX;
                boxYDown = originYDown;
                break;
            case FeatureRenderStyle::LabelDirection::Left:
                originX = hasIcon
                    ? dx - officialHorizontalPaddingPx
                    : dx + officialHorizontalPaddingPx;
                originYDown = hasIcon
                    ? dyDown + iconHeightPx * 0.5f - halfBlock
                    : dyDown - halfBlock;
                penX = originX - totalAdvance;
                boxX = penX;
                boxYDown = originYDown;
                break;
            case FeatureRenderStyle::LabelDirection::Top:
                originX = hasIcon
                    ? dx + iconWidthPx * 0.5f
                    : dx + officialHorizontalPaddingPx;
                originYDown = hasIcon
                    ? dyDown - resolvedLabelSizePx * 0.25f - blockHeight
                    : dyDown;
                penX = originX - totalAdvance * 0.5f;
                boxX = penX;
                boxYDown = originYDown;
                break;
            case FeatureRenderStyle::LabelDirection::Bottom:
                originX = hasIcon
                    ? dx + iconWidthPx * 0.5f
                    : dx + officialHorizontalPaddingPx;
                originYDown = hasIcon ? dyDown + iconHeightPx
                                      : dyDown - blockHeight;
                penX = originX - totalAdvance * 0.5f;
                boxX = penX;
                boxYDown = originYDown;
                break;
            case FeatureRenderStyle::LabelDirection::Center:
                originX = hasIcon ? dx + iconWidthPx * 0.5f : dx;
                originYDown = hasIcon
                    ? dyDown + iconHeightPx * 0.5f - halfBlock
                    : dyDown - halfBlock;
                penX = originX - totalAdvance * 0.5f;
                boxX = penX;
                boxYDown = originYDown;
                break;
        }
        originX += labelOffsetXPx;
        originYDown += labelOffsetYPx;
        penX += labelOffsetXPx;
        boxX += labelOffsetXPx;
        boxYDown += labelOffsetYPx;
        baseY = -originYDown;
        officialOriginX = originX;
        officialTextMinX = boxX;
        officialTextMaxX = boxX + totalAdvance;
        officialTextMinY = -(boxYDown + textBoxHeight);
        officialTextMaxY = -boxYDown;
    }
    const size_t entryVertexStart = labelVerts.size();
    const size_t entryIndexStart = labelIndices.size();
    const size_t entryBackgroundIndexStart =
        backgroundIndices ? backgroundIndices->size() : 0;
    if (providerLayout && backgroundIndices && dynamicBackgroundFrame) {
        const IconAtlas::Frame* frame = dynamicBackgroundFrame;
            const OfficialIconBounds bounds =
                officialIconBounds(*providerLayout);
            const uint32_t base =
                static_cast<uint32_t>(labelVerts.size() / 11);
            const float corners[4][4] = {
                {bounds.minX, bounds.minY, frame->u0, frame->v1},
                {bounds.maxX, bounds.minY, frame->u1, frame->v1},
                {bounds.maxX, bounds.maxY, frame->u1, frame->v0},
                {bounds.minX, bounds.maxY, frame->u0, frame->v0}};
            for (const auto& c : corners) {
                labelVerts.insert(labelVerts.end(),
                                  {rel[0], rel[1], rel[2],
                                   tangentRel[0], tangentRel[1], tangentRel[2],
                                   c[0], c[1], 0.0f, c[2], c[3]});
            }
            const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t index : quad) {
            backgroundIndices->push_back(base + index);
        }
    }
    std::vector<double> pathLengths;
    double pathTotalMeters = 0.0;
    const bool followPath = pathCartographic && pathEllipsoid && pathOrigin &&
                            pathCartographic->size() >= 2 &&
                            (pathMetersPerPixel > 0.0 || projectedPath);
    if (followPath) {
        pathLengths.reserve(pathCartographic->size());
        pathLengths.push_back(0.0);
        const double radius = pathEllipsoid->maximumRadius();
        for (size_t i = 1; i < pathCartographic->size(); ++i) {
            const auto& a = (*pathCartographic)[i - 1];
            const auto& b = (*pathCartographic)[i];
            const double meanLat = (a[1] + b[1]) * 0.5;
            pathTotalMeters += std::hypot(
                (b[0] - a[0]) * std::cos(meanLat) * radius,
                (b[1] - a[1]) * radius);
            pathLengths.push_back(pathTotalMeters);
        }
        // 道路短于文字实际覆盖长度时不把多个字形钳到端点堆叠；该候选
        // 交给更长的同名线段/相邻瓦片承载。
        if (projectedPath && !officialGlyphSamples) {
            if (totalAdvance > projectedPath->totalScreenLengthPx()) return;
        } else if (totalAdvance * pathMetersPerPixel > pathTotalMeters) {
            return;
        }
    }
    auto samplePathPosition = [&](double distanceMeters) {
        distanceMeters = std::clamp(distanceMeters, 0.0, pathTotalMeters);
        auto upper = std::upper_bound(pathLengths.begin(), pathLengths.end(),
                                      distanceMeters);
        size_t i = static_cast<size_t>(upper - pathLengths.begin());
        i = std::clamp<size_t>(i, 1, pathCartographic->size() - 1);
        const auto& a = (*pathCartographic)[i - 1];
        const auto& b = (*pathCartographic)[i];
        const double segment = pathLengths[i] - pathLengths[i - 1];
        const double t = segment > 0.0
                             ? (distanceMeters - pathLengths[i - 1]) / segment
                             : 0.0;
        const double lon = a[0] + (b[0] - a[0]) * t;
        const double lat = a[1] + (b[1] - a[1]) * t;
        const double height = a[2] + (b[2] - a[2]) * t;
        return pathEllipsoid->cartographicToCartesian(
            Cartographic(lon, lat, height));
    };
    auto samplePath = [&](double distanceMeters, Vec3& glyphAnchor,
                          Vec3& glyphTangent, std::array<float, 3>& glyphRel,
                          std::array<float, 3>& glyphTangentRel) {
        if (!followPath || !(pathTotalMeters > 0.0)) return false;
        distanceMeters = std::clamp(distanceMeters, 0.0, pathTotalMeters);
        glyphAnchor = samplePathPosition(distanceMeters);
        // Use a centred path derivative instead of the current segment's
        // direction.  A segment-local tangent jumps discontinuously at every
        // encoded vertex, making adjacent glyphs look kinked even though their
        // anchors lie exactly on the road. Sampling equally on both sides of
        // the glyph follows the polyline through the corner and gives the
        // projected shader a stable, forward tangent. Two framebuffer pixels
        // are enough to suppress quantisation noise without smoothing away a
        // real bend over the width of a glyph.
        const double tangentHalfSpan = std::max(0.25,
                                                pathMetersPerPixel * 2.0);
        double before = std::max(0.0, distanceMeters - tangentHalfSpan);
        double after = std::min(pathTotalMeters,
                                distanceMeters + tangentHalfSpan);
        if (!(after > before)) {
            before = std::max(0.0, distanceMeters - 0.25);
            after = std::min(pathTotalMeters, distanceMeters + 0.25);
        }
        const Vec3 tangentBefore = samplePathPosition(before);
        const Vec3 tangentAfter = samplePathPosition(after);
        Vec3 tangentDelta = tangentAfter - tangentBefore;
        // A perfectly symmetric hairpin can cancel the centred chord. Keep
        // the normal centred result, but degrade deterministically to the
        // forward half (or the backward half at the path end) instead of
        // handing the shader a zero vector and making that glyph horizontal.
        if (tangentDelta.length() <= 1e-6) {
            tangentDelta = tangentAfter - glyphAnchor;
            if (tangentDelta.length() <= 1e-6) {
                tangentDelta = glyphAnchor - tangentBefore;
            }
        }
        // The label shader expects a second position, not a direction vector.
        // Re-anchor the centred chord at the glyph so precision remains local
        // to the tile origin and the tangent always points forward.
        glyphTangent = glyphAnchor + tangentDelta;
        const Vec3 rd = glyphAnchor - *pathOrigin;
        const Vec3 td = glyphTangent - *pathOrigin;
        glyphRel = {static_cast<float>(rd.x()), static_cast<float>(rd.y()),
                    static_cast<float>(rd.z())};
        glyphTangentRel = {static_cast<float>(td.x()),
                           static_cast<float>(td.y()),
                           static_cast<float>(td.z())};
        return true;
    };
    // BO's inter-line gap is 3 CSS px. Provider label sizes are already in
    // framebuffer pixels here, so scale the gap by the same provider ratio;
    // otherwise DPR2 geometry advances 3px while AO/EO collision advances
    // 6px. Generic labels retain their existing framebuffer-pixel contract.
    const float lineStep = resolvedLabelSizePx +
        (providerLayout ? 3.0f * providerPixelRatio : 3.0f);
    std::vector<LabelCollisionPart> pathCollisionParts;
    size_t officialGlyphIndex = 0;
    for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const MeasuredLine& line = lines[lineIndex];
        float linePenX = penX;
        if (providerLayout) {
            switch (providerLayout->direction) {
                case FeatureRenderStyle::LabelDirection::Left:
                    linePenX = officialOriginX - line.advance;
                    break;
                case FeatureRenderStyle::LabelDirection::Right:
                    linePenX = officialOriginX;
                    break;
                default:
                    linePenX = officialOriginX - line.advance * 0.5f;
                    break;
            }
        } else if (lines.size() > 1) {
            linePenX = -line.advance * 0.5f;
        }
        const float lineBaseY = baseY - lineStep * lineIndex;
        size_t emittedInLine = 0;
        for (uint32_t cp : line.codepoints) {
            const GlyphAtlas::Glyph* g = atlas.ensureGlyph(cp);
            if (!g) continue;
            const ProjectedPathSample* officialPathSample = nullptr;
            if (officialGlyphSamples) {
                if (officialGlyphIndex >= officialGlyphSamples->size()) return;
                officialPathSample =
                    &(*officialGlyphSamples)[officialGlyphIndex++];
            }
            if (g->hasBitmap) {
            Vec3 glyphAnchor = anchorEcef;
            Vec3 glyphTangent = tangentEcef;
            std::array<float, 3> glyphRel = rel;
            std::array<float, 3> glyphTangentRel = tangentRel;
            const float glyphCenterX = linePenX + g->advance * s * 0.5f;
            bool sampled = false;
            if (officialPathSample) {
                const ProjectedPathSample& pathSample = *officialPathSample;
                glyphAnchor = pathSample.positionEcef;
                glyphTangent = pathSample.tangentEcef;
                const Vec3 rd = glyphAnchor - *pathOrigin;
                const Vec3 td = glyphTangent - *pathOrigin;
                glyphRel = {static_cast<float>(rd.x()),
                            static_cast<float>(rd.y()),
                            static_cast<float>(rd.z())};
                glyphTangentRel = {static_cast<float>(td.x()),
                                   static_cast<float>(td.y()),
                                   static_cast<float>(td.z())};
                sampled = true;
            } else if (projectedPath) {
                const ProjectedPathSample pathSample = projectedPath->sample(
                    projectedPath->totalScreenLengthPx() * 0.5 +
                    glyphCenterX);
                glyphAnchor = pathSample.positionEcef;
                glyphTangent = pathSample.tangentEcef;
                const Vec3 rd = glyphAnchor - *pathOrigin;
                const Vec3 td = glyphTangent - *pathOrigin;
                glyphRel = {static_cast<float>(rd.x()),
                            static_cast<float>(rd.y()),
                            static_cast<float>(rd.z())};
                glyphTangentRel = {static_cast<float>(td.x()),
                                   static_cast<float>(td.y()),
                                   static_cast<float>(td.z())};
                sampled = true;
            } else {
                sampled = samplePath(
                    pathTotalMeters * 0.5 +
                        glyphCenterX * pathMetersPerPixel,
                    glyphAnchor, glyphTangent, glyphRel, glyphTangentRel);
            }
            const float localPenX = sampled ? -g->advance * s * 0.5f
                                            : linePenX;
            const float x0 = localPenX + g->offsetX * s;
            const float x1 = x0 + g->width * s;
            const float yTop = lineBaseY + g->offsetY * s;
            const float yBot = yTop - g->height * s;
            if (sampled) {
                pathCollisionParts.push_back(LabelCollisionPart{
                    glyphAnchor, glyphTangent,
                    x0 - resolvedHaloPx,
                    yBot - resolvedHaloPx,
                    x1 + resolvedHaloPx,
                    yTop + resolvedHaloPx});
            }
            const uint32_t base =
                static_cast<uint32_t>(labelVerts.size() / 11);
            const float corners[4][4] = {
                {x0, yBot, g->u0, g->v1},
                {x1, yBot, g->u1, g->v1},
                {x1, yTop, g->u1, g->v0},
                {x0, yTop, g->u0, g->v0}};
            for (const auto& c : corners) {
                labelVerts.push_back(glyphRel[0]);
                labelVerts.push_back(glyphRel[1]);
                labelVerts.push_back(glyphRel[2]);
                labelVerts.push_back(glyphTangentRel[0]);
                labelVerts.push_back(glyphTangentRel[1]);
                labelVerts.push_back(glyphTangentRel[2]);
                labelVerts.push_back(c[0]);
                labelVerts.push_back(c[1]);
                labelVerts.push_back(0.0f);  // offset.z=opacity(placement 回写)
                labelVerts.push_back(c[2]);
                labelVerts.push_back(c[3]);
            }
            const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
            for (uint32_t idx : quad) labelIndices.push_back(base + idx);
            }
            linePenX += g->advance * s;
            if (++emittedInLine < line.drawableGlyphs) {
                linePenX += letterSpacingPx;
            }
        }
    }
    // 登记 placement 候选:碰撞盒 = 整行文字盒(行度量 ascent/descent
    // 换算标注字号)+ halo 外扩。空文本/字形全缺 → 无顶点不登记。
    if (labelVerts.size() > entryVertexStart) {
        LabelEntry entry;
        entry.featureId = featureId;
        entry.rank = rank;
        entry.minZoom = minZoom;
        entry.maxZoom = maxZoom;
        entry.anchorEcef = anchorEcef;
        entry.tangentEcef = tangentEcef;
        if (providerLayout) {
            // Official EO returns the text collision rectangle with the
            // provider padding applied per side. The current official style
            // publishes [top=0,right=1,bottom=0,left=1], so only the
            // horizontal sides expand. Do not revive the old symmetric 3px
            // approximation: it over-rejects POIs vertically and also grows
            // the independent icon rectangle, while official `rd` uses the
            // icon frame's exact bounds.
            const float horizontalCollisionPaddingPx = providerPixelRatio;
            entry.boxMinXPx = officialTextMinX -
                horizontalCollisionPaddingPx;
            entry.boxMaxXPx = officialTextMaxX +
                horizontalCollisionPaddingPx;
            entry.boxMinYPx = officialTextMinY;
            entry.boxMaxYPx = officialTextMaxY;
            if (providerLayout->iconWidthPx > 0.0f &&
                providerLayout->iconHeightPx > 0.0f) {
                const OfficialIconBounds bounds =
                    officialIconBounds(*providerLayout);
                entry.hasIconBox = true;
                entry.iconBoxMinXPx = bounds.minX;
                entry.iconBoxMinYPx = bounds.minY;
                entry.iconBoxMaxXPx = bounds.maxX;
                entry.iconBoxMaxYPx = bounds.maxY;
            }
        } else {
            entry.boxMinXPx = -totalAdvance * 0.5f - resolvedHaloPx;
            entry.boxMaxXPx = totalAdvance * 0.5f + resolvedHaloPx;
            // descent() 已取正(基线下距离),下缘 = 基线减。
            entry.boxMinYPx = baseY - atlas.descent() * s - resolvedHaloPx;
            entry.boxMaxYPx = baseY + atlas.ascent() * s + resolvedHaloPx;
        }
        entry.repeatGroup = repeatGroup;
        entry.repeatDistancePx = repeatDistancePx;
        entry.angleRad = angleRad;
        entry.paddingXPx = providerLayout ? 0.0f : paddingXPx;
        entry.paddingYPx = providerLayout ? 0.0f : paddingYPx;
        entry.collisionParts = std::move(pathCollisionParts);
        entry.vertexFloatStart = entryVertexStart;
        entry.vertexFloatCount = labelVerts.size() - entryVertexStart;
        entry.labelIndexStart = entryIndexStart;
        entry.labelIndexCount = labelIndices.size() - entryIndexStart;
        entry.backgroundIndexStart = entryBackgroundIndexStart;
        entry.backgroundIndexCount = backgroundIndices
            ? backgroundIndices->size() - entryBackgroundIndexStart
            : 0;
        labelEntries.push_back(entry);
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
    int paintOrder,
    const AreaSampleFn& sample,
    const std::array<float, 4>& fillColor,
    Vec3& origin,
    bool& hasOrigin,
    VolumeCpuGroups& volumeGroups) {
    if (feature.rings.empty() || feature.rings.front().size() < 3) return;

    // P6b:按解析色归组(同色体积并集计数,组间独立命令对)。
    VolumeCpuGroup& group = volumeGroups[{paintOrder, packColorU32(fillColor)}];
    group.paintOrder = paintOrder;
    group.color = fillColor;
    std::vector<float>& volumeVerts = group.verts;
    std::vector<uint32_t>& volumeIndices = group.indices;

    // ---- 高度范围:区域元数据(优先)或环顶点 + 粗内部网格采样 ± margin ----
    double minH = std::numeric_limits<double>::max();
    double maxH = std::numeric_limits<double>::lowest();
    // 有区域 min/max 元数据时直接用,**整段跳过下面的逐点采样**:那是本函数
    // 里唯一的 O(要素 × 网格) 成本,也是底图量级下走不通几何贴地的原因。
    // 元数据本身更保守(覆盖整块瓦片而非单个要素),体会高一些,代价只是
    // 多画片元 —— 换来的是采样次数归零。
    const bool useRegionRange = ctx.hasTerrainHeightRange;
    auto probe = [&](double lng, double lat) {
        if (useRegionRange) return;
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
    // 有区域元数据时整个网格探测都省掉:pointInRings2D 是 O(环顶点数),
    // 64×64 网格下它本身就是大头,只让 probe 空转是省不掉的。
    if (!useRegionRange) {
        for (int gy = 0; gy <= gridY; ++gy) {
            for (int gx = 0; gx <= gridX; ++gx) {
                const double lng =
                    west + (east - west) * gx / gridX;
                const double lat =
                    south + (north - south) * gy / gridY;
                if (pointInRings2D(lng, lat, feature.rings)) probe(lng, lat);
            }
        }
    }
    if (useRegionRange) {
        minH = ctx.terrainMinHeight;
        maxH = ctx.terrainMaxHeight;
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
    int paintOrder,
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

    // 体的半高:逐点采样时点已贴合地形,±120m 兜住采样误差就够;走区域
    // 元数据时点全在范围中点,半高必须**吞下整个范围**(半跨 + 余量),
    // 否则体穿不透地形 → 该瓦片的线成片消失(不是变淡,是没有)。
    const double volumeHalfHeight =
        ctx.hasTerrainHeightRange
            ? (ctx.terrainMaxHeight - ctx.terrainMinHeight) * 0.5 +
                  kVolumeMarginMeters
            : kVolumeMarginMeters;

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

    VolumeCpuGroup& group =
        lineVolumeGroups[{paintOrder, packColorU32(lineColor)}];
    group.paintOrder = paintOrder;
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
            const Vec3 bottom = sec.center - sec.up * volumeHalfHeight;
            const Vec3 top = sec.center + sec.up * volumeHalfHeight;
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
    const std::vector<PaintRange>& fillRanges,
    const std::vector<float>& lineVerts,
    const std::vector<uint32_t>& lineIndices,
    const std::vector<PaintRange>& lineRanges,
    const std::vector<float>& pointVerts,
    const std::vector<uint32_t>& pointIndices,
    const std::vector<PaintRange>& pointRanges,
    std::vector<float>&& labelVerts,
    const std::vector<uint32_t>& labelIndices,
    const std::vector<PaintRange>& labelRanges,
    std::vector<LabelEntry>&& labelEntries,
    const VolumeCpuGroups& volumeGroups,
    const VolumeCpuGroups& lineVolumeGroups,
    const std::vector<float>& extrudeVerts,
    const std::vector<uint32_t>& extrudeIndices,
    const std::vector<PaintRange>& extrudeRanges,
    BucketGpu& out) const {
    out = BucketGpu{};
    out.origin = origin;
    auto uploadGroups =
        [&](const VolumeCpuGroups& cpu,
            std::vector<BucketGpu::VolumeGroupGpu>& gpuOut) {
            for (const auto& [groupKey, group] : cpu) {
                if (group.indices.empty()) continue;
                BucketGpu::VolumeGroupGpu gpu;
                gpu.paintOrder = group.paintOrder;
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
            for (const PaintRange& range : labelRanges) {
                out.labelRanges.push_back(BucketGpu::PaintRangeGpu{
                    range.paintOrder, static_cast<int>(range.indexOffset),
                    static_cast<int>(range.indexCount), range.minZoom,
                    range.maxZoom, range.styleGroup});
            }
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
            for (const PaintRange& range : pointRanges) {
                out.pointRanges.push_back(BucketGpu::PaintRangeGpu{
                    range.paintOrder, static_cast<int>(range.indexOffset),
                    static_cast<int>(range.indexCount), range.minZoom,
                    range.maxZoom});
            }
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
            for (const PaintRange& range : fillRanges) {
                BucketGpu::PaintRangeGpu gpuRange{
                    range.paintOrder, static_cast<int>(range.indexOffset),
                    static_cast<int>(range.indexCount), range.minZoom,
                    range.maxZoom};
                gpuRange.styleGroup = range.styleGroup;
                out.fillRanges.push_back(gpuRange);
            }
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
            for (const PaintRange& range : lineRanges) {
                BucketGpu::PaintRangeGpu gpuRange{
                    range.paintOrder, static_cast<int>(range.indexOffset),
                    static_cast<int>(range.indexCount), range.minZoom,
                    range.maxZoom};
                gpuRange.styleGroup = range.styleGroup;
                out.lineRanges.push_back(gpuRange);
            }
        }
    }
    if (!extrudeIndices.empty()) {
        out.extrudeVertexBuffer = makeBuffer(
            renderDevice_, extrudeVerts.data(),
            extrudeVerts.size() * sizeof(float), BufferDesc::Type::Vertex);
        out.extrudeIndexBuffer = makeBuffer(
            renderDevice_, extrudeIndices.data(),
            extrudeIndices.size() * sizeof(uint32_t),
            BufferDesc::Type::Index);
        if (out.extrudeVertexBuffer && out.extrudeIndexBuffer) {
            out.extrudeIndexCount =
                static_cast<int>(extrudeIndices.size());
            for (const PaintRange& range : extrudeRanges) {
                BucketGpu::PaintRangeGpu gpuRange{
                    range.paintOrder, static_cast<int>(range.indexOffset),
                    static_cast<int>(range.indexCount), range.minZoom,
                    range.maxZoom};
                gpuRange.styleGroup = range.styleGroup;
                out.extrudeRanges.push_back(gpuRange);
            }
        }
    }
    return out.fillIndexCount > 0 || out.lineIndexCount > 0 ||
           out.pointIndexCount > 0 || out.labelIndexCount > 0 ||
           !out.volumeGroups.empty() || !out.lineVolumeGroups.empty() ||
           out.extrudeIndexCount > 0;
}

void FeatureRenderLayer::rebuildBucket(BucketKey key) {
    if (officialProfileSealed_) {
        // Editable FeatureStore has no provider provenance. Even a feature
        // carrying plausible amap_* properties is not an official decoded
        // payload, so sealed profiles must never consume this second path.
        buckets_.erase(key);
        return;
    }
    const auto* memberIds = store_.featuresInBucket(key);
    if (!memberIds || memberIds->empty()) {
        buckets_.erase(key);
        return;
    }

    // 按 ID 升序镶嵌:桶内 buffer 布局确定性(测试可对拍,编辑重镶稳定)。
    std::vector<FeatureId> ids(memberIds->begin(), memberIds->end());
    std::sort(ids.begin(), ids.end());

    std::map<std::pair<int, int>, PaintGeometryCpu> fillGroups;
    std::map<std::pair<int, int>, PaintGeometryCpu> lineGroups;
    std::map<int, PaintGeometryCpu> pointGroups;
    std::map<std::pair<int, int>, LabelGeometryCpu> labelGroups;
    std::map<std::tuple<int, int, int, int>, PaintGeometryCpu> extrudeGroups;
    std::vector<float> fillVerts;      // xyz,相对桶原点
    std::vector<uint32_t> fillIndices;
    std::vector<PaintRange> fillRanges;
    std::vector<float> lineVerts;      // 12 float/顶点(VectorLine48)
    std::vector<uint32_t> lineIndices;
    std::vector<PaintRange> lineRanges;
    std::vector<float> pointVerts;     // kPointVertexFloats/顶点
    std::vector<uint32_t> pointIndices;
    std::vector<float> labelVerts;     // 11 float/顶点(VectorLabel44)
    std::vector<uint32_t> labelIndices;
    std::vector<LabelEntry> labelEntries;
    VolumeCpuGroups volumeGroups;      // P6 stencil 挤出体(按色分组)
    VolumeCpuGroups lineVolumeGroups;  // P6d stencil 线墙带(按色分组)
    Vec3 origin = Vec3::zero();
    bool hasOrigin = false;
    std::vector<float> extrudeVerts;  // V6 建筑挤出(7 float/顶点)
    std::vector<uint32_t> extrudeIndices;
    std::vector<PaintRange> extrudeRanges;

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
        if (!style_.admitsGeometry(feature->type)) continue;
        if (!hasValidOfficialDrawOrder(style_, *feature)) continue;
        // Official provider points require the tile-symbol admission/resolver
        // pipeline (identity, amap_rank, late-bound icon and collision flags).
        // FeatureStore has no provider-symbol lifecycle, so fail closed rather
        // than exposing its generic circle/rank fallback as a second contract.
        if (feature->type == GeometryType::Point &&
            style_.requiresOfficial(
                FeatureRenderStyle::OfficialRequirement::PointIdentity)) {
            continue;
        }
        const auto featureZoom = featureZoomRange(
            feature->properties, style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::ZoomWindow));
        if (style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::ZoomWindow) && !featureZoom) continue;
        const int paintOrder = resolvePaintOrder(style_, *feature);
        const int fillStyleGroup = feature->type == GeometryType::Polygon
            ? resolveInteger(style_.fillStyleGroupExpr, feature->properties, 0)
            : 0;
        const bool consumesLineIdentity =
            feature->type == GeometryType::LineString ||
            (feature->type == GeometryType::Polygon &&
             style_.fillOutlineEnabled);
        const int lineStyleGroup = consumesLineIdentity
            ? resolveInteger(style_.lineStyleGroupExpr, feature->properties, 0)
            : 0;
        if (feature->type == GeometryType::Polygon &&
            style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::FillIdentity) && fillStyleGroup == 0) {
            continue;
        }
        if (feature->type == GeometryType::LineString &&
            style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity) && lineStyleGroup == 0) {
            continue;
        }
        const int labelPaintOrder = resolveLabelPaintOrder(
            style_, feature->properties, paintOrder);
        const int labelStyleGroup = resolveLabelVisualStyleGroup(
            style_, feature->properties, 0);
        const int extrusionMinZoom = featureZoom ? featureZoom->first : 0;
        const int extrusionMaxZoom = featureZoom ? featureZoom->second : 30;
        tessellateFeatureInto(tessellationContext(), *feature, paintOrder,
                              fillStyleGroup, lineStyleGroup, labelStyleGroup,
                              sample,
                              origin, hasOrigin,
                              fillGroups[{paintOrder, fillStyleGroup}],
                              lineGroups[{paintOrder, lineStyleGroup}],
                              pointGroups[paintOrder],
                              labelGroups[{labelPaintOrder, labelStyleGroup}],
                              volumeGroups, lineVolumeGroups,
                              extrudeGroups[{paintOrder, fillStyleGroup,
                                             extrusionMinZoom,
                                             extrusionMaxZoom}]);
    }

    flattenStylePaintRanges(fillGroups, 4, fillVerts, fillIndices,
                            &fillRanges);
    flattenLinePaintRanges(lineGroups, kLineVertexFloats, lineVerts,
                           lineIndices, &lineRanges);
    std::vector<PaintRange> pointRanges;
    flattenPaintRanges(pointGroups, kPointVertexFloats, pointVerts,
                       pointIndices, &pointRanges);
    std::vector<PaintRange> labelRanges;
    flattenLabelRanges(labelGroups, labelVerts, labelIndices, labelEntries,
                       &labelRanges);
    flattenExtrusionRanges(extrudeGroups, extrudeVerts, extrudeIndices,
                           &extrudeRanges);

    if (fillIndices.empty() && lineIndices.empty() && pointIndices.empty() &&
        labelIndices.empty() && volumeGroups.empty() &&
        lineVolumeGroups.empty() && extrudeIndices.empty()) {
        buckets_.erase(key);
        return;
    }

    BucketGpu gpu;
    if (!uploadBucketGpu(origin, fillVerts, fillIndices, fillRanges,
                         lineVerts, lineIndices, lineRanges,
                         pointVerts, pointIndices, pointRanges,
                         std::move(labelVerts), labelIndices,
                         labelRanges,
                         std::move(labelEntries),
                         volumeGroups, lineVolumeGroups,
                         extrudeVerts, extrudeIndices, extrudeRanges, gpu)) {
        // buffer 创建失败:丢弃本桶,脏区已消费 → 下次编辑该桶时重试。
        buckets_.erase(key);
        return;
    }
    buckets_[key] = std::move(gpu);
    // V27:重镶顶点 opacity 重置 0,新 entries 须即时全量 placement 置 target
    // (等 300ms 节流窗撞上停帧 = 标注隐形)。
    labelsAwaitingPlacement_ = true;
}

// ================= E1:MVT 瓦片桶(worker 全链镶嵌) =================

bool FeatureRenderLayer::stencilClassificationSupported() const {
    return renderDevice_ && renderDevice_->supportsStencilClassification();
}

int FeatureRenderLayer::resolvePaintOrder(const FeatureRenderStyle& style,
                                          const Feature& feature) {
    if (style.requiresOfficial(
            FeatureRenderStyle::OfficialRequirement::DrawOrder)) {
        const auto official = strictIntegerProperty(
            feature.properties, "amap_draworder");
        return official.value_or(0);
    }
    return resolveInteger(style.paintOrderExpr, feature.properties,
                          style.paintOrder);
}

void FeatureRenderLayer::flattenPaintRanges(
    const std::map<int, PaintGeometryCpu>& ranges, size_t floatsPerVertex,
    std::vector<float>& verts, std::vector<uint32_t>& indices,
    std::vector<PaintRange>* outRanges, std::vector<float>* clampSource) {
    verts.clear();
    indices.clear();
    if (clampSource) clampSource->clear();
    if (outRanges) outRanges->clear();
    for (const auto& [paintOrder, group] : ranges) {
        if (group.indices.empty()) continue;
        const uint32_t baseVertex =
            static_cast<uint32_t>(verts.size() / floatsPerVertex);
        const uint32_t indexOffset = static_cast<uint32_t>(indices.size());
        verts.insert(verts.end(), group.verts.begin(), group.verts.end());
        for (uint32_t index : group.indices) {
            indices.push_back(baseVertex + index);
        }
        if (clampSource) {
            clampSource->insert(clampSource->end(), group.clampSource.begin(),
                                group.clampSource.end());
        }
        if (outRanges) {
            outRanges->push_back(PaintRange{
                paintOrder, indexOffset,
                static_cast<uint32_t>(group.indices.size())});
        }
    }
}

void FeatureRenderLayer::flattenExtrusionRanges(
    const std::map<std::tuple<int, int, int, int>, PaintGeometryCpu>& ranges,
    std::vector<float>& verts, std::vector<uint32_t>& indices,
    std::vector<PaintRange>* outRanges,
    std::vector<float>* clampSource) {
    verts.clear();
    indices.clear();
    if (outRanges) outRanges->clear();
    if (clampSource) clampSource->clear();
    for (const auto& [key, group] : ranges) {
        if (group.indices.empty()) continue;
        const auto [paintOrder, styleGroup, minZoom, maxZoom] = key;
        const uint32_t baseVertex = static_cast<uint32_t>(verts.size() / 7);
        const uint32_t indexOffset = static_cast<uint32_t>(indices.size());
        verts.insert(verts.end(), group.verts.begin(), group.verts.end());
        for (uint32_t index : group.indices) indices.push_back(baseVertex + index);
        if (clampSource) {
            clampSource->insert(clampSource->end(), group.clampSource.begin(),
                                group.clampSource.end());
        }
        if (outRanges) {
            PaintRange range{paintOrder, indexOffset,
                             static_cast<uint32_t>(group.indices.size()),
                             minZoom, maxZoom};
            range.styleGroup = styleGroup;
            outRanges->push_back(range);
        }
    }
}

void FeatureRenderLayer::flattenLinePaintRanges(
    const std::map<std::pair<int, int>, PaintGeometryCpu>& ranges,
    size_t floatsPerVertex, std::vector<float>& verts,
    std::vector<uint32_t>& indices, std::vector<PaintRange>* outRanges,
    std::vector<float>* clampSource) {
    verts.clear();
    indices.clear();
    if (clampSource) clampSource->clear();
    if (outRanges) outRanges->clear();
    for (const auto& [key, group] : ranges) {
        if (group.indices.empty()) continue;
        const auto [paintOrder, styleGroup] = key;
        const uint32_t baseVertex =
            static_cast<uint32_t>(verts.size() / floatsPerVertex);
        const uint32_t indexOffset = static_cast<uint32_t>(indices.size());
        verts.insert(verts.end(), group.verts.begin(), group.verts.end());
        for (uint32_t index : group.indices) indices.push_back(baseVertex + index);
        if (clampSource) {
            clampSource->insert(clampSource->end(), group.clampSource.begin(),
                                group.clampSource.end());
        }
        if (outRanges) {
            PaintRange range{paintOrder, indexOffset,
                             static_cast<uint32_t>(group.indices.size())};
            range.styleGroup = styleGroup;
            outRanges->push_back(range);
        }
    }
}

void FeatureRenderLayer::flattenStylePaintRanges(
    const std::map<std::pair<int, int>, PaintGeometryCpu>& ranges,
    size_t floatsPerVertex, std::vector<float>& verts,
    std::vector<uint32_t>& indices, std::vector<PaintRange>* outRanges) {
    verts.clear();
    indices.clear();
    if (outRanges) outRanges->clear();
    for (const auto& [key, group] : ranges) {
        if (group.indices.empty()) continue;
        const auto [paintOrder, styleGroup] = key;
        const uint32_t baseVertex =
            static_cast<uint32_t>(verts.size() / floatsPerVertex);
        const uint32_t indexOffset = static_cast<uint32_t>(indices.size());
        verts.insert(verts.end(), group.verts.begin(), group.verts.end());
        for (uint32_t index : group.indices) {
            indices.push_back(baseVertex + index);
        }
        if (outRanges) {
            PaintRange range{paintOrder, indexOffset,
                             static_cast<uint32_t>(group.indices.size())};
            range.styleGroup = styleGroup;
            outRanges->push_back(range);
        }
    }
}

void FeatureRenderLayer::flattenLabelRanges(
    const std::map<std::pair<int, int>, LabelGeometryCpu>& ranges,
    std::vector<float>& verts,
    std::vector<uint32_t>& indices,
    std::vector<LabelEntry>& entries,
    std::vector<PaintRange>* outRanges,
    std::vector<uint32_t>* backgroundIndices,
    std::vector<PaintRange>* backgroundRanges) {
    // VectorLabel ABI: anchor(3)+tangent(3)+offset/opacity(3)+uv(2).
    // Stale 8-float arithmetic generates out-of-range draw indices.
    constexpr size_t kLabelVertexFloats = 11;
    verts.clear();
    indices.clear();
    entries.clear();
    if (outRanges) outRanges->clear();
    if (backgroundIndices) backgroundIndices->clear();
    if (backgroundRanges) backgroundRanges->clear();
    for (const auto& [key, group] : ranges) {
        const int paintOrder = key.first;
        const int styleGroup = key.second;
        if (group.indices.empty()) continue;
        const uint32_t baseVertex =
            static_cast<uint32_t>(verts.size() / kLabelVertexFloats);
        const uint32_t indexOffset = static_cast<uint32_t>(indices.size());
        const uint32_t backgroundIndexOffset = backgroundIndices
            ? static_cast<uint32_t>(backgroundIndices->size()) : 0;
        const size_t floatOffset = verts.size();
        verts.insert(verts.end(), group.verts.begin(), group.verts.end());
        for (uint32_t index : group.indices) {
            indices.push_back(baseVertex + index);
        }
        if (backgroundIndices) {
            for (uint32_t index : group.backgroundIndices) {
                backgroundIndices->push_back(baseVertex + index);
            }
        }
        for (const LabelEntry& source : group.entries) {
            LabelEntry entry = source;
            entry.vertexFloatStart += floatOffset;
            entry.labelIndexStart += indexOffset;
            entry.backgroundIndexStart += backgroundIndexOffset;
            entries.push_back(std::move(entry));
            const uint32_t entryIndexCount =
                static_cast<uint32_t>(source.labelIndexCount);
            const uint32_t entryOffset = indexOffset +
                static_cast<uint32_t>(source.labelIndexStart);
            if (outRanges) {
                if (entryIndexCount > 0) {
                    bool merged = false;
                    if (!outRanges->empty()) {
                        PaintRange& last = outRanges->back();
                        if (last.paintOrder == paintOrder &&
                            last.styleGroup == styleGroup &&
                            last.minZoom == source.minZoom &&
                            last.maxZoom == source.maxZoom &&
                            last.indexOffset + last.indexCount == entryOffset) {
                            last.indexCount += entryIndexCount;
                            merged = true;
                        }
                    }
                    if (!merged) {
                        PaintRange range{
                            paintOrder, entryOffset, entryIndexCount,
                            source.minZoom, source.maxZoom};
                        range.styleGroup = styleGroup;
                        outRanges->push_back(range);
                    }
                }
            }
            if (backgroundRanges && source.backgroundIndexCount > 0) {
                PaintRange range{
                    paintOrder,
                    backgroundIndexOffset + static_cast<uint32_t>(
                        source.backgroundIndexStart),
                    static_cast<uint32_t>(source.backgroundIndexCount),
                    source.minZoom, source.maxZoom};
                range.styleGroup = styleGroup;
                backgroundRanges->push_back(range);
            }
        }
    }
}

FeatureRenderLayer::TileMeshCpu FeatureRenderLayer::tessellateTileMesh(
    const TessellationContext& ctx, const std::vector<Feature>& features) {
    TileMeshCpu mesh;
    TessellationContext measuredCtx = ctx;
    measuredCtx.tileMeshDiagnostics =
        measuredCtx.collectDiagnostics ? &mesh.diagnostics : nullptr;
    using DiagnosticClock = std::chrono::steady_clock;
    auto elapsedMs = [](const DiagnosticClock::time_point& start) {
        return std::chrono::duration<double, std::milli>(
                   DiagnosticClock::now() - start).count();
    };
    auto propertyInt = [](const Feature& feature, const char* name) {
        const auto it = feature.properties.find(name);
        if (it == feature.properties.end()) return 0;
        char* end = nullptr;
        const long value = std::strtol(it->second.c_str(), &end, 10);
        return end != it->second.c_str() && *end == '\0'
                   ? static_cast<int>(value)
                   : 0;
    };
    // 非点要素的 point/label 副产物は渲染线程で图集定型する。命名
    // Amap road-name payload 折线走 label-only TileSymbolCpu；其它非点标签仍不在
    // tile worker 路径中生成，避免把 point 顶点混进 fill 流。
    // stencil 体不再丢弃:贴地瓦片的内容全在里面。
    std::map<std::pair<int, int>, PaintGeometryCpu> fillGroups;
    std::map<std::pair<int, int>, PaintGeometryCpu> lineGroups;
    std::map<std::tuple<int, int, int, int>, PaintGeometryCpu> extrudeGroups;

    // 贴地采样器恒空:worker 拿不到地形瓦片注册表(那是渲染线程状态)。
    // 贴地改由 ctx 的区域高度范围驱动 —— 那是一对标量,可安全跨线程。
    const AreaSampleFn noSample;

    for (const Feature& feature : features) {
        auto featureStart = ctx.collectDiagnostics
                                ? DiagnosticClock::now()
                                : DiagnosticClock::time_point{};
        size_t featurePoints = 0;
        if (ctx.collectDiagnostics) {
            for (const auto& ring : feature.rings) featurePoints += ring.size();
        }
        auto recordAcceptedInput = [&]() {
            ++mesh.diagnostics.admittedFeatures;
            mesh.diagnostics.rings += feature.rings.size();
            mesh.diagnostics.points += featurePoints;
        };
        auto recordSlowest = [&](double featureMs) {
            if (featureMs <= mesh.diagnostics.slowestFeatureMs) return;
            mesh.diagnostics.slowestFeatureMs = featureMs;
            mesh.diagnostics.slowestClassCode =
                propertyInt(feature, "amap_class");
            mesh.diagnostics.slowestSubKey =
                propertyInt(feature, "amap_subkey");
            mesh.diagnostics.slowestRings = feature.rings.size();
            mesh.diagnostics.slowestPoints = featurePoints;
        };
        using RejectionReason =
            FeatureTileMesh::TessellationDiagnostics::RejectionReason;
        auto reject = [&](RejectionReason reason) {
            if (!ctx.collectDiagnostics) return;
            ++mesh.diagnostics.rejectedFeatures;
            ++mesh.diagnostics.rejectionCounts[static_cast<size_t>(reason)];
            ++mesh.diagnostics.rejectedIdentities[{
                static_cast<uint8_t>(feature.type),
                propertyInt(feature, "amap_class"),
                propertyInt(feature, "amap_subkey")}];
            mesh.diagnostics.admissionMs += elapsedMs(featureStart);
        };
        if (!ctx.style.admitsGeometry(feature.type)) {
            reject(RejectionReason::GeometryType);
            continue;
        }
        if (!hasValidOfficialDrawOrder(ctx.style, feature)) {
            reject(RejectionReason::DrawOrder);
            continue;
        }
        const auto featureZoom = featureZoomRange(
            feature.properties, ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::ZoomWindow));
        if (ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::ZoomWindow) && !featureZoom) {
            reject(RejectionReason::ZoomWindow);
            continue;
        }
        const int paintOrder = resolvePaintOrder(ctx.style, feature);
        // Identity expressions are provider taxonomy walks. Resolve only the
        // identity consumed by this geometry: a line never reads fill style,
        // and a polygon reads line style only when it emits an outline.
        // Expressions are pure, so this removes dead integer/string lookup
        // work without changing geometry, FP evaluation, or output order.
        const int fillStyleGroup = feature.type == GeometryType::Polygon
            ? resolveInteger(ctx.style.fillStyleGroupExpr,
                             feature.properties, 0)
            : 0;
        const bool consumesLineIdentity =
            feature.type == GeometryType::LineString ||
            (feature.type == GeometryType::Polygon &&
             ctx.style.fillOutlineEnabled);
        const int lineStyleGroup = consumesLineIdentity
            ? resolveInteger(ctx.style.lineStyleGroupExpr,
                             feature.properties, 0)
            : 0;
        if (feature.type == GeometryType::Polygon &&
            ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::FillIdentity) && fillStyleGroup == 0) {
            reject(RejectionReason::FillIdentity);
            continue;
        }
        // 点要素不定型 quad,进实例表(TileSymbolCpu):纯计算部分(锚点
        // 投影 + 样式表达式求值)worker 做完,图集相关留给准入定型。
        if (feature.type == GeometryType::Point) {
            const double admissionMs = ctx.collectDiagnostics
                                           ? elapsedMs(featureStart)
                                           : 0.0;
            const auto symbolStart = ctx.collectDiagnostics
                                         ? DiagnosticClock::now()
                                         : DiagnosticClock::time_point{};
            const size_t symbolsBefore = mesh.symbols.size();
            appendTileSymbol(ctx, feature, paintOrder, mesh);
            if (ctx.collectDiagnostics) {
                const double symbolMs = elapsedMs(symbolStart);
                mesh.diagnostics.admissionMs += admissionMs;
                mesh.diagnostics.symbolMs += symbolMs;
                if (mesh.symbols.size() > symbolsBefore) {
                    recordAcceptedInput();
                    ++mesh.diagnostics.symbolFeatures;
                } else {
                    ++mesh.diagnostics.rejectedFeatures;
                    RejectionReason reason = RejectionReason::DegenerateGeometry;
                    const std::string classCode = [&]() {
                        const auto it = feature.properties.find("amap_class");
                        return it == feature.properties.end() ? std::string() : it->second;
                    }();
                    const std::string subKey = [&]() {
                        const auto it = feature.properties.find("amap_subkey");
                        return it == feature.properties.end() ? std::string() : it->second;
                    }();
                    if (ctx.style.pointIdentityValidator &&
                        !ctx.style.pointIdentityValidator(classCode, subKey)) {
                        reason = RejectionReason::PointIdentity;
                    } else if (ctx.style.requiresOfficial(
                                   FeatureRenderStyle::OfficialRequirement::Rank) &&
                               !officialAmapPlacementRank(feature.properties)) {
                        reason = RejectionReason::Rank;
                    }
                    ++mesh.diagnostics.rejectionCounts[
                        static_cast<size_t>(reason)];
                    ++mesh.diagnostics.rejectedIdentities[{
                        static_cast<uint8_t>(feature.type),
                        propertyInt(feature, "amap_class"),
                        propertyInt(feature, "amap_subkey")}];
                }
                recordSlowest(admissionMs + symbolMs);
            }
            continue;
        }
        bool emittedLineSymbol = false;
        double lineSymbolPrefixMs = 0.0;
        if (feature.type == GeometryType::LineString) {
            const auto nameIt = feature.properties.find("name");
            const int labelStyleGroup = resolveLabelVisualStyleGroup(
                ctx.style, feature.properties, 0);
            const bool officialLineLabel =
                ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LabelIdentity) &&
                labelStyleGroup != 0;
            if (officialLineLabel &&
                nameIt != feature.properties.end() && !nameIt->second.empty()) {
                const double admissionMs = ctx.collectDiagnostics
                                               ? elapsedMs(featureStart)
                                               : 0.0;
                const auto symbolStart = ctx.collectDiagnostics
                                             ? DiagnosticClock::now()
                                             : DiagnosticClock::time_point{};
                const size_t symbolsBefore = mesh.symbols.size();
                appendTileLineLabel(ctx, feature, paintOrder, mesh);
                if (ctx.collectDiagnostics) {
                    const double symbolMs = elapsedMs(symbolStart);
                    mesh.diagnostics.admissionMs += admissionMs;
                    mesh.diagnostics.symbolMs += symbolMs;
                    if (mesh.symbols.size() > symbolsBefore) {
                        ++mesh.diagnostics.symbolFeatures;
                        emittedLineSymbol = true;
                    }
                    lineSymbolPrefixMs = admissionMs + symbolMs;
                    featureStart = DiagnosticClock::now();
                }
            }
            if (ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity) && lineStyleGroup == 0) {
                if (ctx.collectDiagnostics && emittedLineSymbol) {
                    recordAcceptedInput();
                    recordSlowest(lineSymbolPrefixMs);
                } else {
                    RejectionReason reason = RejectionReason::LineIdentity;
                    if (labelStyleGroup != 0) {
                        if (ctx.style.requiresOfficial(
                                FeatureRenderStyle::OfficialRequirement::Rank) &&
                            !officialAmapPlacementRank(feature.properties)) {
                            reason = RejectionReason::Rank;
                        } else if (ctx.style.lineLabelLayoutByStyleGroup.find(
                                       labelStyleGroup) ==
                                   ctx.style.lineLabelLayoutByStyleGroup.end()) {
                            reason = RejectionReason::LabelLayout;
                        } else if (feature.rings.empty() ||
                                   feature.rings.front().size() < 2) {
                            reason = RejectionReason::DegenerateGeometry;
                        }
                    }
                    reject(reason);
                }
                continue;
            }
        }
        const double admissionMs = ctx.collectDiagnostics
                                       ? elapsedMs(featureStart)
                                       : 0.0;
        const auto geometryStart = ctx.collectDiagnostics
                                       ? DiagnosticClock::now()
                                       : DiagnosticClock::time_point{};
        const int extrusionMinZoom = featureZoom ? featureZoom->first : 0;
        const int extrusionMaxZoom = featureZoom ? featureZoom->second : 30;
        size_t fillIndexCountBefore = 0;
        size_t fillVolumeIndexCountBefore = 0;
        size_t extrudeIndexCountBefore = 0;
        if (ctx.collectDiagnostics) {
            fillIndexCountBefore =
                fillGroups[{paintOrder, fillStyleGroup}].indices.size();
            for (const auto& entry : mesh.fillVolumeGroups) {
                fillVolumeIndexCountBefore += entry.second.indices.size();
            }
            extrudeIndexCountBefore =
                extrudeGroups[{paintOrder, fillStyleGroup, extrusionMinZoom,
                               extrusionMaxZoom}].indices.size();
        }
        PaintGeometryCpu discardedPoint;
        LabelGeometryCpu discardedLabel;
        tessellateFeatureInto(measuredCtx, feature, paintOrder, fillStyleGroup,
                              lineStyleGroup, 0,
                              noSample, mesh.origin,
                              mesh.hasOrigin,
                              fillGroups[{paintOrder, fillStyleGroup}],
                              lineGroups[{paintOrder, lineStyleGroup}],
                              discardedPoint,
                              discardedLabel, mesh.fillVolumeGroups,
                              mesh.lineVolumeGroups,
                              extrudeGroups[{paintOrder, fillStyleGroup,
                                             extrusionMinZoom,
                                             extrusionMaxZoom}]);
        if (ctx.collectDiagnostics) {
            const double geometryMs = elapsedMs(geometryStart);
            recordAcceptedInput();
            mesh.diagnostics.admissionMs += admissionMs;
            if (feature.type == GeometryType::Polygon) {
                ++mesh.diagnostics.polygonFeatures;
                const size_t extrudeIndexCountAfter =
                    extrudeGroups[{paintOrder, fillStyleGroup,
                                   extrusionMinZoom,
                                   extrusionMaxZoom}].indices.size();
                size_t fillVolumeIndexCountAfter = 0;
                for (const auto& entry : mesh.fillVolumeGroups) {
                    fillVolumeIndexCountAfter += entry.second.indices.size();
                }
                const bool emittedExtrusion =
                    extrudeIndexCountAfter > extrudeIndexCountBefore;
                const bool emittedFill =
                    fillGroups[{paintOrder, fillStyleGroup}].indices.size() >
                        fillIndexCountBefore ||
                    fillVolumeIndexCountAfter > fillVolumeIndexCountBefore;
                if (emittedExtrusion && !emittedFill) {
                    ++mesh.diagnostics.extrusionFeatures;
                    mesh.diagnostics.extrusionMs += geometryMs;
                } else {
                    mesh.diagnostics.polygonMs += geometryMs;
                }
            } else if (feature.type == GeometryType::LineString) {
                ++mesh.diagnostics.lineFeatures;
                mesh.diagnostics.lineMs += geometryMs;
            }
            recordSlowest(lineSymbolPrefixMs + admissionMs + geometryMs);
        }
    }
    flattenLinePaintRanges(fillGroups, 4, mesh.fillVerts, mesh.fillIndices,
                           &mesh.fillRanges, &mesh.fillClampSource);
    flattenLinePaintRanges(lineGroups, kLineVertexFloats, mesh.lineVerts,
                           mesh.lineIndices, &mesh.lineRanges,
                           &mesh.lineClampSource);
    flattenExtrusionRanges(extrudeGroups, mesh.extrudeVerts,
                           mesh.extrudeIndices, &mesh.extrudeRanges,
                           &mesh.extrudeClampSource);
    return mesh;
}

void FeatureRenderLayer::appendTileSymbol(const TessellationContext& ctx,
                                          const Feature& feature,
                                          int paintOrder,
                                          TileMeshCpu& mesh) {
    if (feature.rings.empty() || feature.rings[0].empty()) return;
    const auto propertyValue = [&](const std::string& key) -> std::string {
        const auto it = feature.properties.find(key);
        return it == feature.properties.end() ? std::string() : it->second;
    };
    const std::string pointStyleKeyA =
        propertyValue(ctx.style.pointStylePropertyA);
    const std::string pointStyleKeyB =
        propertyValue(ctx.style.pointStylePropertyB);
    if (ctx.style.requiresOfficial(
            FeatureRenderStyle::OfficialRequirement::PointIdentity)) {
        if (ctx.style.pointIdentityValidator &&
            !ctx.style.pointIdentityValidator(pointStyleKeyA,
                                               pointStyleKeyB)) {
            return;
        }
        if (ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::Rank) &&
            !officialAmapPlacementRank(feature.properties)) return;
    }

    const int labelStyleGroup = resolveLabelVisualStyleGroup(
        ctx.style, feature.properties, 0);

    const Cartographic& c = feature.rings[0][0];
    // origin 只是 RTE 参考点,不必在地面上 —— 用原始高投影即可。锚点的
    // 最终高度(贴地采样 + offset)在准入定型时算。
    if (!mesh.hasOrigin) {
        mesh.origin = ctx.ellipsoid.cartographicToCartesian(c);
        mesh.hasOrigin = true;
    }

    TileSymbolCpu sym;
    sym.paintOrder = paintOrder;
    sym.labelPaintOrder = resolveLabelPaintOrder(
        ctx.style, feature.properties, paintOrder);
    sym.labelStyleGroup = labelStyleGroup;
    sym.lonRad = c.longitude();
    sym.latRad = c.latitude();
    sym.heightM = c.height();
    if (ctx.style.requiresOfficial(
            FeatureRenderStyle::OfficialRequirement::PointIdentity)) {
        // Provider-owned point appearance is resolved exactly once from its
        // official identity on the render thread (atlas/display zoom live
        // there). Do not manufacture a generic icon/color/offset state in the
        // worker and later overwrite it.
    } else {
        const auto pointColor = resolveColor(
            ctx.style.pointColorExpr, feature.properties,
            ctx.style.pointColor);
        sym.genericVisual.emplace();
        sym.genericVisual->colorPacked = packColorFloat(pointColor);
        sym.genericVisual->iconEnabled = pointColor[3] > 0.0f;
        sym.genericVisual->icon = resolveString(ctx.style.pointImageExpr,
                                                feature.properties,
                                                ctx.style.pointImage);
    }
    sym.pointStyleKeyA = pointStyleKeyA;
    sym.pointStyleKeyB = pointStyleKeyB;
    sym.rank = ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::Rank)
        ? *officialAmapPlacementRank(feature.properties)
        : integerProperty(feature.properties, "rank", 6);
    const auto zoomRange = featureZoomRange(
        feature.properties, ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::ZoomWindow));
    if (!zoomRange) return;
    const auto [minZoom, maxZoom] = *zoomRange;
    sym.minZoom = minZoom;
    sym.maxZoom = maxZoom;
    if (sym.genericVisual) {
        sym.genericVisual->labelSizePx = resolvePositiveFloat(
            ctx.style.labelSizeExpr, feature.properties, ctx.style.labelSizePx);
        sym.genericVisual->labelOffsetPx = resolveNonNegativeFloat(
            ctx.style.labelOffsetExpr, feature.properties,
            ctx.style.labelOffsetPx);
    }
    const auto nameIt = feature.properties.find("name");
    if (nameIt != feature.properties.end()) sym.name = nameIt->second;
    sym.labelSplitIndicesUtf16 = feature.labelSplitIndicesUtf16;
    mesh.symbols.push_back(std::move(sym));
}

void FeatureRenderLayer::appendTileLineLabel(const TessellationContext& ctx,
                                             const Feature& feature,
                                             int paintOrder,
                                             TileMeshCpu& mesh) {
    if (feature.rings.empty() || feature.rings.front().size() < 2) return;
    const auto& ring = feature.rings.front();
    double total = 0.0;
    for (size_t i = 1; i < ring.size(); ++i) {
        const double dx = (ring[i].longitude() - ring[i - 1].longitude()) *
                          std::cos((ring[i].latitude() + ring[i - 1].latitude()) * 0.5);
        const double dy = ring[i].latitude() - ring[i - 1].latitude();
        total += std::hypot(dx, dy);
    }
    if (!(total > 0.0) || !std::isfinite(total)) return;
    const double half = total * 0.5;
    double walked = 0.0;
    Cartographic anchor = ring.front();
    for (size_t i = 1; i < ring.size(); ++i) {
        const Cartographic& a = ring[i - 1];
        const Cartographic& b = ring[i];
        const double dx = (b.longitude() - a.longitude()) *
                          std::cos((b.latitude() + a.latitude()) * 0.5);
        const double dy = b.latitude() - a.latitude();
        const double seg = std::hypot(dx, dy);
        if (walked + seg >= half) {
            const double t = seg > 0.0 ? (half - walked) / seg : 0.0;
            anchor = Cartographic(a.longitude() + (b.longitude() - a.longitude()) * t,
                                  a.latitude() + (b.latitude() - a.latitude()) * t,
                                  a.height() + (b.height() - a.height()) * t);
            break;
        }
        walked += seg;
    }
    if (!mesh.hasOrigin) {
        mesh.origin = ctx.ellipsoid.cartographicToCartesian(anchor);
        mesh.hasOrigin = true;
    }
    TileSymbolCpu sym;
    sym.paintOrder = paintOrder;
    sym.labelPaintOrder = resolveLabelPaintOrder(
        ctx.style, feature.properties, paintOrder);
    sym.labelStyleGroup = resolveLabelVisualStyleGroup(
        ctx.style, feature.properties, 0);
    if (ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LabelIdentity) &&
        sym.labelStyleGroup == 0) {
        return;
    }
    const auto officialRank = ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::Rank)
        ? officialAmapPlacementRank(feature.properties)
        : std::optional<int>{};
    if (ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::Rank) && !officialRank) {
        return;
    }
    sym.lonRad = anchor.longitude();
    sym.latRad = anchor.latitude();
    sym.heightM = anchor.height();
    const bool officialLabel = ctx.style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::LabelIdentity);
    const auto officialLayout =
        ctx.style.lineLabelLayoutByStyleGroup.find(sym.labelStyleGroup);
    if (officialLabel &&
        officialLayout == ctx.style.lineLabelLayoutByStyleGroup.end()) {
        return;
    }
    sym.labelRepeatDistancePx = officialLabel
        ? officialLayout->second.repeatDistancePx
        : ctx.style.lineLabelRepeatDistancePx;
    if (sym.labelRepeatDistancePx > 0.0f) {
        uint64_t repeatGroup = 1469598103934665603ull;
        for (unsigned char c : feature.properties.at("name")) {
            repeatGroup ^= c;
            repeatGroup *= 1099511628211ull;
        }
        sym.labelRepeatGroup = repeatGroup != 0 ? repeatGroup : 1;
    }
    sym.labelLetterSpacingEm = officialLabel
        ? officialLayout->second.letterSpacingEm
        : ctx.style.lineLabelLetterSpacingEm;
    sym.labelPaddingXPx = officialLabel
        ? officialLayout->second.paddingXPx
        : ctx.style.lineLabelPaddingXPx;
    sym.labelPaddingYPx = officialLabel
        ? officialLayout->second.paddingYPx
        : ctx.style.lineLabelPaddingYPx;
    // 局部 east/north 切线；shader 后续将用锚点与该角度做屏幕投影。
    const size_t mid = ring.size() / 2;
    const size_t i0 = mid > 0 ? mid - 1 : 0;
    const size_t i1 = std::min(mid + 1, ring.size() - 1);
    const double dx = (ring[i1].longitude() - ring[i0].longitude()) *
                      std::cos((ring[i1].latitude() + ring[i0].latitude()) * 0.5);
    const double dy = ring[i1].latitude() - ring[i0].latitude();
    if (std::hypot(dx, dy) > 0.0) sym.labelAngleRad = static_cast<float>(std::atan2(dy, dx));
    sym.rank = officialRank ? *officialRank
                            : integerProperty(feature.properties, "rank", 6);
    const auto zoomRange = featureZoomRange(
        feature.properties, ctx.style.requiresOfficial(FeatureRenderStyle::OfficialRequirement::ZoomWindow));
    if (!zoomRange) return;
    const auto [minZoom, maxZoom] = *zoomRange;
    sym.minZoom = minZoom;
    sym.maxZoom = maxZoom;
    if (!officialLabel) {
        sym.genericVisual.emplace();
        sym.genericVisual->iconEnabled = false;
        sym.genericVisual->labelSizePx = resolvePositiveFloat(
            ctx.style.labelSizeExpr, feature.properties, ctx.style.labelSizePx);
        sym.genericVisual->labelOffsetPx = resolveNonNegativeFloat(
            ctx.style.labelOffsetExpr, feature.properties,
            ctx.style.labelOffsetPx);
    }
    sym.name = feature.properties.at("name");
    sym.labelPathCartographic = officialLineLabelPath(ring);
    const auto& first = sym.labelPathCartographic.front();
    const auto& last = sym.labelPathCartographic.back();
    const double pathDx = (last[0] - first[0]) *
                          std::cos((last[1] + first[1]) * 0.5);
    const double pathDy = last[1] - first[1];
    if (pathDx < 0.0 || (std::abs(pathDx) < 1e-12 && pathDy < 0.0)) {
        std::reverse(sym.labelPathCartographic.begin(),
                     sym.labelPathCartographic.end());
        sym.labelAngleRad = static_cast<float>(std::atan2(
            -std::sin(sym.labelAngleRad), -std::cos(sym.labelAngleRad)));
    }
    mesh.symbols.push_back(std::move(sym));
}

TileMeshCommitResult FeatureRenderLayer::commitTileMesh(
    const TileKey& key, TileMeshCpu& mesh) {
    if (mesh.empty() || !mesh.hasOrigin) {
        dropTileMesh(key);
        return TileMeshCommitResult::EmptyTerminal;
    }
    const auto rangesCover = [](size_t indexCount, const auto& ranges) {
        if (indexCount == 0) return ranges.empty();
        size_t covered = 0;
        for (const auto& range : ranges) {
            if (range.indexCount == 0 || range.indexOffset != covered ||
                static_cast<size_t>(range.indexOffset) + range.indexCount >
                    indexCount) {
                return false;
            }
            covered += range.indexCount;
        }
        return covered == indexCount;
    };
    if (!rangesCover(mesh.fillIndices.size(), mesh.fillRanges) ||
        !rangesCover(mesh.lineIndices.size(), mesh.lineRanges) ||
        !rangesCover(mesh.extrudeIndices.size(), mesh.extrudeRanges)) {
        // The range-less tile ABI was the old styleGroup=0/paintOrder fallback.
        // It is no longer accepted: every indexed tile mesh must carry the
        // explicit identity/order ranges produced by the current tessellator.
        platformLog(LogLevel::Warning, "FeatureRenderLayer",
                    "reject range-less/incomplete tile mesh z=%d x=%d y=%d",
                    key.z, key.x, key.y);
        dropTileMesh(key);
        return TileMeshCommitResult::EmptyTerminal;
    }
    if (officialProfileSealed_) {
        const auto allRangesKnown = [](const auto& ranges,
                                       const auto& table) {
            return std::all_of(ranges.begin(), ranges.end(), [&](const auto& r) {
                return r.styleGroup != 0 && table.count(r.styleGroup) != 0;
            });
        };
        const bool fillPayloadValid =
            style_.requiresOfficial(
                FeatureRenderStyle::OfficialRequirement::FillIdentity)
                ? allRangesKnown(mesh.fillRanges,
                                 style_.fillColorExprByStyleGroup)
                : mesh.fillIndices.empty() && mesh.fillRanges.empty();
        const bool extrusionPayloadValid =
            style_.requiresOfficial(
                FeatureRenderStyle::OfficialRequirement::FillIdentity)
                ? allRangesKnown(mesh.extrudeRanges,
                                 style_.extrusionRoofColorByStyleGroup)
                : mesh.extrudeIndices.empty() && mesh.extrudeRanges.empty();
        const bool linePayloadValid =
            style_.requiresOfficial(
                FeatureRenderStyle::OfficialRequirement::LineIdentity)
                ? allRangesKnown(mesh.lineRanges,
                                 style_.lineWidthExprByStyleGroup)
                : mesh.lineIndices.empty() && mesh.lineRanges.empty();
        bool symbolPayloadValid = mesh.symbols.empty();
        if (style_.requiresOfficial(
                FeatureRenderStyle::OfficialRequirement::PointIdentity)) {
            symbolPayloadValid = std::all_of(
                mesh.symbols.begin(), mesh.symbols.end(), [&](const auto& s) {
                    if (s.genericVisual) return false;
                    const bool roadLabel = !s.labelPathCartographic.empty();
                    if (roadLabel) {
                        return s.labelStyleGroup != 0 &&
                               style_.labelSizeExprByStyleGroup.count(
                                   s.labelStyleGroup) != 0;
                    }
                    return style_.pointIdentityValidator &&
                           !s.pointStyleKeyA.empty() &&
                           !s.pointStyleKeyB.empty() &&
                           style_.pointIdentityValidator(s.pointStyleKeyA,
                                                         s.pointStyleKeyB);
                });
        }
        if (!fillPayloadValid || !extrusionPayloadValid ||
            !linePayloadValid || !symbolPayloadValid ||
            !mesh.fillVolumeGroups.empty() ||
            !mesh.lineVolumeGroups.empty()) {
            platformLog(LogLevel::Warning, "FeatureRenderLayer",
                        "reject tile mesh outside sealed official contract "
                        "z=%d x=%d y=%d",
                        key.z, key.x, key.y);
            dropTileMesh(key);
            return TileMeshCommitResult::EmptyTerminal;
        }
    }
    if (style_.usesOfficialProviderContract()) {
        for (TileSymbolCpu& symbol : mesh.symbols) {
            if (symbol.name.empty()) continue;
            symbol.officialInsertionOrder = nextOfficialInsertionOrder_++;
        }
    }
    // P6 分段:commit 实测 ~20ms/瓦(渲染线程),4 块闸 → 80ms 尖峰。
    // 切成 符号构建 / GPU 上传 / 标签烘焙 三段定位。
    using PClock = std::chrono::steady_clock;
    const auto tSym = PClock::now();
    const size_t glyphsBefore =
        glyphAtlas_ ? glyphAtlas_->residentGlyphCount() : 0;
    // 准入定型:实例表 → 符号 quad(图集解析必须渲染线程;一瓦一次非逐帧)。
    // 点/标签必须按当前 view zoom 再做 rank 容量闸；commit 不知道相机
    // zoom，任何此处的破坏性截断都会永久删掉其它档位的数据。这里只登记
    // 完整 CPU source，buildRenderCommands 在本帧按当前窗口物化 top-N。
    std::vector<float> pointVerts;
    std::vector<uint32_t> pointIndices;
    std::vector<PaintRange> pointRanges;
    std::vector<BucketGpu::TileLabelSource> labelSources;

    // 符号刀A/B 诊断:worker→commit 链路的落点计数(排查「数据有点但屏上
    // 无符号」时,第一眼看这行有没有出现、syms/labelSrc 是否为 0)。
    if (!mesh.symbols.empty()) {
        const size_t named = static_cast<size_t>(std::count_if(
            mesh.symbols.begin(), mesh.symbols.end(),
            [](const TileSymbolCpu& s) { return !s.name.empty(); }));
        const size_t labelOnly = static_cast<size_t>(std::count_if(
            mesh.symbols.begin(), mesh.symbols.end(),
            [](const TileSymbolCpu& s) {
                return !s.genericVisual || !s.genericVisual->iconEnabled;
            }));
        platformLog(LogLevel::Info, "TileSymbol",
                    "commit z=%d x=%d y=%d syms=%zu named=%zu labelOnly=%zu "
                    "ground=%d",
                    key.z, key.x, key.y, mesh.symbols.size(),
                    named, labelOnly,
                    style_.altitudeMode == FeatureAltitudeMode::ClampToGround
                        ? 1 : 0);
    }

    const auto tUpload = PClock::now();
    // 整瓦原子替换:先建好新 GPU 资源,成功了才换掉旧的。中途失败保留旧瓦
    // 而不是留半张 —— 半张瓦片在画面上是缺口,比旧数据糟。
    BucketGpu gpu;
    // E 方案 P2:commit 时同源采样钳高(worker 只给了椭球面高度 +
    // lineClampSource;store 路径在镶嵌期已钳真高,无源可空转)。
    clampTileFillHeights(mesh);
    clampTileExtrusionHeights(mesh);
    clampTileLineHeights(mesh);
    static const std::vector<uint32_t> kNoIndices;
    const bool gpuUploaded = uploadBucketGpu(
        mesh.origin, mesh.fillVerts, mesh.fillIndices, mesh.fillRanges,
        mesh.lineVerts, mesh.lineIndices, mesh.lineRanges, pointVerts,
        pointIndices, pointRanges, std::vector<float>(), kNoIndices,
        std::vector<PaintRange>(), std::vector<LabelEntry>(),
        mesh.fillVolumeGroups, mesh.lineVolumeGroups, mesh.extrudeVerts,
        mesh.extrudeIndices, mesh.extrudeRanges, gpu);
    const bool hasNonSymbolGeometry =
        !mesh.fillIndices.empty() || !mesh.lineIndices.empty() ||
        !mesh.fillVolumeGroups.empty() || !mesh.lineVolumeGroups.empty() ||
        !mesh.extrudeIndices.empty();
    if (!gpuUploaded && (mesh.symbols.empty() || hasNonSymbolGeometry)) {
        return TileMeshCommitResult::RetryableFailure;
    }
    const auto tStore = PClock::now();
    gpu.lineClampSource = std::move(mesh.lineClampSource);
    gpu.fillClampSource = std::move(mesh.fillClampSource);
    gpu.extrudeClampSource = std::move(mesh.extrudeClampSource);
    gpu.tileLabelSources = std::move(labelSources);
    gpu.tileSymbolSources = std::move(mesh.symbols);
    gpu.sourceTileZoom = key.z;
    gpu.symbolViewZoomBucket = -1;
    BucketGpu& stored = tileBuckets_[key];
    stored = std::move(gpu);
    // A tile can land after this layer's final command-build pass. Seed a
    // bounded initial symbol set immediately using the source zoom (the same
    // discrete Amap tier used to fetch the tile); the next camera frame still
    // rebuilds against the exact view zoom. This avoids both failure modes:
    // committing the entire unranked symbol population, and going idle with
    // symbolViewZoomBucket == -1 before labels ever reach the glyph baker.
    if (!stored.tileSymbolSources.empty()) {
        if (!rebuildTileBucketSymbolsForZoom(stored, key.z, true)) {
            symbolBucketsAwaitingRebuild_ = true;
        }
        // Commit can land after this layer's build but before Scene's end-of-
        // frame ledger audit. Mark the state first, then reconcile through
        // the same canonical predicate used by build/Scene. If no camera zoom
        // gate has been established yet this stays unticketed until build;
        // otherwise it closes the same-frame busy-without-ticket window.
        labelsAwaitingPlacement_ = true;
        syncLabelWorkTicket();
    }
    const auto tEnd = PClock::now();
    auto pms = [](PClock::time_point a, PClock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const double totalMs = pms(tSym, tEnd);
    if (totalMs >= 5.0) {
        platformLog(LogLevel::Info, "TileCommitSlow",
                    "z=%d x=%d y=%d %.1fms = sym %.2f + upload %.2f "
                    "+ store %.2f | verts f=%zu l=%zu p=%zu labels=%d "
                    "newGlyphs=%zu",
                    key.z, key.x, key.y, totalMs, pms(tSym, tUpload),
                    pms(tUpload, tStore), pms(tStore, tEnd),
                    stored.fillIndexCount ? mesh.fillVerts.size() : 0u,
                    mesh.lineVerts.size(), pointVerts.size(),
                    stored.labelIndexCount,
                    (glyphAtlas_ ? glyphAtlas_->residentGlyphCount() : 0) -
                        glyphsBefore);
    }
    return TileMeshCommitResult::Committed;
}

void FeatureRenderLayer::buildTileSymbolGpu(
    const std::vector<TileSymbolCpu>& symbols, const Vec3& origin, int tileZ,
    double viewZoom, float officialScale,
    std::vector<float>& pointVerts, std::vector<uint32_t>& pointIndices,
    std::vector<PaintRange>& pointRanges,
    std::vector<BucketGpu::TileLabelSource>& labelSrc) {
    pointVerts.reserve(symbols.size() * 4 * kPointVertexFloats);
    pointIndices.reserve(symbols.size() * 6);
    // 贴地:锚点落到地面(采样器覆盖全部锚点的 bbox)。采样不可用
    // (地形未注入/瓦片未驻留)退回原始几何高。⚠️ 高度是**采样当刻**的
    // 地形代次:冷启动时地形还粗,细化后山体升上来会把锚点埋掉(硬件深度
    // 与 T2 判定都读它)。故地形代次变化必须重钳 —— 见
    // reclampTileBucketSymbols,别再指望"瓦片换代重 commit 时自愈"
    // (瓦片换代只有缩放才触发,静止加载期不会发生)。
    using SymbolGroupKey = std::tuple<int, int, int>;
    std::map<SymbolGroupKey, std::vector<const TileSymbolCpu*>> groups;
    for (const TileSymbolCpu& s : symbols) {
        const int paintOrder = s.paintOrder;
        int minZoom = s.minZoom;
        int maxZoom = s.maxZoom;
        if (style_.requiresOfficial(
                FeatureRenderStyle::OfficialRequirement::PointIdentity) &&
            style_.pointStyleResolver && !s.pointStyleKeyA.empty() &&
            !s.pointStyleKeyB.empty()) {
            const auto resolved = style_.pointStyleResolver(
                s.pointStyleKeyA, s.pointStyleKeyB, s.name, viewZoom,
                officialScale);
            if (resolved.minZoom && resolved.maxZoom) {
                if (viewZoom < *resolved.minZoom ||
                    viewZoom >= *resolved.maxZoom) continue;
                minZoom = 0;
                maxZoom = 30;
            }
        }
        groups[{paintOrder, minZoom, maxZoom}].push_back(&s);
    }
    std::vector<std::vector<Cartographic>> anchorRing(1);
    anchorRing[0].reserve(symbols.size());
    for (const auto& entry : groups) {
        const auto& group = entry.second;
        for (const TileSymbolCpu* s : group) {
            anchorRing[0].emplace_back(s->lonRad, s->latRad, 0.0);
            for (const auto& point : s->labelPathCartographic) {
                anchorRing[0].emplace_back(point[0], point[1], 0.0);
            }
        }
    }
    const AreaSampleFn groundSample = makeClampSampler(anchorRing);
    // [V29 刀2] 本瓦 commit = 一次匹配 pass 的认领集(语义见 crossTileIdFor)。
    std::unordered_set<uint64_t> claimedIds;
    for (const auto& [groupKey, group] : groups) {
        const auto [paintOrder, minZoom, maxZoom] = groupKey;
        const uint32_t indexOffset = static_cast<uint32_t>(pointIndices.size());
        for (const TileSymbolCpu* sp : group) {
            const TileSymbolCpu& s = *sp;
            double h = s.heightM;
            if (groundSample) {
                const auto ground = groundSample(s.lonRad, s.latRad);
                if (ground) h = *ground;
            }
            const Vec3 anchor = ellipsoid_.cartographicToCartesian(
                Cartographic(s.lonRad, s.latRad, h + style_.heightOffset));
            const Vec3 rel = anchor - origin;
            Vec3 tangent = anchor;
            if (s.labelRepeatGroup != 0) {
                constexpr double kTangentMeters = 10.0;
                const double radius = ellipsoid_.maximumRadius();
                const double east = std::cos(s.labelAngleRad) * kTangentMeters;
                const double north = std::sin(s.labelAngleRad) * kTangentMeters;
                const double lat = s.latRad + north / radius;
                const double cosLat = std::max(1e-6, std::abs(std::cos(s.latRad)));
                const double lon = s.lonRad + east / (radius * cosLat);
                double tangentHeight = h;
                if (groundSample) {
                    if (const auto sampled = groundSample(lon, lat)) {
                        tangentHeight = *sampled;
                    }
                }
                tangent = ellipsoid_.cartographicToCartesian(
                    Cartographic(lon, lat,
                                 tangentHeight + style_.heightOffset));
            }
            const Vec3 tangentRelD = tangent - origin;
            const std::array<float, 3> relF{static_cast<float>(rel.x()),
                                            static_cast<float>(rel.y()),
                                            static_cast<float>(rel.z())};
            const std::array<float, 3> tangentRelF{
                static_cast<float>(tangentRelD.x()),
                static_cast<float>(tangentRelD.y()),
                static_cast<float>(tangentRelD.z())};
            bool iconEnabled = s.genericVisual && s.genericVisual->iconEnabled;
            std::string icon = s.genericVisual ? s.genericVisual->icon
                                               : std::string();
            float colorPacked = s.genericVisual
                ? s.genericVisual->colorPacked
                : 0.0f;
            float sizeScale = 1.0f;
            bool officialCanCovered = false;
            bool providerZoomOverride = false;
            bool providerArtworkReady = true;
            std::optional<FeatureRenderStyle::ProviderLabelLayout>
                providerLayout;
            if (style_.requiresOfficial(
                    FeatureRenderStyle::OfficialRequirement::PointIdentity) &&
                style_.pointStyleResolver && !s.pointStyleKeyA.empty() &&
                !s.pointStyleKeyB.empty()) {
                const auto resolved = style_.pointStyleResolver(
                    s.pointStyleKeyA, s.pointStyleKeyB, s.name, viewZoom,
                    officialScale);
                iconEnabled = resolved.enabled;
                icon = resolved.image;
                colorPacked = packColorFloat(resolved.color);
                officialCanCovered = resolved.officialCanCovered;
                providerZoomOverride =
                    resolved.minZoom.has_value() && resolved.maxZoom.has_value();
                providerLayout = resolved.labelLayout;
                if (officialIconAtlasDemand_) {
                    if (resolved.officialIconAtlas > 0)
                        officialIconAtlasDemand_(resolved.officialIconAtlas);
                    if (resolved.officialDynamicBackgroundAtlas > 0)
                        officialIconAtlasDemand_(
                            resolved.officialDynamicBackgroundAtlas);
                }
                // Provider sizes are absolute CSS pixels. The point command
                // uniform carries DPR only, so the official size is applied
                // exactly once and never depends on a generic layer scalar.
                sizeScale = resolved.sizePx;
                // Official point artwork and text form one provider-owned
                // symbol. Request missing atlases above, but publish neither
                // half until every exact referenced frame is present. This
                // prevents a transient text-only/synthetic representation
                // that does not exist in the official contract.
                if (iconEnabled) {
                    providerArtworkReady =
                        resolveOfficialAtlasSymbol(icon, iconAtlas_)
                            .has_value();
                }
                if (providerArtworkReady && providerLayout &&
                    !providerLayout->dynamicBackgroundImage.empty()) {
                    providerArtworkReady =
                        iconAtlas_ &&
                        iconAtlas_->frame(
                            providerLayout->dynamicBackgroundImage);
                }
            } else if (style_.requiresOfficial(
                           FeatureRenderStyle::OfficialRequirement::PointIdentity)) {
                // Provider-owned identity is atomic. A missing half is not
                // permission to revive the generic pointImage path.
                iconEnabled = false;
            }
            if (!providerArtworkReady) continue;
            if (iconEnabled) {
                if (style_.requiresOfficial(
                        FeatureRenderStyle::OfficialRequirement::PointIdentity)) {
                    const auto sym = resolveOfficialAtlasSymbol(
                        icon, iconAtlas_);
                    if (sym && providerLayout &&
                        providerLayout->iconWidthPx > 0.0f &&
                        providerLayout->iconHeightPx > 0.0f) {
                        appendSymbolQuad(relF, *sym, colorPacked, 1.0f,
                                         pointVerts, pointIndices,
                                         &*providerLayout);
                    }
                } else {
                    const ResolvedSymbol sym =
                        resolveSymbol(icon, style_.pointAnchor, iconAtlas_);
                    appendSymbolQuad(relF, sym, colorPacked, sizeScale,
                                     pointVerts, pointIndices);
                }
            }
            // 符号刀B/C:带 name 的实例记标签源(烘焙推迟到
            // bakeTileBucketLabels —— 字体可能晚于 commit 就绪)。id 经
            // crossTileIdFor 跨瓦继承 —— 瓦片换代(z13→z14 同一 POI,MVT
            // 逐瓦量化坐标略异)时 placement 的 fade/避让账本连续,不闪。
            if (!s.name.empty() &&
                (!style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LabelIdentity) ||
                 s.labelStyleGroup != 0)) {
                const int labelPaintOrder = s.labelPaintOrder;
                const int labelStyleGroup =
                    s.labelStyleGroup;
                const uint64_t id = crossTileIdFor(
                    s.name, s.lonRad, s.latRad, tileZ, &claimedIds);
                std::vector<std::array<double, 3>> labelPath =
                    s.labelPathCartographic;
                for (auto& p : labelPath) {
                    double pathHeight = h;
                    if (groundSample) {
                        if (const auto sampled = groundSample(p[0], p[1])) {
                            pathHeight = *sampled;
                        }
                    }
                    p[2] = pathHeight + style_.heightOffset;
                }
                const int labelMinZoom = providerZoomOverride ? 0 : s.minZoom;
                const int labelMaxZoom = providerZoomOverride ? 30 : s.maxZoom;
                labelSrc.push_back(BucketGpu::TileLabelSource{
                    labelPaintOrder, labelStyleGroup, s.rank,
                    s.officialInsertionOrder, labelMinZoom, labelMaxZoom,
                    s.genericVisual && !providerLayout
                        ? std::optional<BucketGpu::TileLabelSource::
                              GenericVisualPayload>{{
                              s.genericVisual->labelSizePx,
                              s.genericVisual->labelOffsetPx}}
                        : std::nullopt,
                    s.labelRepeatGroup,
                    s.labelRepeatDistancePx, s.labelAngleRad,
                    s.labelLetterSpacingEm, s.labelPaddingXPx,
                    s.labelPaddingYPx,
                    std::move(providerLayout),
                    relF, anchor,
                    tangentRelF,
                    tangent, std::move(labelPath), id, s.name,
                    s.labelSplitIndicesUtf16,
                    officialCanCovered});
            }
        }
        const uint32_t indexCount =
            static_cast<uint32_t>(pointIndices.size()) - indexOffset;
        if (indexCount > 0) {
            pointRanges.push_back(
                PaintRange{paintOrder, indexOffset, indexCount, minZoom,
                           maxZoom});
        }
    }
}

bool FeatureRenderLayer::rebuildTileBucketSymbolsForZoom(
    BucketGpu& gpu, int viewZoomBucket, bool force) {
    if (!renderDevice_) return false;

    std::vector<size_t> selected;
    selected.reserve(gpu.tileSymbolSources.size());
    using SymbolWindow = std::pair<int, int>;
    std::map<SymbolWindow, std::vector<size_t>> windows;
    for (size_t i = 0; i < gpu.tileSymbolSources.size(); ++i) {
        const TileSymbolCpu& symbol = gpu.tileSymbolSources[i];
        // A configured provider identity table is authoritative. Unknown
        // identities fail closed before top-N selection, glyph baking, and
        // collision so transparent legacy defaults cannot consume budget or
        // suppress official labels.
        bool visible = viewZoomBucket >= symbol.minZoom &&
                       viewZoomBucket < symbol.maxZoom;
        if (style_.requiresOfficial(
                FeatureRenderStyle::OfficialRequirement::PointIdentity) &&
            style_.pointStyleResolver && !symbol.pointStyleKeyA.empty() &&
            !symbol.pointStyleKeyB.empty()) {
            const auto resolved = style_.pointStyleResolver(
                symbol.pointStyleKeyA, symbol.pointStyleKeyB, symbol.name,
                currentLabelViewZoom_, lastStylePixelRatio_);
            if (resolved.minZoom && resolved.maxZoom) {
                visible = currentLabelViewZoom_ >= *resolved.minZoom &&
                          currentLabelViewZoom_ < *resolved.maxZoom;
            }
        }
        if (visible) {
            windows[{symbol.minZoom, symbol.maxZoom}].push_back(i);
        }
    }
    // Provider-owned taxonomies already carry official rank/window/collision
    // contracts. Applying the engine's generic 16/32/64/128 heuristic here
    // would silently delete valid AMap identities before official placement.
    // Generic layers retain the defensive engine budget.
    const size_t maxSymbols =
        style_.usesOfficialProviderContract()
        ? gpu.tileSymbolSources.size()
        : maxSymbolsForViewZoom(viewZoomBucket);
    for (auto& [window, candidates] : windows) {
        (void)window;
        if (candidates.size() > maxSymbols) {
            auto lessImportantLast = [&](size_t a, size_t b) {
                const int rankA = gpu.tileSymbolSources[a].rank;
                const int rankB = gpu.tileSymbolSources[b].rank;
                return rankA != rankB ? rankA < rankB : a < b;
            };
            std::nth_element(candidates.begin(),
                             candidates.begin() + maxSymbols,
                             candidates.end(), lessImportantLast);
            candidates.resize(maxSymbols);
        }
        selected.insert(selected.end(), candidates.begin(), candidates.end());
    }
    // Overlapping min/max windows are common in the Amap POI stream. The
    // budget is per tile for the current view, not per metadata window;
    // otherwise four active windows could silently turn a 16-symbol broad
    // budget back into 64.
    if (selected.size() > maxSymbols) {
        auto lessImportantLast = [&](size_t a, size_t b) {
            const int rankA = gpu.tileSymbolSources[a].rank;
            const int rankB = gpu.tileSymbolSources[b].rank;
            return rankA != rankB ? rankA < rankB : a < b;
        };
        std::nth_element(selected.begin(), selected.begin() + maxSymbols,
                         selected.end(), lessImportantLast);
        selected.resize(maxSymbols);
    }
    std::sort(selected.begin(), selected.end());

    uint64_t signature = 1469598103934665603ull;
    for (size_t index : selected) {
        signature ^= static_cast<uint64_t>(index + 1);
        signature *= 1099511628211ull;
    }
    signature ^= static_cast<uint64_t>(selected.size());
    if (style_.usesOfficialProviderContract()) {
        signature *= 1099511628211ull;
        // Provider point appearance is zoom-dependent even when the selected
        // feature indices are unchanged (for example q8t dynamic
        // backgrounds). Generic layers keep their existing active-window
        // reuse contract; only the official resolver requires this key.
        signature ^= static_cast<uint64_t>(
            static_cast<uint32_t>(viewZoomBucket));
    }
    if (!force && signature == gpu.symbolSelectionSignature) {
        gpu.symbolViewZoomBucket = viewZoomBucket;
        return true;
    }

    std::vector<TileSymbolCpu> active;
    active.reserve(selected.size());
    for (size_t index : selected) {
        active.push_back(gpu.tileSymbolSources[index]);
    }
    std::vector<float> pointVerts;
    std::vector<uint32_t> pointIndices;
    std::vector<PaintRange> pointRanges;
    std::vector<BucketGpu::TileLabelSource> labelSrc;
    buildTileSymbolGpu(active, gpu.origin, gpu.sourceTileZoom,
                       static_cast<double>(viewZoomBucket),
                       lastStylePixelRatio_, pointVerts,
                       pointIndices, pointRanges, labelSrc);
    if (force && labelSrc.size() == gpu.tileLabelSources.size()) {
        for (size_t i = 0; i < labelSrc.size(); ++i) {
            if (labelSrc[i].name == gpu.tileLabelSources[i].name) {
                labelSrc[i].featureId = gpu.tileLabelSources[i].featureId;
            }
        }
    }

    std::unique_ptr<Buffer> vb;
    std::unique_ptr<Buffer> ib;
    if (!pointIndices.empty()) {
        vb = makeBuffer(renderDevice_, pointVerts.data(),
                        pointVerts.size() * sizeof(float),
                        BufferDesc::Type::Vertex);
        ib = makeBuffer(renderDevice_, pointIndices.data(),
                        pointIndices.size() * sizeof(uint32_t),
                        BufferDesc::Type::Index);
        if (!vb || !ib) return false;
    }

    const bool labelSetChanged =
        signature != gpu.symbolSelectionSignature || force;
    gpu.pointVertexBuffer = std::move(vb);
    gpu.pointIndexBuffer = std::move(ib);
    gpu.pointIndexCount = static_cast<int>(pointIndices.size());
    gpu.pointRanges.clear();
    for (const PaintRange& range : pointRanges) {
        gpu.pointRanges.push_back(BucketGpu::PaintRangeGpu{
            range.paintOrder, static_cast<int>(range.indexOffset),
            static_cast<int>(range.indexCount), range.minZoom,
            range.maxZoom});
    }
    gpu.tileLabelSources = std::move(labelSrc);
    if (labelSetChanged) {
        invalidateTileBucketLabels(gpu);
        labelsAwaitingPlacement_ = true;
    }
    gpu.symbolSelectionSignature = signature;
    gpu.symbolViewZoomBucket = viewZoomBucket;
    return true;
}

void FeatureRenderLayer::reclampTileBucketSymbols(BucketGpu& gpu) {
    if (gpu.tileSymbolSources.empty() || !renderDevice_ ||
        gpu.symbolViewZoomBucket < 0) {
        return;
    }
    // 只重建当前 active top-N；完整 source 保留，zoom 换档再按新窗口物化。
    // force=true 即使选择集合不变也会重采地形高度并失效标签锚点。
    (void)rebuildTileBucketSymbolsForZoom(
        gpu, gpu.symbolViewZoomBucket, true);
}

void FeatureRenderLayer::invalidateTileBucketLabels(BucketGpu& gpu) {
    gpu.labelIndexCount = 0;
    gpu.labelBakeSettled = false;
    gpu.labelVertexBuffer.reset();
    gpu.labelIndexBuffer.reset();
    gpu.labelRanges.clear();
    gpu.labelBackgroundIndexBuffer.reset();
    gpu.labelBackgroundIndexCount = 0;
    gpu.labelBackgroundRanges.clear();
    gpu.labelVertsCpu.clear();
    gpu.labelEntries.clear();
    gpu.hasCameraDependentLabelBake = false;
    gpu.labelRequiredGlyphs.clear();
    gpu.labelRequiredGlyphsReady = false;
    gpu.labelUploadFailures = 0;
}

// ============================================================
// E 方案 P2:瓦片线 CPU 同源采样贴地(commit 钳高 + 地形代次重钳)
// ============================================================

void FeatureRenderLayer::clampTileFillHeights(TileMeshCpu& mesh) {
    constexpr size_t kSourceFloats = 3;
    if (mesh.fillClampSource.empty() || !mesh.hasOrigin ||
        mesh.fillClampSource.size() % kSourceFloats != 0) return;
    std::vector<std::vector<Cartographic>> rings(1);
    const size_t count = mesh.fillClampSource.size() / kSourceFloats;
    rings[0].reserve(count);
    for (size_t i = 0; i < count; ++i) {
        rings[0].emplace_back(mesh.fillClampSource[kSourceFloats * i],
                              mesh.fillClampSource[kSourceFloats * i + 1], 0.0);
    }
    const AreaSampleFn sample = makeClampSampler(rings);
    if (!sample) return;
    std::vector<float> clamped;
    clamped.reserve(count * 4);
    for (size_t i = 0; i < count; ++i) {
        const size_t base = i * kSourceFloats;
        const double lon = mesh.fillClampSource[base];
        const double lat = mesh.fillClampSource[base + 1];
        const double height = sample(lon, lat).value_or(0.0);
        const Vec3 relative = ellipsoid_.cartographicToCartesian(
                                  Cartographic(lon, lat, height)) -
                              mesh.origin;
        clamped.push_back(static_cast<float>(relative.x()));
        clamped.push_back(static_cast<float>(relative.y()));
        clamped.push_back(static_cast<float>(relative.z()));
        clamped.push_back(mesh.fillClampSource[base + 2]);
    }
    mesh.fillVerts = std::move(clamped);
}

void FeatureRenderLayer::reclampTileBucketFills(BucketGpu& gpu) {
    constexpr size_t kSourceFloats = 3;
    if (gpu.fillClampSource.empty() || !renderDevice_ ||
        gpu.fillIndexCount <= 0 ||
        gpu.fillClampSource.size() % kSourceFloats != 0) return;
    std::vector<std::vector<Cartographic>> rings(1);
    const size_t count = gpu.fillClampSource.size() / kSourceFloats;
    rings[0].reserve(count);
    for (size_t i = 0; i < count; ++i) {
        rings[0].emplace_back(gpu.fillClampSource[kSourceFloats * i],
                              gpu.fillClampSource[kSourceFloats * i + 1], 0.0);
    }
    const AreaSampleFn sample = makeClampSampler(rings);
    if (!sample) return;
    std::vector<float> vertices;
    vertices.reserve(count * 4);
    for (size_t i = 0; i < count; ++i) {
        const size_t base = i * kSourceFloats;
        const double lon = gpu.fillClampSource[base];
        const double lat = gpu.fillClampSource[base + 1];
        const double height = sample(lon, lat).value_or(0.0);
        const Vec3 relative = ellipsoid_.cartographicToCartesian(
                                  Cartographic(lon, lat, height)) -
                              gpu.origin;
        vertices.push_back(static_cast<float>(relative.x()));
        vertices.push_back(static_cast<float>(relative.y()));
        vertices.push_back(static_cast<float>(relative.z()));
        vertices.push_back(gpu.fillClampSource[base + 2]);
    }
    auto buffer = makeBuffer(renderDevice_, vertices.data(),
                             vertices.size() * sizeof(float),
                             BufferDesc::Type::Vertex);
    if (buffer) gpu.fillVertexBuffer = std::move(buffer);
}

namespace {
std::vector<float> reclampedExtrusionVertices(
    const std::vector<float>& source, const Vec3& origin,
    const std::function<std::optional<float>(double, double)>& sample,
    const Ellipsoid& ellipsoid) {
    constexpr size_t kFloats = 7;
    std::vector<float> vertices;
    if (!sample || source.empty() || source.size() % kFloats != 0) {
        return vertices;
    }
    vertices.reserve(source.size());
    for (size_t i = 0; i < source.size(); i += kFloats) {
        const double lon = source[i];
        const double lat = source[i + 1];
        const double relativeHeight = source[i + 2];
        const double ground = sample(lon, lat).value_or(0.0);
        const Vec3 relative = ellipsoid.cartographicToCartesian(
                                  Cartographic(lon, lat,
                                               ground + relativeHeight)) -
                              origin;
        vertices.push_back(static_cast<float>(relative.x()));
        vertices.push_back(static_cast<float>(relative.y()));
        vertices.push_back(static_cast<float>(relative.z()));
        vertices.push_back(source[i + 3]);
        vertices.push_back(source[i + 4]);
        vertices.push_back(source[i + 5]);
        vertices.push_back(source[i + 6]);
    }
    return vertices;
}
} // namespace

void FeatureRenderLayer::clampTileExtrusionHeights(TileMeshCpu& mesh) {
    constexpr size_t kFloats = 7;
    if (mesh.extrudeClampSource.empty() || !mesh.hasOrigin ||
        mesh.extrudeClampSource.size() % kFloats != 0) return;
    std::vector<std::vector<Cartographic>> points(1);
    for (size_t i = 0; i < mesh.extrudeClampSource.size(); i += kFloats) {
        points[0].emplace_back(mesh.extrudeClampSource[i],
                               mesh.extrudeClampSource[i + 1], 0.0);
    }
    const AreaSampleFn sample = makeClampSampler(points);
    auto vertices = reclampedExtrusionVertices(
        mesh.extrudeClampSource, mesh.origin, sample, ellipsoid_);
    if (!vertices.empty()) mesh.extrudeVerts = std::move(vertices);
}

void FeatureRenderLayer::reclampTileBucketExtrusions(BucketGpu& gpu) {
    constexpr size_t kFloats = 7;
    if (gpu.extrudeClampSource.empty() || !renderDevice_ ||
        gpu.extrudeIndexCount <= 0 ||
        gpu.extrudeClampSource.size() % kFloats != 0) return;
    std::vector<std::vector<Cartographic>> points(1);
    for (size_t i = 0; i < gpu.extrudeClampSource.size(); i += kFloats) {
        points[0].emplace_back(gpu.extrudeClampSource[i],
                               gpu.extrudeClampSource[i + 1], 0.0);
    }
    const AreaSampleFn sample = makeClampSampler(points);
    const auto vertices = reclampedExtrusionVertices(
        gpu.extrudeClampSource, gpu.origin, sample, ellipsoid_);
    if (vertices.empty()) return;
    auto buffer = makeBuffer(renderDevice_, vertices.data(),
                             vertices.size() * sizeof(float),
                             BufferDesc::Type::Vertex);
    if (buffer) gpu.extrudeVertexBuffer = std::move(buffer);
}

bool FeatureRenderLayer::reclampLineVertsFromSource(
    const std::vector<float>& clampSource, std::vector<float>& outVerts,
    const Vec3& origin, const AreaSampleFn& sample) const {
    constexpr size_t kClampFloats = 9;
    if (!sample || clampSource.empty() ||
        clampSource.size() % kClampFloats != 0) return false;
    const size_t nverts = clampSource.size() / kClampFloats;
    outVerts.clear();
    outVerts.reserve(nverts * 12);
    for (size_t i = 0; i < nverts; ++i) {
        const size_t base = i * kClampFloats;
        const auto position = [&](size_t offset) {
            const double lon = clampSource[base + offset];
            const double lat = clampSource[base + offset + 1];
            const double height = sample(lon, lat).value_or(0.0);
            return ellipsoid_.cartographicToCartesian(
                       Cartographic(lon, lat, height)) - origin;
        };
        const Vec3 p = position(0);
        const Vec3 pr = position(2);
        const Vec3 nxr = position(4);
        outVerts.push_back(static_cast<float>(p.x()));
        outVerts.push_back(static_cast<float>(p.y()));
        outVerts.push_back(static_cast<float>(p.z()));
        outVerts.push_back(static_cast<float>(pr.x()));
        outVerts.push_back(static_cast<float>(pr.y()));
        outVerts.push_back(static_cast<float>(pr.z()));
        outVerts.push_back(static_cast<float>(nxr.x()));
        outVerts.push_back(static_cast<float>(nxr.y()));
        outVerts.push_back(static_cast<float>(nxr.z()));
        outVerts.push_back(clampSource[base + 6]);
        outVerts.push_back(clampSource[base + 7]);
        outVerts.push_back(clampSource[base + 8]);
    }
    return true;
}

void FeatureRenderLayer::clampTileLineHeights(TileMeshCpu& mesh) {
    if (mesh.lineClampSource.empty() || !mesh.hasOrigin) return;
    std::vector<std::vector<Cartographic>> rings(1);
    constexpr size_t kClampFloats = 9;
    const size_t nverts = mesh.lineClampSource.size() / kClampFloats;
    rings[0].reserve(nverts);
    for (size_t i = 0; i < nverts; ++i) {
        rings[0].emplace_back(mesh.lineClampSource[kClampFloats * i],
                              mesh.lineClampSource[kClampFloats * i + 1], 0.0);
    }
    const AreaSampleFn sample = makeClampSampler(rings);
    if (!sample) return;
    std::vector<float> clamped;
    if (reclampLineVertsFromSource(mesh.lineClampSource, clamped,
                                   mesh.origin, sample)) {
        mesh.lineVerts = std::move(clamped);
    }
}

// ============================================================
// V6 建筑挤出:贴地 footprint + amap_height → 墙带 + CDT 顶面
// ============================================================

void FeatureRenderLayer::appendExtrusionVolume(
    const TessellationContext& ctx, const Feature& feature,
    const std::array<float, 4>& roofColor,
    const std::array<float, 4>& wallColor, Vec3& origin, bool& hasOrigin,
    std::vector<float>& extrudeVerts,
    std::vector<uint32_t>& extrudeIndices,
    std::vector<float>* clampSource) {
    if (feature.type != GeometryType::Polygon || feature.rings.empty() ||
        feature.rings[0].size() < 3) {
        return;
    }
    double height = 0.0;
    const auto hit = feature.properties.find("amap_height");
    if (hit != feature.properties.end()) {
        try {
            height = std::stod(hit->second);
        } catch (...) {
            height = 0.0;
        }
    }
    if (!(height > 0.0) || !std::isfinite(height)) return;
    const auto& ring = feature.rings[0];
    const size_t n = ring.size();
    std::vector<Vec3> base(n), top(n), up(n);
    double cx = 0, cy = 0, cz = 0;
    for (size_t i = 0; i < n; ++i) {
        base[i] = ctx.ellipsoid.cartographicToCartesian(ring[i]);
        up[i] = base[i].normalized();
        top[i] = base[i] + up[i] * height;
        cx += base[i].x();
        cy += base[i].y();
        cz += base[i].z();
    }
    const Vec3 centroid(cx / static_cast<double>(n),
                        cy / static_cast<double>(n),
                        cz / static_cast<double>(n));
    if (!hasOrigin) {
        origin = base[0];
        hasOrigin = true;
    }
    const float roofColorPacked = packColorFloat(roofColor);
    const float wallColorPacked = packColorFloat(wallColor);
    const auto push = [&](const Vec3& p, const Vec3& nm, float colorPacked) {
        const Vec3 rel = p - origin;
        extrudeVerts.push_back(static_cast<float>(rel.x()));
        extrudeVerts.push_back(static_cast<float>(rel.y()));
        extrudeVerts.push_back(static_cast<float>(rel.z()));
        extrudeVerts.push_back(static_cast<float>(nm.x()));
        extrudeVerts.push_back(static_cast<float>(nm.y()));
        extrudeVerts.push_back(static_cast<float>(nm.z()));
        extrudeVerts.push_back(colorPacked);
        if (clampSource) {
            const Cartographic source =
                ctx.ellipsoid.cartesianToCartographic(p);
            clampSource->push_back(static_cast<float>(source.longitude()));
            clampSource->push_back(static_cast<float>(source.latitude()));
            clampSource->push_back(static_cast<float>(source.height()));
            clampSource->push_back(static_cast<float>(nm.x()));
            clampSource->push_back(static_cast<float>(nm.y()));
            clampSource->push_back(static_cast<float>(nm.z()));
            clampSource->push_back(colorPacked);
        }
    };
    const auto pushQuad = [&](const Vec3& a, const Vec3& b, const Vec3& c,
                              const Vec3& d, const Vec3& nm) {
        const uint32_t v0 =
            static_cast<uint32_t>(extrudeVerts.size() / 7);
        push(a, nm, wallColorPacked);
        push(b, nm, wallColorPacked);
        push(c, nm, wallColorPacked);
        push(d, nm, wallColorPacked);
        extrudeIndices.push_back(v0);
        extrudeIndices.push_back(v0 + 1);
        extrudeIndices.push_back(v0 + 2);
        extrudeIndices.push_back(v0);
        extrudeIndices.push_back(v0 + 2);
        extrudeIndices.push_back(v0 + 3);
    };
    // 墙带(双面渲染,绕向免调)。
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        const Vec3 edge = base[j] - base[i];
        Vec3 outw = edge.cross(up[i]).normalized();
        if (outw.dot(base[i] - centroid) < 0.0) {
            outw = Vec3(-outw.x(), -outw.y(), -outw.z());
        }
        pushQuad(base[i], base[j], top[j], top[i], outw);
    }
    // 顶面(CDT,抬 height)。
    Feature topFeature;
    topFeature.type = GeometryType::Polygon;
    topFeature.rings = {ring};
    const TessellatedFill topFill =
        PolygonTessellator::tessellate(topFeature, ctx.ellipsoid, height);
    const uint32_t topBase =
        static_cast<uint32_t>(extrudeVerts.size() / 7);
    for (const Vec3& p : topFill.positions) {
        push(p, p.normalized(), roofColorPacked);
    }
    for (uint32_t idx : topFill.fillIndices) {
        extrudeIndices.push_back(topBase + idx);
    }
}

void FeatureRenderLayer::reclampTileBucketLines(BucketGpu& gpu) {
    if (gpu.lineClampSource.empty() || !renderDevice_ ||
        gpu.lineIndexCount <= 0) {
        return;
    }
    std::vector<std::vector<Cartographic>> rings(1);
    constexpr size_t kClampFloats = 9;
    const size_t nverts = gpu.lineClampSource.size() / kClampFloats;
    rings[0].reserve(nverts);
    for (size_t i = 0; i < nverts; ++i) {
        rings[0].emplace_back(gpu.lineClampSource[kClampFloats * i],
                              gpu.lineClampSource[kClampFloats * i + 1], 0.0);
    }
    const AreaSampleFn sample = makeClampSampler(rings);
    if (!sample) return;
    std::vector<float> lineVerts;
    if (!reclampLineVertsFromSource(gpu.lineClampSource, lineVerts,
                                    gpu.origin, sample)) {
        return;
    }
    // 只换顶点缓冲:重钳只改高度,索引拓扑不变。
    auto vb = makeBuffer(renderDevice_, lineVerts.data(),
                         lineVerts.size() * sizeof(float),
                         BufferDesc::Type::Vertex);
    if (!vb) return;
    gpu.lineVertexBuffer = std::move(vb);
}

uint64_t FeatureRenderLayer::crossTileIdFor(
    const std::string& name, double lonRad, double latRad, int tileZ,
    std::unordered_set<uint64_t>* claimed) {
    uint64_t nameHash = 1469598103934665603ull;
    for (unsigned char c : name) {
        nameHash ^= c;
        nameHash *= 1099511628211ull;
    }
    // [V29 刀1] 匹配窗 = 1/256 瓦(maplibre crossTileSymbolIndex 等效:
    // roundingFactor=1/32·EXTENT → 4px 格,±1 格窗;z14 ≈ ±9.5m)。旧窗
    // 1.5×MVT 量化格(z14 ≈ ±0.9m)只够吸逐瓦量化误差,吸不住换代锚点
    // 漂移 —— 线标注锚点取瓦内几何弧长中点,瓦片切分一变中点米级挪,
    // 真机量得同名匹配失败率 ≈22%(立项文档 §1)。窗按较粗方 zoom 取
    // (粗格大),与 maplibre 粗代容差乘 2^Δz 同向。
    constexpr double kTwoPi = 6.283185307179586;
    auto gridRad = [&](int z) { return kTwoPi / (256.0 * std::exp2(z)); };
    std::vector<CrossTileEntry>& entries = crossTileIndex_[nameHash];
    for (CrossTileEntry& e : entries) {
        // [V29 刀2] 本 pass 已被认领的 entry 跳过(1:1 贪心,声明见 .h)。
        if (claimed && claimed->count(e.id)) continue;
        const double tol = gridRad(std::min(e.zoom, tileZ));
        if (std::abs(e.lonRad - lonRad) <= tol &&
            std::abs(e.latRad - latRad) <= tol) {
            if (tileZ > e.zoom) {
                // 细 zoom 坐标更准,升级锚点参考,后续匹配容差收紧。
                e.lonRad = lonRad;
                e.latRad = latRad;
                e.zoom = tileZ;
            }
            if (claimed) claimed->insert(e.id);
            return e.id;
        }
    }
    entries.push_back(CrossTileEntry{lonRad, latRad, tileZ,
                                     nextCrossTileId_++});
    // [V29 刀2] 新建的也认领:同 pass 后续符号不得匹配到刚建的 entry(两个
    // 真实实例各自新建,不误并)。
    if (claimed) claimed->insert(entries.back().id);
    ++crossTileEntryCount_;
    // 只增不淘汰的容量哨兵:城市级 POI 全量 ~1.4k,项字节级;跨大区域
    // 漫游把它顶过阈值时打一行,再谈 LRU(先观察,不预支复杂度)。
    if ((crossTileEntryCount_ & (crossTileEntryCount_ - 1)) == 0 &&
        crossTileEntryCount_ >= 16384) {
        platformLog(LogLevel::Warning, "TileSymbol",
                    "crossTileIndex entries=%zu(只增不淘汰,考虑上 LRU)",
                    crossTileEntryCount_);
    }
    return entries.back().id;
}

FeatureRenderLayer::TileLabelBakeResult
FeatureRenderLayer::bakeTileBucketLabels(BucketGpu& gpu, double viewZoom,
                                         float stylePixelRatio,
                                         const Mat4& viewProjection,
                                         double viewportWidth,
                                         double viewportHeight,
                                         bool forceCameraRebake) {
    if ((!forceCameraRebake &&
         (gpu.labelBakeSettled || gpu.labelIndexCount > 0)) ||
        gpu.tileLabelSources.empty()) {
        return TileLabelBakeResult::Settled;
    }
    if (!glyphAtlas_ || !glyphAtlas_->ready() || !renderDevice_) {
        return TileLabelBakeResult::Deferred;
    }
    const double gateZoom = visibilityZoom(style_, viewZoom);
    // P6:先按预算补齐本桶所需的新字形。补不齐 → 整桶推迟(本函数幂等,
    // 每帧的重试 drain 会再来)。**决不半烘**:半桶标签在屏上是缺字。
    {
        if (!gpu.labelRequiredGlyphsReady) {
            std::unordered_set<uint32_t> seen;
            seen.reserve(gpu.tileLabelSources.size() * 4);
            gpu.labelRequiredGlyphs.clear();
            for (const BucketGpu::TileLabelSource& src :
                 gpu.tileLabelSources) {
                const int effectiveMinZoom = FeatureRenderLayer::effectiveLabelMinZoom(
                    style_, src.styleGroup, src.minZoom);
                const int effectiveMaxZoom = FeatureRenderLayer::effectiveLabelMaxZoom(
                    style_, src.styleGroup, src.maxZoom);
                if (gateZoom < static_cast<double>(effectiveMinZoom) ||
                    gateZoom >= static_cast<double>(effectiveMaxZoom) ||
                    !FeatureRenderLayer::labelStyleVisibleAtZoom(
                        style_, src.styleGroup, gateZoom)) {
                    continue;
                }
                if (!(FeatureRenderLayer::resolvedLabelSizePx(
                          style_, src.styleGroup, viewZoom,
                          src.genericVisual
                              ? src.genericVisual->labelSizePx
                              : 0.0f) > 0.0f)) {
                    continue;
                }
                for (uint32_t cp : GlyphAtlas::decodeUtf8(src.name)) {
                    if (cp == '\n' || cp == '\r') continue;
                    if (seen.insert(cp).second) {
                        gpu.labelRequiredGlyphs.push_back(cp);
                    }
                }
            }
            gpu.labelRequiredGlyphsReady = true;
        }
        bool deferred = false;
        for (size_t i = 0; i < gpu.labelRequiredGlyphs.size();) {
            const uint32_t cp = gpu.labelRequiredGlyphs[i];
            const bool resident = glyphAtlas_->hasGlyph(cp);
            if (resident) {
                gpu.labelRequiredGlyphs[i] = gpu.labelRequiredGlyphs.back();
                gpu.labelRequiredGlyphs.pop_back();
                continue;
            }
            const auto result = glyphAtlas_->ensureGlyphBudgeted(cp);
            if (result == GlyphAtlas::BudgetedGlyphResult::Ready ||
                result == GlyphAtlas::BudgetedGlyphResult::MissingTerminal) {
                gpu.labelRequiredGlyphs[i] = gpu.labelRequiredGlyphs.back();
                gpu.labelRequiredGlyphs.pop_back();
                continue;
            }
            if (result == GlyphAtlas::BudgetedGlyphResult::Saturated) {
                // 全局队列/预算已满，本桶剩余项本帧不可能启动；立即停，
                // 并把状态传给外层，避免再调用其余 76 个桶。
                return TileLabelBakeResult::AtlasSaturated;
            }
            if (result == GlyphAtlas::BudgetedGlyphResult::Deferred) {
                // 继续扫本桶剩余 codepoint，让全局有界队列能并行启动不重复
                // 的字符；循环结束后整桶仍保持未发布（不画半字标签）。
                deferred = true;
            }
            ++i;
        }
        if (deferred || !gpu.labelRequiredGlyphs.empty()) {
            return TileLabelBakeResult::Deferred;
        }
    }
    const double geometryStartMs = perf::nowMs();
    std::map<std::pair<int, int>, LabelGeometryCpu> labelGroups;
    bool cameraDependentBake = false;
    for (const BucketGpu::TileLabelSource& src : gpu.tileLabelSources) {
        const int effectiveMinZoom = FeatureRenderLayer::effectiveLabelMinZoom(
            style_, src.styleGroup, src.minZoom);
        const int effectiveMaxZoom = FeatureRenderLayer::effectiveLabelMaxZoom(
            style_, src.styleGroup, src.maxZoom);
        if (gateZoom < static_cast<double>(effectiveMinZoom) ||
            gateZoom >= static_cast<double>(effectiveMaxZoom) ||
            !FeatureRenderLayer::labelStyleVisibleAtZoom(
                style_, src.styleGroup, gateZoom)) {
            continue;
        }
        LabelGeometryCpu& group =
            labelGroups[{src.paintOrder, src.styleGroup}];
        const float labelSizePx = FeatureRenderLayer::resolvedLabelSizePx(
            style_, src.styleGroup, viewZoom,
            src.genericVisual ? src.genericVisual->labelSizePx : 0.0f) *
            stylePixelRatio;
        if (!(labelSizePx > 0.0f)) continue;
        const double metersPerCssPixel =
            2.0 * M_PI * ellipsoid_.maximumRadius() *
            std::max(1e-6, std::abs(std::cos(
                ellipsoid_.cartesianToCartographic(src.anchorEcef).latitude()))) /
            (256.0 * std::pow(2.0, viewZoom));
        const double metersPerFramebufferPixel =
            metersPerCssPixel / std::max(0.1f, stylePixelRatio);
        std::optional<FeatureRenderStyle::ProviderLabelLayout> scaledLayout =
            src.providerLayout;
        if (scaledLayout) {
            scaledLayout->offsetXPx *= stylePixelRatio;
            scaledLayout->offsetYPx *= stylePixelRatio;
            scaledLayout->iconWidthPx *= stylePixelRatio;
            scaledLayout->iconHeightPx *= stylePixelRatio;
            scaledLayout->iconAnchorXPx *= stylePixelRatio;
            scaledLayout->iconAnchorYPx *= stylePixelRatio;
        }
        std::vector<Vec3> pathEcef;
        std::vector<ProjectedPathSampler> projectedPaths;
        if (!src.pathCartographic.empty()) {
            pathEcef.reserve(src.pathCartographic.size());
            for (const auto& point : src.pathCartographic) {
                pathEcef.push_back(ellipsoid_.cartographicToCartesian(
                    Cartographic(point[0], point[1], point[2])));
            }
            projectedPaths = ProjectedPathSampler::createCandidates(
                pathEcef, viewProjection, viewportWidth, viewportHeight,
                labelSizePx, viewZoom, stylePixelRatio);
            if (!src.genericVisual && projectedPaths.empty()) {
                // Official line labels have one screen-space path contract.
                // Projection failure is fail-closed; never revive the retired
                // world-metre tangent path for official data.
                continue;
            }
            cameraDependentBake = cameraDependentBake || !projectedPaths.empty();
        }
        const float labelHaloCssPx = resolvedLabelHaloWidthPx(
            style_, src.styleGroup, viewZoom);
        if (labelHaloCssPx < 0.0f) continue;
        const float labelHaloPx = labelHaloCssPx * stylePixelRatio;
        const size_t pathCandidateCount = projectedPaths.empty()
            ? 1 : projectedPaths.size();
        for (size_t fragment = 0; fragment < pathCandidateCount; ++fragment) {
        std::vector<OfficialPathGlyphGroup> officialGroups;
        if (!projectedPaths.empty() && !src.genericVisual) {
            std::vector<double> advances;
            for (uint32_t cp : GlyphAtlas::decodeUtf8(src.name)) {
                if (cp != '\r' && cp != '\n') {
                    // Fixed inner classic-normal road labels use Yp's
                    // non-global step: fontSize * 1.3. This is intentionally
                    // independent of our atlas metrics.
                    advances.push_back(static_cast<double>(labelSizePx) * 1.3);
                }
            }
            officialGroups = projectedPaths[fragment]
                .layoutOfficialGlyphGroups(advances, fragment == 0,
                                           stylePixelRatio);
            if (officialGroups.empty()) continue;
        }
        const size_t pathGroupCount = officialGroups.empty()
            ? 1 : officialGroups.size();
        for (size_t groupIndex = 0; groupIndex < pathGroupCount; ++groupIndex) {
        const size_t fragmentEntryStart = group.entries.size();
        const uint32_t officialOrder = static_cast<uint32_t>(
            fragment * 60 + (officialGroups.empty()
                ? 0 : officialGroups[groupIndex].groupOrder));
        const FeatureId fragmentId = projectedPaths.empty()
            ? src.featureId
            : officialPathFragmentId(src.featureId, officialOrder);
        appendLabelTextQuads(*glyphAtlas_, style_, fragmentId,
                             src.anchorEcef, src.tangentEcef, src.rel,
                             src.tangentRel, src.name,
                             &src.labelSplitIndicesUtf16, group.verts,
                             group.indices, &group.backgroundIndices,
                             group.entries, src.rank,
                             effectiveMinZoom, effectiveMaxZoom, labelSizePx,
                             (src.genericVisual
                                  ? src.genericVisual->labelOffsetPx
                                  : 0.0f) *
                                 stylePixelRatio,
                             labelHaloPx,
                             scaledLayout ? &*scaledLayout : nullptr,
                             stylePixelRatio,
                             src.repeatGroup,
                             src.repeatDistancePx * stylePixelRatio,
                             src.angleRad, src.letterSpacingEm,
                             src.paddingXPx * stylePixelRatio,
                             src.paddingYPx * stylePixelRatio,
                             src.pathCartographic.empty()
                                 ? nullptr
                                 : &src.pathCartographic,
                             &ellipsoid_, &gpu.origin,
                             metersPerFramebufferPixel, iconAtlas_,
                             projectedPaths.empty() ? nullptr
                                                    : &projectedPaths[fragment],
                             officialGroups.empty() ? nullptr
                                 : &officialGroups[groupIndex].glyphSamples);
        if (group.entries.size() > fragmentEntryStart) {
            group.entries.back().officialInsertionOrder =
                src.officialInsertionOrder;
            group.entries.back().officialFragmentOrder =
                officialOrder;
            group.entries.back().officialCanCovered =
                src.officialCanCovered;
        }
        }
        }
    }
    std::vector<float> labelVerts;
    std::vector<uint32_t> labelIndices;
    std::vector<uint32_t> backgroundIndices;
    std::vector<LabelEntry> labelEntries;
    std::vector<PaintRange> labelRanges;
    std::vector<PaintRange> backgroundRanges;
    flattenLabelRanges(labelGroups, labelVerts, labelIndices, labelEntries,
                       &labelRanges, &backgroundIndices, &backgroundRanges);
    const double geometryMs = perf::nowMs() - geometryStartMs;
    if (labelIndices.empty()) {
        // 无可显示字形 = 稳态(非在途),谓词不再计入(见 labelBakeSettled)。
        gpu.labelBakeSettled = true;
        return TileLabelBakeResult::Settled;
    }
    // [V24 文字硬闪根修] 新桶顶点 opacity 初始 0,而回写只在 300ms 节流的
    // placement 或"有 fade 在推进"时发生 —— 稳态手势期换桶(瓦片 LOD 微跨
    // 级的原子换手)落在两个时机之间,同名标签(crossTileID 连续、fade 早已
    // 收敛在 1)黑到下一个节流窗才被一次性写亮 = 硬闪、只打文字(图标无
    // opacity)。建 buffer 前直接烘入 placement 当前 opacity:老 id 同帧
    // 恢复原亮度,新 id 仍从 0 按既有 fade-in 走,且省一次回写上传。
    for (LabelEntry& e : labelEntries) {
        const float op = labelPlacement_.opacity(e.featureId);
        if (op != 0.0f) {
            for (size_t i = e.vertexFloatStart + 8;
                 i < e.vertexFloatStart + e.vertexFloatCount; i += 11) {
                labelVerts[i] = op;
            }
        }
        e.appliedOpacity = op;
    }
    const double uploadStartMs = perf::nowMs();
    auto vb = makeBuffer(renderDevice_, labelVerts.data(),
                         labelVerts.size() * sizeof(float),
                         BufferDesc::Type::Vertex);
    auto ib = makeBuffer(renderDevice_, labelIndices.data(),
                         labelIndices.size() * sizeof(uint32_t),
                         BufferDesc::Type::Index);
    auto backgroundIb = backgroundIndices.empty()
        ? nullptr
        : makeBuffer(renderDevice_, backgroundIndices.data(),
                     backgroundIndices.size() * sizeof(uint32_t),
                     BufferDesc::Type::Index);
    if (!vb || !ib || (!backgroundIndices.empty() && !backgroundIb)) {
        // 瞬时 GPU/驱动失败不能在稳定视野永久丢标。前三次跨帧重试；若
        // 持续失败则视为资源枯竭，停止白烧并保留 source，之后 zoom/font/
        // reclamp 失效会重新给它机会。
        ++gpu.labelUploadFailures;
        platformLog(LogLevel::Warning, "FeatureRenderLayer",
                    "label buffer create failed attempt=%d sources=%zu",
                    gpu.labelUploadFailures, gpu.tileLabelSources.size());
        if (gpu.labelUploadFailures < 3) {
            return TileLabelBakeResult::RetryableFailure;
        }
        gpu.labelBakeSettled = true;
        return TileLabelBakeResult::Settled;
    }
    gpu.labelUploadFailures = 0;
    gpu.labelVertexBuffer = std::move(vb);
    gpu.labelIndexBuffer = std::move(ib);
    gpu.labelIndexCount = static_cast<int>(labelIndices.size());
    gpu.labelBackgroundIndexBuffer = std::move(backgroundIb);
    gpu.labelBackgroundIndexCount =
        static_cast<int>(backgroundIndices.size());
    gpu.labelRanges.clear();
    for (const PaintRange& range : labelRanges) {
        gpu.labelRanges.push_back(BucketGpu::PaintRangeGpu{
            range.paintOrder, static_cast<int>(range.indexOffset),
            static_cast<int>(range.indexCount), range.minZoom,
            range.maxZoom, range.styleGroup});
    }
    gpu.labelBackgroundRanges.clear();
    for (const PaintRange& range : backgroundRanges) {
        gpu.labelBackgroundRanges.push_back(BucketGpu::PaintRangeGpu{
            range.paintOrder, static_cast<int>(range.indexOffset),
            static_cast<int>(range.indexCount), range.minZoom,
            range.maxZoom, range.styleGroup});
    }
    gpu.labelVertsCpu = std::move(labelVerts);
    gpu.labelEntries = std::move(labelEntries);
    gpu.labelBakeViewProjection = viewProjection;
    gpu.labelBakeViewportWidth = viewportWidth;
    gpu.labelBakeViewportHeight = viewportHeight;
    gpu.hasCameraDependentLabelBake = cameraDependentBake;
    gpu.labelBakeSettled = true;
    const double uploadMs = perf::nowMs() - uploadStartMs;
    if (geometryMs > 1.0 || uploadMs > 1.0) {
        platformLog(LogLevel::Info, "TileSymbolPerf",
                    "layer=%s labelBake geometry=%.2fms upload=%.2fms "
                    "sources=%zu entries=%zu indices=%d",
                    layerId_.c_str(), geometryMs, uploadMs,
                    gpu.tileLabelSources.size(), gpu.labelEntries.size(),
                    gpu.labelIndexCount);
    }
    // V27:本桶烘出一批新标注 —— 其中 fades_ 查不到的新 id 上面烘的是
    // opacity=0,必须下一帧全量 placement 置 target(绕过 300ms 节流)。
    // 冷启动首批 bake 时 fades_ 整个是空的,不置位 = 整屏标注隐形到
    // 用户缩放为止(V27 报告的主复现态)。
    labelsAwaitingPlacement_ = true;
    return TileLabelBakeResult::Settled;
}

void FeatureRenderLayer::dropTileMesh(const TileKey& key) {
    const auto it = tileBuckets_.find(key);
    if (it == tileBuckets_.end()) return;
    // V27:drop 带标注的桶 = 碰撞格局变化(换代中间态里老 entry 靠 id 小
    // tie-break 压住同名新 entry;老的退场后新 entry 的 target 还停在
    // collided 的 0,不重跑 placement 它永远隐形)。与 commit/重镶同为
    // 换代事件,一并置位。
    if (!it->second.labelEntries.empty() ||
        !it->second.tileLabelSources.empty()) {
        labelsAwaitingPlacement_ = true;
    }
    tileBuckets_.erase(it);
}

#if defined(EARTH_ENGINE_TESTING)
std::optional<FeatureRenderLayer::LabelCollisionBoundsForTest>
FeatureRenderLayer::firstTileLabelCollisionBoundsForTest() const {
    for (const auto& [key, bucket] : tileBuckets_) {
        (void)key;
        if (bucket.labelEntries.empty()) continue;
        const LabelEntry& entry = bucket.labelEntries.front();
        LabelCollisionBoundsForTest out;
        out.text = {entry.boxMinXPx, entry.boxMinYPx,
                    entry.boxMaxXPx, entry.boxMaxYPx};
        out.hasSecondary = entry.hasIconBox;
        out.secondary = {entry.iconBoxMinXPx, entry.iconBoxMinYPx,
                         entry.iconBoxMaxXPx, entry.iconBoxMaxYPx};
        return out;
    }
    return std::nullopt;
}

std::optional<uint64_t>
FeatureRenderLayer::officialTileLabelInsertionOrderForTest(
    const TileKey& key, const std::string& name) const {
    const auto bucket = tileBuckets_.find(key);
    if (bucket == tileBuckets_.end()) return std::nullopt;
    for (const BucketGpu::TileLabelSource& source :
         bucket->second.tileLabelSources) {
        if (source.name == name) return source.officialInsertionOrder;
    }
    return std::nullopt;
}

FeatureRenderLayer::TerrainReclampSnapshotForTest
FeatureRenderLayer::terrainReclampSnapshotForTest() const {
    TerrainReclampSnapshotForTest out;
    out.appliedRevision = lastClampRevision_;
    out.pendingBuckets = pendingReclamp_.size();
    for (const auto& [key, bucket] : tileBuckets_) {
        (void)key;
        if (!out.origin) out.origin = bucket.origin;
        if (!out.fillVertexBuffer) {
            out.fillVertexBuffer = bucket.fillVertexBuffer.get();
        }
        if (!out.lineVertexBuffer) {
            out.lineVertexBuffer = bucket.lineVertexBuffer.get();
        }
        if (!out.pointVertexBuffer) {
            out.pointVertexBuffer = bucket.pointVertexBuffer.get();
        }
        if (!out.labelVertexBuffer) {
            out.labelVertexBuffer = bucket.labelVertexBuffer.get();
        }
        if (!out.extrusionVertexBuffer) {
            out.extrusionVertexBuffer = bucket.extrudeVertexBuffer.get();
        }
        if (!out.firstLabelAnchorHeightMeters &&
            !bucket.tileLabelSources.empty()) {
            out.firstLabelAnchorHeightMeters =
                ellipsoid_.cartesianToCartographic(
                    bucket.tileLabelSources.front().anchorEcef).height();
        }
    }
    return out;
}
#endif

void FeatureRenderLayer::buildRenderCommands(const FrameState& frameState,
                                             Renderer& renderer,
                                             RenderCommandList& commands) {
    const double buildStartMs = perf::nowMs();
    const size_t commandsBefore = commands.size();
    double atlasDrainMs = 0.0;
    double labelScanMs = 0.0;
    double reclampMs = 0.0;
    double terrainRevisionMs = 0.0;
    double dirtySyncMs = 0.0;
    double visibleMs = 0.0;
    double placementMs = 0.0;
    double appendMs = 0.0;
    size_t labelBucketsScanned = 0;
    size_t labelBucketsCompleted = 0;
    size_t reclampBuckets = 0;
    // 票据真值必须先纳入“本层在当前视野实际可推进”这个前提。不可见层
    // 由 setVisible(false) 同步释放；无 device/camera 时也不能继续扣住帧循环。
    if (!visible_ || !renderDevice_ || !frameState.camera) {
        labelWorkActiveForCurrentView_ = false;
        syncLabelWorkTicket();
        return;
    }

    const Camera& commandCamera = *frameState.camera;
    CommandFrameParams commandFrame;
    commandFrame.viewportWidth =
        static_cast<double>(frameState.viewportWidthPixels);
    commandFrame.viewportHeight =
        static_cast<double>(frameState.viewportHeightPixels);
    commandFrame.viewProjection = commandCamera.viewProjectionMatrix(
        commandFrame.viewportWidth, commandFrame.viewportHeight);
    commandFrame.view = commandCamera.viewMatrix();
    commandFrame.cameraHeight = ellipsoid_.cartesianToCartographic(
                                    commandCamera.position())
                                    .height();
    commandFrame.zoomLevel = std::min(
        24.0, std::max(0.0, std::log2(
            4.0e7 / std::max(1.0, commandFrame.cameraHeight))));
    const bool amapOfficialContract =
        style_.usesOfficialProviderContract();
    commandFrame.lineWidthPx = amapOfficialContract ? 0.0f : style_.lineWidthPx;
    commandFrame.pointSizePx = amapOfficialContract ? 0.0f : style_.pointSizePx;
    auto evalFrameNumber = [&](const StyleExpression::Ptr& expr,
                               float fallback) -> float {
        if (!expr) return fallback;
        const auto value = expr->evaluate(nullptr, commandFrame.zoomLevel);
        if (!value || value->kind() != StyleValue::Kind::Number) {
            return fallback;
        }
        return static_cast<float>(value->number());
    };
    if (!amapOfficialContract) {
        commandFrame.lineWidthPx = evalFrameNumber(
            style_.lineWidthExpr, commandFrame.lineWidthPx);
        commandFrame.pointSizePx = evalFrameNumber(
            style_.pointSizeExpr, commandFrame.pointSizePx);
    }
    commandFrame.stylePixelRatio = amapOfficialContract
        ? (frameState.devicePixelRatio > 1.0f ? 2.0f : 1.0f)
        : (style_.scaleStylePixelsByDevicePixelRatio
               ? std::max(0.1f, frameState.devicePixelRatio)
               : 1.0f);
    commandFrame.pointSizePx = amapOfficialContract
        ? commandFrame.stylePixelRatio
        : commandFrame.pointSizePx * commandFrame.stylePixelRatio;
    constexpr float kSymbolDepthPushNdc = 0.9999f;
    commandFrame.symbolDepthPush =
        (!amapOfficialContract &&
         presentationPolicy_.symbolDepthPushCameraHeightMeters > 0.0f &&
         commandFrame.cameraHeight >
             presentationPolicy_.symbolDepthPushCameraHeightMeters)
            ? kSymbolDepthPushNdc
            : 0.0f;
    commandFrame.halfWidthPerEyeZ = static_cast<float>(
        static_cast<double>(commandFrame.lineWidthPx) *
        std::tan(commandCamera.verticalFovRadians() * 0.5) /
        std::max(1.0, commandFrame.viewportHeight));

    // LOD 粗源 zoom 门控:近景由主源细面承接时,粗源整层不发命令。
    // zoom 口径与 widthExpr 一致(log2(赤道周长/视高))。
    labelWorkActiveForCurrentView_ =
        commandFrame.zoomLevel >= style_.minZoom &&
        commandFrame.zoomLevel <= style_.maxZoom;
    syncLabelWorkTicket();
    if (!labelWorkActiveForCurrentView_) {
        return;
    }

    // 文字标注(P5b):缓存图集指针(重镶/预览路径无 Renderer 引用);字体
    // 就绪状态翻转 → 全部桶重镶补标注(字体注入通常晚于要素导入)。
    // 图标图集(P6c)同构:图标注入通常也晚于要素导入,代次变化 → 重镶
    // 补 uv(顶点里烘的是 frame 的 uv 与宽高比,新图标不重镶就画不出)。
    iconAtlas_ = renderer.iconAtlas();
    const uint64_t iconRevision = iconAtlas_ ? iconAtlas_->revision() : 0;
    glyphAtlas_ = renderer.glyphAtlas();
    const double currentLabelViewZoom =
        labelViewZoom(ellipsoid_, *frameState.camera);
    currentLabelViewZoom_ = currentLabelViewZoom;
    const int currentLabelZoomBucket = static_cast<int>(
        visibilityZoom(style_, currentLabelViewZoom));
    const bool atlasReady = glyphAtlas_ && glyphAtlas_->ready();
    const uint64_t glyphRevision = glyphAtlas_ ? glyphAtlas_->revision() : 0;
    const bool glyphAtlasChanged =
        atlasReady != lastAtlasReady_ || glyphRevision != lastGlyphRevision_;
    const bool iconAtlasChanged = iconRevision != lastIconRevision_;
    const bool stylePixelRatioChanged =
        std::abs(commandFrame.stylePixelRatio - lastStylePixelRatio_) > 1e-4f;
    const bool labelZoomChanged =
        currentLabelZoomBucket != lastLabelBakeZoomBucket_;
    if (atlasReady) {
        const double startMs = perf::nowMs();
        glyphAtlas_->beginFrameGlyphBudget(frameState.frameId,
                                           kGlyphRasterBudgetMs);
        atlasDrainMs = perf::nowMs() - startMs;
    }
    if (glyphAtlasChanged || iconAtlasChanged || stylePixelRatioChanged) {
        lastAtlasReady_ = atlasReady;
        lastGlyphRevision_ = glyphRevision;
        lastIconRevision_ = iconRevision;
        lastStylePixelRatio_ = commandFrame.stylePixelRatio;
        std::vector<BucketKey> keys;
        keys.reserve(buckets_.size());
        for (const auto& entry : buckets_) keys.push_back(entry.first);
        for (BucketKey key : keys) rebuildBucket(key);
        // 瓦片桶的官方位图符号在下方按 icon revision 重建；文字 UV 则由
        // 保留的标签源重新烘焙。两者各自失效，不保留旧图集或合成图形路径。
        if ((glyphAtlasChanged || stylePixelRatioChanged) && atlasReady) {
            for (auto& entry : tileBuckets_) {
                // 字体翻转或图集换代:旧 UV 已失效，必须同时清掉 indexCount，
                // 否则 bakeTileBucketLabels 会被旧 buffer 的早退条件挡住。
                invalidateTileBucketLabels(entry.second);
            }
        }
        previewDirty_ = true;
    }
    if (labelZoomChanged) {
        lastLabelBakeZoomBucket_ = currentLabelZoomBucket;
        std::vector<BucketKey> keys;
        keys.reserve(buckets_.size());
        for (const auto& entry : buckets_) keys.push_back(entry.first);
        for (BucketKey key : keys) rebuildBucket(key);
        previewDirty_ = true;
    }
    // commit 保留全部 zoom 档 source；这里按当前整数 zoom 选 active top-N
    // 并物化点/标签派生数据。新 commit 的桶即便 camera 未换档也会因 -1
    // 进入；GPU 瞬时失败保留旧数据并由 Pumped 票下一帧重试。
    symbolBucketsAwaitingRebuild_ = false;
    for (auto& entry : tileBuckets_) {
        BucketGpu& gpu = entry.second;
        if (iconAtlasChanged || stylePixelRatioChanged ||
            gpu.symbolViewZoomBucket != currentLabelZoomBucket) {
            (void)rebuildTileBucketSymbolsForZoom(
                gpu, currentLabelZoomBucket,
                iconAtlasChanged || stylePixelRatioChanged ||
                    gpu.symbolViewZoomBucket < 0);
        }
        if (gpu.symbolViewZoomBucket != currentLabelZoomBucket) {
            symbolBucketsAwaitingRebuild_ = true;
        }
    }

    // P6:GlyphAtlas 已按 frameId 重置 Renderer 级共享预算；这里给
    // "因缺字形推迟"的瓦片桶一次有界 drain。桶数量级 ~视口瓦数
    // (几十)，预算耗尽后 ensureGlyphBudgeted 立即 Deferred。
    if (atlasReady) {
        const double startMs = perf::nowMs();
        // Along-path labels are baked against projected screen arc. Keep the
        // last stable geometry during camera motion and refresh at most one
        // bucket per frame once its sampled path moves beyond 2 framebuffer
        // pixels. This bounds upload spikes without accepting visible drift.
        constexpr double kProjectedPathRefreshThresholdPx = 2.0;
        bool refreshedCameraDependentBucket = false;
        for (auto& entry : tileBuckets_) {
            BucketGpu& gpu = entry.second;
            if (refreshedCameraDependentBucket ||
                !gpu.hasCameraDependentLabelBake ||
                gpu.labelBakeViewportWidth <= 0.0 ||
                gpu.labelBakeViewportHeight <= 0.0) continue;
            bool exceeds = false;
            for (const auto& source : gpu.tileLabelSources) {
                if (source.pathCartographic.empty()) continue;
                const size_t samples[] = {
                    0, source.pathCartographic.size() / 2,
                    source.pathCartographic.size() - 1};
                for (size_t index : samples) {
                    const auto& p = source.pathCartographic[index];
                    const Vec3 ecef = ellipsoid_.cartographicToCartesian(
                        Cartographic(p[0], p[1], p[2]));
                    const auto oldPoint = projectScreenPoint(
                        ecef, gpu.labelBakeViewProjection,
                        gpu.labelBakeViewportWidth,
                        gpu.labelBakeViewportHeight);
                    const auto newPoint = projectScreenPoint(
                        ecef, commandFrame.viewProjection,
                        commandFrame.viewportWidth,
                        commandFrame.viewportHeight);
                    if (!oldPoint || !newPoint ||
                        std::hypot((*newPoint)[0] - (*oldPoint)[0],
                                   (*newPoint)[1] - (*oldPoint)[1]) >
                            kProjectedPathRefreshThresholdPx) {
                        exceeds = true;
                        break;
                    }
                }
                if (exceeds) break;
            }
            if (exceeds) {
                // Atomic camera refresh: bake replacement buffers while the
                // previous stable label remains drawable. bake swaps only
                // after every new buffer succeeds.
                const TileLabelBakeResult result = bakeTileBucketLabels(
                    gpu, currentLabelViewZoom,
                    commandFrame.stylePixelRatio,
                    commandFrame.viewProjection,
                    commandFrame.viewportWidth,
                    commandFrame.viewportHeight, true);
                refreshedCameraDependentBucket = true;
                if (result == TileLabelBakeResult::Settled) {
                    labelsAwaitingPlacement_ = true;
                }
            }
        }
        for (auto& entry : tileBuckets_) {
            if (entry.second.symbolViewZoomBucket == currentLabelZoomBucket &&
                !entry.second.labelBakeSettled &&
                entry.second.labelIndexCount == 0 &&
                !entry.second.tileLabelSources.empty()) {
                ++labelBucketsScanned;
                const bool wasSettled = entry.second.labelBakeSettled;
                const TileLabelBakeResult result =
                    bakeTileBucketLabels(entry.second,
                                         currentLabelViewZoom,
                                         commandFrame.stylePixelRatio,
                                         commandFrame.viewProjection,
                                         commandFrame.viewportWidth,
                                         commandFrame.viewportHeight);
                if (!wasSettled && entry.second.labelBakeSettled) {
                    ++labelBucketsCompleted;
                }
                if (result == TileLabelBakeResult::AtlasSaturated ||
                    result == TileLabelBakeResult::RetryableFailure) {
                    break;
                }
            }
        }
        // Android 将本层这一轮新字形封成一张有界 Landing 批次；整批后台
        // 结果入箱后再唤醒一次。host 同步路径为 no-op。
        glyphAtlas_->finishGlyphRasterDispatch();
        labelScanMs = perf::nowMs() - startMs;
    }
    // P6:重钳排队消化(每帧至多 kReclampBucketsPerFrame 个桶)
    if (!pendingReclamp_.empty()) {
        const double startMs = perf::nowMs();
        int done = 0;
        while (!pendingReclamp_.empty() && done < kReclampBucketsPerFrame) {
            auto it = tileBuckets_.find(pendingReclamp_.back());
            pendingReclamp_.pop_back();
            if (it != tileBuckets_.end()) {
                reclampTileBucketSymbols(it->second);
                reclampTileBucketFills(it->second);
                reclampTileBucketExtrusions(it->second);
                // E 方案 P2:线桶同款重钳(只换顶点缓冲)。
                reclampTileBucketLines(it->second);
                ++done;
            }
        }
        reclampBuckets = static_cast<size_t>(done);
        reclampMs = perf::nowMs() - startMs;
    }

    // 贴地重钳(P3 方案 A 过渡态):地形代次变化 → 节流重镶全部桶
    // (LOD 细化/加载会改高度;不重钳则要素浮沉)。节流防加载期重镶风暴;
    // 万级桶规模需配可见性门控(后续)。
    // [V27 家族第四缺口] 节流由"120 帧"改**时间 2s**:帧数节流在按需渲染下
    // 语义破产——冷启动帧门控只出几十帧就 idle,frameId 永远到不了 120,
    // 重钳一次都没跑过(真机 lastClampRev 恒 0 而 terrRev 涨到 45),锚点
    // 停在粗地形高度被细化后的山体埋掉 → T2 遮挡压到 0.2 = 冷启动 POI
    // "applied=1 却无像素"。"代次落后"已进 hasPendingLabelWork 谓词供帧,
    // 此处只管节流不管唤醒。
    if (style_.altitudeMode == FeatureAltitudeMode::ClampToGround &&
        terrainSampling_.revision) {
        const double startMs = perf::nowMs();
        reclampCooldownSeconds_ -= frameState.deltaSeconds;
        const uint64_t rev = terrainSampling_.revision();
        if (rev != lastClampRevision_ && reclampCooldownSeconds_ <= 0.0) {
            lastClampRevision_ = rev;
            reclampCooldownSeconds_ = 2.0;
            std::vector<BucketKey> keys;
            keys.reserve(buckets_.size());
            for (const auto& entry : buckets_) keys.push_back(entry.first);
            for (BucketKey key : keys) rebuildBucket(key);
            // 瓦片桶(POI 符号)同样要重钳。**此前漏了这一半** —— 锚点停在
            // commit 当刻的粗地形高度上,地形细化后山体升上来把它埋掉,
            // 硬件深度与 T2 判定一起吃掉符号 = "标记点闪一下就没";而
            // 缩放会触发瓦片换代重 commit,于是"缩放后又出现"。两个现象
            // 同一个因。(V24/B.6)
            pendingReclamp_.clear();
            pendingReclamp_.reserve(tileBuckets_.size());
            for (const auto& entry : tileBuckets_) {
                pendingReclamp_.push_back(entry.first);
            }
            previewDirty_ = true;
        }
        terrainRevisionMs = perf::nowMs() - startMs;
    }

    const double dirtyStartMs = perf::nowMs();
    syncDirtyBuckets();
    dirtySyncMs = perf::nowMs() - dirtyStartMs;

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
        std::vector<PaintRange> fillRanges;
        std::vector<float> lineVerts;
        std::vector<uint32_t> lineIndices;
        std::vector<PaintRange> lineRanges;
        std::vector<float> pointVerts;
        std::vector<uint32_t> pointIndices;
        std::vector<PaintRange> pointRanges;
        std::vector<float> labelVerts;
        std::vector<uint32_t> labelIndices;
        std::vector<PaintRange> labelRanges;
        std::vector<LabelEntry> labelEntries;
        VolumeCpuGroups volumeGroups;
        VolumeCpuGroups lineVolumeGroups;
        std::map<std::pair<int, int>, PaintGeometryCpu> fillGroups;
        std::map<std::pair<int, int>, PaintGeometryCpu> lineGroups;
        std::map<int, PaintGeometryCpu> pointGroups;
        std::map<std::pair<int, int>, LabelGeometryCpu> labelGroups;
        std::map<std::tuple<int, int, int, int>, PaintGeometryCpu> extrudeGroups;
        std::vector<float> extrudeVerts;
        std::vector<uint32_t> extrudeIndices;
        std::vector<PaintRange> extrudeRanges;
        Vec3 origin = Vec3::zero();
        bool hasOrigin = false;
        const int previewPaintOrder =
            resolvePaintOrder(style_, previewFeature);
        const int previewFillStyleGroup = resolveInteger(
            style_.fillStyleGroupExpr, previewFeature.properties,
            0);
        const int previewLineStyleGroup = resolveInteger(
            style_.lineStyleGroupExpr, previewFeature.properties,
            0);
        const int previewLabelPaintOrder = resolveLabelPaintOrder(
            style_, previewFeature.properties, previewPaintOrder);
        const int previewLabelStyleGroup = resolveLabelVisualStyleGroup(
            style_, previewFeature.properties, 0);
        tessellateFeatureInto(tessellationContext(), previewFeature,
                              previewPaintOrder,
                              previewFillStyleGroup,
                              previewLineStyleGroup,
                              previewLabelStyleGroup,
                              makeClampSampler(previewRings_),
                              origin, hasOrigin,
                              fillGroups[{previewPaintOrder,
                                          previewFillStyleGroup}],
                              lineGroups[{previewPaintOrder,
                                          previewLineStyleGroup}],
                              pointGroups[previewPaintOrder],
                              labelGroups[{previewLabelPaintOrder,
                                           previewLabelStyleGroup}],
                              volumeGroups, lineVolumeGroups,
                              extrudeGroups[{previewPaintOrder,
                                             previewFillStyleGroup, 0, 30}]);
        flattenStylePaintRanges(fillGroups, 4, fillVerts, fillIndices,
                                &fillRanges);
        flattenLinePaintRanges(lineGroups, kLineVertexFloats, lineVerts,
                               lineIndices, &lineRanges);
        flattenPaintRanges(pointGroups, kPointVertexFloats, pointVerts,
                           pointIndices, &pointRanges);
        flattenLabelRanges(labelGroups, labelVerts, labelIndices,
                           labelEntries, &labelRanges);
        flattenExtrusionRanges(extrudeGroups, extrudeVerts, extrudeIndices,
                               &extrudeRanges);
        if (hasOrigin) {
            previewGpuValid_ = uploadBucketGpu(
                origin, fillVerts, fillIndices, fillRanges,
                lineVerts, lineIndices, lineRanges,
                pointVerts, pointIndices, pointRanges, std::move(labelVerts),
                labelIndices, labelRanges, std::move(labelEntries),
                volumeGroups, lineVolumeGroups, extrudeVerts, extrudeIndices,
                extrudeRanges, previewGpu_);
        }
    }

    // 视口桶裁剪:命令生成与标签避让只覆盖地平线圆内的桶(见
    // visibleBucketKeys)。视野外桶保留 GPU 资源、跳过每帧成本——帧成本
    // 从"总桶数"收敛到"可见桶数"。
    const double visibleStartMs = perf::nowMs();
    const std::vector<BucketKey> visibleKeys = visibleBucketKeys(frameState);
    visibleMs = perf::nowMs() - visibleStartMs;

    // P5c:逐帧标签避让 placement + fade 回写(在命令生成前,顶点流为准)。
    const double placementStartMs = perf::nowMs();
    updateLabelPlacement(frameState, visibleKeys);
    placementMs = perf::nowMs() - placementStartMs;

    if (buckets_.empty() && tileBuckets_.empty() && !previewGpuValid_) return;

    // E1:MVT 瓦片桶先发(垫底,与 store 路径同一命令层)。可见性由上游
    // (VectorTileTree 的视口选择)负责 —— 驻留的瓦片桶本就是「本帧该画的」,
    // 这里再套一遍空间桶可见性判定是重复且会误杀(瓦片矩形与桶网格不对齐)。
    const double appendStartMs = perf::nowMs();
    for (const auto& entry : tileBuckets_) {
        appendBucketCommands(entry.second, frameState, commandFrame,
                             renderer, commands);
    }

    for (BucketKey key : visibleKeys) {
        auto it = buckets_.find(key);
        if (it == buckets_.end()) continue;
        appendBucketCommands(it->second, frameState, commandFrame,
                             renderer, commands);
    }
    if (previewFeatureId_ != kInvalidFeatureId && previewGpuValid_) {
        appendBucketCommands(previewGpu_, frameState, commandFrame,
                             renderer, commands);
    }
    appendMs = perf::nowMs() - appendStartMs;

    // V27:帧尾再 reconcile 一次 —— 帧头那次覆盖不到本帧中段置位的换代
    // (flip 重镶 / bake / drop),若那恰是最后一帧,谓词真而票没领 = 停帧
    // 冻住新标注(原 bug 的缩小残留窗口)。幂等,µs 级。
    syncLabelWorkTicket();
    const double buildMs = perf::nowMs() - buildStartMs;
    if (buildMs > 8.0) {
        platformLog(
            LogLevel::Info, "FeatureLayerPerf",
            "frame=%llu layer=%s total=%.2fms atlas=%.2f labelScan=%.2f"
            "(scan=%zu done=%zu) reclamp=%.2f(n=%zu pending=%zu) "
            "terrainRev=%.2f dirty=%.2f visible=%.2f placement=%.2f "
            "append=%.2f buckets=%zu cmds=%zu",
            static_cast<unsigned long long>(frameState.frameId),
            layerId_.c_str(), buildMs, atlasDrainMs, labelScanMs,
            labelBucketsScanned, labelBucketsCompleted, reclampMs,
            reclampBuckets, pendingReclamp_.size(), terrainRevisionMs,
            dirtySyncMs, visibleMs, placementMs, appendMs,
            tileBuckets_.size(), commands.size() - commandsBefore);
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
    // 符号刀D:碰撞判定节流 ~300ms(maplibre stillRecent 同款)——贵的
    // 是全量锚点投影 + 网格碰撞,而结果(谁显谁隐)在亚秒尺度本就稳定;
    // 渐变收敛必须逐帧平滑,节流间隙走 advanceFades + apply 回写。
    // 提权要素变更(编辑联动)即时重跑,交互不等节流窗。
    placementCooldownSeconds_ -= frameState.deltaSeconds;
    const bool priorityChanged =
        labelPlacement_.priorityFeature() != lastPlacementPriority_;
    const Camera& cam = *frameState.camera;
    const double viewZoom = labelViewZoom(ellipsoid_, cam);
    const double gateZoom = visibilityZoom(style_, viewZoom);
    const int zoomBucket = static_cast<int>(gateZoom);
    const bool zoomBucketChanged = zoomBucket != lastPlacementZoomBucket_;
    // V27:桶换代(bake 出新标注/重镶)即时全量 —— 新 entries 的 target 若等
    // 300ms 节流窗,停帧(settle 仅 ~3 帧)会让它们永远停在 opacity=0。
    const bool runFull = placementCooldownSeconds_ <= 0.0 || priorityChanged ||
                         zoomBucketChanged || labelsAwaitingPlacement_;
    if (!runFull) {
        if (labelPlacement_.advanceFades(frameState.deltaSeconds)) {
            applyLabelOpacity(visibleKeys);
        }
        return;
    }
    constexpr double kPlacementIntervalSeconds = 0.3;
    placementCooldownSeconds_ = kPlacementIntervalSeconds;
    lastPlacementPriority_ = labelPlacement_.priorityFeature();
    lastPlacementZoomBucket_ = zoomBucket;
    const double placeStartMs = perf::nowMs();

    // collect:可见桶 + 预览的 LabelEntry → 候选。视野外桶不进候选:
    // 它们的 fade 状态由 placement 状态机按"消失要素"清扫,重入视野按
    // 新候选淡入;其顶点 opacity 停留旧值无妨——桶本身不出命令。
    //
    // [V29 刀3] 同 id 候选按代去重,细代(高 tileZ)胜(maplibre
    // seenCrossTileIDs 最小版)。刀1/2 让换代双桶并存期的同一标注**共享
    // 同一 id**,而 targets 按 id 键:两份候选一 placed 一 collided,后写
    // 覆盖前写 + sort 非稳定 tie-break 又是同 id → target 在 0/1 间抖 =
    // 换代闪。去重后同 id 恒一份进碰撞;旧桶 entry 的顶点 opacity 经
    // applyLabelOpacity 按同 id 回写,与新代同亮度渐变(重叠期亚可见),
    // drop 后自然消失 —— holdingForFade 的最小等价。
    std::vector<LabelCandidate> candidates;
    std::unordered_map<FeatureId, std::pair<size_t, int>> dedup;  // id→(下标,代)
    auto collect = [&](const BucketGpu& gpu, int generation) {
        for (const LabelEntry& e : gpu.labelEntries) {
            if (gateZoom < static_cast<double>(e.minZoom) ||
                gateZoom >= static_cast<double>(e.maxZoom)) {
                continue;
            }
            LabelCandidate c;
            c.featureId = e.featureId;
            c.rank = e.rank;
            c.officialInsertionOrder = e.officialInsertionOrder;
            c.officialFragmentOrder = e.officialFragmentOrder;
            c.anchorEcef = e.anchorEcef;
            c.tangentEcef = e.tangentEcef;
            c.boxMinXPx = e.boxMinXPx;
            c.boxMinYPx = e.boxMinYPx;
            c.boxMaxXPx = e.boxMaxXPx;
            c.boxMaxYPx = e.boxMaxYPx;
            c.hasSecondaryBox = e.hasIconBox;
            c.secondaryBoxMinXPx = e.iconBoxMinXPx;
            c.secondaryBoxMinYPx = e.iconBoxMinYPx;
            c.secondaryBoxMaxXPx = e.iconBoxMaxXPx;
            c.secondaryBoxMaxYPx = e.iconBoxMaxYPx;
            c.repeatGroup = e.repeatGroup;
            c.repeatDistancePx = e.repeatDistancePx;
            c.angleRad = e.angleRad;
            c.paddingXPx = e.paddingXPx;
            c.paddingYPx = e.paddingYPx;
            c.officialCanCovered = e.officialCanCovered;
            c.collisionParts = e.collisionParts;
            const auto [it, inserted] = dedup.try_emplace(
                e.featureId, candidates.size(), generation);
            if (inserted) {
                candidates.push_back(c);
            } else if (generation > it->second.second) {
                candidates[it->second.first] = c;  // 细代替换粗代
                it->second.second = generation;
            }  // 同代/粗代:保留既有(先到先得,与 claim 先建序一致)
        }
    };
    // store 桶/预览:非瓦片代际语义,恒最新(编辑层显示优先);其 id 空间
    // 与 crossTile id 理论不重叠,generation 取 INT_MAX 仅作防御。
    for (BucketKey key : visibleKeys) {
        auto it = buckets_.find(key);
        if (it != buckets_.end()) collect(it->second, std::numeric_limits<int>::max());
    }
    // 瓦片桶(符号刀B):驻留集即渲染集,无空间桶可见性判定(同
    // buildRenderCommands 的理由),标签候选全量进 placement——地平线/
    // 视锥剔除由 placement 逐锚点做。代 = 瓦 z(细代胜)。
    for (auto& entry : tileBuckets_) collect(entry.second, entry.first.z);
    if (previewGpuValid_) collect(previewGpu_, std::numeric_limits<int>::max());

    const double collectMs = perf::nowMs() - placeStartMs;
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
    const double updateStartMs = perf::nowMs();
    labelPlacement_.update(in, candidates);
    const double updateMs = perf::nowMs() - updateStartMs;
    labelsAwaitingPlacement_ = false;  // V27:换代 entries 已进本次全量

    // 容量哨兵:单次全量 placement 超 4ms = 候选规模逼近"单帧一口气跑"
    // 的边界,该上 maplibre 式时间片增量(getBucketParts/可暂停推进)了。
    // 节流下 4ms 尖刺每 300ms 一次尚可接受,先报数再决定。
    const double placeMs = perf::nowMs() - placeStartMs;
    lastPlacementMs_ = placeMs;
    lastPlacementCandidates_ = candidates.size();
    if (placeMs > 4.0) {
        platformLog(LogLevel::Warning, "TileSymbol",
                    "placement 全量 %.2fms collect=%.2f update=%.2f "
                    "cand=%zu(超 4ms,考虑时间片增量)",
                    placeMs, collectMs, updateMs, candidates.size());
    }

    applyLabelOpacity(visibleKeys);

}

void FeatureRenderLayer::applyLabelOpacity(
    const std::vector<BucketKey>& visibleKeys) {
    // commit 回写:按 appliedOpacity vs 当前值的偏差同步,**不依赖
    // update() 的变化位** —— 桶重镶(贴地 revision/字体翻转/编辑)会把顶点
    // 流 opacity 重置 0,而 fade 状态可能早已收敛"无变化",若以变化位做
    // 早退,重镶后的桶永远不再回写(真机曾表现为标签 10s 后集体隐形)。
    // 稳态下逐 entry 比较即免写,成本 O(标签数) 浮点比较。
    const double uploadStartMs = perf::nowMs();
    size_t updateCalls = 0;
    size_t updateBytes = 0;
    size_t dirtyEntries = 0;
    auto apply = [&](BucketGpu& gpu) {
        struct DirtyRange {
            size_t beginFloat = 0;
            size_t endFloat = 0;
        };
        std::vector<DirtyRange> dirtyRanges;
        for (LabelEntry& e : gpu.labelEntries) {
            const float op = labelPlacement_.opacity(e.featureId);
            if (op == e.appliedOpacity) continue;
            // 顶点 8 float,opacity 在 offset.z(下标 5)。
            for (size_t i = e.vertexFloatStart + 8;
                 i < e.vertexFloatStart + e.vertexFloatCount; i += 11) {
                gpu.labelVertsCpu[i] = op;
            }
            e.appliedOpacity = op;
            ++dirtyEntries;
            const size_t begin = e.vertexFloatStart;
            const size_t end = begin + e.vertexFloatCount;
            if (!dirtyRanges.empty() && dirtyRanges.back().endFloat == begin) {
                dirtyRanges.back().endFloat = end;
            } else {
                dirtyRanges.push_back(DirtyRange{begin, end});
            }
        }
        if (!gpu.labelVertexBuffer) return;
        for (const DirtyRange& range : dirtyRanges) {
            const size_t offset = range.beginFloat * sizeof(float);
            const size_t size =
                (range.endFloat - range.beginFloat) * sizeof(float);
            const bool ok = renderDevice_->updateBuffer(
                gpu.labelVertexBuffer.get(), offset,
                gpu.labelVertsCpu.data() + range.beginFloat, size);
            if (!ok) {
                // 上传断链 = 标签隐形(顶点 opacity 停 0),必须可见。
                platformLog(LogLevel::Warning, "FeatureRenderLayer",
                            "label opacity updateBuffer FAILED off=%zu size=%zu",
                            offset, size);
                continue;
            }
            ++updateCalls;
            updateBytes += size;
        }
    };
    for (BucketKey key : visibleKeys) {
        auto it = buckets_.find(key);
        if (it != buckets_.end()) apply(it->second);
    }
    for (auto& entry : tileBuckets_) apply(entry.second);
    if (previewGpuValid_) apply(previewGpu_);
    const double uploadMs = perf::nowMs() - uploadStartMs;
    if (uploadMs > 4.0) {
        platformLog(LogLevel::Warning, "TileSymbol",
                    "opacity upload %.2fms calls=%zu bytes=%zu dirty=%zu",
                    uploadMs, updateCalls, updateBytes, dirtyEntries);
    }
}

bool FeatureRenderLayer::hasPendingLabelWork() const {
    // 无 device 无法推进任何标注工作,计入只会白烧帧(谓词必须有终止态)。
    if (!renderDevice_ || !labelWorkActiveForCurrentView_) return false;
    if (symbolBucketsAwaitingRebuild_) return true;
    // ② 换代待全量 placement / ③ fade 未收敛(便宜的先查)。
    if (labelsAwaitingPlacement_) return true;
    // ④⑤ [V27 家族第四缺口] 贴地重钳在途:队列未消化 / 地形代次落后(重钳
    // 触发与队列消化都只在渲染帧里跑,停帧=锚点停在粗地形高度被细化后的
    // 山体埋掉→T2 遮挡压暗)。代次落后进谓词也堵"末帧竞态":地形最后一个
    // 代次落地那帧若节流窗未到,之后停帧则永不触发——谓词持续供帧到重钳
    // 跑完(lastClampRevision_ 追平即转假,地形代次有限收敛,有终止态)。
    if (!pendingReclamp_.empty()) return true;
    if (style_.altitudeMode == FeatureAltitudeMode::ClampToGround &&
        terrainSampling_.revision &&
        terrainSampling_.revision() != lastClampRevision_) {
        return true;
    }
    if (labelPlacement_.hasPendingFades()) return true;
    // ① 未烘桶(字形按预算逐帧补,依赖帧循环 drain)。仅 atlas 就绪时计:
    // 字体没注入时补帧也无法推进,计入会白烧(谓词必须有终止态)。
    if (glyphAtlas_ && glyphAtlas_->ready()) {
        for (const auto& entry : tileBuckets_) {
            const BucketGpu& gpu = entry.second;
            // labelBakeSettled 排除"不会再有产物"的桶(烘出空/上传失败等翻
            // 转重试)—— 只有预算推迟的才算在途,否则谓词恒真白烧帧。
            if (!gpu.labelBakeSettled && gpu.labelIndexCount == 0 &&
                !gpu.tileLabelSources.empty()) {
                // Android 字形 SDF 在专用 worker 上自行推进。本帧只要已派发
                // 或已有任务在途，继续出帧就只是重复扫描并把约 900 条矢量
                // 命令全部重画；此时由 glyphRaster Landing 完成后唤醒。只有
                // 零在途且一次也没派成才保留 Pumped 重试，不降低标签或细节。
                return glyphAtlas_->needsFrameForGlyphRasterDispatch();
            }
        }
    }
    return false;
}

std::string FeatureRenderLayer::dumpLabelLifecycle(
    const std::string& nameFilter) const {
    // 行协议:首行层级态,随后每桶一行 + 每标注一行(缩进两格)。逐行
    // 独立可 grep(logcat 单条有截断上限,调用方按 \n 切开打)。
    char buf[256];
    std::string out;
    const uint64_t clampRevCur =
        terrainSampling_.revision ? terrainSampling_.revision() : 0;
    const LabelPlacementStats& st = labelPlacement_.stats();
    size_t unsettledBuckets = 0;
    for (const auto& entry : tileBuckets_) {
        const BucketGpu& gpu = entry.second;
        if (!gpu.labelBakeSettled && gpu.labelIndexCount == 0 &&
            !gpu.tileLabelSources.empty()) {
            ++unsettledBuckets;
        }
    }
    std::snprintf(
        buf, sizeof(buf),
        "LabelDump layer=%s vis=%d pending=%d await=%d fades=%d reclampQ=%zu "
        "clampRev=%llu/%llu cdPlace=%.2f cdReclamp=%.2f atlas=%d "
        "glyphJobs=%zu unsettled=%zu xt=%zu cand=%d placed=%d col=%d rep=%d "
        "horiz=%d proj=%d\n",
        layerId_.c_str(), visible_ ? 1 : 0, hasPendingLabelWork() ? 1 : 0,
        labelsAwaitingPlacement_ ? 1 : 0,
        labelPlacement_.hasPendingFades() ? 1 : 0, pendingReclamp_.size(),
        static_cast<unsigned long long>(clampRevCur),
        static_cast<unsigned long long>(lastClampRevision_),
        placementCooldownSeconds_, reclampCooldownSeconds_,
        (glyphAtlas_ && glyphAtlas_->ready()) ? 1 : 0,
        glyphAtlas_ ? glyphAtlas_->pendingGlyphRasterCount() : 0,
        unsettledBuckets, crossTileEntryCount_, st.candidates, st.placed,
        st.collided, st.repeated, st.culledHorizon, st.culledProjection);
    out += buf;

    // 每标注一行:fade 的 current→target 读 placement 账本(按 id 键,
    // 跨桶换代存活),applied 读顶点流回写值 —— 三者不一致时谁在说谎
    // 一眼可见。src 无对应 entry = 烘焙未产出(字形缺/预算推迟/空产物)。
    auto emitEntry = [&](const LabelEntry& e, const std::string& name) {
        if (!nameFilter.empty() &&
            (name.empty() || name.find(nameFilter) == std::string::npos)) {
            return;
        }
        std::snprintf(buf, sizeof(buf),
                      "  id=%llu name=%s fade=%.2f->%.2f applied=%.2f\n",
                      static_cast<unsigned long long>(e.featureId),
                      name.empty() ? "-" : name.c_str(),
                      labelPlacement_.opacity(e.featureId),
                      labelPlacement_.fadeTarget(e.featureId),
                      e.appliedOpacity);
        out += buf;
    };

    // 瓦片桶按 key 排序输出(unordered_map 遍历序不稳定,dump 要可比对)。
    std::vector<const std::pair<const TileKey, BucketGpu>*> tiles;
    tiles.reserve(tileBuckets_.size());
    for (const auto& entry : tileBuckets_) tiles.push_back(&entry);
    std::sort(tiles.begin(), tiles.end(), [](const auto* a, const auto* b) {
        const TileKey& ka = a->first;
        const TileKey& kb = b->first;
        if (ka.z != kb.z) return ka.z < kb.z;
        if (ka.x != kb.x) return ka.x < kb.x;
        return ka.y < kb.y;
    });
    for (const auto* entry : tiles) {
        const TileKey& key = entry->first;
        const BucketGpu& gpu = entry->second;
        if (gpu.tileLabelSources.empty() && gpu.labelEntries.empty()) continue;
        const bool reclampPend =
            std::find(pendingReclamp_.begin(), pendingReclamp_.end(), key) !=
            pendingReclamp_.end();
        std::snprintf(buf, sizeof(buf),
                      "LabelDump tile z=%d x=%d y=%d srcs=%zu entries=%zu "
                      "idx=%d settled=%d reclampPend=%d\n",
                      key.z, key.x, key.y, gpu.tileLabelSources.size(),
                      gpu.labelEntries.size(), gpu.labelIndexCount,
                      gpu.labelBakeSettled ? 1 : 0, reclampPend ? 1 : 0);
        out += buf;
        for (const LabelEntry& e : gpu.labelEntries) {
            // entry 名字经 featureId(=crossTile id)回连烘焙源。
            const std::string* name = nullptr;
            for (const auto& src : gpu.tileLabelSources) {
                if (src.featureId == e.featureId) { name = &src.name; break; }
            }
            emitEntry(e, name ? *name : std::string());
        }
        for (const auto& src : gpu.tileLabelSources) {
            bool baked = false;
            for (const LabelEntry& e : gpu.labelEntries) {
                if (e.featureId == src.featureId) { baked = true; break; }
            }
            if (baked) continue;
            if (!nameFilter.empty() &&
                src.name.find(nameFilter) == std::string::npos) {
                continue;
            }
            std::snprintf(buf, sizeof(buf),
                          "  id=%llu name=%s SRC-ONLY(未烘出 entry)\n",
                          static_cast<unsigned long long>(src.featureId),
                          src.name.c_str());
            out += buf;
        }
    }
    // store 桶(编辑路径)只有 entries 无烘焙源,无名 —— 空过滤时才输出。
    if (nameFilter.empty()) {
        for (const auto& entry : buckets_) {
            const BucketGpu& gpu = entry.second;
            if (gpu.labelEntries.empty()) continue;
            std::snprintf(buf, sizeof(buf),
                          "LabelDump store entries=%zu idx=%d\n",
                          gpu.labelEntries.size(), gpu.labelIndexCount);
            out += buf;
            for (const LabelEntry& e : gpu.labelEntries) {
                emitEntry(e, std::string());
            }
        }
    }
    return out;
}

void FeatureRenderLayer::syncLabelWorkTicket() {
    // 不可见层判不忙:不出命令的层不许扣住帧循环(否则隐藏层的半程 fade
    // 让整机永不 idle)。层析构时票 RAII 自释放。口径 = visible &&
    // hasPendingLabelWork,与 Scene::hasConvergingWork ④ 逐字一致(audit
    // 每帧对拍两判据,口径漂移会刷分歧日志)。
    const bool busy = visible_ && labelWorkActiveForCurrentView_ &&
                      hasPendingLabelWork();
    if (busy && !labelWorkTicket_.valid()) {
        labelWorkTicket_ = WorkLedger::shared().acquire(
            WorkLedger::Kind::Pumped, "labelConverge");
    } else if (!busy && labelWorkTicket_.valid()) {
        labelWorkTicket_.release();
    }
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
    cmd.hasVectorUniforms = true;
    cmd.vectorUniforms.terrainOcclusion = {
        enabled ? 1.0f : 0.0f,
        occ.nearPlaneMeters,
        occ.farPlaneMeters,
        occ.toleranceRatio};
    // y 只被点 shader 消费(文字侧不留底);两个 shader 都声明同一个 uniform,
    // 故这里统一挂,不按 kind 分叉。
    cmd.vectorUniforms.symbolOcclusion = {
        occ.minToleranceMeters,
        style_.usesOfficialProviderContract()
            ? 0.0f
            : presentationPolicy_.symbolOccludedMinOpacity,
        0.0f, 0.0f};
}

void FeatureRenderLayer::appendBucketCommands(
    const BucketGpu& gpu,
    const FrameState& frameState,
    const CommandFrameParams& frameParams,
    Renderer& renderer,
    RenderCommandList& commands) const {
    const double vpW = frameParams.viewportWidth;
    const double vpH = frameParams.viewportHeight;
    const glm::dmat4 viewProj(frameParams.viewProjection.raw());

    ShaderProgram* fillShader = renderer.colorShader();
    ShaderProgram* lineShader = renderer.vectorLineShader();

    // P6b zoom 驱动宽度/尺寸:每帧按相机大地高求 zoom(web 墨卡托惯例
    // zoom ≈ log2(赤道周长 4e7m / 视高),z0 ≈ 全球一屏),表达式求值进
    // uniform;求值失败/非数值回落字面量。
    const double zoomLevel = frameParams.zoomLevel;
    const double gateZoom = visibilityZoom(style_, zoomLevel);
    const float lineWidthPx = frameParams.lineWidthPx;
    const float pointSizePx = frameParams.pointSizePx;
    const float symbolDepthPush = frameParams.symbolDepthPush;
    const bool amapOfficialContract =
        style_.usesOfficialProviderContract();

    {
        // 双精度 compose 后降 float(RTE):顶点已相对 origin,mvp 吸收平移。
        const glm::dmat4 model = glm::translate(
            glm::dmat4(1.0),
            glm::dvec3(gpu.origin.x(), gpu.origin.y(), gpu.origin.z()));
        const glm::mat4 mvp = glm::mat4(viewProj * model);
        VectorUniformBlock bucketUniforms;
        std::memcpy(bucketUniforms.modelViewProjection.data(),
                    glm::value_ptr(mvp),
                    16 * sizeof(float));
        bucketUniforms.viewport = {static_cast<float>(vpW),
                                   static_cast<float>(vpH)};
        bucketUniforms.lightDir = {frameState.lightDir.x,
                                   frameState.lightDir.y,
                                   frameState.lightDir.z};
        bucketUniforms.ambient = 0.25f;
        bucketUniforms.lineWidthPx = lineWidthPx;
        bucketUniforms.halfWidthPerEyeZ = frameParams.halfWidthPerEyeZ;
        // Amap/MapLibre dash phase is based on source/tile cumulative distance
        // times a command-level tile-units-to-pixels scale, not on per-vertex
        // eye depth.  Our source distance is ground meters, so use the
        // equivalent Web-Mercator ground-resolution scale at the bucket
        // latitude.  Perspective interpolation then compresses the pattern
        // under pitch without re-scaling all prior arclength at each endpoint.
        constexpr double kMercatorWorldMeters = 40075016.68557849;
        constexpr double kStyleTilePixels = 512.0;
        const Cartographic bucketCartographic =
            ellipsoid_.cartesianToCartographic(gpu.origin);
        const double latitudeScale = std::max(
            0.01, std::abs(std::cos(bucketCartographic.latitude())));
        bucketUniforms.dashPixelsPerMeter = static_cast<float>(
            kStyleTilePixels * std::exp2(zoomLevel) /
            (kMercatorWorldMeters * latitudeScale));
        bucketUniforms.dashPeriodMeters =
            amapOfficialContract ? 0.0f : style_.lineDashPeriodMeters;
        bucketUniforms.dashOnFraction =
            amapOfficialContract ? 0.0f : style_.lineDashOnFraction;
        bucketUniforms.pointSizePx = pointSizePx;
        bucketUniforms.depthPushNdc = symbolDepthPush;
        bucketUniforms.sdfEdge = amapOfficialContract
            ? static_cast<float>(GlyphAtlas::kAmapSdfOnEdge) / 256.0f
            : static_cast<float>(GlyphAtlas::kSdfOnEdge) / 255.0f;
        bucketUniforms.sdfHaloDelta = 0.0f;
        bucketUniforms.sdfGamma = 0.0f;
        if (!amapOfficialContract) {
            const float genericGlyphScale =
                style_.labelSizePx * frameParams.stylePixelRatio /
                glyphAtlas_->metricPixelHeight();
            bucketUniforms.sdfHaloDelta =
                style_.labelHaloPx * frameParams.stylePixelRatio /
                std::max(0.01f, genericGlyphScale) *
                GlyphAtlas::kSdfDistScale / 255.0f;
        }

        auto attachVectorUniforms = [&](RenderCommand& cmd) {
            cmd.hasVectorUniforms = true;
            cmd.vectorUniforms = bucketUniforms;
        };

        // P6 stencil 贴地(方案 B):体积按解析色分组(P6b),每组一对相邻
        // 命令。两 phase 共享普通矢量的 MVP order，由 vectorPaintOrder
        // 决定其相对水/绿地/用地/道路的压盖顺序；同组 stable_sort 保持
        // 体 pass 在色 pass 前，色 pass op ZERO 顺手清零，组间不串。
        // **契约**:色 pass 恒用 pos-only + uniform 纯色。分类着色的是地形
        // 像素、光栅化的却是体表面,任何"从体面插值 varying 再决定 fragment
        // 外观"的做法(pattern/渐变/沿线里程)在侧视下都有视差,线 dash 已
        // 为此付过代价(终态改镶嵌期几何切分)。fill 要加图案同理走几何或
        // 地形深度重建,别加 varying。
        for (const auto& group : gpu.volumeGroups) {
            if (group.indexCount <= 0 || !fillShader) continue;
            RenderCommand vol;
            vol.kind = RenderCommandKind::VectorStencil;
            vol.vectorPaintOrder = group.paintOrder;
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
            attachVectorUniforms(vol);
            vol.vectorUniforms.color = {0.0f, 0.0f, 0.0f, 0.0f};

            RenderCommand col = vol;
            col.stencilPhase = StencilPhase::ClassifyColor;
            col.depthTest = false;  // 覆盖面自身别被地形挡
            // 覆盖面只需把 stencil 选中的像素盖一次:水密体的背面单独就覆盖
            // 整个轮廓,双面是白烧一倍光栅化。取背面而非正面 —— 相机进体内时
            // 正面被近平面切掉,背面永远在。
            col.cullFace = true;
            col.cullMode = RenderCommand::CullMode::Front;
            col.blend = true;
            col.vectorUniforms.color = {group.color[0], group.color[1],
                                        group.color[2], group.color[3]};
            commands.push_back(std::move(vol));
            commands.push_back(std::move(col));
        }

        // P6d stencil 贴地线:墙带体命令对(同 MVP order + ordinal，插入序紧邻契约;
        // 每组色 pass op ZERO 清零,与 fill 组间互不串)。宽度在 VS 按眼深
        // 换算世界米挤出:halfW = u_halfWidthPerEyeZ * |ec.z|,
        // u_halfWidthPerEyeZ = lineWidthPx * tan(fovy/2) / vpH(即每米眼深
        // 对应的半宽米数),像素语义与方案 A ribbon 一致。
        if (!gpu.lineVolumeGroups.empty() &&
            renderer.vectorLineStencilShader()) {
            const glm::dmat4 view(frameParams.view.raw());
            const glm::mat4 modelView = glm::mat4(view * model);
            std::array<float, 16> modelViewUniform{};
            std::memcpy(modelViewUniform.data(), glm::value_ptr(modelView),
                        modelViewUniform.size() * sizeof(float));
            for (const auto& group : gpu.lineVolumeGroups) {
                if (group.indexCount <= 0) continue;
                RenderCommand vol;
                vol.kind = RenderCommandKind::VectorStencil;
                vol.vectorPaintOrder = group.paintOrder;
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
                attachVectorUniforms(vol);
                vol.vectorUniforms.modelView = modelViewUniform;
                // dash 已在镶嵌期切成独立划体(几何边界),FS 无需判里程。
                vol.vectorUniforms.color = {0.0f, 0.0f, 0.0f, 0.0f};

                RenderCommand col = vol;
                col.stencilPhase = StencilPhase::ClassifyColor;
                col.depthTest = false;
                // 同 fill 组:墙带同样水密(见 emitStrip 的「任意边恰被 2
                // 三角引用」),背面即完整覆盖面。
                col.cullFace = true;
                col.cullMode = RenderCommand::CullMode::Front;
                col.blend = true;
                col.vectorUniforms.color = {group.color[0], group.color[1],
                                            group.color[2], group.color[3]};
                commands.push_back(std::move(vol));
                commands.push_back(std::move(col));
            }
        }

        auto appendFill = [&](int paintOrder, int styleGroup, int indexOffset,
                              int indexCount) {
            if (indexCount <= 0 || !renderer.vectorFillShader()) return;
            const auto minIt = style_.fillMinZoomByStyleGroup.find(styleGroup);
            if (minIt != style_.fillMinZoomByStyleGroup.end() &&
                gateZoom < minIt->second) return;
            const auto maxIt = style_.fillMaxZoomByStyleGroup.find(styleGroup);
            if (maxIt != style_.fillMaxZoomByStyleGroup.end() &&
                gateZoom >= maxIt->second) return;
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorFill;
            cmd.vectorPaintOrder = paintOrder;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = renderer.vectorFillShader();
            cmd.vertexBuffer = gpu.fillVertexBuffer.get();
            cmd.indexBuffer = gpu.fillIndexBuffer.get();
            cmd.indexOffset = indexOffset;
            cmd.indexCount = indexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            cmd.vertexStride = 16;  // pos(12)+color(4,RGBA8)
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            cmd.depthTest = true;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            attachVectorUniforms(cmd);
            const auto colorIt =
                style_.fillColorExprByStyleGroup.find(styleGroup);
            if (style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::FillIdentity) &&
                (colorIt == style_.fillColorExprByStyleGroup.end() ||
                 !colorIt->second)) {
                return;
            }
            if (colorIt != style_.fillColorExprByStyleGroup.end() &&
                colorIt->second) {
                const auto value = colorIt->second->evaluate(nullptr, zoomLevel);
                if (!value || value->kind() != StyleValue::Kind::Color ||
                    value->color()[3] <= 0.0f) {
                    return;
                }
                cmd.vectorUniforms.color = value->color();
            }
            commands.push_back(std::move(cmd));
        };
        if (!gpu.fillRanges.empty()) {
            for (const auto& range : gpu.fillRanges) {
                appendFill(range.paintOrder, range.styleGroup,
                           range.indexOffset,
                           range.indexCount);
            }
        }

        auto appendExtrusion = [&](int paintOrder, int styleGroup, int indexOffset,
                                   int indexCount, int featureMinZoom,
                                   int featureMaxZoom) {
            if (indexCount <= 0 || !renderer.vectorExtrusionShader()) return;
            if (gateZoom < featureMinZoom || gateZoom >= featureMaxZoom) return;
            const auto roofColor =
                style_.extrusionRoofColorByStyleGroup.find(styleGroup);
            const auto wallColor =
                style_.extrusionWallColorByStyleGroup.find(styleGroup);
            if (roofColor == style_.extrusionRoofColorByStyleGroup.end() ||
                wallColor == style_.extrusionWallColorByStyleGroup.end()) return;
            const auto minZoom = style_.fillMinZoomByStyleGroup.find(styleGroup);
            if (minZoom != style_.fillMinZoomByStyleGroup.end() &&
                gateZoom < minZoom->second) return;
            const auto maxZoom = style_.fillMaxZoomByStyleGroup.find(styleGroup);
            if (maxZoom != style_.fillMaxZoomByStyleGroup.end() &&
                gateZoom >= maxZoom->second) return;
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorExtrusion;
            cmd.vectorPaintOrder = paintOrder;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = renderer.vectorExtrusionShader();
            cmd.vertexBuffer = gpu.extrudeVertexBuffer.get();
            cmd.indexBuffer = gpu.extrudeIndexBuffer.get();
            cmd.indexOffset = indexOffset;
            cmd.indexCount = indexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            cmd.vertexStride = 28;  // pos(12)+normal(12)+color(4)
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            const bool translucent = roofColor->second[3] < 0.999f ||
                                     wallColor->second[3] < 0.999f;
            cmd.depthTest = true;
            cmd.depthWrite = !translucent;
            cmd.blend = translucent;
            cmd.cullFace = false;
            attachVectorUniforms(cmd);
            commands.push_back(std::move(cmd));
        };
        if (!gpu.extrudeRanges.empty()) {
            for (const auto& range : gpu.extrudeRanges) {
                appendExtrusion(range.paintOrder, range.styleGroup, range.indexOffset,
                                range.indexCount, range.minZoom, range.maxZoom);
            }
        }

        auto appendLine = [&](int paintOrder, int styleGroup, int indexOffset,
                              int indexCount) {
            if (indexCount <= 0 || !lineShader) return;
            const auto minZoomIt =
                style_.lineMinZoomByStyleGroup.find(styleGroup);
            if (minZoomIt != style_.lineMinZoomByStyleGroup.end() &&
                gateZoom < minZoomIt->second) return;
            const auto maxZoomIt =
                style_.lineMaxZoomByStyleGroup.find(styleGroup);
            if (maxZoomIt != style_.lineMaxZoomByStyleGroup.end() &&
                gateZoom >= maxZoomIt->second) return;
            auto evalStrokeNumber = [&](const auto& table,
                                        bool allowNegative = false)
                -> std::optional<float> {
                auto it = table.find(styleGroup);
                if (it == table.end() || !it->second) return std::nullopt;
                const auto value = it->second->evaluate(nullptr, zoomLevel);
                if (!value || value->kind() != StyleValue::Kind::Number ||
                    !std::isfinite(value->number()) ||
                    (!allowNegative && value->number() < 0.0))
                    return std::nullopt;
                return static_cast<float>(value->number());
            };
            auto rangeLineWidthCssPxValue = evalStrokeNumber(
                style_.lineWidthExprByStyleGroup);
            if (!rangeLineWidthCssPxValue && !amapOfficialContract) {
                rangeLineWidthCssPxValue = lineWidthPx;
            }
            const float rangeLineWidthCssPx =
                rangeLineWidthCssPxValue.value_or(0.0f);
            const float rangeLineWidthPx =
                rangeLineWidthCssPx * frameParams.stylePixelRatio;
            const bool casingEligible =
                (amapOfficialContract
                     ? style_.lineCasingStyleGroups.count(styleGroup) != 0
                     : (style_.lineCasingEnabled &&
                        gateZoom >= style_.lineCasingMinZoom &&
                        gateZoom <= style_.lineCasingMaxZoom)) &&
                (style_.lineCasingStyleGroups.empty() ||
                 style_.lineCasingStyleGroups.count(styleGroup) != 0);
            RenderCommand center;
            center.kind = RenderCommandKind::VectorLine;
            center.vectorPaintOrder = paintOrder;
            center.vectorPaintSubOrder = casingEligible ? 1 : 0;
            center.owner = layerId_;
            center.pass = "color";
            center.frameId = frameState.frameId;
            center.shader = lineShader;
            center.vertexBuffer = gpu.lineVertexBuffer.get();
            center.indexBuffer = gpu.lineIndexBuffer.get();
            center.indexOffset = indexOffset;
            center.indexCount = indexCount;
            center.indexType = RenderCommand::IndexType::UInt32;
            center.vertexStride =
                static_cast<int>(kLineVertexFloats * sizeof(float));  // 48
            center.primitive = RenderCommand::PrimitiveType::Triangles;
            center.depthTest = true;
            center.depthWrite = false;
            center.blend = true;
            center.cullFace = false;
            attachVectorUniforms(center);
            center.vectorUniforms.lineWidthPx = rangeLineWidthPx;
            const auto solidCapIt =
                style_.lineSolidCapExprByStyleGroup.find(styleGroup);
            if (solidCapIt != style_.lineSolidCapExprByStyleGroup.end() &&
                solidCapIt->second) {
                const auto value =
                    solidCapIt->second->evaluate(nullptr, zoomLevel);
                if (value && value->kind() == StyleValue::Kind::Number &&
                    std::isfinite(value->number())) {
                    const int cap = static_cast<int>(std::lround(
                        value->number()));
                    if (cap == static_cast<int>(
                                   FeatureRenderStyle::LineCap::Round)) {
                        center.vectorUniforms.solidCapStyle =
                            static_cast<float>(cap);
                    }
                }
            }
            auto centerColorIt =
                style_.lineColorExprByStyleGroup.find(styleGroup);
            bool centerColorResolved = !style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity);
            if (centerColorIt != style_.lineColorExprByStyleGroup.end() &&
                centerColorIt->second) {
                const auto value =
                    centerColorIt->second->evaluate(nullptr, zoomLevel);
                if (value && value->kind() == StyleValue::Kind::Color) {
                    center.vectorUniforms.color = value->color();
                    centerColorResolved = true;
                }
            }
            bool centerVisible =
                !style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity) ||
                (rangeLineWidthCssPxValue.has_value() && centerColorResolved &&
                 center.vectorUniforms.lineWidthPx > 0.0f &&
                 center.vectorUniforms.color[3] > 0.0f);
            auto applyPixelDash = [&](RenderCommand& command,
                                      const auto& table) {
                auto it = table.find(styleGroup);
                if (it == table.end()) return;
                const auto& dash = it->second;
                if (dash.count != 2 && dash.count != 4) return;
                bool valid = true;
                for (uint8_t i = 0; i < dash.count; ++i) {
                    valid = valid && std::isfinite(dash.lengths[i]) &&
                            dash.lengths[i] > 0.0f;
                }
                if (!valid) return;
                command.vectorUniforms.dashPattern = dash.lengths;
                for (uint8_t i = 0; i < dash.count; ++i) {
                    command.vectorUniforms.dashPattern[i] *=
                        frameParams.stylePixelRatio;
                }
                command.vectorUniforms.dashPatternCount =
                    static_cast<float>(dash.count);
                command.vectorUniforms.dashCapStyle =
                    static_cast<float>(dash.cap);
                // Pixel-relative style dash owns the command.  Never combine
                // it with the legacy world-meter phase by accident.
                command.vectorUniforms.dashPeriodMeters = 0.0f;
            };
            auto applyLineType = [&](RenderCommand& command,
                                     const auto& table) -> bool {
                if (!style_.lineTypeResolver) return false;
                auto it = table.find(styleGroup);
                if (it == table.end() || !it->second) return false;
                const auto value = it->second->evaluate(nullptr, zoomLevel);
                if (!value || value->kind() != StyleValue::Kind::Number ||
                    !std::isfinite(value->number())) return false;
                const auto dash = style_.lineTypeResolver(
                    static_cast<int>(std::lround(value->number())));
                if (!dash) return false;
                command.vectorUniforms.dashPattern = dash->lengths;
                for (uint8_t i = 0; i < dash->count; ++i) {
                    command.vectorUniforms.dashPattern[i] *=
                        frameParams.stylePixelRatio;
                }
                command.vectorUniforms.dashPatternCount =
                    static_cast<float>(dash->count);
                command.vectorUniforms.dashCapStyle =
                    static_cast<float>(dash->cap);
                if (dash->count == 0) {
                    command.vectorUniforms.solidCapStyle =
                        static_cast<float>(dash->cap);
                }
                command.vectorUniforms.dashPeriodMeters = 0.0f;
                return true;
            };
            const bool centerLineTypeResolved =
                applyLineType(center, style_.lineTypeExprByStyleGroup);
            if (!centerLineTypeResolved && !amapOfficialContract) {
                applyPixelDash(center, style_.lineDashByStyleGroup);
            }
            if ((amapOfficialContract ||
                 (style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity) &&
                  style_.lineTypeResolver)) &&
                !centerLineTypeResolved) {
                centerVisible = false;
            }
            if (casingEligible) {
                RenderCommand casing = center;
                casing.vectorPaintSubOrder = 0;
                // Casing and center are independent Amap style layers.  The
                // command is cloned only to share geometry/state; never let a
                // center dash leak into an explicitly solid casing.
                casing.vectorUniforms.dashPattern = {0.0f, 0.0f, 0.0f, 0.0f};
                casing.vectorUniforms.dashPatternCount = 0.0f;
                casing.vectorUniforms.dashCapStyle = 0.0f;
                // Official road border/casing owns an independent lineType;
                // never inherit the center's solid round endpoint contract.
                casing.vectorUniforms.solidCapStyle = 0.0f;
                const auto casingCapIt =
                    style_.lineCasingSolidCapExprByStyleGroup.find(styleGroup);
                if (casingCapIt !=
                        style_.lineCasingSolidCapExprByStyleGroup.end() &&
                    casingCapIt->second) {
                    const auto cap = casingCapIt->second->evaluate(
                        nullptr, zoomLevel);
                    if (cap && cap->kind() == StyleValue::Kind::Number &&
                        cap->number() >= static_cast<double>(
                            FeatureRenderStyle::LineCap::Round) - 0.5) {
                        casing.vectorUniforms.solidCapStyle =
                            static_cast<float>(cap->number());
                    }
                }
                casing.vectorUniforms.dashPeriodMeters =
                    amapOfficialContract ? 0.0f
                                         : style_.lineDashPeriodMeters;
                casing.vectorUniforms.dashOnFraction =
                    amapOfficialContract ? 0.0f
                                         : style_.lineDashOnFraction;
                const bool hasStyleGroupCasing =
                    style_.lineCasingWidthExprByStyleGroup.count(styleGroup) != 0;
                if (amapOfficialContract && !hasStyleGroupCasing) {
                    if (centerVisible) commands.push_back(std::move(center));
                    return;
                }
                auto rangeCasingExtraCssPx = evalStrokeNumber(
                    style_.lineCasingWidthExprByStyleGroup, true);
                if (!rangeCasingExtraCssPx && !amapOfficialContract) {
                    rangeCasingExtraCssPx = std::max(
                        0.0f,
                        style_.lineCasingWidthRatio > 0.0f
                            ? rangeLineWidthCssPx *
                                  style_.lineCasingWidthRatio
                            : style_.lineCasingExtraWidthPx);
                }
                const float rangeCasingDeltaPx =
                    rangeCasingExtraCssPx.value_or(0.0f) *
                    frameParams.stylePixelRatio;
                const float rangeCasingTotalPx =
                    rangeLineWidthPx + rangeCasingDeltaPx;
                if (rangeCasingTotalPx <= 0.0f &&
                    (amapOfficialContract || hasStyleGroupCasing ||
                     style_.lineCasingExtraWidthPx <= 0.0f)) {
                    if (centerVisible) commands.push_back(std::move(center));
                    return;
                }
                casing.vectorUniforms.lineWidthPx = rangeCasingTotalPx;
                const bool casingLineTypeResolved = applyLineType(
                    casing, style_.lineCasingTypeExprByStyleGroup);
                if (!casingLineTypeResolved && !amapOfficialContract) {
                    applyPixelDash(casing,
                                   style_.lineCasingDashByStyleGroup);
                }
                auto casingColorIt =
                    style_.lineCasingColorByStyleGroup.find(styleGroup);
                bool casingColorResolved = !style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity);
                if (casingColorIt !=
                    style_.lineCasingColorByStyleGroup.end()) {
                    casing.vectorUniforms.color = casingColorIt->second;
                    casingColorResolved = true;
                } else if (!style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity)) {
                    casing.vectorUniforms.color = style_.lineCasingColor;
                }
                auto casingColorExprIt =
                    style_.lineCasingColorExprByStyleGroup.find(styleGroup);
                if (casingColorExprIt !=
                        style_.lineCasingColorExprByStyleGroup.end() &&
                    casingColorExprIt->second) {
                    const auto value = casingColorExprIt->second->evaluate(
                        nullptr, zoomLevel);
                    if (value && value->kind() == StyleValue::Kind::Color) {
                        casing.vectorUniforms.color = value->color();
                        casingColorResolved = true;
                    }
                }
                if ((!style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity) ||
                    (rangeCasingExtraCssPx.has_value() &&
                     casingColorResolved &&
                     casing.vectorUniforms.lineWidthPx > 0.0f &&
                     casing.vectorUniforms.color[3] > 0.0f)) &&
                    (!style_.requiresOfficial(FeatureRenderStyle::OfficialRequirement::LineIdentity) ||
                     !style_.lineTypeResolver || casingLineTypeResolved) &&
                    (!amapOfficialContract || casingLineTypeResolved)) {
                    commands.push_back(std::move(casing));
                }
            }
            if (centerVisible) commands.push_back(std::move(center));
        };
        if (!gpu.lineRanges.empty()) {
            for (const auto& range : gpu.lineRanges) {
                appendLine(range.paintOrder, range.styleGroup,
                           range.indexOffset,
                           range.indexCount);
            }
        }

        auto appendPoint = [&](int paintOrder, int indexOffset,
                               int indexCount) {
            if (indexCount <= 0 || !renderer.vectorPointShader()) return;
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorPoint;
            cmd.vectorPaintOrder = paintOrder;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = renderer.vectorPointShader();
            cmd.vertexBuffer = gpu.pointVertexBuffer.get();
            cmd.indexBuffer = gpu.pointIndexBuffer.get();
            cmd.indexOffset = indexOffset;
            cmd.indexCount = indexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            // anchor(12)+offsetUnit(8)+uv(8)+color(4,RGBA8)+shape(4)
            cmd.vertexStride = 36;
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            // 符号**不做硬件逐像素深度测试**:billboard 四角共用锚点深度,
            // 逐像素比对只会把 quad 切掉一块 —— 而 quad 像素没有 3D 位置
            // 语义,那道切口传达的是不存在的形状边界。遮挡判定全部交给
            // 锚点(shader 的 eeSymbolTerrainVisibility)。三家引擎同解:
            // maplibre 地形模式关深度测试、osgEarth 默认 Depth(ALWAYS)、
            // cesium 默认 depthTestAgainstTerrain=false。
            cmd.depthTest = false;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            attachVectorUniforms(cmd);
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
        };
        if (!gpu.pointRanges.empty()) {
            // Amap POI 常见多个 minZoom 档，但同 paintOrder 且当前可见的
            // 区间在 IBO 中连续；合并成尽量少的 draw，避免按档位放大
            // 每瓦命令数。只有 maxZoom 造成真实空洞时才拆命令。
            int mergedOrder = 0;
            int mergedOffset = 0;
            int mergedCount = 0;
            auto flushPointRange = [&]() {
                if (mergedCount <= 0) return;
                appendPoint(mergedOrder, mergedOffset, mergedCount);
                mergedCount = 0;
            };
            for (const auto& range : gpu.pointRanges) {
                if (gateZoom < static_cast<double>(range.minZoom) ||
                    gateZoom >= static_cast<double>(range.maxZoom)) {
                    flushPointRange();
                    continue;
                }
                if (mergedCount > 0 && mergedOrder == range.paintOrder &&
                    mergedOffset + mergedCount == range.indexOffset) {
                    mergedCount += range.indexCount;
                } else {
                    flushPointRange();
                    mergedOrder = range.paintOrder;
                    mergedOffset = range.indexOffset;
                    mergedCount = range.indexCount;
                }
            }
            flushPointRange();
        }

        auto appendLabelBackground = [&](int paintOrder, int indexOffset,
                                         int indexCount) {
            if (indexCount <= 0 ||
                !renderer.vectorLabelBackgroundShader() || !iconAtlas_ ||
                !iconAtlas_->texture() || !gpu.labelVertexBuffer ||
                !gpu.labelBackgroundIndexBuffer) return;
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorLabel;
            cmd.vectorPaintOrder = paintOrder;
            cmd.vectorPaintSubOrder = 2;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = renderer.vectorLabelBackgroundShader();
            cmd.vertexBuffer = gpu.labelVertexBuffer.get();
            cmd.indexBuffer = gpu.labelBackgroundIndexBuffer.get();
            cmd.indexOffset = indexOffset;
            cmd.indexCount = indexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            cmd.vertexStride = 44;
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            cmd.depthTest = false;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            cmd.textures.push_back(iconAtlas_->texture());
            attachVectorUniforms(cmd);
            appendTerrainOcclusion(renderer, cmd);
            commands.push_back(std::move(cmd));
        };
        for (const auto& range : gpu.labelBackgroundRanges) {
            if (gateZoom >= static_cast<double>(range.minZoom) &&
                gateZoom < static_cast<double>(range.maxZoom)) {
                appendLabelBackground(range.paintOrder, range.indexOffset,
                                      range.indexCount);
            }
        }

        auto appendLabel = [&](int paintOrder, int styleGroup,
                               int indexOffset,
                               int indexCount) {
            if (indexCount <= 0 || !renderer.vectorLabelShader() ||
                !glyphAtlas_ || !glyphAtlas_->texture()) return;
            RenderCommand cmd;
            cmd.kind = RenderCommandKind::VectorLabel;
            cmd.vectorPaintOrder = paintOrder;
            cmd.vectorPaintSubOrder = 3;
            cmd.owner = layerId_;
            cmd.pass = "color";
            cmd.frameId = frameState.frameId;
            cmd.shader = renderer.vectorLabelShader();
            cmd.vertexBuffer = gpu.labelVertexBuffer.get();
            cmd.indexBuffer = gpu.labelIndexBuffer.get();
            cmd.indexOffset = indexOffset;
            cmd.indexCount = indexCount;
            cmd.indexType = RenderCommand::IndexType::UInt32;
            cmd.vertexStride = 44;  // anchor+tangent+offset/opacity+uv
            cmd.primitive = RenderCommand::PrimitiveType::Triangles;
            // 同点符号:遮挡只在锚点判,不做逐像素切割(见上方注释)。
            cmd.depthTest = false;
            cmd.depthWrite = false;
            cmd.blend = true;
            cmd.cullFace = false;
            cmd.textures.push_back(glyphAtlas_->texture());
            attachVectorUniforms(cmd);
            appendTerrainOcclusion(renderer, cmd);
            cmd.vectorUniforms.color =
                resolvedLabelColor(style_, styleGroup, zoomLevel);
            cmd.vectorUniforms.haloColor =
                resolvedLabelHaloColor(style_, styleGroup, zoomLevel);
            const float labelSizeCssPx = resolvedLabelSizePx(
                style_, styleGroup, zoomLevel,
                amapOfficialContract ? 0.0f : style_.labelSizePx);
            const float labelHaloCssPx = resolvedLabelHaloWidthPx(
                style_, styleGroup, zoomLevel);
            if (amapOfficialContract &&
                (!(labelSizeCssPx > 0.0f) || labelHaloCssPx < 0.0f)) {
                return;
            }
            if (amapOfficialContract) {
                const float physicalStrokePx = std::min(
                    10.0f, labelHaloCssPx * frameParams.stylePixelRatio);
                const float baseEdge = labelSizeCssPx < 10.0f
                    ? 0.78125f
                    : static_cast<float>(GlyphAtlas::kAmapSdfOnEdge) / 256.0f;
                const float dpr = frameParams.stylePixelRatio;
                const float buffer = baseEdge + 1.5f / 256.0f * (dpr - 1.0f);
                const float borderBuffer = baseEdge *
                    (1.0f - physicalStrokePx / 10.1f);
                cmd.vectorUniforms.sdfEdge = buffer;
                cmd.vectorUniforms.sdfHaloDelta = buffer - borderBuffer;
                cmd.vectorUniforms.sdfGamma = 1.4142f *
                    ((labelSizeCssPx <= 24.0f && dpr <= 1.0f) ? 1.5f : 1.7f) /
                    labelSizeCssPx;
            } else {
                const float labelGlyphScale =
                    labelSizeCssPx / glyphAtlas_->metricPixelHeight();
                cmd.vectorUniforms.sdfHaloDelta = labelHaloCssPx /
                    std::max(0.01f, labelGlyphScale) *
                    GlyphAtlas::kSdfDistScale / 255.0f;
            }
            commands.push_back(std::move(cmd));
        };
        if (!gpu.labelRanges.empty()) {
            int mergedOrder = 0;
            int mergedStyleGroup = 0;
            int mergedOffset = 0;
            int mergedCount = 0;
            auto flushLabelRange = [&]() {
                if (mergedCount <= 0) return;
                appendLabel(mergedOrder, mergedStyleGroup, mergedOffset,
                            mergedCount);
                mergedCount = 0;
            };
            for (const auto& range : gpu.labelRanges) {
                if (gateZoom < static_cast<double>(range.minZoom) ||
                    gateZoom >= static_cast<double>(range.maxZoom)) {
                    flushLabelRange();
                    continue;
                }
                if (mergedCount > 0 && mergedOrder == range.paintOrder &&
                    mergedStyleGroup == range.styleGroup &&
                    mergedOffset + mergedCount == range.indexOffset) {
                    mergedCount += range.indexCount;
                } else {
                    flushLabelRange();
                    mergedOrder = range.paintOrder;
                    mergedStyleGroup = range.styleGroup;
                    mergedOffset = range.indexOffset;
                    mergedCount = range.indexCount;
                }
            }
            flushLabelRange();
        }
    }
}

// ============================================================
// 编辑预览通道
// ============================================================

bool FeatureRenderLayer::beginEditPreview(FeatureId id) {
    if (!renderDevice_) return false;
    if (style_.usesOfficialProviderContract()) return false;
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
    Vec3 worldPosition = Vec3::zero();
    Cartographic renderedPosition;
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
    if (officialProfileSealed_ || !visible_ || !frameState.camera ||
        store_.empty()) return result;

    const Camera& cam = *frameState.camera;
    const double vpW = static_cast<double>(frameState.viewportWidthPixels);
    const double vpH = static_cast<double>(frameState.viewportHeightPixels);
    if (vpW <= 0.0 || vpH <= 0.0) return result;
    const double cameraHeight =
        ellipsoid_.cartesianToCartographic(cam.position()).height();
    const double viewZoom = std::min(
        24.0,
        std::max(0.0, std::log2(
            4.0e7 / std::max(1.0, cameraHeight))));
    if (viewZoom < style_.minZoom || viewZoom > style_.maxZoom) {
        return result;
    }

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
        const Cartographic rendered(c.longitude(), c.latitude(), h);
        const Vec3 ecef = ellipsoid_.cartographicToCartesian(rendered);
        const glm::dvec4 clip =
            viewProj * glm::dvec4(ecef.x(), ecef.y(), ecef.z(), 1.0);
        if (clip.w <= 0.0) return sv;
        sv.x = (clip.x / clip.w + 1.0) * 0.5 * vpW;
        sv.y = (1.0 - clip.y / clip.w) * 0.5 * vpH;
        sv.valid = true;
        sv.worldPosition = ecef;
        sv.renderedPosition = rendered;
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
                    bestVertex.renderedPosition = ring[i].renderedPosition;
                    bestVertex.worldPosition = ring[i].worldPosition;
                    bestVertex.distanceMeters =
                        ring[i].worldPosition.distanceTo(cam.position());
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
                    const Cartographic& renderedA =
                        ring[i].renderedPosition;
                    const Cartographic& renderedB =
                        ring[j].renderedPosition;
                    bestEdge.renderedPosition = Cartographic(
                        renderedA.longitude() +
                            (renderedB.longitude() - renderedA.longitude()) * t,
                        renderedA.latitude() +
                            (renderedB.latitude() - renderedA.latitude()) * t,
                        renderedA.height() +
                            (renderedB.height() - renderedA.height()) * t);
                    // Interpolating endpoint ECEF positions follows the chord
                    // through the ellipsoid. For long surface lines that can
                    // place the representative hit hundreds of meters below
                    // terrain and make the feature lose unified depth sorting.
                    bestEdge.worldPosition =
                        ellipsoid_.cartographicToCartesian(
                            bestEdge.renderedPosition);
                    bestEdge.distanceMeters =
                        bestEdge.worldPosition.distanceTo(cam.position());
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
            bestFill.renderedPosition = hitCarto;
            bestFill.worldPosition = hitEcef;
            bestFill.distanceMeters = hitEcef.distanceTo(cam.position());
        }
    }

    if (bestFill.isValid() && !clampMode) {
        // Screen containment establishes one winning fill candidate. Resolve
        // only that feature against the same absolute-height triangle mesh as
        // rendering, avoiding per-overlap re-tessellation during a click.
        if (const Feature* feature = store_.getFeature(bestFill.featureId)) {
            const TessellatedFill fill = PolygonTessellator::tessellate(
                *feature, ellipsoid_, style_.heightOffset, nullptr,
                style_.globeFillMaxEdgeMeters);
            double nearestT = std::numeric_limits<double>::infinity();
            for (size_t i = 0; i + 2 < fill.fillIndices.size(); i += 3) {
                const uint32_t ia = fill.fillIndices[i];
                const uint32_t ib = fill.fillIndices[i + 1];
                const uint32_t ic = fill.fillIndices[i + 2];
                if (ia >= fill.positions.size() ||
                    ib >= fill.positions.size() ||
                    ic >= fill.positions.size()) {
                    continue;
                }
                const auto triangleT =
                    IntersectionTests::rayTriangleParametric(
                        ray, fill.positions[ia], fill.positions[ib],
                        fill.positions[ic], false);
                if (triangleT && *triangleT >= 0.0 &&
                    *triangleT < nearestT) {
                    nearestT = *triangleT;
                }
            }
            if (std::isfinite(nearestT)) {
                bestFill.worldPosition = ray.pointAt(nearestT);
                bestFill.renderedPosition =
                    ellipsoid_.cartesianToCartographic(
                        bestFill.worldPosition);
                bestFill.distanceMeters =
                    bestFill.worldPosition.distanceTo(cam.position());
            }
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
