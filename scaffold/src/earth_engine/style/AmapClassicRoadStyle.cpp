#include "earth_engine/style/AmapClassicRoadStyle.h"
#include "earth_engine/style/AmapClassicStyleInternal.h"
#include "earth_engine/style/AmapClassicZoom.h"

#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

constexpr int officialStyleIdentity(int classCode, int subKey) {
    return classCode * 1000 + subKey;
}

struct RoadWidthStop { int amapZoom; float width; };
struct RoadWidthCurve { int styleGroup; uint16_t offset; uint8_t count; };
struct RoadColorStop { int amapZoom; uint32_t argb; };
struct RoadLineTypeStop { int amapZoom; int lineType; };
struct RoadStrokeCurve { int styleGroup; uint16_t offset; uint8_t count; };
struct RoadIdentity { int classCode; int subKey; };
struct RoadVisibility { int styleGroup; int minZoom; int maxZoom; };
using LineCap = FeatureRenderStyle::LineCap;
struct LineTypeRecord {
    int lineType;
    std::array<float, 4> lengths;
    uint8_t count;
    LineCap cap;
};

#include "AmapClassicRoadWidthData.inc"
#include "AmapClassicLineTypeData.inc"

template <size_t N>
constexpr bool roadIdentitiesSorted(const RoadIdentity (&records)[N]) {
    for (size_t i = 1; i < N; ++i) {
        if (records[i - 1].classCode > records[i].classCode ||
            (records[i - 1].classCode == records[i].classCode &&
             records[i - 1].subKey > records[i].subKey)) return false;
    }
    return true;
}

static_assert(roadIdentitiesSorted(kOrdinaryRoadIdentities));
static_assert(roadIdentitiesSorted(kOfficialLineLabelIdentities));

template <size_t N>
bool containsRoadIdentity(const RoadIdentity (&records)[N],
                          int classCode, int subKey) {
    const auto it = std::lower_bound(
        std::begin(records), std::end(records),
        std::pair{classCode, subKey},
        [](const RoadIdentity& row, const std::pair<int, int>& identity) {
            return std::pair{row.classCode, row.subKey} < identity;
        });
    return it != std::end(records) && it->classCode == classCode &&
           it->subKey == subKey;
}

template <size_t N>
void installRoadWidthCurves(
    std::unordered_map<int, StyleExpression::Ptr>& target,
    const RoadWidthStop* stops, const RoadWidthCurve (&curves)[N],
    std::unordered_map<int, double>* minZoomByStyleGroup = nullptr,
    bool zeroBeforeFirstStop = false) {
    std::map<std::pair<uint16_t, uint8_t>, StyleExpression::Ptr> canonical;
    for (const RoadWidthCurve& curve : curves) {
        const auto key = std::make_pair(curve.offset, curve.count);
        auto [it, inserted] = canonical.emplace(key, nullptr);
        if (inserted) {
            std::vector<std::pair<double, StyleExpression::Ptr>> values;
            values.reserve(curve.count + (zeroBeforeFirstStop ? 1 : 0));
            const double firstDisplayZoom =
                stops[curve.offset].amapZoom - 1.0;
            if (zeroBeforeFirstStop && firstDisplayZoom > 0.0) {
                values.emplace_back(0.0, StyleExpression::literal(0.0));
            }
            for (uint8_t i = 0; i < curve.count; ++i) {
                const RoadWidthStop& stop = stops[curve.offset + i];
                values.emplace_back(stop.amapZoom - 1.0,
                                    StyleExpression::literal(stop.width));
            }
            it->second = StyleExpression::step(amapClassicDiscreteZoom(),
                                               std::move(values));
        }
        target[curve.styleGroup] = it->second;
        if (minZoomByStyleGroup) {
            (*minZoomByStyleGroup)[curve.styleGroup] =
                stops[curve.offset].amapZoom - 1.0;
        }
    }
}

std::array<float, 4> roadColor(uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xff) / 255.0f,
            static_cast<float>((argb >> 8) & 0xff) / 255.0f,
            static_cast<float>(argb & 0xff) / 255.0f,
            static_cast<float>((argb >> 24) & 0xff) / 255.0f};
}

template <size_t N>
void installRoadColorCurves(
    std::unordered_map<int, StyleExpression::Ptr>& target,
    const RoadColorStop* stops, const RoadStrokeCurve (&curves)[N],
    bool transparentBeforeFirstStop = false) {
    std::map<std::pair<uint16_t, uint8_t>, StyleExpression::Ptr> canonical;
    for (const RoadStrokeCurve& curve : curves) {
        const auto key = std::make_pair(curve.offset, curve.count);
        auto [it, inserted] = canonical.emplace(key, nullptr);
        if (inserted) {
            std::vector<std::pair<double, StyleExpression::Ptr>> values;
            values.reserve(curve.count +
                           (transparentBeforeFirstStop ? 1 : 0));
            const double firstDisplayZoom =
                stops[curve.offset].amapZoom - 1.0;
            if (transparentBeforeFirstStop && firstDisplayZoom > 0.0) {
                values.emplace_back(
                    0.0, StyleExpression::literal(
                             std::array<float, 4>{0, 0, 0, 0}));
            }
            for (uint8_t i = 0; i < curve.count; ++i) {
                const RoadColorStop& stop = stops[curve.offset + i];
                values.emplace_back(stop.amapZoom - 1.0,
                                    StyleExpression::literal(
                                        roadColor(stop.argb)));
            }
            it->second = StyleExpression::step(amapClassicDiscreteZoom(),
                                               std::move(values));
        }
        target[curve.styleGroup] = it->second;
    }
}

template <size_t N>
void installRoadLineTypeCurves(
    std::unordered_map<int, StyleExpression::Ptr>& target,
    const RoadLineTypeStop* stops, const RoadStrokeCurve (&curves)[N],
    std::unordered_map<int, StyleExpression::Ptr>* solidCapGeometry = nullptr) {
    std::map<std::pair<uint16_t, uint8_t>, StyleExpression::Ptr> canonical;
    for (const RoadStrokeCurve& curve : curves) {
        const auto key = std::make_pair(curve.offset, curve.count);
        auto [it, inserted] = canonical.emplace(key, nullptr);
        if (inserted) {
            std::vector<std::pair<double, StyleExpression::Ptr>> values;
            values.reserve(curve.count);
            for (uint8_t i = 0; i < curve.count; ++i) {
                const RoadLineTypeStop& stop = stops[curve.offset + i];
                values.emplace_back(stop.amapZoom - 1.0,
                                    StyleExpression::literal(
                                        static_cast<double>(stop.lineType)));
            }
            it->second = StyleExpression::step(amapClassicDiscreteZoom(),
                                               std::move(values));
        }
        target[curve.styleGroup] = it->second;
        if (solidCapGeometry) {
            bool needsEndpointPrimitive = false;
            for (uint8_t i = 0; i < curve.count; ++i) {
                const int lineType = stops[curve.offset + i].lineType;
                needsEndpointPrimitive =
                    needsEndpointPrimitive || lineType == 13 || lineType == 14;
            }
            if (needsEndpointPrimitive) {
                // Geometry admission only. The exact Butt/Square/Round value
                // remains uniquely owned by the same official lineType curve
                // and is resolved at command time by lineTypeResolver.
                (*solidCapGeometry)[curve.styleGroup] =
                    StyleExpression::literal(0.0);
            }
        }
    }
}

template <size_t N>
StyleExpression::Ptr officialIdentityExpression(
    const RoadIdentity (&source)[N]) {
    const auto none = StyleExpression::literal(0.0);
    std::map<int, std::vector<int>> identities;
    for (const RoadIdentity& identity : source) {
        identities[identity.classCode].push_back(identity.subKey);
    }
    StyleExpression::Ptr out = none;
    for (auto it = identities.rbegin(); it != identities.rend(); ++it) {
        std::vector<std::pair<std::string, StyleExpression::Ptr>> subKeys;
        for (int subKey : it->second) {
            subKeys.emplace_back(
                std::to_string(subKey),
                StyleExpression::literal(static_cast<double>(
                    officialStyleIdentity(it->first, subKey))));
        }
        out = StyleExpression::match(
            "amap_class",
            {{std::to_string(it->first),
              StyleExpression::match("amap_subkey", std::move(subKeys), none)}},
            out);
    }
    return out;
}

StyleExpression::Ptr officialLineLabelStyleGroupExpression() {
    return officialIdentityExpression(kOfficialLineLabelIdentities);
}

StyleExpression::Ptr officialLineStyleGroupExpression() {
    return officialIdentityExpression(kOrdinaryRoadIdentities);
}

std::optional<FeatureRenderStyle::LineDashPattern>
officialRoadDashForLineType(int lineType) {
    using Pattern = FeatureRenderStyle::LineDashPattern;
    for (const LineTypeRecord& record : kOfficialLineTypes) {
        if (record.lineType == lineType) {
            return Pattern{record.lengths, record.count, record.cap};
        }
    }
    return Pattern{kOfficialUnknownLineType.lengths,
                   kOfficialUnknownLineType.count,
                   kOfficialUnknownLineType.cap};
}

}  // namespace

bool isAmapClassicTransportIdentity(int classCode, int subKey) {
    return containsRoadIdentity(kOrdinaryRoadIdentities, classCode, subKey);
}

bool isAmapClassicRoadLabelIdentity(int classCode, int subKey) {
    return containsRoadIdentity(kOfficialLineLabelIdentities,
                                classCode, subKey);
}

void AmapClassicStyleContract::applyRoadLabelPlacement(FeatureRenderStyle& style) {
    style.installAmapClassicScope(
        FeatureRenderStyle::AmapClassicScope::RoadLabel);
    style.labelStyleGroupExpr = officialLineLabelStyleGroupExpression();
    // The dedicated road-name stream owns label geometry only. Without an
    // installed transport identity expression, its source polylines must stop
    // after producing the official along-line label candidate.
    // NebulaLabelFormat publishes strokeWidth=2. The SDF renderer stores the
    // radius on each side, so the official outline contract is one CSS pixel.
    // Official Nebula roadName formatter forwards provider-selected records
    // directly. It does not add an engine-local same-name distance or bend
    // rejection. Field-5 text padding is [top,right,bottom,left]=[0,1,0,1].
    // BO/JO advances every glyph by CONSTS.ic(1px) at the official 24px
    // glyph metric size, hence spacing scales as fontSize / 24.
    for (const RoadIdentity& identity : kOfficialLineLabelIdentities) {
        const int styleGroup = officialStyleIdentity(
            identity.classCode, identity.subKey);
        style.lineLabelLayoutByStyleGroup[styleGroup] = {
            0.0f, 1.0f / 24.0f, 1.0f, 0.0f};
    }
}

void AmapClassicStyleContract::applyLineLabel(FeatureRenderStyle& style) {
    style.installAmapClassicScope(
        FeatureRenderStyle::AmapClassicScope::RoadLabel);
    style.labelStyleGroupExpr = officialLineLabelStyleGroupExpression();
    installRoadWidthCurves(style.labelSizeExprByStyleGroup,
                           kOfficialLineLabelSizeStops,
                           kOfficialLineLabelSizeCurves);
    installRoadColorCurves(style.labelColorExprByStyleGroup,
                           kOfficialLineLabelColorStops,
                           kOfficialLineLabelColorCurves);
    installRoadColorCurves(style.labelHaloColorExprByStyleGroup,
                           kOfficialLineLabelCasingColorStops,
                           kOfficialLineLabelCasingColorCurves);
    for (const RoadIdentity& identity : kOfficialLineLabelIdentities) {
        const int styleGroup = officialStyleIdentity(
            identity.classCode, identity.subKey);
        style.labelHaloWidthExprByStyleGroup[styleGroup] =
            StyleExpression::literal(1.0);
    }
    for (const RoadVisibility& window : kOfficialLineLabelVisibility) {
        style.labelMinZoomByStyleGroup[window.styleGroup] = window.minZoom - 1;
        style.labelMaxZoomByStyleGroup[window.styleGroup] = window.maxZoom;
        auto& windows = style.labelZoomWindowsByStyleGroup[window.styleGroup];
        const std::pair<int, int> displayWindow{window.minZoom - 1,
                                                 window.maxZoom};
        if (!windows.empty() && windows.back().second >= displayWindow.first)
            windows.back().second =
                std::max(windows.back().second, displayWindow.second);
        else
            windows.push_back(displayWindow);
    }
}

void AmapClassicStyleContract::applyTransport(FeatureRenderStyle& style) {
    style.installAmapClassicScope(
        FeatureRenderStyle::AmapClassicScope::Transport);
    style.lineStyleGroupExpr = officialLineStyleGroupExpression();
    style.visibilityZoomCeilFraction = 0.8;
    installRoadWidthCurves(style.lineWidthExprByStyleGroup,
                           kOrdinaryRoadCenterWidthStops,
                           kOrdinaryRoadCenterWidthCurves,
                           &style.lineMinZoomByStyleGroup, true);
    installRoadWidthCurves(style.lineCasingWidthExprByStyleGroup,
                           kOrdinaryRoadCasingWidthStops,
                           kOrdinaryRoadCasingWidthCurves, nullptr, true);
    installRoadColorCurves(style.lineColorExprByStyleGroup,
                           kOrdinaryRoadCenterColorStops,
                           kOrdinaryRoadCenterColorCurves, true);
    installRoadColorCurves(style.lineCasingColorExprByStyleGroup,
                           kOrdinaryRoadCasingColorStops,
                           kOrdinaryRoadCasingColorCurves, true);
    installRoadLineTypeCurves(style.lineTypeExprByStyleGroup,
                              kOrdinaryRoadCenterLineTypeStops,
                              kOrdinaryRoadCenterLineTypeCurves,
                              &style.lineSolidCapExprByStyleGroup);
    installRoadLineTypeCurves(style.lineCasingTypeExprByStyleGroup,
                              kOrdinaryRoadCasingLineTypeStops,
                              kOrdinaryRoadCasingLineTypeCurves,
                              &style.lineCasingSolidCapExprByStyleGroup);
    style.lineTypeResolver = officialRoadDashForLineType;
    for (const RoadVisibility& window : kOfficialLineVisibility) {
        style.lineMinZoomByStyleGroup[window.styleGroup] = window.minZoom - 1;
        style.lineMaxZoomByStyleGroup[window.styleGroup] = window.maxZoom;
    }
    for (const RoadIdentity& identity : kOfficialCasingIdentities) {
        style.lineCasingStyleGroups.insert(
            officialStyleIdentity(identity.classCode, identity.subKey));
    }
}

#if defined(EARTH_ENGINE_TESTING)
std::vector<AmapClassicRoadIdentityForTest>
amapClassicRoadIdentitiesForTest() {
    std::vector<AmapClassicRoadIdentityForTest> out;
    out.reserve(std::size(kOrdinaryRoadIdentities));
    for (const auto& identity : kOrdinaryRoadIdentities) {
        const int group = officialStyleIdentity(
            identity.classCode, identity.subKey);
        const auto visibility = std::find_if(
            std::begin(kOfficialLineVisibility),
            std::end(kOfficialLineVisibility),
            [&](const RoadVisibility& row) {
                return row.styleGroup == group;
            });
        if (visibility == std::end(kOfficialLineVisibility)) continue;
        out.push_back({identity.classCode, identity.subKey,
                       visibility->minZoom, visibility->maxZoom});
    }
    return out;
}

StyleExpression::Ptr amapClassicLineLabelStyleGroupExpression() {
    return officialLineLabelStyleGroupExpression();
}
StyleExpression::Ptr amapClassicLineStyleGroupExpression() {
    return officialLineStyleGroupExpression();
}
std::optional<FeatureRenderStyle::LineDashPattern>
amapClassicRoadDashForLineType(int lineType) {
    return officialRoadDashForLineType(lineType);
}
bool amapClassicRoadColorsAreTransparentBeforeFirstStopForTest(
    const FeatureRenderStyle& style) {
    const auto verify = [](const auto& curves, const RoadColorStop* stops,
                           const auto& installed) {
        for (const auto& curve : curves) {
            const int firstDisplayZoom =
                stops[curve.offset].amapZoom - 1;
            if (firstDisplayZoom <= 0) continue;
            const auto expression = installed.find(curve.styleGroup);
            if (expression == installed.end()) return false;
            const auto value = expression->second->evaluate(
                nullptr, static_cast<double>(firstDisplayZoom - 1));
            if (!value || value->kind() != StyleValue::Kind::Color ||
                value->color()[3] != 0.0f) {
                return false;
            }
        }
        return true;
    };
    return verify(kOrdinaryRoadCenterColorCurves,
                  kOrdinaryRoadCenterColorStops,
                  style.lineColorExprByStyleGroup) &&
           verify(kOrdinaryRoadCasingColorCurves,
                  kOrdinaryRoadCasingColorStops,
                  style.lineCasingColorExprByStyleGroup);
}
bool amapClassicRoadTablesReachFinalConsumerForTest(
    const FeatureRenderStyle& style) {
    const auto findCurve = [](const auto& curves, int styleGroup) {
        return std::find_if(std::begin(curves), std::end(curves),
                            [&](const auto& curve) {
                                return curve.styleGroup == styleGroup;
                            });
    };
    const auto numberAt = [](const auto* stops, const auto& curve,
                             int providerZoom, double beforeFirst) {
        double out = beforeFirst;
        for (uint8_t i = 0; i < curve.count; ++i) {
            const auto& stop = stops[curve.offset + i];
            if (stop.amapZoom <= providerZoom) out = stop.width;
        }
        return out;
    };
    const auto lineTypeAt = [](const auto* stops, const auto& curve,
                               int providerZoom) {
        int out = 0;
        for (uint8_t i = 0; i < curve.count; ++i) {
            const auto& stop = stops[curve.offset + i];
            if (stop.amapZoom <= providerZoom) out = stop.lineType;
        }
        return out;
    };
    const auto colorAt = [](const auto* stops, const auto& curve,
                            int providerZoom) {
        std::array<float, 4> out{0, 0, 0, 0};
        for (uint8_t i = 0; i < curve.count; ++i) {
            const auto& stop = stops[curve.offset + i];
            if (stop.amapZoom <= providerZoom) out = roadColor(stop.argb);
        }
        return out;
    };
    const auto evaluatedNumber = [](const auto& expressions, int styleGroup,
                                    double displayZoom) {
        const auto it = expressions.find(styleGroup);
        if (it == expressions.end()) return std::optional<double>{};
        const auto value = it->second->evaluate(nullptr, displayZoom);
        if (!value || value->kind() != StyleValue::Kind::Number)
            return std::optional<double>{};
        return std::optional<double>{value->number()};
    };
    const auto evaluatedColor = [](const auto& expressions, int styleGroup,
                                   double displayZoom) {
        const auto it = expressions.find(styleGroup);
        if (it == expressions.end())
            return std::optional<std::array<float, 4>>{};
        const auto value = it->second->evaluate(nullptr, displayZoom);
        if (!value || value->kind() != StyleValue::Kind::Color)
            return std::optional<std::array<float, 4>>{};
        return std::optional<std::array<float, 4>>{value->color()};
    };
    for (const RoadVisibility& visibility : kOfficialLineVisibility) {
        const int group = visibility.styleGroup;
        const auto centerWidth = findCurve(
            kOrdinaryRoadCenterWidthCurves, group);
        const auto casingWidth = findCurve(
            kOrdinaryRoadCasingWidthCurves, group);
        const auto centerColor = findCurve(
            kOrdinaryRoadCenterColorCurves, group);
        const auto casingColor = findCurve(
            kOrdinaryRoadCasingColorCurves, group);
        const auto centerType = findCurve(
            kOrdinaryRoadCenterLineTypeCurves, group);
        const auto casingType = findCurve(
            kOrdinaryRoadCasingLineTypeCurves, group);
        if (centerWidth == std::end(kOrdinaryRoadCenterWidthCurves) ||
            casingWidth == std::end(kOrdinaryRoadCasingWidthCurves) ||
            centerColor == std::end(kOrdinaryRoadCenterColorCurves) ||
            casingColor == std::end(kOrdinaryRoadCasingColorCurves) ||
            centerType == std::end(kOrdinaryRoadCenterLineTypeCurves) ||
            casingType == std::end(kOrdinaryRoadCasingLineTypeCurves))
            return false;
        const auto minIt = style.lineMinZoomByStyleGroup.find(group);
        const auto maxIt = style.lineMaxZoomByStyleGroup.find(group);
        if (minIt == style.lineMinZoomByStyleGroup.end() ||
            maxIt == style.lineMaxZoomByStyleGroup.end() ||
            minIt->second != visibility.minZoom - 1.0 ||
            maxIt->second != visibility.maxZoom) return false;
        for (int providerZoom = visibility.minZoom;
             providerZoom <= visibility.maxZoom; ++providerZoom) {
            const double displayZoom = providerZoom - 1.0;
            const double expectedCenterWidth = numberAt(
                kOrdinaryRoadCenterWidthStops, *centerWidth, providerZoom, 0);
            const double expectedCasingWidth = numberAt(
                kOrdinaryRoadCasingWidthStops, *casingWidth, providerZoom, 0);
            const auto expectedCenterColor = colorAt(
                kOrdinaryRoadCenterColorStops, *centerColor, providerZoom);
            const auto expectedCasingColor = colorAt(
                kOrdinaryRoadCasingColorStops, *casingColor, providerZoom);
            if (evaluatedNumber(style.lineWidthExprByStyleGroup, group,
                                displayZoom) != expectedCenterWidth ||
                evaluatedNumber(style.lineCasingWidthExprByStyleGroup, group,
                                displayZoom) != expectedCasingWidth ||
                evaluatedColor(style.lineColorExprByStyleGroup, group,
                               displayZoom) != expectedCenterColor ||
                evaluatedColor(style.lineCasingColorExprByStyleGroup, group,
                               displayZoom) != expectedCasingColor) return false;
            if (expectedCenterWidth > 0 && expectedCenterColor[3] > 0 &&
                evaluatedNumber(style.lineTypeExprByStyleGroup, group,
                                displayZoom) !=
                    lineTypeAt(kOrdinaryRoadCenterLineTypeStops, *centerType,
                               providerZoom)) return false;
            if (expectedCenterWidth + expectedCasingWidth > 0 &&
                expectedCasingColor[3] > 0 &&
                evaluatedNumber(style.lineCasingTypeExprByStyleGroup, group,
                                displayZoom) !=
                    lineTypeAt(kOrdinaryRoadCasingLineTypeStops, *casingType,
                               providerZoom)) return false;
        }
    }
    return true;
}
bool amapClassicRoadLabelTablesCoverEveryOfficialIdentityForTest(
    const FeatureRenderStyle& style) {
    const auto numberAt = [](const auto& table, int group, double zoom) {
        const auto it = table.find(group);
        if (it == table.end() || !it->second)
            return std::optional<double>{};
        const auto value = it->second->evaluate(nullptr, zoom);
        return value && value->kind() == StyleValue::Kind::Number
                   ? std::optional<double>{value->number()}
                   : std::optional<double>{};
    };
    const auto colorAt = [](const auto& table, int group, double zoom) {
        const auto it = table.find(group);
        if (it == table.end() || !it->second)
            return std::optional<std::array<float, 4>>{};
        const auto value = it->second->evaluate(nullptr, zoom);
        return value && value->kind() == StyleValue::Kind::Color
                   ? std::optional<std::array<float, 4>>{value->color()}
                   : std::optional<std::array<float, 4>>{};
    };
    const auto widthCurve = [](int group) -> const RoadWidthCurve* {
        for (const auto& curve : kOfficialLineLabelSizeCurves)
            if (curve.styleGroup == group) return &curve;
        return nullptr;
    };
    const auto strokeCurve = [](const auto& curves,
                                int group) -> const RoadStrokeCurve* {
        for (const auto& curve : curves)
            if (curve.styleGroup == group) return &curve;
        return nullptr;
    };
    const auto expectedWidth = [](const RoadWidthCurve& curve, int zoom) {
        float result = kOfficialLineLabelSizeStops[curve.offset].width;
        for (uint8_t i = 0; i < curve.count; ++i) {
            const auto& stop = kOfficialLineLabelSizeStops[curve.offset + i];
            if (stop.amapZoom <= zoom) result = stop.width;
        }
        return result;
    };
    const auto expectedColor = [](const auto* stops,
                                  const RoadStrokeCurve& curve, int zoom) {
        uint32_t result = stops[curve.offset].argb;
        for (uint8_t i = 0; i < curve.count; ++i) {
            const auto& stop = stops[curve.offset + i];
            if (stop.amapZoom <= zoom) result = stop.argb;
        }
        return roadColor(result);
    };
    for (const RoadIdentity& identity : kOfficialLineLabelIdentities) {
        const int styleGroup = officialStyleIdentity(
            identity.classCode, identity.subKey);
        if (style.labelSizeExprByStyleGroup.count(styleGroup) != 1 ||
            style.labelColorExprByStyleGroup.count(styleGroup) != 1 ||
            style.labelHaloColorExprByStyleGroup.count(styleGroup) != 1 ||
            style.labelHaloWidthExprByStyleGroup.count(styleGroup) != 1 ||
            style.labelMinZoomByStyleGroup.count(styleGroup) != 1 ||
            style.labelMaxZoomByStyleGroup.count(styleGroup) != 1 ||
            style.lineLabelLayoutByStyleGroup.count(styleGroup) != 1) {
            return false;
        }
        const RoadWidthCurve* size = widthCurve(styleGroup);
        const RoadStrokeCurve* text = strokeCurve(
            kOfficialLineLabelColorCurves, styleGroup);
        const RoadStrokeCurve* halo = strokeCurve(
            kOfficialLineLabelCasingColorCurves, styleGroup);
        if (!size || !text || !halo) return false;
        const auto visibility = std::find_if(
            std::begin(kOfficialLineLabelVisibility),
            std::end(kOfficialLineLabelVisibility),
            [styleGroup](const auto& row) {
                return row.styleGroup == styleGroup;
            });
        if (visibility == std::end(kOfficialLineLabelVisibility)) return false;
        for (int displayZoom = 0; displayZoom < 30; ++displayZoom) {
            const int providerZoom = displayZoom + 1;
            if (FeatureRenderLayer::labelStyleVisibleAtZoom(
                    style, styleGroup, displayZoom) !=
                (providerZoom >= visibility->minZoom &&
                 providerZoom <= visibility->maxZoom)) return false;
            if (numberAt(style.labelSizeExprByStyleGroup, styleGroup,
                         displayZoom) != expectedWidth(*size, providerZoom) ||
                colorAt(style.labelColorExprByStyleGroup, styleGroup,
                        displayZoom) !=
                    expectedColor(kOfficialLineLabelColorStops, *text,
                                  providerZoom) ||
                colorAt(style.labelHaloColorExprByStyleGroup, styleGroup,
                        displayZoom) !=
                    expectedColor(kOfficialLineLabelCasingColorStops, *halo,
                                  providerZoom) ||
                numberAt(style.labelHaloWidthExprByStyleGroup, styleGroup,
                         displayZoom) != 1.0) return false;
        }
    }
    return true;
}
#endif


}  // namespace earth_engine
