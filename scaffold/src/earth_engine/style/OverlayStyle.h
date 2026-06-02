#pragma once

#include <array>
#include <string>
#include <vector>
#include <variant>
#include <cstdint>

namespace earth_engine {

// ============================================================
// 基础类型
// ============================================================

/// RGBA 颜色（0.0–1.0）
struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    static Color fromHex(uint32_t hex) {
        return {
            static_cast<float>((hex >> 24) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f
        };
    }

    /// 预置颜色
    static Color red()    { return {1.0f, 0.0f, 0.0f, 1.0f}; }
    static Color green()  { return {0.0f, 1.0f, 0.0f, 1.0f}; }
    static Color blue()   { return {0.0f, 0.0f, 1.0f, 1.0f}; }
    static Color yellow() { return {1.0f, 1.0f, 0.0f, 1.0f}; }
    static Color cyan()   { return {0.0f, 1.0f, 1.0f, 1.0f}; }
    static Color white()  { return {1.0f, 1.0f, 1.0f, 1.0f}; }
    static Color black()  { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    std::array<float, 4> toArray() const { return {{r, g, b, a}}; }
};

// ============================================================
// 高度模式
// ============================================================

enum class AltitudeMode {
    ClampToGround,     // 贴椭球表面
    RelativeToGround,  // 相对椭球表面高度
    Absolute            // 绝对椭球高（ECEF）
};

// ============================================================
// 几何样式
// ============================================================

/// 点样式
struct PointStyle {
    enum class Shape { Circle, Square, Diamond };

    Shape shape = Shape::Circle;
    float size = 6.0f;            // 屏幕像素
    Color color = Color::red();
    Color strokeColor = Color::white();
    float strokeWidth = 1.0f;
    bool depthTest = true;
    bool billboard = true;        // 始终朝向相机
};

/// 线样式
struct LineStyle {
    float width = 2.0f;           // 屏幕像素
    Color color = Color::yellow();
    float opacity = 1.0f;
    std::vector<float> dashArray; // 空 = 实线
    enum class Cap { Butt, Round, Square } cap = Cap::Round;
    enum class Join { Miter, Round, Bevel } join = Join::Round;
};

/// 面样式
struct PolygonStyle {
    Color fillColor = {0.0f, 0.5f, 1.0f, 0.3f};
    float fillOpacity = 1.0f;
    Color outlineColor = {0.0f, 0.4f, 0.8f, 1.0f};
    float outlineWidth = 1.5f;
    float outlineOpacity = 1.0f;
};

/// 几何样式变体
using GeometryStyle = std::variant<PointStyle, LineStyle, PolygonStyle>;

// ============================================================
// 图层样式
// ============================================================

struct LayerStyle {
    float opacity = 1.0f;
    int minZoom = 0;
    int maxZoom = 20;
    int renderOrder = 0;
    AltitudeMode altitudeMode = AltitudeMode::ClampToGround;
    GeometryStyle geometry;  // 默认几何样式
};

// ============================================================
// 交互状态样式
// ============================================================

/// 交互状态下的样式覆盖
struct InteractionStyleOverride {
    std::string state;                 // "hover" | "selected" | "highlighted" | "editing"
    Color colorShift = {0,0,0,0};      // 颜色偏移（rgb 叠加，a 保持）
    float scaleMultiplier = 1.0f;      // 尺寸缩放（点 size × scale）
    float outlineWidthAdd = 0.0f;      // 描边加粗
    Color outlineColor = {1.0f, 1.0f, 0.0f, 1.0f};  // 黄色描边（hover 默认）

    InteractionStyleOverride() = default;
    InteractionStyleOverride(std::string s, Color cs,
                             float sm = 1.0f, float owa = 0.0f,
                             Color oc = {1.0f, 1.0f, 0.0f, 1.0f})
        : state(std::move(s)), colorShift(cs), scaleMultiplier(sm),
          outlineWidthAdd(owa), outlineColor(oc) {}
};

/// 交互样式集合
struct InteractionStyle {
    std::vector<InteractionStyleOverride> states;
    InteractionStyleOverride normalOverride;  // 默认状态

    /// 查找指定状态的覆盖
    const InteractionStyleOverride* find(const std::string& stateName) const;
};

// ============================================================
// OverlayStyle（组合所有层级）
// ============================================================

struct OverlayStyle {
    std::string version = "1.0";
    LayerStyle layer;
    InteractionStyle interaction;
};

// ============================================================
// 便捷构造函数
// ============================================================

/// 创建默认点图层样式
inline OverlayStyle makeDefaultPointStyle() {
    OverlayStyle s;
    s.layer.geometry = PointStyle{};
    s.interaction.states = {
        InteractionStyleOverride("hover",  {0.2f, 0.2f, 0.0f, 0.0f}, 1.5f, 1.0f),
        InteractionStyleOverride("selected", {0.0f, 0.4f, 0.4f, 0.0f}, 1.3f, 2.0f, Color::cyan()),
    };
    return s;
}

/// 创建默认线图层样式
inline OverlayStyle makeDefaultLineStyle() {
    OverlayStyle s;
    s.layer.geometry = LineStyle{};
    s.interaction.states = {
        InteractionStyleOverride("hover",  {0.3f, 0.3f, 0.0f, 0.0f}, 1.0f, 2.0f),
        InteractionStyleOverride("selected", {0.0f, 0.4f, 0.4f, 0.0f}, 1.0f, 3.0f, Color::cyan()),
    };
    return s;
}

/// 创建默认面图层样式
inline OverlayStyle makeDefaultPolygonStyle() {
    OverlayStyle s;
    s.layer.geometry = PolygonStyle{};
    s.interaction.states = {
        InteractionStyleOverride("hover",  {0.1f, 0.1f, 0.0f, 0.0f}, 1.0f, 1.0f),
        InteractionStyleOverride("selected", {0.0f, 0.2f, 0.2f, 0.0f}, 1.0f, 2.0f, Color::cyan()),
    };
    return s;
}

} // namespace earth_engine
