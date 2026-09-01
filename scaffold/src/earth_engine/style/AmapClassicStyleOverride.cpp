#include "earth_engine/style/AmapClassicStyleOverride.h"

namespace earth_engine {
namespace {
int styleGroup(int classCode, int subKey) { return classCode * 1000 + subKey; }
}  // namespace

void applyAmapClassicStyleOverrides(
    FeatureRenderStyle& style, const AmapClassicStyleOverrides& overrides) {
    if (overrides.empty()) return;
    for (const auto& s : overrides.surface) {
        style.fillColorExprByStyleGroup[styleGroup(s.classCode, s.subKey)] =
            StyleExpression::literal(s.color);
    }
    for (const auto& l : overrides.line) {
        const int sg = styleGroup(l.classCode, l.subKey);
        style.lineColorExprByStyleGroup[sg] =
            StyleExpression::literal(l.color);
        style.lineWidthExprByStyleGroup[sg] =
            StyleExpression::literal(l.widthPx);
    }
}

} // namespace earth_engine