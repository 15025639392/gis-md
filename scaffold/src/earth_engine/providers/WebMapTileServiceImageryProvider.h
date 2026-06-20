#pragma once

#include "XYZImageryProvider.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

struct WebMapTileServiceImageryOptions {
    std::optional<std::string> format;
    std::vector<std::string> subdomains;
    std::string layer;
    std::string style;
    std::string tileMatrixSetId;
    std::optional<std::vector<std::string>> tileMatrixLabels;
    std::optional<std::map<std::string, std::string>> dimensions;
    int minimumLevel = 0;
    int maximumLevel = 25;
    int tileWidth = 256;
    int tileHeight = 256;
};

/// Web Map Tile Service imagery provider aligned with cesium-native's WMTS
/// URL selection and template substitution semantics.
class WebMapTileServiceImageryProvider : public XYZImageryProvider {
public:
    WebMapTileServiceImageryProvider(
        std::string url,
        WebMapTileServiceImageryOptions options = {},
        std::string attribution = "");

    std::string id() const override;
    std::string type() const override { return "wmts-imagery"; }

    std::string buildUrl(const TileKey& key) const override;
    bool supportsTile(const TileKey& key) const override;

    bool usesKeyValueParameters() const { return useKvp_; }
    const WebMapTileServiceImageryOptions& options() const { return options_; }

private:
    std::string url_;
    WebMapTileServiceImageryOptions options_;
    bool useKvp_ = true;
};

} // namespace earth_engine
