#include "GeoJsonImporter.h"

#include "GeoJsonParser.h"

namespace earth_engine {

namespace {

GeometryType mapType(GeoFeature::Type t) {
    switch (t) {
        case GeoFeature::Type::Point:      return GeometryType::Point;
        case GeoFeature::Type::LineString: return GeometryType::LineString;
        case GeoFeature::Type::Polygon:    return GeometryType::Polygon;
    }
    return GeometryType::Point;
}

} // namespace

size_t GeoJsonImporter::importInto(const std::string& geoJsonText,
                                   FeatureStore& store) {
    std::vector<GeoFeature> parsed = GeoJsonParser::parse(geoJsonText);
    size_t imported = 0;
    for (auto& gf : parsed) {
        Feature f;
        f.id = kInvalidFeatureId;          // store 分配稳定 ID
        f.sourceId = gf.id;                // 保留源 feature.id 原值
        f.type = mapType(gf.type);
        f.rings = std::move(gf.rings);
        f.properties = std::move(gf.properties);
        f.bounds = gf.bounds;              // parser 已算(空则 store 兜底重算)
        f.version = 1;
        store.addFeature(std::move(f));
        ++imported;
    }
    return imported;
}

} // namespace earth_engine
