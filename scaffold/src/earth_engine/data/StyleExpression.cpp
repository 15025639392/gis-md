#include "StyleExpression.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace earth_engine {

namespace {

/// 两值线性插值(类型须一致;不一致返回 nullopt)。
std::optional<StyleValue> lerpValues(const StyleValue& a,
                                     const StyleValue& b,
                                     double t) {
    if (a.kind() != b.kind()) return std::nullopt;
    if (a.kind() == StyleValue::Kind::String) return std::nullopt;  // 不可插值
    if (a.kind() == StyleValue::Kind::Number) {
        return StyleValue(a.number() + (b.number() - a.number()) * t);
    }
    std::array<float, 4> c;
    for (int i = 0; i < 4; ++i) {
        c[i] = static_cast<float>(
            a.color()[i] + (b.color()[i] - a.color()[i]) * t);
    }
    return StyleValue(c);
}

} // namespace

StyleExpression::Ptr StyleExpression::literal(double n) {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::Literal;
    e->literal_ = StyleValue(n);
    return e;
}

StyleExpression::Ptr StyleExpression::literal(
    const std::array<float, 4>& color) {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::Literal;
    e->literal_ = StyleValue(color);
    return e;
}

StyleExpression::Ptr StyleExpression::literalString(std::string s) {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::Literal;
    e->literal_ = StyleValue(std::move(s));
    return e;
}

StyleExpression::Ptr StyleExpression::get(std::string property) {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::Get;
    e->property_ = std::move(property);
    return e;
}

StyleExpression::Ptr StyleExpression::zoom() {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::Zoom;
    return e;
}

StyleExpression::Ptr StyleExpression::discreteZoom(double ceilFraction) {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::DiscreteZoom;
    e->parameter_ = std::clamp(ceilFraction, 0.0, 1.0);
    return e;
}

StyleExpression::Ptr StyleExpression::match(
    std::string property,
    std::vector<std::pair<std::string, Ptr>> cases,
    Ptr fallback) {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::Match;
    e->property_ = std::move(property);
    e->cases_ = std::move(cases);
    e->fallback_ = std::move(fallback);
    return e;
}

StyleExpression::Ptr StyleExpression::interpolateLinear(
    Ptr input, std::vector<std::pair<double, Ptr>> stops) {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::InterpolateLinear;
    e->input_ = std::move(input);
    e->stops_ = std::move(stops);
    return e;
}

StyleExpression::Ptr StyleExpression::step(
    Ptr input, std::vector<std::pair<double, Ptr>> stops) {
    auto e = std::shared_ptr<StyleExpression>(new StyleExpression());
    e->op_ = Op::Step;
    e->input_ = std::move(input);
    e->stops_ = std::move(stops);
    return e;
}

std::optional<StyleValue> StyleExpression::evaluate(
    const PropertyMap* properties, double zoomLevel) const {
    switch (op_) {
        case Op::Literal:
            return literal_;

        case Op::Get: {
            if (!properties) return std::nullopt;
            const auto it = properties->find(property_);
            if (it == properties->end()) return std::nullopt;
            char* end = nullptr;
            const double v = std::strtod(it->second.c_str(), &end);
            if (end == it->second.c_str()) return std::nullopt;  // 非数值
            return StyleValue(v);
        }

        case Op::Zoom:
            if (std::isnan(zoomLevel)) return std::nullopt;
            return StyleValue(zoomLevel);

        case Op::DiscreteZoom: {
            if (!std::isfinite(zoomLevel)) return std::nullopt;
            const double lo = std::floor(zoomLevel);
            // Decimal camera thresholds such as 0.8 are not exactly
            // representable in binary. Without a small scale-aware tolerance,
            // 3.80 and 9.80 can fall on opposite sides of the same threshold.
            // Stabilize only representation noise; values meaningfully below
            // the configured fraction still select floor.
            const double fraction = zoomLevel - lo;
            const double tolerance =
                8.0 * std::numeric_limits<double>::epsilon() *
                std::max({1.0, std::abs(zoomLevel), std::abs(parameter_)});
            return StyleValue(fraction + tolerance < parameter_
                                  ? lo
                                  : std::ceil(zoomLevel));
        }

        case Op::Match: {
            if (!properties) {
                return fallback_ ? fallback_->evaluate(properties, zoomLevel)
                                 : std::nullopt;
            }
            const auto it = properties->find(property_);
            if (it != properties->end()) {
                for (const auto& [label, expr] : cases_) {
                    if (label == it->second) {
                        return expr ? expr->evaluate(properties, zoomLevel)
                                    : std::nullopt;
                    }
                }
            }
            return fallback_ ? fallback_->evaluate(properties, zoomLevel)
                             : std::nullopt;
        }

        case Op::InterpolateLinear: {
            if (!input_ || stops_.empty()) return std::nullopt;
            const auto in = input_->evaluate(properties, zoomLevel);
            if (!in || in->kind() != StyleValue::Kind::Number) {
                return std::nullopt;
            }
            const double x = in->number();
            // 越界钳制到端点 stop(MapLibre 同款语义)。
            if (x <= stops_.front().first) {
                return stops_.front().second
                    ? stops_.front().second->evaluate(properties, zoomLevel)
                    : std::nullopt;
            }
            if (x >= stops_.back().first) {
                return stops_.back().second
                    ? stops_.back().second->evaluate(properties, zoomLevel)
                    : std::nullopt;
            }
            for (size_t i = 1; i < stops_.size(); ++i) {
                if (x <= stops_[i].first) {
                    const auto lo = stops_[i - 1].second
                        ? stops_[i - 1].second->evaluate(properties,
                                                         zoomLevel)
                        : std::nullopt;
                    const auto hi = stops_[i].second
                        ? stops_[i].second->evaluate(properties, zoomLevel)
                        : std::nullopt;
                    if (!lo || !hi) return std::nullopt;
                    const double span =
                        stops_[i].first - stops_[i - 1].first;
                    const double t =
                        span > 0.0 ? (x - stops_[i - 1].first) / span : 0.0;
                    return lerpValues(*lo, *hi, t);
                }
            }
            return std::nullopt;  // 不可达
        }
        case Op::Step: {
            if (!input_ || stops_.empty()) return std::nullopt;
            const auto in = input_->evaluate(properties, zoomLevel);
            if (!in || in->kind() != StyleValue::Kind::Number) {
                return std::nullopt;
            }
            const double x = in->number();
            size_t selected = 0;
            for (size_t i = 1; i < stops_.size(); ++i) {
                if (x < stops_[i].first) break;
                selected = i;
            }
            return stops_[selected].second
                ? stops_[selected].second->evaluate(properties, zoomLevel)
                : std::nullopt;
        }
    }
    return std::nullopt;
}

bool StyleExpression::referencesProperties() const {
    switch (op_) {
        case Op::Literal:
            return false;
        case Op::Get:
        case Op::Match:
            // Match 的分支/兜底即使全字面量,分支选择本身依赖属性。
            return true;
        case Op::Zoom:
        case Op::DiscreteZoom:
            return false;
        case Op::InterpolateLinear:
        case Op::Step:
            if (input_ && input_->referencesProperties()) return true;
            for (const auto& [stop, expr] : stops_) {
                if (expr && expr->referencesProperties()) return true;
            }
            return false;
    }
    return false;
}

bool StyleExpression::referencesZoom() const {
    switch (op_) {
        case Op::Literal:
        case Op::Get:
            return false;
        case Op::Zoom:
        case Op::DiscreteZoom:
            return true;
        case Op::Match: {
            for (const auto& [label, expr] : cases_) {
                if (expr && expr->referencesZoom()) return true;
            }
            return fallback_ && fallback_->referencesZoom();
        }
        case Op::InterpolateLinear:
        case Op::Step:
            if (input_ && input_->referencesZoom()) return true;
            for (const auto& [stop, expr] : stops_) {
                if (expr && expr->referencesZoom()) return true;
            }
            return false;
    }
    return false;
}

} // namespace earth_engine
