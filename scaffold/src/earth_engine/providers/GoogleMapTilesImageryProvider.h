#pragma once

#include "XYZImageryProvider.h"

#include <string>

namespace earth_engine {

struct GoogleMapTilesExistingSessionOptions {
    std::string key;
    std::string session;
    std::string apiBaseUrl = "https://tile.googleapis.com/";
    int maximumLevel = 28;
    int tileWidth = 256;
    int tileHeight = 256;
    bool showLogo = true;
};

class GoogleMapTilesImageryProvider : public XYZImageryProvider {
public:
    explicit GoogleMapTilesImageryProvider(
        GoogleMapTilesExistingSessionOptions options,
        std::string attribution = "");

    std::string id() const override;
    std::string type() const override { return "google-map-tiles-imagery"; }
    bool supportsTile(const TileKey& key) const override;
    std::string buildUrl(const TileKey& key) const override;

    const GoogleMapTilesExistingSessionOptions& options() const {
        return options_;
    }

private:
    GoogleMapTilesExistingSessionOptions options_;
};

} // namespace earth_engine
