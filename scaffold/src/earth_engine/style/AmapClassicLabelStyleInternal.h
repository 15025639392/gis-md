#pragma once

#include "earth_engine/layers/FeatureRenderLayer.h"

#include <array>
#include <vector>

namespace earth_engine {

// Runtime-private encoding and lookup types for the generated official
// field-5 contract. Production callers consume them only through the sealed
// AmapClassicRuntime; this header is also available to focused contract tests.
constexpr int amapClassicLabelIdentity(int classCode, int subKey) {
    return classCode * 10000 + subKey;
}

struct AmapClassicPoiIconStyle {
    bool enabled = false;
    int atlas = 0;
    int iconIndex = 0;
    int cellWidth = 0;
    int cellHeight = 0;
    int atlasWidth = 0;
    int atlasHeight = 0;
    float displayWidthPx = 0.0f;
    float displayHeightPx = 0.0f;
    float anchorXPx = 0.0f;
    float anchorYPx = 0.0f;
    FeatureRenderStyle::ProviderLabelLayout::IconAnchor anchor =
        FeatureRenderStyle::ProviderLabelLayout::IconAnchor::NumericTopLeft;

    std::string frameName() const;
};

struct AmapClassicPoiIconFrame {
    int atlas = 0;
    int iconIndex = 0;
    int cellWidth = 0;
    int cellHeight = 0;
    int atlasWidth = 0;
    int atlasHeight = 0;
};

AmapClassicPoiIconStyle resolveAmapClassicPoiIconStyle(
    int classCode, int subKey, double displayZoom);
AmapClassicPoiIconStyle resolveAmapClassicPoiDynamicBackgroundStyle(
    int classCode, int subKey, double displayZoom);
std::vector<AmapClassicPoiIconFrame> amapClassicPoiIconFrames(int atlas);
bool amapClassicPoiIconContractsValid();
int amapClassicPoiFlags(int classCode, int subKey, double displayZoom);
bool amapClassicPoiCanCovered(int classCode, int subKey,
                              double displayZoom);
std::vector<int> amapClassicPoiIconAtlases();
std::vector<std::pair<int, int>> amapClassicPoiIdentities();

struct AmapClassicPoiLabelRecordForTest {
    int classCode = 0;
    int subKey = 0;
    int minZoom = 0;
    int maxZoom = 0;
    float sizePx = 0.0f;
    std::array<float, 4> color{};
    std::array<float, 4> haloColor{};
};

std::vector<AmapClassicPoiLabelRecordForTest>
amapClassicPoiLabelRecordsForTest();

}  // namespace earth_engine
