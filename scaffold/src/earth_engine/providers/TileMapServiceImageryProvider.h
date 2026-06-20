#pragma once

#include "TileMapServiceUrl.h"
#include "XYZImageryProvider.h"

#include <memory>
#include <optional>
#include <string>

namespace earth_engine {

class TileScheme;
class TileMapServiceImageryProvider;

struct TileMapServiceImagerySource {
    std::unique_ptr<TileMapServiceImageryProvider> provider;
    std::unique_ptr<TileScheme> scheme;
    std::optional<Rectangle> coverageRectangle;
};

/// Tile Map Service imagery provider aligned with cesium-native's
/// TileMapServiceTileProvider URL and level selection semantics.
class TileMapServiceImageryProvider : public XYZImageryProvider {
public:
    TileMapServiceImageryProvider(std::string tileMapResourceUrl,
                                  TileMapServiceMetadata metadata,
                                  std::string attribution = "");

    std::string id() const override;
    std::string type() const override { return "tms-imagery"; }

    std::string buildUrl(const TileKey& key) const override;
    bool supportsTile(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    const TileMapServiceMetadata& metadata() const { return metadata_; }

private:
    std::string tileMapResourceUrl_;
    TileMapServiceMetadata metadata_;
};

TileMapServiceImagerySource createTileMapServiceImagerySource(
    const std::string& tileMapResourceUrl,
    const std::string& tileMapResourceXml,
    const std::string& attribution = "");

} // namespace earth_engine
