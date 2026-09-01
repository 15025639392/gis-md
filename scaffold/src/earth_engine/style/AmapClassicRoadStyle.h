#pragma once

#include <optional>
#include <vector>

#include "earth_engine/layers/FeatureRenderLayer.h"

namespace earth_engine {

#if defined(EARTH_ENGINE_TESTING)
/// Test-only inspection helpers. Production style identity and dash lookup are
/// private implementation details of the sealed profile.
/// Collision-free internal representation of the official AMap
/// (classCode, subKey) tuple. This is an encoding only; visual meaning remains
/// exclusively in the official tuple and drawOrder remains independent.
constexpr int amapClassicStyleIdentity(int classCode, int subKey) {
    return classCode * 1000 + subKey;
}

struct AmapClassicRoadIdentityForTest {
    int classCode = 0;
    int subKey = 0;
    int minZoom = 0;
    int maxZoom = 0;
};
std::vector<AmapClassicRoadIdentityForTest>
amapClassicRoadIdentitiesForTest();

/// Converts a current-JSAPI ordinary road lineType to its width-relative dash
/// contract. Types 0..16 consume the generated switch cases, including the
/// railway dash at type 6; any other numeric value consumes the official
/// `default: "solid"` butt-cap branch. A returned pattern with count 0 is a
/// verified solid road. Butt, square, and round caps are preserved by the
/// three-state LineDashPattern contract for both solid endpoints and dashes.
std::optional<FeatureRenderStyle::LineDashPattern>
amapClassicRoadDashForLineType(int lineType);

/// Complete official line visual identity selector. Unknown tuples resolve to
/// zero; drawOrder and caller expressions are never consulted.
StyleExpression::Ptr amapClassicLineStyleGroupExpression();

/// Complete official line-label identity. Every line-label payload uses its
/// official amap_class/subKey tuple directly; payload role does not define a
/// second visual identity path.
/// Unknown tuples resolve to zero and drawOrder is never consulted.
StyleExpression::Ptr amapClassicLineLabelStyleGroupExpression();

/// Every generated road color curve must be transparent before its first
/// official record. This prevents Step endpoint clamping from inventing a
/// center or casing pass at earlier zooms.
bool amapClassicRoadColorsAreTransparentBeforeFirstStopForTest(
    const FeatureRenderStyle& style);
bool amapClassicRoadTablesReachFinalConsumerForTest(
    const FeatureRenderStyle& style);
bool amapClassicRoadLabelTablesCoverEveryOfficialIdentityForTest(
    const FeatureRenderStyle& style);
#endif

}  // namespace earth_engine
