#include "MinimalGlobeDemoLayers.h"

#include "earth_engine/Engine.h"
#include "earth_engine/data/GeoJsonParser.h"
#include "earth_engine/layers/VectorLayer.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/style/OverlayStyle.h"

#include <android/log.h>
#include <memory>
#include <utility>

#define LOG_TAG "MinimalGlobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace earth_engine::minimal_globe_demo {

void addDemoVectorLayer(Engine& engine, RenderDevice& renderDevice) {
    static constexpr const char* kDemoGeoJson = R"json(
{
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "id": "beijing-marker",
      "properties": { "name": "Beijing" },
      "geometry": { "type": "Point", "coordinates": [116.3913, 39.9075, 0] }
    },
    {
      "type": "Feature",
      "id": "demo-route",
      "properties": { "name": "Demo route" },
      "geometry": {
        "type": "LineString",
        "coordinates": [[116.30, 39.86], [116.39, 39.91], [116.48, 39.95]]
      }
    }
  ]
}
)json";

    auto features = GeoJsonParser::parse(kDemoGeoJson);
    if (features.empty()) {
        LOGE("Demo GeoJSON parse failed");
        return;
    }

    auto style = makeDefaultPointStyle();
    style.layer.geometry = PointStyle{
        PointStyle::Shape::Circle,
        12.0f,
        Color{0.95f, 0.20f, 0.12f, 1.0f},
        Color::white(),
        2.0f,
        true,
        true
    };

    auto vectorLayer = std::make_unique<VectorLayer>(
        "android-demo-vector",
        std::move(features),
        style,
        &renderDevice);
    engine.addVectorLayer(std::move(vectorLayer));
    LOGI("Demo vector layer added");
}

} // namespace earth_engine::minimal_globe_demo
