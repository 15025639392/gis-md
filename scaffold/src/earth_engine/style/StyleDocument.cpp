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
        layer.sourceLayer = jl.value("source-layer", "");
        if (layer.sourceLayer.empty()) {
            addError(errors, where + ".source-layer", "必填(MVT 源图层名)");
        }
        // ⚠️ zoom 三义:min/maxzoom 与 filter 里的 zoom 同为**页 z**。
        layer.minZoom = jl.value("minzoom", 0);
        layer.maxZoom = jl.value("maxzoom", 24);
        int maxZoomSeen = -1;
        if (jl.contains("filter")) {
            layer.filter = parseFilter(jl["filter"], where + ".filter", errors,
                                       maxZoomSeen);
        }
        derivedFieldMaxZoom = std::max(derivedFieldMaxZoom, maxZoomSeen);

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

        // Re-bake 指纹:规范化序列化该层的重烘相关字段。nlohmann object 按键
        // 有序 → dump 即规范形。Uniform 类(line-color/ramp)**刻意不进**指纹。
        json fp;
        fp["source-layer"] = layer.sourceLayer;
        fp["minzoom"] = layer.minZoom;
        fp["maxzoom"] = layer.maxZoom;
        if (jl.contains("filter")) fp["filter"] = jl["filter"];
        if (layer.type == "fill") {
            fp["fill-color"] = jl.value("paint", json::object())
                                   .value("fill-color", "");
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
        return plan;
    }
    plan.rebakeDrape = oldStyle->drapeFingerprint != newStyle.drapeFingerprint;
    plan.rebakeField = oldStyle->fieldFingerprint != newStyle.fieldFingerprint;
    return plan;
}

} // namespace earth_engine
