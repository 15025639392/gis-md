#pragma once

#include "earth_engine/layers/FeatureRenderLayer.h"

#include <array>
#include <optional>
#include <vector>

namespace earth_engine {

// Generated official transport admission. This is shared by the sealed style
// selector and the typed source converter so zero-output line payloads are
// discarded before Feature/property/geometry allocation.
bool isAmapClassicTransportIdentity(int classCode, int subKey);
bool isAmapClassicSurfaceIdentity(int classCode, int subKey);
bool isAmapClassicRoadLabelIdentity(int classCode, int subKey);
bool isAmapClassicPoiIdentity(int classCode, int subKey);
std::array<float, 4> amapClassicLandBaseColor();

/// Resolve one ordinary AMap surface-fill color at renderer display zoom.
/// Returns nullopt for identities that are not terrain-draped surface fills
/// (notably buildings/extrusions) or for a style window that is not visible.
/// The display zoom uses the same +1 provider-zoom convention as the sealed
/// FeatureRenderStyle expressions.
std::optional<std::array<float, 4>> amapClassicSurfaceColorForDisplayZoom(
    int classCode, int subKey, double displayZoom);

/// Register runtime surface-color overrides (amap-vector.json style.surface)
/// so the terrain-baked surface fill mask honors them.  Overrides replace the
/// sealed official record for matching (classCode, subKey); an empty map
/// clears all overrides back to the sealed table.  Thread-safe.
void setAmapClassicSurfaceColorOverrides(
    const std::vector<std::pair<std::pair<int, int>,
                                std::array<float, 4>>>& overrides);

#if defined(EARTH_ENGINE_TESTING)
struct AmapClassicSurfaceRecordForTest {
    int classCode = 0;
    int subKey = 0;
    int minZoom = 0;
    int maxZoom = 0;
    std::array<float, 4> color{};
};
std::vector<AmapClassicSurfaceRecordForTest>
amapClassicSurfaceRecordsForTest();
struct AmapClassicBuildingRecordForTest {
    int subKey = 0;
    int minZoom = 0;
    int maxZoom = 0;
    std::array<float, 4> roofColor{};
    std::array<float, 4> wallColor{};
};
std::vector<AmapClassicBuildingRecordForTest>
amapClassicBuildingRecordsForTest();
/// Exhaustively compares the installed surface expressions with a direct
/// scan of every generated official record at every supported integer display
/// zoom. Kept test-only so production has one executable style path.
bool amapClassicSurfaceExpressionsMatchOfficialRecordsForTest(
    const FeatureRenderStyle& style);
bool amapClassicBuildingExpressionsMatchOfficialRecordsForTest(
    const FeatureRenderStyle& style);
#endif

// Complete official profiles are assembled only by FeatureRenderLayer.
// Keeping these scope installers private prevents production callers from
// constructing a partial/dual AMap style contract.
class AmapClassicStyleContract final {
private:
    friend class FeatureRenderLayer;
    static void applySurface(FeatureRenderStyle& style);
    static void applyTransport(FeatureRenderStyle& style);
    static void applyRoadLabelPlacement(FeatureRenderStyle& style);
    static void applyLineLabel(FeatureRenderStyle& style);
    static void applyPoi(FeatureRenderStyle& style);

};

} // namespace earth_engine
