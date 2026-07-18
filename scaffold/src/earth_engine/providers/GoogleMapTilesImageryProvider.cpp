#include "GoogleMapTilesImageryProvider.h"

#include "../core/geodesy/WebMercatorProjection.h"
#include "../core/math/MathUtils.h"
#include "../core/math/Rectangle.h"
#include "../platform/bridge/CurlMultiRequestScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

std::string escapeQueryValue(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 0x0f]);
        }
    }
    return result;
}

std::string ensureTrailingSlash(std::string url) {
    if (url.empty() || url.back() == '/') {
        return url;
    }
    return url + "/";
}

std::string withQuery(
    std::string url,
    const std::vector<std::pair<std::string, std::string>>& query) {
    std::string fragment;
    const size_t fragmentStart = url.find('#');
    if (fragmentStart != std::string::npos) {
        fragment = url.substr(fragmentStart);
        url.erase(fragmentStart);
    }

    url += url.find('?') == std::string::npos ? '?' : '&';
    for (size_t i = 0; i < query.size(); ++i) {
        if (i > 0) {
            url.push_back('&');
        }
        url += query[i].first;
        url.push_back('=');
        url += escapeQueryValue(query[i].second);
    }
    return url + fragment;
}

int invertedY(int level, int y) {
    if (level < 0 || level > 30) {
        return y;
    }
    return static_cast<int>((int64_t{1} << level) - 1 - y);
}

bool tileInRanges(const TileKey& key,
                  const std::vector<GoogleMapTilesTileRange>& ranges,
                  const std::string& schemeId) {
    if (key.schemeId != schemeId) {
        return false;
    }
    for (const GoogleMapTilesTileRange& range : ranges) {
        if (range.level == key.z &&
            key.x >= range.minimumX && key.x <= range.maximumX &&
            key.y >= range.minimumY && key.y <= range.maximumY) {
            return true;
        }
    }
    return false;
}

std::string trimCopy(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> splitCredits(const std::string& value) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const size_t end = comma == std::string::npos ? value.size() : comma;
        std::string part = trimCopy(value.substr(start, end - start));
        if (!part.empty()) {
            parts.push_back(std::move(part));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return parts;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

int safeIntOrDefault(const nlohmann::json& value, int defaultValue) {
    if (value.is_number_integer()) {
        const int64_t parsed = value.get<int64_t>();
        if (parsed >= std::numeric_limits<int>::min() &&
            parsed <= std::numeric_limits<int>::max()) {
            return static_cast<int>(parsed);
        }
        return defaultValue;
    }
    if (value.is_number_unsigned()) {
        const uint64_t parsed = value.get<uint64_t>();
        if (parsed <= static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return static_cast<int>(parsed);
        }
        return defaultValue;
    }
    if (value.is_number_float()) {
        const double parsed = value.get<double>();
        if (!std::isfinite(parsed) ||
            parsed < static_cast<double>(std::numeric_limits<int>::min()) ||
            parsed > static_cast<double>(std::numeric_limits<int>::max())) {
            return defaultValue;
        }
        const int narrowed = static_cast<int>(parsed);
        return static_cast<double>(narrowed) == parsed
            ? narrowed
            : defaultValue;
    }
    return defaultValue;
}

} // namespace

std::string googleMapTilesCreateSessionUrl(
    const GoogleMapTilesNewSessionOptions& options) {
    const std::string url = ensureTrailingSlash(options.apiBaseUrl) +
        "v1/createSession";
    if (options.key.empty()) {
        return url;
    }
    return withQuery(url, {{"key", options.key}});
}

std::string googleMapTilesCreateSessionPayload(
    const GoogleMapTilesNewSessionOptions& options) {
    nlohmann::json payload = {
        {"mapType", options.mapType},
        {"language", options.language},
        {"region", options.region}};
    if (options.imageFormat) {
        payload["imageFormat"] = *options.imageFormat;
    }
    if (options.scale) {
        payload["scale"] = *options.scale;
    }
    if (options.highDpi) {
        payload["highDpi"] = *options.highDpi;
    }
    if (options.layerTypes) {
        payload["layerTypes"] = *options.layerTypes;
    }
    if (options.styles) {
        payload["styles"] = *options.styles;
    }
    if (options.overlay) {
        payload["overlay"] = *options.overlay;
    }
    return payload.dump();
}

GoogleMapTilesSessionParseResult parseGoogleMapTilesCreateSessionResponse(
    const std::string& responseJson,
    const GoogleMapTilesNewSessionOptions& requestOptions) {
    nlohmann::json response =
        nlohmann::json::parse(responseJson, nullptr, false);
    if (response.is_discarded()) {
        return GoogleMapTilesSessionParseResult{
            false,
            GoogleMapTilesExistingSessionOptions{},
            "Failed to parse response from Google Map Tiles API createSession service:"};
    }
    if (!response.is_object()) {
        return GoogleMapTilesSessionParseResult{
            false,
            GoogleMapTilesExistingSessionOptions{},
            "Response from Google Map Tiles API createSession service was not a JSON object."};
    }

    const auto sessionIt = response.find("session");
    if (sessionIt == response.end() || !sessionIt->is_string()) {
        return GoogleMapTilesSessionParseResult{
            false,
            GoogleMapTilesExistingSessionOptions{},
            "Response from Google Map Tiles API createSession service did not contain a valid 'session' property."};
    }

    const auto tileWidthIt = response.find("tileWidth");
    if (tileWidthIt == response.end() || !tileWidthIt->is_number()) {
        return GoogleMapTilesSessionParseResult{
            false,
            GoogleMapTilesExistingSessionOptions{},
            "Response from Google Map Tiles API createSession service did not contain a valid 'tileWidth' property."};
    }

    const auto tileHeightIt = response.find("tileHeight");
    if (tileHeightIt == response.end() || !tileHeightIt->is_number()) {
        return GoogleMapTilesSessionParseResult{
            false,
            GoogleMapTilesExistingSessionOptions{},
            "Response from Google Map Tiles API createSession service did not contain a valid 'tileHeight' property."};
    }

    GoogleMapTilesExistingSessionOptions session;
    session.key = requestOptions.key;
    session.session = sessionIt->get<std::string>();
    session.apiBaseUrl = ensureTrailingSlash(requestOptions.apiBaseUrl);
    session.maximumLevel = 28;
    session.tileWidth = safeIntOrDefault(*tileWidthIt, 256);
    session.tileHeight = safeIntOrDefault(*tileHeightIt, 256);
    session.showLogo = true;

    return GoogleMapTilesSessionParseResult{
        true,
        std::move(session),
        std::string()};
}

std::string googleMapTilesViewportUrl(
    const GoogleMapTilesExistingSessionOptions& options,
    int zoom,
    double west,
    double south,
    double east,
    double north) {
    std::ostringstream westString;
    std::ostringstream southString;
    std::ostringstream eastString;
    std::ostringstream northString;
    westString << west;
    southString << south;
    eastString << east;
    northString << north;
    return withQuery(
        ensureTrailingSlash(options.apiBaseUrl) + "tile/v1/viewport",
        {{"session", options.session},
         {"key", options.key},
         {"zoom", std::to_string(zoom)},
         {"west", westString.str()},
         {"south", southString.str()},
         {"east", eastString.str()},
         {"north", northString.str()}});
}

GoogleMapTilesViewportParseResult parseGoogleMapTilesViewportResponse(
    const std::string& responseJson) {
    nlohmann::json response =
        nlohmann::json::parse(responseJson, nullptr, false);
    if (response.is_discarded()) {
        return GoogleMapTilesViewportParseResult{
            false,
            {},
            false,
            "Error when parsing Google Map Tiles API viewport service JSON."};
    }
    if (!response.is_object()) {
        return GoogleMapTilesViewportParseResult{
            false,
            {},
            false,
            "Google Map Tiles API viewport service JSON was not an object."};
    }

    const auto rectsIt = response.find("maxZoomRects");
    if (rectsIt == response.end() || !rectsIt->is_array()) {
        return GoogleMapTilesViewportParseResult{
            false,
            {},
            false,
            "Google Map Tiles API viewport service JSON is missing the `maxZoomRects` property."};
    }

    GoogleMapTilesViewportParseResult result;
    result.valid = true;
    result.complete = rectsIt->size() < 100;
    for (const nlohmann::json& rectJson : *rectsIt) {
        if (!rectJson.is_object()) {
            continue;
        }
        const auto maxZoomIt = rectJson.find("maxZoom");
        const auto westIt = rectJson.find("west");
        const auto southIt = rectJson.find("south");
        const auto eastIt = rectJson.find("east");
        const auto northIt = rectJson.find("north");
        if (maxZoomIt == rectJson.end() || !maxZoomIt->is_number() ||
            westIt == rectJson.end() || !westIt->is_number() ||
            southIt == rectJson.end() || !southIt->is_number() ||
            eastIt == rectJson.end() || !eastIt->is_number() ||
            northIt == rectJson.end() || !northIt->is_number()) {
            continue;
        }

        const int maxZoom = safeIntOrDefault(*maxZoomIt, -1);
        if (maxZoom < 0) {
            continue;
        }
        result.maxZoomRects.push_back(GoogleMapTilesViewportRect{
            maxZoom,
            westIt->get<double>(),
            southIt->get<double>(),
            eastIt->get<double>(),
            northIt->get<double>()});
    }
    return result;
}

std::string parseGoogleMapTilesViewportCopyright(
    const std::string& responseJson) {
    nlohmann::json response =
        nlohmann::json::parse(responseJson, nullptr, false);
    if (response.is_discarded() || !response.is_object()) {
        return {};
    }
    const auto copyrightIt = response.find("copyright");
    if (copyrightIt == response.end() || !copyrightIt->is_string()) {
        return {};
    }
    return copyrightIt->get<std::string>();
}

std::string combineGoogleMapTilesCredits(
    const std::vector<std::string>& copyrights) {
    static const std::string copyrightPrefix = "Imagery ©";
    std::set<std::string> uniqueCredits;
    std::vector<std::string> credits;
    std::string preamble;

    for (std::string creditString : copyrights) {
        if (creditString.empty()) {
            continue;
        }
        if (startsWith(creditString, copyrightPrefix)) {
            const size_t firstNonSpace =
                creditString.find_first_not_of(' ', copyrightPrefix.size());
            const size_t firstNonNumberAfterSpace =
                firstNonSpace == std::string::npos
                    ? std::string::npos
                    : creditString.find_first_not_of(
                          "0123456789",
                          firstNonSpace);
            if (firstNonNumberAfterSpace != std::string::npos) {
                if (preamble.empty()) {
                    preamble =
                        creditString.substr(0, firstNonNumberAfterSpace);
                }
                creditString = creditString.substr(firstNonNumberAfterSpace);
            }
        }

        std::vector<std::string> parts = splitCredits(creditString);
        for (size_t partIndex = 0; partIndex < parts.size(); ++partIndex) {
            std::string credit = parts[partIndex];
            if (partIndex != parts.size() - 1 &&
                parts[partIndex + 1] == "Inc.") {
                credit += ", Inc.";
                ++partIndex;
            }
            if (uniqueCredits.insert(credit).second) {
                credits.push_back(std::move(credit));
            }
        }
    }

    std::string joined;
    for (size_t i = 0; i < credits.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += credits[i];
    }
    if (!preamble.empty()) {
        if (joined.empty()) {
            return trimCopy(preamble);
        }
        joined = trimCopy(preamble) + " " + joined;
    }
    return joined;
}

std::vector<GoogleMapTilesTileRange> googleMapTilesViewportTileRanges(
    const GoogleMapTilesViewportParseResult& viewport) {
    if (!viewport.valid) {
        return {};
    }

    std::vector<GoogleMapTilesTileRange> ranges;
    const std::unique_ptr<TileScheme> scheme =
        TileScheme::createXYZWebMercator();
    for (const GoogleMapTilesViewportRect& rect : viewport.maxZoomRects) {
        if (rect.maxZoom < 0 || rect.maxZoom > 30) {
            continue;
        }

        const double maxLatitudeDegrees =
            MathUtils::radiansToDegrees(
                WebMercatorProjection::maximumLatitude());
        const double south =
            std::clamp(rect.south, -maxLatitudeDegrees, maxLatitudeDegrees);
        const double north =
            std::clamp(rect.north, -maxLatitudeDegrees, maxLatitudeDegrees);
        if (south > north) {
            continue;
        }

        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;
        scheme->tileRange(
            Rectangle::fromDegrees(rect.west, south, rect.east, north),
            rect.maxZoom,
            minX,
            minY,
            maxX,
            maxY);

        for (int level = rect.maxZoom; level >= 0; --level) {
            ranges.push_back(GoogleMapTilesTileRange{
                level,
                minX,
                minY,
                maxX,
                maxY});
            minX >>= 1;
            minY >>= 1;
            maxX >>= 1;
            maxY >>= 1;
        }
    }
    return ranges;
}

GoogleMapTilesTileRange googleMapTilesCompleteAvailabilityRange(
    const TileKey& requestedKey,
    int maximumLevel) {
    const std::unique_ptr<TileScheme> scheme =
        TileScheme::createXYZWebMercator();
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    scheme->tileRange(
        scheme->tileToRectangle(requestedKey),
        maximumLevel,
        minX,
        minY,
        maxX,
        maxY);
    return GoogleMapTilesTileRange{
        maximumLevel,
        minX,
        minY,
        maxX,
        maxY};
}

std::string googleMapTilesViewportUrlForTile(
    const GoogleMapTilesExistingSessionOptions& options,
    const TileKey& key) {
    const std::unique_ptr<TileScheme> scheme =
        TileScheme::createXYZWebMercator();
    const Rectangle rectangle = scheme->tileToRectangle(key);
    return googleMapTilesViewportUrl(
        options,
        key.z,
        rectangle.westDegrees(),
        rectangle.southDegrees(),
        rectangle.eastDegrees(),
        rectangle.northDegrees());
}

GoogleMapTilesImageryProvider::GoogleMapTilesImageryProvider(
    GoogleMapTilesExistingSessionOptions options,
    std::string attribution)
    : XYZImageryProvider(std::string(), std::move(attribution))
    , options_(std::move(options)) {
    options_.apiBaseUrl = ensureTrailingSlash(options_.apiBaseUrl);
    setSchemeId("XYZ-WebMercator");
    setZoomRange(0, options_.maximumLevel < 0 ? 0 : options_.maximumLevel);
    setTileSize(options_.tileWidth < 1 ? 1 : options_.tileWidth,
                options_.tileHeight < 1 ? 1 : options_.tileHeight);
}

std::string GoogleMapTilesImageryProvider::id() const {
    std::ostringstream oss;
    oss << "google-map-tiles-"
        << std::hash<std::string>{}(options_.apiBaseUrl + options_.session);
    return oss.str();
}

bool GoogleMapTilesImageryProvider::supportsTile(const TileKey& key) const {
    if (!XYZImageryProvider::supportsTile(key)) {
        return false;
    }
    if (!hasKnownAvailability()) {
        return true;
    }
    if (isTileKnownAvailable(key)) {
        return true;
    }
    return !isTileInCompleteAvailabilityRange(key);
}

std::string GoogleMapTilesImageryProvider::buildUrl(
    const TileKey& key) const {
    if (!supportsTile(key)) {
        return std::string();
    }

    const std::string path =
        "v1/2dtiles/" + std::to_string(key.z) + "/" +
        std::to_string(key.x) + "/" +
        std::to_string(invertedY(key.z, key.y));
    return withQuery(
        options_.apiBaseUrl + path,
        {{"session", options_.session}, {"key", options_.key}});
}

void GoogleMapTilesImageryProvider::requestTile(
    const TileKey& key,
    CancellationToken token,
    TileCallback callback,
    HttpRequestPriority priority) {
    if (!XYZImageryProvider::supportsTile(key)) {
        callback(key, nullptr);
        return;
    }
    if (isTileKnownAvailable(key)) {
        XYZImageryProvider::requestTile(
            key,
            std::move(token),
            std::move(callback),
            priority);
        return;
    }
    if (isTileInCompleteAvailabilityRange(key)) {
        callback(key, nullptr);
        return;
    }

    if (token.isCancelled()) {
        callback(key, nullptr);
        return;
    }

    const std::string viewportUrl =
        googleMapTilesViewportUrlForTile(options_, key);
    auto requestHandle =
        std::make_shared<std::unique_ptr<HttpRequest>>();
    auto callbackPtr =
        std::make_shared<TileCallback>(std::move(callback));
    auto tokenPtr =
        std::make_shared<CancellationToken>(std::move(token));
    auto onViewport =
        [this,
         key,
         priority,
         viewportUrl,
         callbackPtr,
         tokenPtr,
         requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
            (void)requestHandle;
            if (tokenPtr->isCancelled() ||
                statusCode != 200 ||
                body.empty()) {
                (*callbackPtr)(key, nullptr);
                return;
            }

            GoogleMapTilesViewportParseResult viewport =
                parseGoogleMapTilesViewportResponse(
                    std::string(body.begin(), body.end()));
            applyViewportAvailability(viewport, key);
            if (!viewport.valid || !supportsTile(key)) {
                (*callbackPtr)(key, nullptr);
                return;
            }

            XYZImageryProvider::requestTile(
                key,
                *tokenPtr,
                std::move(*callbackPtr),
                priority);
        };

    if (platformBridge()) {
        *requestHandle = platformBridge()->get(
            viewportUrl,
            std::move(onViewport),
            {priority});
    } else {
        *requestHandle = CurlMultiRequestScheduler::shared().get(
            viewportUrl,
            std::move(onViewport),
            {priority});
    }
}

void GoogleMapTilesImageryProvider::addAvailableTileRanges(
    const std::vector<GoogleMapTilesTileRange>& ranges) {
    std::lock_guard<std::mutex> lock(availabilityMutex_);
    availableRanges_.insert(
        availableRanges_.end(),
        ranges.begin(),
        ranges.end());
}

void GoogleMapTilesImageryProvider::addCompleteAvailabilityRanges(
    const std::vector<GoogleMapTilesTileRange>& ranges) {
    std::lock_guard<std::mutex> lock(availabilityMutex_);
    completeAvailabilityRanges_.insert(
        completeAvailabilityRanges_.end(),
        ranges.begin(),
        ranges.end());
}

void GoogleMapTilesImageryProvider::loadCredits(HttpRequestPriority priority) {
    if (options_.session.empty() || options_.key.empty() ||
        options_.maximumLevel < 0) {
        return;
    }

    size_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(creditMutex_);
        ++creditLoadSerial_;
        serial = creditLoadSerial_;
        creditRequests_.clear();
        creditRequests_.reserve(static_cast<size_t>(options_.maximumLevel) + 1);
    }

    struct CreditLoadState {
        std::mutex mutex;
        size_t pending = 0;
        std::vector<std::string> copyrights;
    };
    auto state = std::make_shared<CreditLoadState>();
    state->pending = static_cast<size_t>(options_.maximumLevel) + 1;
    state->copyrights.resize(state->pending);

    for (int zoom = 0; zoom <= options_.maximumLevel; ++zoom) {
        const std::string url = googleMapTilesViewportUrl(
            options_,
            zoom,
            -180.0,
            -90.0,
            180.0,
            90.0);
        auto onCredit =
            [this, state, serial, zoom](int statusCode,
                                        std::vector<uint8_t> body) {
                bool complete = false;
                std::vector<std::string> copyrights;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (statusCode == 200 && !body.empty()) {
                        state->copyrights[static_cast<size_t>(zoom)] =
                            parseGoogleMapTilesViewportCopyright(
                                std::string(body.begin(), body.end()));
                    }
                    if (state->pending > 0) {
                        --state->pending;
                    }
                    complete = state->pending == 0;
                    if (complete) {
                        copyrights = state->copyrights;
                    }
                }

                if (!complete) {
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(creditMutex_);
                    if (serial != creditLoadSerial_) {
                        return;
                    }
                }

                std::string combined =
                    combineGoogleMapTilesCredits(copyrights);
                if (!combined.empty()) {
                    setAttribution(std::move(combined));
                }
            };

        std::unique_ptr<HttpRequest> request;
        if (platformBridge()) {
            request = platformBridge()->get(url, std::move(onCredit),
                                            {priority});
        } else {
            request = CurlMultiRequestScheduler::shared().get(
                url,
                std::move(onCredit),
                {priority});
        }

        std::lock_guard<std::mutex> lock(creditMutex_);
        if (serial == creditLoadSerial_ && request) {
            creditRequests_.push_back(std::move(request));
        }
    }
}

void GoogleMapTilesImageryProvider::applyViewportAvailability(
    const GoogleMapTilesViewportParseResult& viewport,
    const TileKey& requestedKey) {
    if (!viewport.valid) {
        return;
    }
    addAvailableTileRanges(googleMapTilesViewportTileRanges(viewport));
    if (viewport.complete && requestedKey.schemeId == schemeId()) {
        addCompleteAvailabilityRanges(
            {googleMapTilesCompleteAvailabilityRange(
                requestedKey,
                maxZoom())});
    }
}

bool GoogleMapTilesImageryProvider::hasKnownAvailability() const {
    std::lock_guard<std::mutex> lock(availabilityMutex_);
    return !availableRanges_.empty() || !completeAvailabilityRanges_.empty();
}

bool GoogleMapTilesImageryProvider::isTileKnownAvailable(
    const TileKey& key) const {
    std::lock_guard<std::mutex> lock(availabilityMutex_);
    return tileInRanges(key, availableRanges_, schemeId());
}

bool GoogleMapTilesImageryProvider::isTileInCompleteAvailabilityRange(
    const TileKey& key) const {
    std::lock_guard<std::mutex> lock(availabilityMutex_);
    return tileInRanges(key, completeAvailabilityRanges_, schemeId());
}

GoogleMapTilesImagerySource createGoogleMapTilesImagerySource(
    GoogleMapTilesExistingSessionOptions options,
    std::string attribution) {
    GoogleMapTilesImagerySource source;
    source.provider = std::make_unique<GoogleMapTilesImageryProvider>(
        std::move(options),
        std::move(attribution));
    source.scheme = TileScheme::createXYZWebMercator();
    return source;
}

} // namespace earth_engine
