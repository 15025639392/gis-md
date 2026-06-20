#pragma once

#include "XYZImageryProvider.h"

#include <string>

namespace earth_engine {

struct WebMapServiceImageryOptions {
    std::string version = "1.3.0";
    std::string layers;
    std::string format = "image/png";
    int minimumLevel = 0;
    int maximumLevel = 14;
    int tileWidth = 256;
    int tileHeight = 256;
};

struct WebMapServiceCapabilitiesValidation {
    bool valid = false;
    std::string error;
};

/// Web Map Service imagery provider aligned with cesium-native's WMS
/// GetMap URL construction. Network loading and image decoding are inherited
/// from XYZImageryProvider.
class WebMapServiceImageryProvider : public XYZImageryProvider {
public:
    WebMapServiceImageryProvider(std::string baseUrl,
                                 WebMapServiceImageryOptions options = {},
                                 std::string attribution = "");

    std::string id() const override;
    std::string type() const override { return "wms-imagery"; }

    std::string buildUrl(const TileKey& key) const override;
    bool supportsTile(const TileKey& key) const override;

    const std::string& baseUrl() const { return baseUrl_; }
    const WebMapServiceImageryOptions& options() const { return options_; }

private:
    std::string baseUrl_;
    WebMapServiceImageryOptions options_;
};

WebMapServiceCapabilitiesValidation validateWebMapServiceCapabilities(
    const std::string& capabilitiesXml,
    const WebMapServiceImageryOptions& options);

std::string webMapServiceCapabilitiesUrl(
    const std::string& baseUrl,
    const std::string& version = "1.3.0");

} // namespace earth_engine
