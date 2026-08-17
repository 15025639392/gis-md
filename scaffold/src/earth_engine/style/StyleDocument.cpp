#include "StyleDocument.h"

#include <algorithm>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace earth_engine {

using json = nlohmann::json;

namespace {

// ============ A 案精简 schema:每 type 的 paint 契约表 ============
// 表示能力契约的 schema 半面(编译期语义半面在 compileStyleDocument):
// 未列出的 paint 键一律 fail-loud。特例键给"为什么不支持+出路"的人话
// (设计文档 §4.1 点名的 line-dasharray 即在此)。
struct PaintContract {
    const char* type;
    std::vector<const char*> allowed;
};
const PaintContract kPaintContracts[] = {
    {"fill", {"fill-color"}},
    {"line", {"line-color", "line-width-ramp"}},
    // 三期 symbol 子集:点符号 + 标注(FeatureRenderStyle 对应字段)。
    // 值可为字面量或表达式对象(match/interpolate,见 parseStyleValueExpr)。
    {"symbol",
     {"point-color", "point-size", "point-image", "point-anchor",
      "label-property", "label-color", "label-halo-color", "label-size",
      "label-offset", "label-halo"}},
};

/// 已知但表示画不出的键 → 定制错误(不是"未知键"而是"知道你要什么,
/// 但当前表示给不了,出路在哪")。
const char* unsupportedPaintHint(const std::string& type,
                                 const std::string& key) {
    if (type == "line" && key == "line-dasharray") {
        return "D2 场编码无沿线弧长通道,同语义 dash 画不出(northstar V9);"
               "段内相位 dash 未实现,同语义需换几何表示(E 案)";
    }
    if (type == "line" && key == "line-color-expr") {
        return "场是单色 uniform(D2 无分类通道,northstar V26 差距#3);"
               "多色路网需扩通道或加平面,样式层无法兜";
    }
    return nullptr;
}

/// symbol 型的 layer 级键限制:层过滤(SourceLayerRule)归 MvtVectorSource
/// 的 C++ 配置(三期范围裁定,牵动 source 侧失效面),文档给要报清楚。
const char* unsupportedSymbolLayerHint(const std::string& key) {
    if (key == "filter" || key == "minzoom" || key == "maxzoom" ||
        key == "source-layer") {
        return "symbol 层的源图层/过滤/zoom 档归 MvtVectorSource 的"
               " includeLayers/layerRules(C++ 配置)——文档只管样式;"
               "过滤 JSON 化牵动 source 侧失效面,三期未纳入";
    }
    return nullptr;
}

void addError(std::vector<StyleError>& errors, std::string where,
              std::string message) {
    errors.push_back({std::move(where), std::move(message)});
}

/// "#rrggbb" / "#rrggbbaa" → RGBA8。失败返回 nullopt。
std::optional<std::array<uint8_t, 4>> parseHexColor(const std::string& s) {
    if (s.empty() || s[0] != '#') return std::nullopt;
    const size_t n = s.size() - 1;
    if (n != 6 && n != 8) return std::nullopt;
    std::array<uint8_t, 4> c{0, 0, 0, 255};
    for (size_t i = 0; i < n / 2; ++i) {
        unsigned v = 0;
        if (std::sscanf(s.c_str() + 1 + i * 2, "%2x", &v) != 1)
            return std::nullopt;
        c[i] = static_cast<uint8_t>(v);
    }
    return c;
}

std::array<float, 4> toFloatColor(const std::array<uint8_t, 4>& c) {
    return {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f};
}

std::optional<StyleFilter::Compare> parseCompareOp(const std::string& op) {
    if (op == "==") return StyleFilter::Compare::Equal;
    if (op == "!=") return StyleFilter::Compare::NotEqual;
    if (op == "<") return StyleFilter::Compare::Less;
    if (op == "<=") return StyleFilter::Compare::LessEqual;
    if (op == ">") return StyleFilter::Compare::Greater;
    if (op == ">=") return StyleFilter::Compare::GreaterEqual;
    return std::nullopt;
}

/// filter 对象递归解析。对象风格(见 StyleDocument.h 头注释),每节点恰一个
/// 识别键。maxZoomSeen:途中见到的最大 zoom 字面量(fieldMaxZoom 推导用)。
StyleFilter::Ptr parseFilter(const json& node, const std::string& where,
                             std::vector<StyleError>& errors,
                             int& maxZoomSeen) {
    if (!node.is_object() || node.size() != 1) {
        addError(errors, where,
                 "filter 节点须是恰含一个键的对象"
                 "(all/any/not/has/!has/in/zoom/== != < <= > >=)");
        return nullptr;
    }
    const std::string key = node.begin().key();
    const json& v = node.begin().value();

    if (key == "all" || key == "any") {
        if (!v.is_array()) {
            addError(errors, where + "." + key, "须是 filter 数组");
            return nullptr;
        }
        std::vector<StyleFilter::Ptr> children;
        for (size_t i = 0; i < v.size(); ++i) {
            auto c = parseFilter(v[i], where + "." + key + "[" +
                                 std::to_string(i) + "]", errors, maxZoomSeen);
            if (c) children.push_back(std::move(c));
        }
        return key == "all" ? StyleFilter::all(std::move(children))
                            : StyleFilter::any(std::move(children));
    }
    if (key == "not") {
        auto c = parseFilter(v, where + ".not", errors, maxZoomSeen);
        return c ? StyleFilter::negate(std::move(c)) : nullptr;
    }
    if (key == "has" || key == "!has") {
        if (!v.is_string()) {
            addError(errors, where + "." + key, "须是属性名字符串");
            return nullptr;
        }
        return key == "has" ? StyleFilter::has(v.get<std::string>())
                            : StyleFilter::notHas(v.get<std::string>());
    }
    if (key == "in") {
        if (!v.is_object() || !v.contains("property") ||
            !v.contains("values") || !v["values"].is_array()) {
            addError(errors, where + ".in",
                     "须是 {\"property\": p, \"values\": [s...]}");
            return nullptr;
        }
        std::vector<std::string> values;
        for (const auto& s : v["values"]) {
            if (!s.is_string()) {
                addError(errors, where + ".in.values", "值须全为字符串");
                return nullptr;
            }
            values.push_back(s.get<std::string>());
        }
        return StyleFilter::in(v["property"].get<std::string>(),
                               std::move(values));
    }
    if (key == "zoom") {
        // ⚠️ zoom 三义:此处语义随消费路 —— drape/场的 styleZoom 是**页 z**
        // (比地图直觉高 ~2-3 档),不是瓦片 z 也不是相机 z。写档位前先读
        // mvt-vector-architecture.md §4.2。
        if (!v.is_object() || !v.contains("op") || !v.contains("value") ||
            !v["value"].is_number()) {
            addError(errors, where + ".zoom",
                     "须是 {\"op\": 比较符, \"value\": 数}(value 为页 zoom)");
            return nullptr;
        }
        auto op = parseCompareOp(v["op"].get<std::string>());
        if (!op) {
            addError(errors, where + ".zoom.op", "未知比较符");
            return nullptr;
        }
        const double z = v["value"].get<double>();
        maxZoomSeen = std::max(maxZoomSeen, static_cast<int>(z));
        return StyleFilter::zoomCompare(*op, z);
    }
    if (auto op = parseCompareOp(key)) {
        if (!v.is_object() || !v.contains("property") || !v.contains("value")) {
            addError(errors, where + "." + key,
                     "须是 {\"property\": p, \"value\": 字符串或数}");
            return nullptr;
        }
        const json& val = v["value"];
        if (val.is_string()) {
            return StyleFilter::compare(v["property"].get<std::string>(), *op,
                                        val.get<std::string>());
        }
        if (val.is_number()) {
            return StyleFilter::compare(v["property"].get<std::string>(), *op,
                                        val.get<double>());
        }
        addError(errors, where + "." + key + ".value", "须是字符串或数");
        return nullptr;
    }
    addError(errors, where, "未知 filter 键 \"" + key + "\"");
    return nullptr;
}

// ===== 三期:StyleExpression 的 JSON 对象形态 =====
// 值域按**目标属性**定(色/数/字符串),解析器带 kind —— 色属性收 hex 串、
// 尺寸属性收数、图形属性收名字,类型错位在 parse 期报,不进求值期。
// 形态:
//   字面量:      "#rrggbbaa" / 26 / "star"
//   match:       {"match": {"property": p, "cases": {值键: 值...},
//                           "default": 值}}(数据驱动,禁 zoom —— 由
//                 FeatureRenderLayer::setStyle 语义校验兜底)
//   interpolate: {"interpolate": {"input": "zoom", "stops": [[z, 值]...]}}
enum class ValueKind { Color, Number, String };

StyleExpression::Ptr parseLeafValue(const json& v, ValueKind kind,
                                    const std::string& where,
                                    std::vector<StyleError>& errors) {
    switch (kind) {
        case ValueKind::Color: {
            auto c = v.is_string() ? parseHexColor(v.get<std::string>())
                                   : std::nullopt;
            if (!c) {
                addError(errors, where, "色值须是 \"#rrggbb(aa)\"");
                return nullptr;
            }
            return StyleExpression::literal(toFloatColor(*c));
        }
        case ValueKind::Number:
            if (!v.is_number()) {
                addError(errors, where, "须是数");
                return nullptr;
            }
            return StyleExpression::literal(v.get<double>());
        case ValueKind::String:
            if (!v.is_string()) {
                addError(errors, where, "须是字符串");
                return nullptr;
            }
            return StyleExpression::literalString(v.get<std::string>());
    }
    return nullptr;
}

/// 值或表达式。返回 {expr, isLiteral}:isLiteral=true 时调用方可选择落
/// 字面量字段(省一层求值);表达式恒落 *Expr 字段。
struct ParsedValue {
    StyleExpression::Ptr expr;
    bool isLiteral = false;
};

ParsedValue parseStyleValueExpr(const json& v, ValueKind kind,
                                const std::string& where,
                                std::vector<StyleError>& errors) {
    if (!v.is_object()) {
        return {parseLeafValue(v, kind, where, errors), true};
    }
    if (v.size() != 1) {
        addError(errors, where, "表达式对象须恰含一个键(match/interpolate)");
        return {nullptr, false};
    }
    const std::string key = v.begin().key();
    const json& body = v.begin().value();
    if (key == "match") {
        if (!body.is_object() || !body.contains("property") ||
            !body.contains("cases") || !body["cases"].is_object() ||
            !body.contains("default")) {
            addError(errors, where + ".match",
                     "须是 {\"property\": p, \"cases\": {…}, \"default\": 值}");
            return {nullptr, false};
        }
        std::vector<std::pair<std::string, StyleExpression::Ptr>> cases;
        for (const auto& [ck, cv] : body["cases"].items()) {
            auto leaf = parseLeafValue(cv, kind,
                                       where + ".match.cases." + ck, errors);
            if (leaf) cases.emplace_back(ck, std::move(leaf));
        }
        auto fallback = parseLeafValue(body["default"], kind,
                                       where + ".match.default", errors);
        if (!fallback) return {nullptr, false};
        return {StyleExpression::match(body["property"].get<std::string>(),
                                       std::move(cases), std::move(fallback)),
                false};
    }
    if (key == "interpolate") {
        if (!body.is_object() || body.value("input", "") != "zoom" ||
            !body.contains("stops") || !body["stops"].is_array()) {
            addError(errors, where + ".interpolate",
                     "须是 {\"input\": \"zoom\", \"stops\": [[z, 值]...]}"
                     "(input 目前仅 zoom —— 数据驱动插值属后置)");
            return {nullptr, false};
        }
        std::vector<std::pair<double, StyleExpression::Ptr>> stops;
        for (size_t i = 0; i < body["stops"].size(); ++i) {
            const json& s = body["stops"][i];
            const std::string sw =
                where + ".interpolate.stops[" + std::to_string(i) + "]";
            if (!s.is_array() || s.size() != 2 || !s[0].is_number()) {
                addError(errors, sw, "须是 [zoom, 值] 二元组");
                continue;
            }
            auto leaf = parseLeafValue(s[1], kind, sw, errors);
            if (leaf) stops.emplace_back(s[0].get<double>(), std::move(leaf));
        }
        if (stops.empty()) {
            addError(errors, where + ".interpolate.stops", "至少一个 stop");
            return {nullptr, false};
        }
        return {StyleExpression::interpolateLinear(StyleExpression::zoom(),
                                                   std::move(stops)),
                false};
    }
    addError(errors, where, "未知表达式键 \"" + key + "\"");
    return {nullptr, false};
}

/// symbol paint → FeatureRenderStyle + set 掩码(字面量落标量字段,表达式
/// 落 *Expr;掩码记录"文档写了哪些",应用侧按掩码合成不洗未写字段)。
void parseSymbolPaint(const json& paint, FeatureRenderStyle& style,
                      StyleDocumentLayer::SymbolMask& mask,
                      const std::string& where,
                      std::vector<StyleError>& errors) {
    for (const auto& [k, v] : paint.items()) {
        const std::string kw = where + "." + k;
        if (k == "point-color") {
            ParsedValue pv = parseStyleValueExpr(v, ValueKind::Color, kw, errors);
            if (!pv.expr) continue;
            mask.pointColor = true;
            if (pv.isLiteral) {
                auto out = pv.expr->evaluate(nullptr, 0.0);
                style.pointColor = out->color();
            } else {
                style.pointColorExpr = pv.expr;
            }
        } else if (k == "point-size") {
            ParsedValue pv = parseStyleValueExpr(v, ValueKind::Number, kw, errors);
            if (!pv.expr) continue;
            mask.pointSize = true;
            if (pv.isLiteral) {
                style.pointSizePx =
                    static_cast<float>(pv.expr->evaluate(nullptr, 0.0)->number());
            } else {
                style.pointSizeExpr = pv.expr;
            }
        } else if (k == "point-image") {
            ParsedValue pv = parseStyleValueExpr(v, ValueKind::String, kw, errors);
            if (!pv.expr) continue;
            mask.pointImage = true;
            if (pv.isLiteral) {
                style.pointImage = pv.expr->evaluate(nullptr, 0.0)->string();
            } else {
                style.pointImageExpr = pv.expr;
            }
        } else if (k == "point-anchor") {
            const std::string a = v.is_string() ? v.get<std::string>() : "";
            if (a == "auto") style.pointAnchor = SymbolAnchor::Auto;
            else if (a == "center") style.pointAnchor = SymbolAnchor::Center;
            else if (a == "bottom") style.pointAnchor = SymbolAnchor::Bottom;
            else { addError(errors, kw, "须是 auto|center|bottom"); continue; }
            mask.pointAnchor = true;
        } else if (k == "label-property") {
            if (!v.is_string()) { addError(errors, kw, "须是属性名字符串"); continue; }
            style.labelProperty = v.get<std::string>();
            mask.labelProperty = true;
        } else if (k == "label-color" || k == "label-halo-color") {
            auto c = v.is_string() ? parseHexColor(v.get<std::string>())
                                   : std::nullopt;
            if (!c) { addError(errors, kw, "色值须是 \"#rrggbb(aa)\""); continue; }
            if (k == "label-color") {
                style.labelColor = toFloatColor(*c);
                mask.labelColor = true;
            } else {
                style.labelHaloColor = toFloatColor(*c);
                mask.labelHaloColor = true;
            }
        } else if (k == "label-size" || k == "label-offset" ||
                   k == "label-halo") {
            if (!v.is_number()) { addError(errors, kw, "须是数(px)"); continue; }
            const float f = v.get<float>();
            if (k == "label-size") { style.labelSizePx = f; mask.labelSize = true; }
            else if (k == "label-offset") { style.labelOffsetPx = f; mask.labelOffset = true; }
            else { style.labelHaloPx = f; mask.labelHalo = true; }
        }
        // 未知键由调用方的契约表统一拦,此处只处理已允许键。
    }
}

}  // namespace

StyleDocument parseStyleDocument(const std::string& jsonText,
                                 std::vector<StyleError>& errors) {
    StyleDocument doc;
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const json::parse_error& e) {
        addError(errors, "$", std::string("JSON 语法错误: ") + e.what());
        return doc;
    }
    if (!root.is_object()) {
        addError(errors, "$", "顶层须是对象");
        return doc;
    }
    doc.version = root.value("version", 1);
    if (doc.version != 1) {
        addError(errors, "$.version", "仅支持 version 1");
    }
    if (root.contains("field-max-zoom")) {
        doc.fieldMaxZoom = root["field-max-zoom"].get<int>();
    }
    // 顶层未知键 fail-loud(A 案无前向兼容负担,拒绝比静默强)。
    for (const auto& [k, v] : root.items()) {
        if (k != "version" && k != "layers" && k != "field-max-zoom") {
            addError(errors, "$." + k, "未知顶层键");
        }
    }
    if (!root.contains("layers") || !root["layers"].is_array()) {
        addError(errors, "$.layers", "须是 layer 数组");
        return doc;
    }

    int derivedFieldMaxZoom = -1;
    for (size_t i = 0; i < root["layers"].size(); ++i) {
        const json& jl = root["layers"][i];
        const std::string where = "layers[" + std::to_string(i) + "]";
        if (!jl.is_object()) {
            addError(errors, where, "layer 须是对象");
            continue;
        }
        StyleDocumentLayer layer;
        layer.id = jl.value("id", "");
        if (layer.id.empty()) {
            addError(errors, where + ".id", "必填(错误定位用)");
        }
        layer.type = jl.value("type", "");
        const PaintContract* contract = nullptr;
        for (const PaintContract& c : kPaintContracts) {
            if (layer.type == c.type) contract = &c;
        }
        if (!contract) {
            addError(errors, where + ".type",
                     "未知 type \"" + layer.type +
                         "\"(二期支持 fill/line;symbol 属三期)");
            continue;
        }
        const bool isSymbol = layer.type == "symbol";
        if (isSymbol) {
            // symbol 型:源图层/过滤/zoom 档归 MvtVectorSource C++ 配置,
            // 文档里出现即报(带出路提示,不是泛化"未知键")。
            for (const char* k : {"source-layer", "filter", "minzoom",
                                  "maxzoom"}) {
                if (jl.contains(k)) {
                    addError(errors, where + "." + k,
                             unsupportedSymbolLayerHint(k));
                }
            }
        } else {
            layer.sourceLayer = jl.value("source-layer", "");
            if (layer.sourceLayer.empty()) {
                addError(errors, where + ".source-layer", "必填(MVT 源图层名)");
            }
            // ⚠️ zoom 三义:min/maxzoom 与 filter 里的 zoom 同为**页 z**。
            layer.minZoom = jl.value("minzoom", 0);
            layer.maxZoom = jl.value("maxzoom", 24);
            int maxZoomSeen = -1;
            if (jl.contains("filter")) {
                layer.filter = parseFilter(jl["filter"], where + ".filter",
                                           errors, maxZoomSeen);
            }
            derivedFieldMaxZoom = std::max(derivedFieldMaxZoom, maxZoomSeen);
        }

        const json paint = jl.value("paint", json::object());
        for (const auto& [k, v] : paint.items()) {
            const bool allowed =
                std::any_of(contract->allowed.begin(), contract->allowed.end(),
                            [&](const char* a) { return k == a; });
            if (!allowed) {
                const char* hint = unsupportedPaintHint(layer.type, k);
                addError(errors, where + ".paint." + k,
                         hint ? hint
                              : "type \"" + layer.type + "\" 不支持该属性");
                continue;
            }
            if (isSymbol) continue;  // symbol 键统一在循环后整体解析
            if (k == "fill-color" || k == "line-color") {
                auto c = v.is_string() ? parseHexColor(v.get<std::string>())
                                       : std::nullopt;
                if (!c) {
                    addError(errors, where + ".paint." + k,
                             "须是 \"#rrggbb\" 或 \"#rrggbbaa\"");
                    continue;
                }
                if (k == "fill-color") layer.fillColor = *c;
                else layer.lineColor = toFloatColor(*c);
            } else if (k == "line-width-ramp") {
                if (!v.is_array() || v.size() != 4 ||
                    !std::all_of(v.begin(), v.end(),
                                 [](const json& x) { return x.is_number(); })) {
                    addError(errors, where + ".paint." + k,
                             "须是 [z0, halfPx0, z1, halfPx1] 四数"
                             "(z 为页 zoom,halfPx 为设备像素线半宽)");
                    continue;
                }
                for (int j = 0; j < 4; ++j)
                    layer.lineWidthRamp[j] = v[j].get<float>();
            }
        }
        // layer 级未知键 fail-loud。
        for (const auto& [k, v] : jl.items()) {
            static const char* kKnown[] = {"id",      "type",   "source-layer",
                                           "minzoom", "maxzoom", "filter",
                                           "paint"};
            if (std::none_of(std::begin(kKnown), std::end(kKnown),
                             [&](const char* a) { return k == a; })) {
                addError(errors, where + "." + k, "未知 layer 键");
            }
        }

        if (isSymbol) {
            parseSymbolPaint(paint, layer.symbol, layer.symbolMask,
                             where + ".paint", errors);
        }

        // Re-bake/Re-tess 指纹:规范化序列化该层的重建相关字段。nlohmann
        // object 按键有序 → dump 即规范形。Uniform 类(line-color/ramp)
        // **刻意不进**指纹;symbol 整个 paint 都是 Re-tess(全桶重镶)。
        json fp;
        if (isSymbol) {
            fp["paint"] = paint;
        } else {
            fp["source-layer"] = layer.sourceLayer;
            fp["minzoom"] = layer.minZoom;
            fp["maxzoom"] = layer.maxZoom;
            if (jl.contains("filter")) fp["filter"] = jl["filter"];
            if (layer.type == "fill") {
                fp["fill-color"] = jl.value("paint", json::object())
                                       .value("fill-color", "");
            }
        }
        layer.rebakeFingerprint = fp.dump();

        doc.layers.push_back(std::move(layer));
    }
    if (doc.fieldMaxZoom < 0) doc.fieldMaxZoom = derivedFieldMaxZoom;
    return doc;
}

CompiledStyle compileStyleDocument(const StyleDocument& doc,
                                   std::vector<StyleError>& errors) {
    CompiledStyle out;
    bool haveFieldUniforms = false;
    for (const StyleDocumentLayer& layer : doc.layers) {
        const std::string where = "layer \"" + layer.id + "\"";
        if (layer.type == "fill") {
            VectorRasterLayerPaint paint;
            paint.layer = layer.sourceLayer;
            paint.filter = layer.filter;
            paint.minZoom = layer.minZoom;
            paint.maxZoom = layer.maxZoom;
            paint.fillColor = layer.fillColor;
            if (paint.fillColor[3] == 0) {
                addError(errors, where,
                         "fill 层缺 paint.fill-color(alpha=0 = 不画,"
                         "空层无意义)");
            }
            out.drapeStyle.layers.push_back(std::move(paint));
            out.drapeFingerprint += layer.rebakeFingerprint;
        } else if (layer.type == "line") {
            VectorRasterLayerPaint paint;
            paint.layer = layer.sourceLayer;
            paint.filter = layer.filter;
            paint.minZoom = layer.minZoom;
            paint.maxZoom = layer.maxZoom;
            // 参与开关(真实线色在 FS uniform,见 VectorRasterStyle 注释)。
            paint.lineColor = {255, 255, 255, 255};
            out.fieldStyle.layers.push_back(std::move(paint));
            out.fieldFingerprint += layer.rebakeFingerprint;
            // 契约:场是单色单 ramp(D2 无分类通道)。多 line 层必须一致。
            if (!haveFieldUniforms) {
                out.fieldLineColor = layer.lineColor;
                out.fieldWidthRamp = layer.lineWidthRamp;
                haveFieldUniforms = true;
                if (layer.lineColor[3] <= 0.0f) {
                    addError(errors, where, "line 层缺 paint.line-color");
                }
                if (layer.lineWidthRamp[1] <= 0.0f) {
                    addError(errors, where,
                             "line 层缺 paint.line-width-ramp(半宽须 >0)");
                }
            } else if (layer.lineColor != out.fieldLineColor ||
                       layer.lineWidthRamp != out.fieldWidthRamp) {
                addError(errors, where,
                         "多 line 层线色/ramp 必须一致 —— 场是单色 uniform"
                         "(D2 无分类通道,northstar V26 差距#3;多色路网"
                         "需扩通道,样式层兜不了)");
            }
        } else if (layer.type == "symbol") {
            if (out.hasSymbol) {
                addError(errors, where,
                         "至多一个 symbol 层(编译目标是单 FeatureRenderLayer;"
                         "多符号层拆分归后续,现在给两个会静默丢一个 —— 拒收)");
                continue;
            }
            out.symbolStyle = layer.symbol;
            out.symbolMask = layer.symbolMask;
            out.hasSymbol = true;
            out.symbolFingerprint = layer.rebakeFingerprint;
        }
        // parser 已挡未知 type,此处不重复。
    }
    out.fieldMaxZoom = doc.fieldMaxZoom;
    if (!out.fieldStyle.layers.empty() && out.fieldMaxZoom < 0) {
        addError(errors, "$",
                 "无法推导 field-max-zoom(line filter 无 zoom 档)——"
                 "显式给 $.field-max-zoom(封小了末梢路整体消失,真机踩过"
                 "封 14,见 MinimalGlobeDemoConfig kMvtRoadFieldMaxZoom)");
    }
    out.fieldFingerprint += "|cap=" + std::to_string(out.fieldMaxZoom);
    return out;
}

StyleApplyPlan planStyleApply(const CompiledStyle* oldStyle,
                              const CompiledStyle& newStyle) {
    StyleApplyPlan plan;
    if (!oldStyle) {
        plan.rebakeDrape = true;
        plan.rebakeField = true;
        plan.retessSymbols = newStyle.hasSymbol;  // 无 symbol 层不许洗默认样式
        return plan;
    }
    plan.rebakeDrape = oldStyle->drapeFingerprint != newStyle.drapeFingerprint;
    plan.rebakeField = oldStyle->fieldFingerprint != newStyle.fieldFingerprint;
    plan.retessSymbols =
        newStyle.hasSymbol &&
        oldStyle->symbolFingerprint != newStyle.symbolFingerprint;
    return plan;
}

FeatureRenderStyle mergeSymbolStyle(
    const FeatureRenderStyle& current, const FeatureRenderStyle& fromDoc,
    const StyleDocumentLayer::SymbolMask& mask) {
    FeatureRenderStyle out = current;  // 未置位字段(含几何语义)全保持现状
    if (mask.pointColor) {
        out.pointColor = fromDoc.pointColor;
        out.pointColorExpr = fromDoc.pointColorExpr;  // 可为空 = literal 压 expr
    }
    if (mask.pointSize) {
        out.pointSizePx = fromDoc.pointSizePx;
        out.pointSizeExpr = fromDoc.pointSizeExpr;
    }
    if (mask.pointImage) {
        out.pointImage = fromDoc.pointImage;
        out.pointImageExpr = fromDoc.pointImageExpr;
    }
    if (mask.pointAnchor) out.pointAnchor = fromDoc.pointAnchor;
    if (mask.labelProperty) out.labelProperty = fromDoc.labelProperty;
    if (mask.labelColor) out.labelColor = fromDoc.labelColor;
    if (mask.labelHaloColor) out.labelHaloColor = fromDoc.labelHaloColor;
    if (mask.labelSize) out.labelSizePx = fromDoc.labelSizePx;
    if (mask.labelOffset) out.labelOffsetPx = fromDoc.labelOffsetPx;
    if (mask.labelHalo) out.labelHaloPx = fromDoc.labelHaloPx;
    return out;
}

} // namespace earth_engine
