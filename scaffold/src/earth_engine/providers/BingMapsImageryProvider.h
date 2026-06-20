#pragma once

#include "XYZImageryProvider.h"
#include "../tiling/TileScheme.h"

#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

class BingMapsImageryProvider;

struct BingMapsImageryOptions {
    std::string culture;
    std::vector<std::string> subdomains;
    int minimumLevel = 0;
    int maximumLevel = 30;
    int tileWidth = 256;
    int tileHeight = 256;
};

struct BingMapsCreditCoverageArea {
    double southDegrees = 0.0;
    double westDegrees = 0.0;
    double northDegrees = 0.0;
    double eastDegrees = 0.0;
    int zoomMin = 0;
    int zoomMax = 0;
};

struct BingMapsCredit {
    std::string attribution;
    std::vector<BingMapsCreditCoverageArea> coverageAreas;
};

struct BingMapsMetadata {
    std::string imageUrl;
    std::vector<std::string> imageUrlSubdomains;
    int imageWidth = 256;
    int imageHeight = 256;
    int zoomMax = 30;
    std::vector<BingMapsCredit> credits;
};

struct BingMapsMetadataParseResult {
    bool valid = false;
    BingMapsMetadata metadata;
    std::string error;
};

struct BingMapsImagerySource {
    std::unique_ptr<BingMapsImageryProvider> provider;
    std::unique_ptr<TileScheme> scheme;
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

BingMapsMetadataParseResult parseBingMapsMetadata(
    const std::string& metadataJson);

BingMapsImagerySource createBingMapsImagerySource(
    std::string baseUrl,
    const BingMapsMetadata& metadata,
    std::string culture = "",
    std::string attribution = "");

} // namespace earth_engine
