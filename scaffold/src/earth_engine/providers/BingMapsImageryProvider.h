#pragma once

#include "XYZImageryProvider.h"

#include <string>
#include <vector>

namespace earth_engine {

struct BingMapsImageryOptions {
    std::string culture;
    std::vector<std::string> subdomains;
    int minimumLevel = 0;
    int maximumLevel = 30;
    int tileWidth = 256;
    int tileHeight = 256;
};

class BingMapsImageryProvider : public XYZImageryProvider {
public:
    BingMapsImageryProvider(std::string baseUrl,
                            std::string urlTemplate,
                            BingMapsImageryOptions options = {},
                            std::string attribution = "");

    std::string id() const override;
    std::string type() const override { return "bing-maps-imagery"; }
    bool supportsTile(const TileKey& key) const override;
    std::string buildUrl(const TileKey& key) const override;

    const std::string& baseUrl() const { return baseUrl_; }
    const std::string& urlTemplate() const { return urlTemplate_; }
    const BingMapsImageryOptions& options() const { return options_; }

    static std::string tileXYToQuadKey(int level, int x, int y);

private:
    std::string baseUrl_;
    std::string urlTemplate_;
    BingMapsImageryOptions options_;
};

std::string bingMapsMetadataUrl(const std::string& baseUrl,
                                const std::string& mapStyle,
                                const std::string& key,
                                const std::string& culture = "");

} // namespace earth_engine
