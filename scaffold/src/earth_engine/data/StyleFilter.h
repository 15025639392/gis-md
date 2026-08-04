#pragma once

#include <memory>
#include <string>
#include <vector>

#include "StyleExpression.h"

namespace earth_engine {

/// 要素过滤谓词(矢量 E2,MapLibre style-spec filter 子集)。
///
/// **为什么不并进 StyleExpression**:那棵树的值域是数值/颜色/字符串,且
/// 头文件明确约定「不让弱类型在树里流动」。过滤器的值域是布尔,并进去要么
/// 新增 Kind::Boolean 并让所有既有消费点(镶嵌期取色、每帧取宽)都得处理
/// 一个它们永远用不到的类型,要么用 0/1 数值冒充布尔 —— 正是那条约定要
/// 避免的。分开是两个值域各自封闭,组合处只有一个:属性上下文。
///
/// **求值语义**(对齐 maplibre):
/// - 属性缺失 → 该比较为 false(不是求值失败)。底图数据的属性天然稀疏,
///   把「没有 highway 标签」当错误会让整条过滤链退化。
/// - 数值比较(< <= > >=)要求两侧都能数值化,否则 false。
/// - 等值比较(== !=)按**字符串原文**比,与 StyleExpression::match 同约定
///   (避免 "1" 与 "1.0" 这类数值化后相等、语义上不同的坑)。
/// - all() 空集 = true,any() 空集 = false(布尔代数惯例)。
///
/// 不可变、纯函数、无内部状态 → **可在 worker 线程并发求值**(E1 的瓦片桶
/// 镶嵌就在 worker 上跑)。
class StyleFilter {
public:
    using Ptr = std::shared_ptr<const StyleFilter>;
    using PropertyMap = StyleExpression::PropertyMap;

    enum class Compare { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

    /// 属性与字面量比较。Equal/NotEqual 走字符串原文;其余走数值。
    static Ptr compare(std::string property, Compare op, std::string value);
    /// 数值比较的便利重载(内部转字符串存,求值时数值化)。
    static Ptr compare(std::string property, Compare op, double value);
    /// 属性值 ∈ 集合(字符串原文)。空集恒 false。
    static Ptr in(std::string property, std::vector<std::string> values);
    /// 与**瓦片 zoom** 比较。语义上等价 maplibre filter 里的 ["zoom"] ——
    /// 但求值时机不同:瓦片的 z 是固定的,每块瓦片按自己的 z 镶一次,故
    /// 相机缩放**不触发任何重镶**(换的是画哪些瓦片,不是重算已有瓦片)。
    /// 这是 tippecanoe -j 分级过滤的运行时等价物。
    static Ptr zoomCompare(Compare op, double zoom);
    /// 属性存在 / 不存在。
    static Ptr has(std::string property);
    static Ptr notHas(std::string property);

    static Ptr all(std::vector<Ptr> children);
    static Ptr any(std::vector<Ptr> children);
    static Ptr negate(Ptr child);

    /// @param properties 要素属性(nullptr = 无属性,所有比较取 false)
    /// @param zoom       瓦片 zoom(NaN = 无 zoom 上下文,zoomCompare 取 false)
    bool matches(const PropertyMap* properties, double zoom) const;

private:
    enum class Op { Compare, ZoomCompare, In, Has, NotHas, All, Any, Not };

    StyleFilter() = default;

    Op op_ = Op::All;
    std::string property_;
    Compare compare_ = Compare::Equal;
    std::string value_;
    double zoomValue_ = 0.0;
    std::vector<std::string> values_;
    std::vector<Ptr> children_;
};

/// 源图层级规则(E2):把「哪些层在哪些 zoom 画、层内哪些要素画」从数据
/// 侧搬回样式侧。
///
/// 背景:P4 是用 tippecanoe 的 -j 把道路分级过滤**烘死在瓦片里**(z<9 只留
/// 干线等)。那样换一次分级策略就要重切整套瓦片,而且同一份数据没法给不同
/// 样式复用。maplibre 的做法是 layer minzoom/maxzoom 在建桶前整层跳过、
/// filter 表达式在逐要素时求值 —— 数据只管密度,样式管取舍。
struct SourceLayerRule {
    /// 源图层名(MVT layer name)。
    std::string layer;
    /// 该层生效的**瓦片 zoom** 闭区间。区间外整层跳过(建桶前,零逐要素成本)。
    int minZoom = 0;
    int maxZoom = 24;
    /// 逐要素过滤(空 = 全收)。
    StyleFilter::Ptr filter;
};

} // namespace earth_engine
