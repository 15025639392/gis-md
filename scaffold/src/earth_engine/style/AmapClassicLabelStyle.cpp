#include "earth_engine/style/AmapClassicLabelStyleInternal.h"
#include "earth_engine/style/AmapClassicStyleInternal.h"
#include "earth_engine/style/AmapClassicZoom.h"
#include "earth_engine/renderer/GlyphAtlas.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace earth_engine {
namespace {

struct PoiLabelRecord {
    int styleGroup, minZoom, maxZoom;
    float size;
    uint32_t colorArgb, haloArgb;
};
struct GuideLabelRecord {
    int sub, minZoom, maxZoom;
    float size;
    uint32_t colorArgb;
};
struct PoiIconRecord {
    int cls, sub, minZoom, maxZoom, atlas, index, cellW, cellH, atlasW,
        atlasH;
    float displayW, displayH;
    float anchorOffsetX = 0.0f;
    float anchorOffsetY = 0.0f;
};
struct PoiFlagRecord { int cls, sub, minZoom, maxZoom, flags; };
struct PoiCityWindowRecord { const char* name; double minZoom, maxZoom; };
struct PoiDirectionRecord { const char* name; int direction; float x, y; };

#include "AmapClassicPoiData.inc"

template <size_t N>
constexpr bool labelRecordsSorted(const PoiLabelRecord (&records)[N]) {
    for (size_t i = 1; i < N; ++i)
        if (records[i - 1].styleGroup > records[i].styleGroup) return false;
    return true;
}

template <size_t N>
constexpr bool iconRecordsSorted(const PoiIconRecord (&records)[N]) {
    for (size_t i = 1; i < N; ++i) {
        if (records[i - 1].cls > records[i].cls ||
            (records[i - 1].cls == records[i].cls &&
             records[i - 1].sub > records[i].sub)) return false;
    }
    return true;
}

static_assert(labelRecordsSorted(kPoiLabelRecords));
static_assert(iconRecordsSorted(kPoiIconRecords));
static_assert(iconRecordsSorted(kPoiDynamicTextBackgroundRecords));

template <size_t N>
constexpr bool flagRecordsSorted(const PoiFlagRecord (&records)[N]) {
    for (size_t i = 1; i < N; ++i) {
        if (records[i - 1].cls > records[i].cls ||
            (records[i - 1].cls == records[i].cls &&
             records[i - 1].sub > records[i].sub) ||
            (records[i - 1].cls == records[i].cls &&
             records[i - 1].sub == records[i].sub &&
             records[i - 1].minZoom > records[i].minZoom)) return false;
    }
    return true;
}

static_assert(flagRecordsSorted(kPoiFlagRecords));

constexpr float officialPoiIconAnchor(float providerOffset) {
    return 10.0f - 20.0f * providerOffset / 24.0f;
}

size_t officialJavascriptStringLength(const std::string& utf8) {
    size_t utf16CodeUnits = 0;
    for (uint32_t codepoint : GlyphAtlas::decodeUtf8(utf8)) {
        utf16CodeUnits += codepoint > 0xFFFF ? 2u : 1u;
    }
    return utf16CodeUnits;
}

std::array<float, 4> color(uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xff) / 255.0f,
            static_cast<float>((argb >> 8) & 0xff) / 255.0f,
            static_cast<float>(argb & 0xff) / 255.0f,
            static_cast<float>((argb >> 24) & 0xff) / 255.0f};
}

template <typename Value>
StyleExpression::Ptr makeSteps(
    const std::vector<std::pair<double, Value>>& source) {
    std::vector<std::pair<double, StyleExpression::Ptr>> stops;
    stops.reserve(source.size());
    for (const auto& [zoom, value] : source)
        stops.emplace_back(zoom, StyleExpression::literal(value));
    return StyleExpression::step(amapClassicDiscreteZoom(), std::move(stops));
}

void installOfficialLabelRecords(FeatureRenderStyle& style, int classFilter) {
    std::map<int, std::vector<const PoiLabelRecord*>> grouped;
    for (const auto& record : kPoiLabelRecords) {
        if (record.styleGroup / 10000 == classFilter)
            grouped[record.styleGroup].push_back(&record);
    }
    style.labelStyleGroupPropertyA = "amap_class";
    style.labelStyleGroupPropertyB = "amap_subkey";
    for (const auto& [styleGroup, records] : grouped) {
        std::vector<const PoiLabelRecord*> ordered = records;
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->minZoom < b->minZoom;
        });
        std::vector<std::pair<double, float>> sizes;
        std::vector<std::pair<double, std::array<float, 4>>> colors, halos;
        for (const auto* record : ordered) {
            const double displayZoom = record->minZoom - 1.0;
            if (sizes.empty() || sizes.back().second != record->size)
                sizes.emplace_back(displayZoom, record->size);
            const auto text = color(record->colorArgb);
            const auto halo = color(record->haloArgb);
            if (colors.empty() || colors.back().second != text)
                colors.emplace_back(displayZoom, text);
            if (halos.empty() || halos.back().second != halo)
                halos.emplace_back(displayZoom, halo);
        }
        style.labelSizeExprByStyleGroup[styleGroup] = makeSteps(sizes);
        style.labelColorExprByStyleGroup[styleGroup] = makeSteps(colors);
        style.labelHaloColorExprByStyleGroup[styleGroup] = makeSteps(halos);
        style.labelHaloWidthExprByStyleGroup[styleGroup] =
            StyleExpression::literal(1.0);
        style.labelMinZoomByStyleGroup[styleGroup] = ordered.front()->minZoom - 1;
        style.labelMaxZoomByStyleGroup[styleGroup] = (*std::max_element(
            ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
                return a->maxZoom < b->maxZoom;
            }))->maxZoom;
        auto& windows = style.labelZoomWindowsByStyleGroup[styleGroup];
        for (const auto* record : ordered) {
            const std::pair<int, int> window{record->minZoom - 1,
                                              record->maxZoom};
            if (!windows.empty() && windows.back().second >= window.first) {
                windows.back().second =
                    std::max(windows.back().second, window.second);
            } else {
                windows.push_back(window);
            }
        }
        const int classCode = styleGroup / 10000;
        const int subKey = styleGroup % 10000;
        style.labelStyleGroupByProperty[std::to_string(classCode) + ":" +
                                        std::to_string(subKey)] = styleGroup;
    }
}

void installOfficialGuideRecords(FeatureRenderStyle& style) {
    for (const auto& record : kGuideLabelRecords) {
        const int styleGroup = -record.sub;
        style.labelStyleGroupByProperty["40001:" +
            std::to_string(record.sub)] = styleGroup;
        auto install = [&](auto& target, StyleExpression::Ptr value) {
            const auto found = target.find(styleGroup);
            if (found == target.end()) {
                target[styleGroup] = std::move(value);
            }
        };
        install(style.labelSizeExprByStyleGroup,
                StyleExpression::literal(record.size));
        install(style.labelColorExprByStyleGroup,
                StyleExpression::literal(color(record.colorArgb)));
        install(style.labelHaloColorExprByStyleGroup,
                StyleExpression::literal(std::array<float, 4>{0, 0, 0, 0}));
        install(style.labelHaloWidthExprByStyleGroup,
                StyleExpression::literal(0.0));
        const double minimum = record.minZoom - 1.0;
        const auto min = style.labelMinZoomByStyleGroup.find(styleGroup);
        if (min == style.labelMinZoomByStyleGroup.end() ||
            minimum < min->second) {
            style.labelMinZoomByStyleGroup[styleGroup] = minimum;
        }
        const auto max = style.labelMaxZoomByStyleGroup.find(styleGroup);
        if (max == style.labelMaxZoomByStyleGroup.end() ||
            record.maxZoom > max->second) {
            style.labelMaxZoomByStyleGroup[styleGroup] = record.maxZoom;
        }
    }
}

template <size_t N>
AmapClassicPoiIconStyle resolveIcon(const PoiIconRecord (&records)[N],
    int classCode, int subKey,
                                    double displayZoom, bool dynamic) {
    displayZoom = amapClassicDiscreteZoomValue(displayZoom);
    const auto first = std::lower_bound(
        std::begin(records), std::end(records), std::pair{classCode, subKey},
        [](const PoiIconRecord& row, const std::pair<int, int>& identity) {
            return std::pair{row.cls, row.sub} < identity;
        });
    for (auto it = first; it != std::end(records) &&
                          it->cls == classCode && it->sub == subKey; ++it) {
        const auto& row = *it;
        if (displayZoom < row.minZoom || displayZoom >= row.maxZoom) continue;
        if (!dynamic && row.atlas == 1 && row.index == 128) return {};
        using IconAnchor =
            FeatureRenderStyle::ProviderLabelLayout::IconAnchor;
        return {true, row.atlas, row.index, row.cellW, row.cellH,
                row.atlasW, row.atlasH, row.displayW, row.displayH,
                officialPoiIconAnchor(row.anchorOffsetX),
                officialPoiIconAnchor(row.anchorOffsetY),
                row.atlas == 9 ? IconAnchor::BottomCenter
                               : IconAnchor::NumericTopLeft};
    }
    return {};
}

template <size_t N>
bool hasIdentity(const PoiIconRecord (&records)[N], int cls, int sub) {
    const auto it = std::lower_bound(
        std::begin(records), std::end(records), std::pair{cls, sub},
        [](const PoiIconRecord& row, const std::pair<int, int>& identity) {
            return std::pair{row.cls, row.sub} < identity;
        });
    return it != std::end(records) && it->cls == cls && it->sub == sub;
}

}  // namespace

std::string AmapClassicPoiIconStyle::frameName() const {
    return enabled ? "amap-icons-" + std::to_string(atlas) + "-" +
                         std::to_string(iconIndex) : std::string();
}

AmapClassicPoiIconStyle resolveAmapClassicPoiIconStyle(
    int classCode, int subKey, double displayZoom) {
    if (resolveAmapClassicPoiDynamicBackgroundStyle(
            classCode, subKey, displayZoom).enabled) return {};
    return resolveIcon(kPoiIconRecords, classCode, subKey, displayZoom, false);
}

AmapClassicPoiIconStyle resolveAmapClassicPoiDynamicBackgroundStyle(
    int classCode, int subKey, double displayZoom) {
    return resolveIcon(kPoiDynamicTextBackgroundRecords, classCode, subKey,
                       displayZoom, true);
}

bool amapClassicPoiIconContractsValid() {
    std::map<std::pair<int, int>, AmapClassicPoiIconFrame> frames;
    const auto inspect = [&](const auto& records) {
        for (const auto& row : records) {
            if (row.atlas <= 0 || row.index <= 0 || row.cellW <= 0 ||
                row.cellH <= 0 || row.atlasW <= 0 || row.atlasH <= 0)
                return false;
            const int columns = row.atlasW / row.cellW;
            const int zeroBased = row.index - 1;
            if (columns <= 0 || (zeroBased % columns + 1) * row.cellW > row.atlasW ||
                (zeroBased / columns + 1) * row.cellH > row.atlasH) return false;
            const AmapClassicPoiIconFrame frame{row.atlas, row.index, row.cellW,
                                                row.cellH, row.atlasW, row.atlasH};
            auto [it, inserted] = frames.emplace(
                std::pair{row.atlas, row.index}, frame);
            if (!inserted && (it->second.cellWidth != row.cellW ||
                it->second.cellHeight != row.cellH ||
                it->second.atlasWidth != row.atlasW ||
                it->second.atlasHeight != row.atlasH)) return false;
        }
        return true;
    };
    return inspect(kPoiIconRecords) && inspect(kPoiDynamicTextBackgroundRecords);
}

bool isAmapClassicPoiIdentity(int classCode, int subKey) {
    const int target = amapClassicLabelIdentity(classCode, subKey);
    const auto labelIt = std::lower_bound(
        std::begin(kPoiLabelRecords), std::end(kPoiLabelRecords), target,
        [](const PoiLabelRecord& row, int identity) {
            return row.styleGroup < identity;
        });
    const bool label = labelIt != std::end(kPoiLabelRecords) &&
                       labelIt->styleGroup == target;
    return label || hasIdentity(kPoiIconRecords, classCode, subKey) ||
           hasIdentity(kPoiDynamicTextBackgroundRecords, classCode, subKey);
}

int amapClassicPoiFlags(int classCode, int subKey, double displayZoom) {
    displayZoom = amapClassicDiscreteZoomValue(displayZoom);
    const auto first = std::lower_bound(
        std::begin(kPoiFlagRecords), std::end(kPoiFlagRecords),
        std::pair<int, int>{classCode, subKey},
        [](const PoiFlagRecord& row, const std::pair<int, int>& key) {
            return row.cls < key.first ||
                   (row.cls == key.first && row.sub < key.second);
        });
    for (auto it = first; it != std::end(kPoiFlagRecords) &&
                          it->cls == classCode && it->sub == subKey; ++it) {
        if (displayZoom >= it->minZoom && displayZoom < it->maxZoom)
            return it->flags;
    }
    return 0;
}

bool amapClassicPoiCanCovered(int classCode, int subKey,
                              double displayZoom) {
    return (amapClassicPoiFlags(classCode, subKey, displayZoom) & 1) != 0;
}

std::vector<AmapClassicPoiIconFrame> amapClassicPoiIconFrames(int atlas) {
    std::vector<AmapClassicPoiIconFrame> out;
    if (!amapClassicPoiIconContractsValid()) return out;
    const auto append = [&](const auto& records) {
        for (const auto& row : records) {
            if (row.atlas != atlas) continue;
            if (std::any_of(out.begin(), out.end(), [&](const auto& frame) {
                    return frame.iconIndex == row.index;
                })) continue;
            out.push_back({row.atlas, row.index, row.cellW, row.cellH,
                           row.atlasW, row.atlasH});
        }
    };
    append(kPoiIconRecords);
    append(kPoiDynamicTextBackgroundRecords);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.iconIndex < b.iconIndex;
    });
    return out;
}

std::vector<int> amapClassicPoiIconAtlases() {
    std::vector<int> out;
    const auto append = [&](const auto& records) {
        for (const auto& row : records)
            if (std::find(out.begin(), out.end(), row.atlas) == out.end())
                out.push_back(row.atlas);
    };
    append(kPoiIconRecords);
    append(kPoiDynamicTextBackgroundRecords);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<int, int>> amapClassicPoiIdentities() {
    std::vector<std::pair<int, int>> out;
    const auto append = [&](int cls, int sub) {
        const std::pair<int, int> identity{cls, sub};
        if (std::find(out.begin(), out.end(), identity) == out.end())
            out.push_back(identity);
    };
    for (const auto& row : kPoiLabelRecords)
        append(row.styleGroup / 10000, row.styleGroup % 10000);
    for (const auto& row : kPoiIconRecords) append(row.cls, row.sub);
    for (const auto& row : kPoiDynamicTextBackgroundRecords)
        append(row.cls, row.sub);
    std::sort(out.begin(), out.end());
    return out;
}

#ifdef EARTH_ENGINE_TESTING
std::vector<AmapClassicPoiLabelRecordForTest>
amapClassicPoiLabelRecordsForTest() {
    std::vector<AmapClassicPoiLabelRecordForTest> out;
    out.reserve(std::size(kPoiLabelRecords));
    for (const auto& row : kPoiLabelRecords) {
        out.push_back({row.styleGroup / 10000, row.styleGroup % 10000,
                       row.minZoom, row.maxZoom, row.size,
                       color(row.colorArgb), color(row.haloArgb)});
    }
    return out;
}
#endif

void AmapClassicStyleContract::applyPoi(FeatureRenderStyle& style) {
    style.installAmapClassicScope(FeatureRenderStyle::AmapClassicScope::Poi);
    style.visibilityZoomCeilFraction = 0.8;
    installOfficialGuideRecords(style);
    installOfficialLabelRecords(style, 10002);
    std::vector<int> officialPointClasses;
    for (const auto& record : kPoiLabelRecords) {
        const int classCode = record.styleGroup / 10000;
        if (std::find(officialPointClasses.begin(), officialPointClasses.end(),
                      classCode) == officialPointClasses.end()) {
            officialPointClasses.push_back(classCode);
        }
    }
    for (const int classCode : officialPointClasses) {
        if (classCode != 10002) installOfficialLabelRecords(style, classCode);
    }
    style.pointColorExpr.reset();
    style.pointImageExpr.reset();
    style.pointSizeExpr.reset();
    style.pointImage.clear();
    style.labelOffsetExpr.reset();
    style.pointStylePropertyA = "amap_class";
    style.pointStylePropertyB = "amap_subkey";
    style.pointIdentityValidator = [](const std::string& cls,
                                      const std::string& sub) {
        char *ce = nullptr, *se = nullptr;
        const long c = std::strtol(cls.c_str(), &ce, 10);
        const long s = std::strtol(sub.c_str(), &se, 10);
        return ce && *ce == '\0' && se && *se == '\0' &&
               c >= std::numeric_limits<int>::min() &&
               c <= std::numeric_limits<int>::max() &&
               s >= std::numeric_limits<int>::min() &&
               s <= std::numeric_limits<int>::max() &&
               isAmapClassicPoiIdentity(static_cast<int>(c), static_cast<int>(s));
    };
    style.pointStyleResolver = [](const std::string& cls,
                                  const std::string& sub,
                                  const std::string& name,
                                  double displayZoom, float officialScale) {
        FeatureRenderStyle::ResolvedPointStyle out;
        char *ce = nullptr, *se = nullptr;
        const long c = std::strtol(cls.c_str(), &ce, 10);
        const long s = std::strtol(sub.c_str(), &se, 10);
        if (!ce || *ce || !se || *se || c < std::numeric_limits<int>::min() ||
            c > std::numeric_limits<int>::max() ||
            s < std::numeric_limits<int>::min() ||
            s > std::numeric_limits<int>::max()) return out;
        const int classCode = static_cast<int>(c), subKey = static_cast<int>(s);
        out.labelLayout.emplace();
        for (const auto& record : kPoiCityWindowRecords) {
            if (name == record.name) {
                out.minZoom = record.minZoom;
                out.maxZoom = record.maxZoom;
                break;
            }
        }
        const auto icon = resolveAmapClassicPoiIconStyle(classCode, subKey, displayZoom);
        const auto dynamic = resolveAmapClassicPoiDynamicBackgroundStyle(
            classCode, subKey, displayZoom);
        out.enabled = icon.enabled;
        out.image = icon.frameName();
        out.officialIconAtlas = icon.atlas;
        out.sizePx = icon.displayHeightPx;
        out.color = {1, 1, 1, 1};
        const int flags = amapClassicPoiFlags(classCode, subKey, displayZoom);
        out.officialCanCovered = (flags & 1) != 0;
        out.labelLayout->iconWidthPx = icon.displayWidthPx;
        out.labelLayout->iconHeightPx = icon.displayHeightPx;
        out.labelLayout->iconAnchorXPx = icon.anchorXPx;
        out.labelLayout->iconAnchorYPx = icon.anchorYPx;
        out.labelLayout->iconAnchor = icon.anchor;
        if (classCode == 40001 && icon.enabled) {
            // NebulaLabelFormat.oV reads the binary Support.scale contract:
            // T=max(1, x.length/4), where JavaScript String.length counts
            // UTF-16 code units rather than Unicode scalar values. Retina
            // (>1) uses 24*T CSS px; 1x uses 24*9*T/7 CSS px.
            const float units = std::max(1.0f,
                static_cast<float>(officialJavascriptStringLength(name)) /
                    4.0f);
            out.labelLayout->iconWidthPx = 24.0f * units *
                (officialScale > 1.0f ? 1.0f : 9.0f / 7.0f);
            out.labelLayout->iconHeightPx = 24.0f;
            out.labelLayout->iconAnchorXPx =
                out.labelLayout->iconWidthPx * 0.5f;
            out.labelLayout->iconAnchorYPx = 12.0f;
        }
        if (dynamic.enabled) {
            out.labelLayout->dynamicBackgroundImage = dynamic.frameName();
            out.officialDynamicBackgroundAtlas = dynamic.atlas;
            out.labelLayout->iconAnchorXPx = dynamic.anchorXPx;
            out.labelLayout->iconAnchorYPx = dynamic.anchorYPx;
            out.labelLayout->iconAnchor = dynamic.anchor;
        }
        using Direction = FeatureRenderStyle::LabelDirection;
        const auto set = [&](Direction direction, float x, float y) {
            out.labelLayout->direction = direction;
            out.labelLayout->offsetXPx = x;
            out.labelLayout->offsetYPx = y;
        };
        bool officialDirection = false;
        for (const auto& record : kPoiDirectionRecords) {
            if (name != record.name) continue;
            set(static_cast<Direction>(record.direction), record.x, record.y);
            officialDirection = true;
            break;
        }
        if (!officialDirection) {
            if (classCode == 40001) {
                set(Direction::Center, 0, 0);
            } else
            if (dynamic.enabled) {
                // Official kgt(q8t) keeps the measured text centered inside
                // the stretched background and applies offset [0,-1]. It is
                // not an ordinary fixed icon with bottom-placed text.
                set(Direction::Center, 0, -1);
            } else if (icon.enabled) {
                if (!kPoiAccessOversea) return out;
                set(Direction::Bottom, 0, 0);
            } else {
                set(Direction::Center, 0, 0);
            }
        }
        return out;
    };
}

}  // namespace earth_engine
