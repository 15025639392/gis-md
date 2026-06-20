#include "TileMapServiceImageryProvider.h"

#include "earth_engine/tiling/TileScheme.h"

#include <functional>
#include <sstream>
#include <utility>

namespace earth_engine {

TileMapServiceImageryProvider::TileMapServiceImageryProvider(
    std::string tileMapResourceUrl,
    TileMapServiceMetadata metadata,
    std::string attribution)
    : XYZImageryProvider(std::string(), std::move(attribution))
    , tileMapResourceUrl_(std::move(tileMapResourceUrl))
    , metadata_(std::move(metadata)) {
    setSchemeId(metadata_.schemeId);
    setZoomRange(static_cast<int>(metadata_.minimumLevel),
                 static_cast<int>(metadata_.maximumLevel));
    setTileSize(static_cast<int>(metadata_.tileWidth),
                static_cast<int>(metadata_.tileHeight));
}

std::string TileMapServiceImageryProvider::id() const {
    std::ostringstream oss;
    oss << "tms-" << std::hash<std::string>{}(tileMapResourceUrl_);
    return oss.str();
}

std::string TileMapServiceImageryProvider::buildUrl(const TileKey& key) const {
    return tileMapServiceTileUrlForKey(tileMapResourceUrl_, metadata_, key)
        .value_or(std::string());
}

bool TileMapServiceImageryProvider::supportsTile(const TileKey& key) const {
    return XYZImageryProvider::supportsTile(key) &&
           tileMapServiceTileUrlForKey(tileMapResourceUrl_, metadata_, key)
               .has_value();
}

void TileMapServiceImageryProvider::requestTile(
    const TileKey& key,
    CancellationToken token,
    TileCallback callback,
    HttpRequestPriority priority) {
    if (token.isCancelled() || !supportsTile(key)) {
        callback(key, nullptr);
        return;
    }

    XYZImageryProvider::requestTile(
        key,
        std::move(token),
        std::move(callback),
        priority);
}

TileMapServiceImagerySource createTileMapServiceImagerySource(
    const std::string& tileMapResourceUrl,
    const std::string& tileMapResourceXml,
    const std::string& attribution) {
    TileMapServiceImagerySource source;
    if (!tileMapServiceXmlIsLoadable(tileMapResourceXml)) {
        return source;
    }

    TileMapServiceMetadata metadata =
        parseTileMapServiceMetadata(tileMapResourceXml);
    if (metadata.tileSets.empty()) {
        return source;
    }

    source.coverageRectangle =
        tileMapServiceResolvedGeographicCoverageRectangle(metadata);
    source.scheme = tileMapServiceTileScheme(metadata);
    source.provider = std::make_unique<TileMapServiceImageryProvider>(
        tileMapResourceUrl,
        std::move(metadata),
        attribution);
    return source;
}

} // namespace earth_engine
