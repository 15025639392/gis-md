#include "StyleFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace earth_engine {

namespace {

/// 属性原文 → 数值。整串必须被消费完("12abc" 视为非数值),否则
/// "12abc" < 20 会莫名成立。
std::optional<double> toNumber(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return std::nullopt;
    return v;
}

const std::string* lookup(const StyleFilter::PropertyMap* properties,
                          const std::string& key) {
    if (!properties) return nullptr;
    auto it = properties->find(key);
    return it == properties->end() ? nullptr : &it->second;
}

} // namespace

StyleFilter::Ptr StyleFilter::compare(std::string property, Compare op,
                                      std::string value) {
    auto f = std::shared_ptr<StyleFilter>(new StyleFilter());
    f->op_ = Op::Compare;
    f->property_ = std::move(property);
    f->compare_ = op;
    f->value_ = std::move(value);
    return f;
}

StyleFilter::Ptr StyleFilter::compare(std::string property, Compare op,
                                      double value) {
    // 存字符串而非数值:等值比较按原文语义(与 StyleExpression::match 同
    // 约定),数值比较时再数值化。两条路径共用一个存储,不必分支。
    std::string text = std::to_string(value);
    // 去掉 to_string 的尾随零,免得 == 12 与属性原文 "12" 比不上。
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return compare(std::move(property), op, std::move(text));
}

StyleFilter::Ptr StyleFilter::in(std::string property,
                                 std::vector<std::string> values) {
    auto f = std::shared_ptr<StyleFilter>(new StyleFilter());
    f->op_ = Op::In;
    f->property_ = std::move(property);
    f->values_ = std::move(values);
    return f;
}

StyleFilter::Ptr StyleFilter::zoomCompare(Compare op, double zoom) {
    auto f = std::shared_ptr<StyleFilter>(new StyleFilter());
    f->op_ = Op::ZoomCompare;
    f->compare_ = op;
    f->zoomValue_ = zoom;
    return f;
}

StyleFilter::Ptr StyleFilter::has(std::string property) {
    auto f = std::shared_ptr<StyleFilter>(new StyleFilter());
    f->op_ = Op::Has;
    f->property_ = std::move(property);
    return f;
}

StyleFilter::Ptr StyleFilter::notHas(std::string property) {
    auto f = std::shared_ptr<StyleFilter>(new StyleFilter());
    f->op_ = Op::NotHas;
    f->property_ = std::move(property);
    return f;
}

StyleFilter::Ptr StyleFilter::all(std::vector<Ptr> children) {
    auto f = std::shared_ptr<StyleFilter>(new StyleFilter());
    f->op_ = Op::All;
    f->children_ = std::move(children);
    return f;
}

StyleFilter::Ptr StyleFilter::any(std::vector<Ptr> children) {
    auto f = std::shared_ptr<StyleFilter>(new StyleFilter());
    f->op_ = Op::Any;
    f->children_ = std::move(children);
    return f;
}

StyleFilter::Ptr StyleFilter::negate(Ptr child) {
    auto f = std::shared_ptr<StyleFilter>(new StyleFilter());
    f->op_ = Op::Not;
    f->children_.push_back(std::move(child));
    return f;
}

bool StyleFilter::matches(const PropertyMap* properties,
                          double zoom) const {
    switch (op_) {
        case Op::Compare: {
            const std::string* text = lookup(properties, property_);
            // 属性缺失 → false(不是求值失败)。底图属性天然稀疏,把「没这
            // 个标签」当错误会让整条过滤链退化。NotEqual 同样返回 false:
            // 「没有 highway」不该算作「highway != motorway」成立,否则
            // 排除式过滤会把无关要素全放进来。
            if (!text) return false;
            if (compare_ == Compare::Equal) return *text == value_;
            if (compare_ == Compare::NotEqual) return *text != value_;
            const std::optional<double> lhs = toNumber(*text);
            const std::optional<double> rhs = toNumber(value_);
            if (!lhs || !rhs) return false;
            switch (compare_) {
                case Compare::Less: return *lhs < *rhs;
                case Compare::LessEqual: return *lhs <= *rhs;
                case Compare::Greater: return *lhs > *rhs;
                case Compare::GreaterEqual: return *lhs >= *rhs;
                default: return false;
            }
        }
        case Op::ZoomCompare: {
            if (std::isnan(zoom)) return false;
            switch (compare_) {
                case Compare::Equal: return zoom == zoomValue_;
                case Compare::NotEqual: return zoom != zoomValue_;
                case Compare::Less: return zoom < zoomValue_;
                case Compare::LessEqual: return zoom <= zoomValue_;
                case Compare::Greater: return zoom > zoomValue_;
                case Compare::GreaterEqual: return zoom >= zoomValue_;
            }
            return false;
        }
        case Op::In: {
            const std::string* text = lookup(properties, property_);
            if (!text) return false;
            return std::find(values_.begin(), values_.end(), *text) !=
                   values_.end();
        }
        case Op::Has:
            return lookup(properties, property_) != nullptr;
        case Op::NotHas:
            return lookup(properties, property_) == nullptr;
        case Op::All:
            // 空集 = true(布尔代数惯例);也让「无过滤」可以写成 all({})。
            for (const Ptr& child : children_) {
                if (!child || !child->matches(properties, zoom)) return false;
            }
            return true;
        case Op::Any:
            for (const Ptr& child : children_) {
                if (child && child->matches(properties, zoom)) return true;
            }
            return false;  // 空集 = false
        case Op::Not:
            return children_.empty() || !children_.front() ||
                   !children_.front()->matches(properties, zoom);
    }
    return false;
}

} // namespace earth_engine
