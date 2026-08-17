#include <gtest/gtest.h>

#include "earth_engine/style/StyleDocument.h"

#include <string>
#include <vector>

using namespace earth_engine;

namespace {

/// 收集错误路径,断言用。
std::vector<std::string> paths(const std::vector<StyleError>& errors) {
    std::vector<std::string> out;
    for (const auto& e : errors) out.push_back(e.where);
    return out;
}

bool hasErrorAt(const std::vector<StyleError>& errors,
                const std::string& where) {
    for (const auto& e : errors)
        if (e.where == where) return true;
    return false;
}

/// demo 日版路网分级的 JSON 复刻(与 MinimalGlobeDemoConfig 的 builder 版
/// 语义等价;对拍测试的两侧)。截取三档足够覆盖 all/any/in/zoom 组合。
const char* kGradingJson = R"json({
  "any": [
    {"all": [{"zoom": {"op": "<", "value": 10}},
             {"in": {"property": "highway", "values": ["motorway", "trunk"]}}]},
    {"all": [{"zoom": {"op": ">=", "value": 10}},
             {"zoom": {"op": "<", "value": 12}},
             {"in": {"property": "highway",
                     "values": ["motorway", "trunk", "primary"]}}]},
    {"zoom": {"op": ">=", "value": 15}}
  ]
})json";

StyleFilter::Ptr builderGrading() {
    using C = StyleFilter::Compare;
    return StyleFilter::any({
        StyleFilter::all({
            StyleFilter::zoomCompare(C::Less, 10),
            StyleFilter::in("highway", {"motorway", "trunk"})}),
        StyleFilter::all({
            StyleFilter::zoomCompare(C::GreaterEqual, 10),
            StyleFilter::zoomCompare(C::Less, 12),
            StyleFilter::in("highway", {"motorway", "trunk", "primary"})}),
        StyleFilter::zoomCompare(C::GreaterEqual, 15),
    });
}

std::string docWithFilter(const std::string& filterJson) {
    return std::string(R"json({
      "version": 1,
      "layers": [{"id": "roads", "type": "line", "source-layer": "roads",
                  "filter": )json") +
           filterJson + R"json(,
                  "paint": {"line-color": "#f2f2e6d9",
                            "line-width-ramp": [12, 1.05, 16, 3.15]}}]
    })json";
}

}  // namespace

// ---------------- parser:fail-loud ----------------

TEST(StyleDocumentParse, BadJsonReportsSyntaxError) {
    std::vector<StyleError> errors;
    parseStyleDocument("{ not json", errors);
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].where, "$");
}

TEST(StyleDocumentParse, UnknownKeysFailLoudWithPath) {
    std::vector<StyleError> errors;
    parseStyleDocument(R"json({
      "version": 1, "surprise": 1,
      "layers": [{"id": "w", "type": "fill", "source-layer": "water",
                  "opacity": 0.5,
                  "paint": {"fill-color": "#102a5c", "fill-glow": 1}}]
    })json",
                       errors);
    EXPECT_TRUE(hasErrorAt(errors, "$.surprise"));
    EXPECT_TRUE(hasErrorAt(errors, "layers[0].opacity"));
    EXPECT_TRUE(hasErrorAt(errors, "layers[0].paint.fill-glow"));
    // 一次收齐,不首错即停。
    EXPECT_GE(errors.size(), 3u);
}

TEST(StyleDocumentParse, LineDasharrayGetsCapabilityHintNotGenericError) {
    std::vector<StyleError> errors;
    parseStyleDocument(R"json({
      "layers": [{"id": "r", "type": "line", "source-layer": "roads",
                  "paint": {"line-color": "#ffffff",
                            "line-width-ramp": [12, 1, 16, 3],
                            "line-dasharray": [2, 1]}}]
    })json",
                       errors);
    ASSERT_TRUE(hasErrorAt(errors, "layers[0].paint.line-dasharray"));
    for (const auto& e : errors) {
        if (e.where == "layers[0].paint.line-dasharray") {
            EXPECT_NE(e.message.find("D2"), std::string::npos)
                << "画不出的属性要给能力提示(为什么+出路),不是泛化报错";
        }
    }
}

TEST(StyleDocumentParse, UnknownLayerTypeNamesPhase) {
    std::vector<StyleError> errors;
    parseStyleDocument(R"json({
      "layers": [{"id": "poi", "type": "symbol", "source-layer": "poi"}]
    })json",
                       errors);
    ASSERT_TRUE(hasErrorAt(errors, "layers[0].type"));
}

// ---------------- filter:JSON ↔ builder 求值对拍 ----------------

TEST(StyleDocumentParse, FilterJsonEvaluatesIdenticallyToBuilder) {
    std::vector<StyleError> errors;
    StyleDocument doc = parseStyleDocument(docWithFilter(kGradingJson), errors);
    ASSERT_TRUE(errors.empty()) << errors[0].where << ": " << errors[0].message;
    ASSERT_EQ(doc.layers.size(), 1u);
    ASSERT_NE(doc.layers[0].filter, nullptr);

    StyleFilter::Ptr builder = builderGrading();
    const char* highways[] = {"motorway", "trunk", "primary", "secondary",
                              "footway", nullptr};
    for (double zoom : {8.0, 10.0, 11.0, 12.0, 14.0, 15.0, 17.0}) {
        for (const char* hw : highways) {
            StyleExpression::PropertyMap props;
            if (hw) props["highway"] = hw;
            const bool a = doc.layers[0].filter->matches(&props, zoom);
            const bool b = builder->matches(&props, zoom);
            EXPECT_EQ(a, b) << "zoom=" << zoom
                            << " highway=" << (hw ? hw : "(无)");
        }
    }
}

// ---------------- compiler:契约 + 产物 ----------------

TEST(StyleDocumentCompile, FillAndLineRouteToDrapeAndField) {
    std::vector<StyleError> errors;
    StyleDocument doc = parseStyleDocument(R"json({
      "field-max-zoom": 15,
      "layers": [
        {"id": "water", "type": "fill", "source-layer": "water",
         "paint": {"fill-color": "#4080d98c"}},
        {"id": "bldg", "type": "fill", "source-layer": "building",
         "minzoom": 13, "filter": {"has": "name"},
         "paint": {"fill-color": "#99999e8c"}},
        {"id": "roads", "type": "line", "source-layer": "roads",
         "paint": {"line-color": "#f2f2e6d9",
                   "line-width-ramp": [12, 1.05, 16, 3.15]}}
      ]
    })json",
                                           errors);
    ASSERT_TRUE(errors.empty()) << errors[0].where << ": " << errors[0].message;
    CompiledStyle style = compileStyleDocument(doc, errors);
    ASSERT_TRUE(errors.empty()) << errors[0].where << ": " << errors[0].message;

    // 面:两层按序进 drape;色值逐字节。
    ASSERT_EQ(style.drapeStyle.layers.size(), 2u);
    EXPECT_EQ(style.drapeStyle.layers[0].layer, "water");
    EXPECT_EQ(style.drapeStyle.layers[0].fillColor,
              (std::array<uint8_t, 4>{0x40, 0x80, 0xd9, 0x8c}));
    EXPECT_EQ(style.drapeStyle.layers[1].minZoom, 13);
    ASSERT_NE(style.drapeStyle.layers[1].filter, nullptr);
    // 线:分级层进场样式(参与开关 alpha>0),uniform 单独出。
    ASSERT_EQ(style.fieldStyle.layers.size(), 1u);
    EXPECT_GT(style.fieldStyle.layers[0].lineColor[3], 0);
    EXPECT_NEAR(style.fieldLineColor[0], 0xf2 / 255.0f, 1e-4);
    EXPECT_NEAR(style.fieldLineColor[3], 0xd9 / 255.0f, 1e-4);
    EXPECT_EQ(style.fieldWidthRamp,
              (std::array<float, 4>{12.0f, 1.05f, 16.0f, 3.15f}));
    EXPECT_EQ(style.fieldMaxZoom, 15);
}

TEST(StyleDocumentCompile, MultiLineLayerColorMismatchFailsLoud) {
    std::vector<StyleError> errors;
    StyleDocument doc = parseStyleDocument(R"json({
      "field-max-zoom": 15,
      "layers": [
        {"id": "a", "type": "line", "source-layer": "roads",
         "paint": {"line-color": "#ffffff", "line-width-ramp": [12,1,16,3]}},
        {"id": "b", "type": "line", "source-layer": "rails",
         "paint": {"line-color": "#ff0000", "line-width-ramp": [12,1,16,3]}}
      ]
    })json",
                                           errors);
    ASSERT_TRUE(errors.empty());
    compileStyleDocument(doc, errors);
    ASSERT_FALSE(errors.empty()) << "场单色契约必须拦多色";
    EXPECT_NE(errors[0].message.find("单色"), std::string::npos);
}

TEST(StyleDocumentCompile, FieldMaxZoomDerivedFromFilterOrExplicit) {
    // 无显式封顶:从 line filter 的最大 zoom 档推导(=15)。
    std::vector<StyleError> errors;
    StyleDocument doc = parseStyleDocument(docWithFilter(kGradingJson), errors);
    ASSERT_TRUE(errors.empty());
    CompiledStyle style = compileStyleDocument(doc, errors);
    ASSERT_TRUE(errors.empty());
    EXPECT_EQ(style.fieldMaxZoom, 15) << "封小了末梢路整体消失(真机踩过封 14)";

    // 无档且无显式 → fail-loud,不猜。
    errors.clear();
    StyleDocument bare = parseStyleDocument(R"json({
      "layers": [{"id": "r", "type": "line", "source-layer": "roads",
                  "paint": {"line-color": "#ffffff",
                            "line-width-ramp": [12,1,16,3]}}]
    })json",
                                            errors);
    ASSERT_TRUE(errors.empty());
    compileStyleDocument(bare, errors);
    EXPECT_TRUE(hasErrorAt(errors, "$")) << "封顶无来源必须显式报,不默认";
}

// ---------------- 成本类路由 ----------------

TEST(StyleApplyPlan, UniformOnlyChangeSkipsRebake) {
    std::vector<StyleError> errors;
    const std::string day = docWithFilter(kGradingJson);
    // 只换线色(#f2f2e6d9 → 琥珀):Uniform 类,不该触发场重烘。
    std::string night = day;
    const std::string oldColor = "#f2f2e6d9", newColor = "#ffb833e6";
    night.replace(night.find(oldColor), oldColor.size(), newColor);

    CompiledStyle a = compileStyleDocument(parseStyleDocument(day, errors), errors);
    CompiledStyle b = compileStyleDocument(parseStyleDocument(night, errors), errors);
    ASSERT_TRUE(errors.empty());

    StyleApplyPlan plan = planStyleApply(&a, b);
    EXPECT_FALSE(plan.rebakeField) << "线色是 FS uniform,重烘是白干";
    EXPECT_FALSE(plan.rebakeDrape);
    EXPECT_NE(a.fieldLineColor, b.fieldLineColor) << "前置:色确实变了";
}

TEST(StyleApplyPlan, GradingChangeRebakesFieldOnly) {
    std::vector<StyleError> errors;
    const std::string day = docWithFilter(kGradingJson);
    std::string tightened = day;
    // 改分级档(z<10 干线档改 z<11):Re-bake 类。
    const std::string oldZ = R"("value": 10})", newZ = R"("value": 11})";
    tightened.replace(tightened.find(oldZ), oldZ.size(), newZ);

    CompiledStyle a = compileStyleDocument(parseStyleDocument(day, errors), errors);
    CompiledStyle b =
        compileStyleDocument(parseStyleDocument(tightened, errors), errors);
    ASSERT_TRUE(errors.empty());

    StyleApplyPlan plan = planStyleApply(&a, b);
    EXPECT_TRUE(plan.rebakeField);
    EXPECT_FALSE(plan.rebakeDrape) << "面没动,不许连坐";
}

TEST(StyleApplyPlan, FirstApplyRebakesEverything) {
    std::vector<StyleError> errors;
    CompiledStyle b = compileStyleDocument(
        parseStyleDocument(docWithFilter(kGradingJson), errors), errors);
    ASSERT_TRUE(errors.empty());
    StyleApplyPlan plan = planStyleApply(nullptr, b);
    EXPECT_TRUE(plan.rebakeField);
    EXPECT_TRUE(plan.rebakeDrape);
}
