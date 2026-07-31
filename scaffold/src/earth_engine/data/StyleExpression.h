#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

/// 样式表达式的值:数值 或 RGBA 颜色(分量 [0,1])。字符串只作 match 的
/// 分支标签与 get 的属性原文,不作为值在树里流动(避免弱类型泥潭)。
class StyleValue {
public:
    enum class Kind { Number, Color };

    StyleValue() : kind_(Kind::Number), number_(0.0) {}
    explicit StyleValue(double n) : kind_(Kind::Number), number_(n) {}
    explicit StyleValue(const std::array<float, 4>& c)
        : kind_(Kind::Color), color_(c) {}

    Kind kind() const { return kind_; }
    double number() const { return number_; }
    const std::array<float, 4>& color() const { return color_; }

private:
    Kind kind_;
    double number_ = 0.0;
    std::array<float, 4> color_{0, 0, 0, 1};
};

/// 数据驱动样式表达式(矢量 P6b,设计 §12 MapLibre style-spec 子集)。
///
/// 不可变表达式树,C++ builder API 构造(JSON 样式表解析属 MVT/样式表
/// 阶段,后置)。求值上下文 = 要素属性表(可空)+ zoom(可 NaN):
/// - literal(v)                     字面量(数值/颜色)
/// - get(prop)                      属性数值化(strtod;解析失败 = 求值失败)
/// - zoom()                         相机 zoom 标量
/// - match(prop, cases, fallback)   属性字符串等值分支(原文比较)
/// - interpolateLinear(input, stops) 分段线性插值(数值/颜色 stop 皆可,
///                                  stop 输出类型须一致;input 须为数值)
///
/// 求值失败(缺属性/类型不符/上下文缺 zoom)返回 nullopt,调用方回落
/// 字面量样式 —— 表达式永不让渲染断链。
///
/// 语义分割约定(FeatureRenderLayer 接线,setStyle 校验):颜色表达式 =
/// 数据驱动(烘进顶点色,禁 zoom);宽度/尺寸表达式 = zoom 驱动(每帧进
/// uniform,禁 properties)。referencesProperties/referencesZoom 供校验。
class StyleExpression {
public:
    using Ptr = std::shared_ptr<const StyleExpression>;
    using PropertyMap = std::unordered_map<std::string, std::string>;

    static Ptr literal(double n);
    static Ptr literal(const std::array<float, 4>& color);
    static Ptr get(std::string property);
    static Ptr zoom();
    static Ptr match(std::string property,
                     std::vector<std::pair<std::string, Ptr>> cases,
                     Ptr fallback);
    static Ptr interpolateLinear(Ptr input,
                                 std::vector<std::pair<double, Ptr>> stops);

    /// @param properties 要素属性(nullptr = 无属性上下文)
    /// @param zoomLevel  相机 zoom(NaN = 无 zoom 上下文)
    std::optional<StyleValue> evaluate(const PropertyMap* properties,
                                       double zoomLevel) const;

    bool referencesProperties() const;
    bool referencesZoom() const;

private:
    enum class Op { Literal, Get, Zoom, Match, InterpolateLinear };

    StyleExpression() = default;

    Op op_ = Op::Literal;
    StyleValue literal_;
    std::string property_;                                // Get / Match
    std::vector<std::pair<std::string, Ptr>> cases_;      // Match
    Ptr fallback_;                                        // Match
    Ptr input_;                                           // Interpolate
    std::vector<std::pair<double, Ptr>> stops_;           // Interpolate
};

} // namespace earth_engine
