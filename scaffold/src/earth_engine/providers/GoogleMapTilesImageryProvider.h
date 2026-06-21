#pragma once

#include "XYZImageryProvider.h"
#include "../tiling/TileScheme.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

class GoogleMapTilesImageryProvider;

struct GoogleMapTilesExistingSessionOptions {
    std::string key;
    std::string session;
    std::string apiBaseUrl = "https://tile.googleapis.com/";
    int maximumLevel = 28;
    int tileWidth = 256;
    int tileHeight = 256;
    bool showLogo = true;
};

struct GoogleMapTilesNewSessionOptions {
    std::string key;
    std::string mapType = "satellite";
    std::string language = "en-US";
    std::string region = "US";
    std::optional<std::string> imageFormat;
    std::optional<std::string> scale;
    std::optional<bool> highDpi;
    std::optional<std::vector<std::string>> layerTypes;
    std::optional<nlohmann::json> styles;
    std::optional<bool> overlay;
    std::string apiBaseUrl = "https://tile.googleapis.com/";
};

struct GoogleMapTilesSessionParseResult {
    bool valid = false;
    GoogleMapTilesExistingSessionOptions session;
    std::string error;
};

struct GoogleMapTilesViewportRect {
    int maxZoom = -1;
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;
};

struct GoogleMapTilesViewportParseResult {
    bool valid = false;
    std::vector<GoogleMapTilesViewportRect> maxZoomRects;
    bool complete = false;
    std::string error;
};

struct GoogleMapTilesTileRange {
    int level = 0;
    int minimumX = 0;
    int minimumY = 0;
    int maximumX = 0;
    int maximumY = 0;
};

struct GoogleMapTilesImagerySource {
    std::unique_ptr<GoogleMapTilesImageryProvider> provider;
    std::unique_ptr<TileScheme> scheme;
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
    void addAvailableTileRanges(
        const std::vector<GoogleMapTilesTileRange>& ranges);
    void addCompleteAvailabilityRanges(
        const std::vector<GoogleMapTilesTileRange>& ranges);
    void applyViewportAvailability(
        const GoogleMapTilesViewportParseResult& viewport,
        const TileKey& requestedKey);
    bool hasKnownAvailability() const;
    bool isTileKnownAvailable(const TileKey& key) const;
    bool isTileInCompleteAvailabilityRange(const TileKey& key) const;

    const GoogleMapTilesExistingSessionOptions& options() const {
        return options_;
    }

private:
    GoogleMapTilesExistingSessionOptions options_;
    std::vector<GoogleMapTilesTileRange> availableRanges_;
    std::vector<GoogleMapTilesTileRange> completeAvailabilityRanges_;
};

std::string googleMapTilesCreateSessionUrl(
    const GoogleMapTilesNewSessionOptions& options);

std::string googleMapTilesCreateSessionPayload(
    const GoogleMapTilesNewSessionOptions& options);

GoogleMapTilesSessionParseResult parseGoogleMapTilesCreateSessionResponse(
    const std::string& responseJson,
    const GoogleMapTilesNewSessionOptions& requestOptions);

std::string googleMapTilesViewportUrl(
    const GoogleMapTilesExistingSessionOptions& options,
    int zoom,
    double west,
    double south,
    double east,
    double north);

GoogleMapTilesViewportParseResult parseGoogleMapTilesViewportResponse(
    const std::string& responseJson);

std::vector<GoogleMapTilesTileRange> googleMapTilesViewportTileRanges(
    const GoogleMapTilesViewportParseResult& viewport);

GoogleMapTilesImagerySource createGoogleMapTilesImagerySource(
    GoogleMapTilesExistingSessionOptions options,
    std::string attribution = "");

} // namespace earth_engine
