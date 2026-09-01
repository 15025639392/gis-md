#include "earth_engine/style/AmapClassicStyleInternal.h"
#include "earth_engine/style/AmapClassicZoom.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

struct SurfaceRecord {
    int classCode;
    int subKey;
    int minZoom;
    int maxZoom;
    uint32_t argb;
};

// Runtime surface-color overrides (amap-vector.json style.surface) consulted
// by amapClassicSurfaceColorForDisplayZoom BEFORE the sealed official table, so
// the terrain-baked surface fill mask honors them.  Keyed by (class<<32|subKey).
std::mutex gSurfaceOverrideMutex;
std::unordered_map<uint64_t, std::array<float, 4>> gSurfaceOverrides;
uint64_t surfaceOverrideKey(int classCode, int subKey) {
    return (uint64_t(static_cast<uint32_t>(classCode)) << 32) |
           static_cast<uint32_t>(subKey);
}
struct BuildingRecord {
    int subKey;
    int minZoom;
    int maxZoom;
    uint32_t roofArgb;
    uint32_t wallArgb;
};

#include "AmapClassicSurfaceData.inc"

template <size_t N>
constexpr bool surfaceRecordsSorted(const SurfaceRecord (&records)[N]) {
    for (size_t i = 1; i < N; ++i) {
        if (records[i - 1].classCode > records[i].classCode ||
            (records[i - 1].classCode == records[i].classCode &&
             records[i - 1].subKey > records[i].subKey)) return false;
    }
    return true;
}

template <size_t N>
constexpr bool buildingRecordsSorted(const BuildingRecord (&records)[N]) {
    for (size_t i = 1; i < N; ++i)
        if (records[i - 1].subKey > records[i].subKey) return false;
    return true;
}

static_assert(surfaceRecordsSorted(kOfficialSurfaceRecords));
static_assert(buildingRecordsSorted(kBuilding55001));

std::array<float, 4> color(uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xff) / 255.0f,
            static_cast<float>((argb >> 8) & 0xff) / 255.0f,
            static_cast<float>(argb & 0xff) / 255.0f,
            static_cast<float>((argb >> 24) & 0xff) / 255.0f};
}

void appendSurfaceCommandStyle(FeatureRenderStyle& style) {
    std::map<int, std::map<int, std::vector<SurfaceRecord>>> byIdentity;
    for (const auto& record : kOfficialSurfaceRecords)
        byIdentity[record.classCode][record.subKey].push_back(record);
    std::vector<std::pair<std::string, StyleExpression::Ptr>> classes;
    for (auto& [classCode, bySubKey] : byIdentity) {
      std::vector<std::pair<std::string, StyleExpression::Ptr>> identities;
      for (auto& [subKey, windows] : bySubKey) {
        const int styleGroup = classCode * 1000 + subKey;
        identities.emplace_back(std::to_string(subKey),
            StyleExpression::literal(static_cast<double>(styleGroup)));
        std::sort(windows.begin(), windows.end(), [](const auto& a, const auto& b) {
            return a.minZoom < b.minZoom;
        });
        std::vector<std::pair<double, StyleExpression::Ptr>> stops{
            {0.0, StyleExpression::literal(std::array<float, 4>{0, 0, 0, 0})}};
        int coveredThrough = -1;
        for (const auto& window : windows) {
            if (window.minZoom > coveredThrough + 1) {
                stops.emplace_back(window.minZoom - 1.0,
                    StyleExpression::literal(std::array<float, 4>{0, 0, 0, 0}));
            }
            stops.emplace_back(window.minZoom - 1.0,
                               StyleExpression::literal(color(window.argb)));
            coveredThrough = std::max(coveredThrough, window.maxZoom);
            if (window.maxZoom < 30) {
                stops.emplace_back(window.maxZoom,
                    StyleExpression::literal(std::array<float, 4>{0, 0, 0, 0}));
            }
        }
        style.fillColorExprByStyleGroup[styleGroup] = StyleExpression::step(
            amapClassicDiscreteZoom(), std::move(stops));
      }
      classes.emplace_back(std::to_string(classCode), StyleExpression::match(
          "amap_subkey", std::move(identities), StyleExpression::literal(0.0)));
    }
    style.fillStyleGroupExpr = StyleExpression::match(
        "amap_class", std::move(classes),
        style.fillStyleGroupExpr);
}

template <size_t N>
void appendBuildingCommandStyle(const BuildingRecord (&records)[N],
                                FeatureRenderStyle& style) {
    std::vector<std::pair<std::string, StyleExpression::Ptr>> identities;
    for (const auto& record : records) {
        const int styleGroup = 55001 * 1000 + record.subKey;
        identities.emplace_back(std::to_string(record.subKey),
            StyleExpression::literal(static_cast<double>(styleGroup)));
        style.fillColorExprByStyleGroup[styleGroup] = StyleExpression::step(
            amapClassicDiscreteZoom(), {
                {0.0, StyleExpression::literal(std::array<float, 4>{0, 0, 0, 0})},
                {record.minZoom - 1.0, StyleExpression::literal(color(record.roofArgb))},
                {static_cast<double>(record.maxZoom),
                 StyleExpression::literal(std::array<float, 4>{0, 0, 0, 0})}});
        style.extrusionRoofColorByStyleGroup[styleGroup] = color(record.roofArgb);
        style.extrusionWallColorByStyleGroup[styleGroup] = color(record.wallArgb);
        style.fillMinZoomByStyleGroup[styleGroup] = record.minZoom - 1.0;
        style.fillMaxZoomByStyleGroup[styleGroup] = record.maxZoom;
    }
    style.fillStyleGroupExpr = StyleExpression::match(
        "amap_class", {{"55001", StyleExpression::match(
            "amap_subkey", std::move(identities), StyleExpression::literal(0.0))}},
        style.fillStyleGroupExpr);
}

}  // namespace

bool isAmapClassicSurfaceIdentity(int classCode, int subKey) {
    if (classCode == 55001) {
        const auto it = std::lower_bound(
            std::begin(kBuilding55001), std::end(kBuilding55001), subKey,
            [](const BuildingRecord& row, int identity) {
                return row.subKey < identity;
            });
        return it != std::end(kBuilding55001) && it->subKey == subKey;
    }
    const auto it = std::lower_bound(
        std::begin(kOfficialSurfaceRecords),
        std::end(kOfficialSurfaceRecords), std::pair{classCode, subKey},
        [](const SurfaceRecord& row, const std::pair<int, int>& identity) {
            return std::pair{row.classCode, row.subKey} < identity;
        });
    return it != std::end(kOfficialSurfaceRecords) &&
           it->classCode == classCode && it->subKey == subKey;
}

std::array<float, 4> amapClassicLandBaseColor() {
    {
        // Runtime override (amap-vector.json style.surface) wins over the
        // sealed land-base record; the terrain base color is NOT baked into
        // the surface mask, so it must be overridden here separately.
        std::lock_guard<std::mutex> lock(gSurfaceOverrideMutex);
        const auto it =
            gSurfaceOverrides.find(surfaceOverrideKey(30001, 1));
        if (it != gSurfaceOverrides.end()) return it->second;
    }
    for (const auto& record : kOfficialSurfaceRecords) {
        if (record.classCode == 30001 && record.subKey == 1 &&
            record.minZoom <= 2 && record.maxZoom >= 2) {
            return color(record.argb);
        }
    }
    return {0, 0, 0, 0};
}

void setAmapClassicSurfaceColorOverrides(
    const std::vector<std::pair<std::pair<int, int>,
                                std::array<float, 4>>>& overrides) {
    std::lock_guard<std::mutex> lock(gSurfaceOverrideMutex);
    gSurfaceOverrides.clear();
    for (const auto& [identity, color] : overrides) {
        gSurfaceOverrides[surfaceOverrideKey(identity.first, identity.second)] =
            color;
    }
}

std::optional<std::array<float, 4>>
amapClassicSurfaceColorForDisplayZoom(int classCode, int subKey,
                                      double displayZoom) {
    if (!std::isfinite(displayZoom) || classCode == 55001) {
        return std::nullopt;
    }
    {
        // Runtime override takes precedence over the sealed official record.
        std::lock_guard<std::mutex> lock(gSurfaceOverrideMutex);
        const auto it =
            gSurfaceOverrides.find(surfaceOverrideKey(classCode, subKey));
        if (it != gSurfaceOverrides.end()) return it->second;
    }
    const int providerZoom = static_cast<int>(
        amapClassicDiscreteZoomValue(displayZoom)) + 1;
    for (const auto& record : kOfficialSurfaceRecords) {
        if (record.classCode != classCode || record.subKey != subKey) {
            continue;
        }
        if (record.minZoom <= providerZoom && providerZoom <= record.maxZoom) {
            return color(record.argb);
        }
    }
    return std::nullopt;
}

void AmapClassicStyleContract::applySurface(FeatureRenderStyle& style) {
    style.installAmapClassicScope(FeatureRenderStyle::AmapClassicScope::Surface);
    style.fillStyleGroupExpr = StyleExpression::literal(0.0);
    appendSurfaceCommandStyle(style);
    appendBuildingCommandStyle(kBuilding55001, style);
}

#if defined(EARTH_ENGINE_TESTING)
std::vector<AmapClassicSurfaceRecordForTest>
amapClassicSurfaceRecordsForTest() {
    std::vector<AmapClassicSurfaceRecordForTest> out;
    out.reserve(std::size(kOfficialSurfaceRecords));
    for (const auto& record : kOfficialSurfaceRecords) {
        out.push_back({record.classCode, record.subKey, record.minZoom,
                       record.maxZoom, color(record.argb)});
    }
    return out;
}

std::vector<AmapClassicBuildingRecordForTest>
amapClassicBuildingRecordsForTest() {
    std::vector<AmapClassicBuildingRecordForTest> out;
    out.reserve(std::size(kBuilding55001));
    for (const auto& record : kBuilding55001) {
        out.push_back({record.subKey, record.minZoom, record.maxZoom,
                       color(record.roofArgb), color(record.wallArgb)});
    }
    return out;
}

bool amapClassicSurfaceExpressionsMatchOfficialRecordsForTest(
    const FeatureRenderStyle& style) {
    std::map<std::pair<int, int>, std::vector<SurfaceRecord>> byIdentity;
    for (const auto& record : kOfficialSurfaceRecords) {
        byIdentity[{record.classCode, record.subKey}].push_back(record);
    }
    for (const auto& [identity, records] : byIdentity) {
        const int styleGroup = identity.first * 1000 + identity.second;
        const auto expression = style.fillColorExprByStyleGroup.find(styleGroup);
        if (expression == style.fillColorExprByStyleGroup.end()) return false;
        for (int displayZoom = 0; displayZoom < 30; ++displayZoom) {
            std::array<float, 4> expected{0, 0, 0, 0};
            const int providerZoom = displayZoom + 1;
            for (const auto& record : records) {
                if (record.minZoom <= providerZoom &&
                    providerZoom <= record.maxZoom) {
                    expected = color(record.argb);
                }
            }
            const auto actual = expression->second->evaluate(
                nullptr, static_cast<double>(displayZoom));
            if (!actual || actual->kind() != StyleValue::Kind::Color ||
                actual->color() != expected) {
                return false;
            }
        }
    }
    return true;
}

bool amapClassicBuildingExpressionsMatchOfficialRecordsForTest(
    const FeatureRenderStyle& style) {
    for (const auto& record : kBuilding55001) {
        const int styleGroup = 55001 * 1000 + record.subKey;
        const auto fill = style.fillColorExprByStyleGroup.find(styleGroup);
        const auto roof = style.extrusionRoofColorByStyleGroup.find(styleGroup);
        const auto wall = style.extrusionWallColorByStyleGroup.find(styleGroup);
        const auto minZoom = style.fillMinZoomByStyleGroup.find(styleGroup);
        const auto maxZoom = style.fillMaxZoomByStyleGroup.find(styleGroup);
        if (fill == style.fillColorExprByStyleGroup.end() ||
            roof == style.extrusionRoofColorByStyleGroup.end() ||
            wall == style.extrusionWallColorByStyleGroup.end() ||
            minZoom == style.fillMinZoomByStyleGroup.end() ||
            maxZoom == style.fillMaxZoomByStyleGroup.end() ||
            roof->second != color(record.roofArgb) ||
            wall->second != color(record.wallArgb) ||
            minZoom->second != record.minZoom - 1.0 ||
            maxZoom->second != record.maxZoom) {
            return false;
        }
        for (int displayZoom = 0; displayZoom < 30; ++displayZoom) {
            const bool visible = record.minZoom <= displayZoom + 1 &&
                                 displayZoom + 1 <= record.maxZoom;
            const auto actual = fill->second->evaluate(
                nullptr, static_cast<double>(displayZoom));
            if (!actual || actual->kind() != StyleValue::Kind::Color ||
                actual->color() != (visible ? color(record.roofArgb)
                                           : std::array<float, 4>{0, 0, 0, 0})) {
                return false;
            }
        }
    }
    return true;
}
#endif

}  // namespace earth_engine
